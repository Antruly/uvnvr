<script setup lang="ts">
import { onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'SystemView' })

interface Settings {
  security: {
    webMaxFailures: number
    webLockMinutes: number
    apiMaxFailures: number
    apiLockMinutes: number
  }
  audit: { retentionDays: number }
  system: {
    timezone: string
    language: string
    gpuEnabled: boolean
    ntpEnabled: boolean
    ntpServer: string
    ntpOffsetMs: number
  }
  smtp: {
    enabled: boolean
    host: string
    port: number
    username: string
    from: string
    to: string
    hasPassword: boolean
  }
}

interface Summary {
  version: string
  commit: string
  serial: string
  hostname: string
  os: string
  uptimeSeconds: number
  maintenance: boolean
  setupRequired: boolean
  disk: { path: string; usagePercent: number; usedBytes: number; totalBytes: number }
  counts: { users: number; sessions: number; audit: number }
  performance?: { tier: string; gpu: { available: boolean; vendor: string; enabled: boolean } }
  cache?: {
    hits: number
    misses: number
    bytesRead: number
    bytesCached: number
    entries: number
    capacityBytes: number
  }
}

interface TokenRow {
  id: number
  name: string
  createdAt: number
  expiresAt: number
  lastUsedAt: number
}

interface StartupConfig {
  http: { port: number; host: string }
  rtsp: { port: number }
  dataDir: string
  maintenance: boolean
  storage: { cacheMb: number; readaheadMb: number }
  config: { path: string; loaded: boolean }
}

const { t } = useI18n()
const auth = useAuthStore()

const settings = reactive<Settings>({
  security: {
    webMaxFailures: 10,
    webLockMinutes: 10,
    apiMaxFailures: 100,
    apiLockMinutes: 10,
  },
  audit: { retentionDays: 180 },
  system: {
    timezone: 'UTC',
    language: 'auto',
    gpuEnabled: true,
    ntpEnabled: false,
    ntpServer: 'pool.ntp.org',
    ntpOffsetMs: 0,
  },
  smtp: {
    enabled: false,
    host: 'smtp.qq.com',
    port: 465,
    username: '',
    from: '',
    to: '',
    hasPassword: false,
  },
})
const smtpPassword = ref('')
const summary = ref<Summary | null>(null)
const startup = ref<StartupConfig | null>(null)
const tokens = ref<TokenRow[]>([])
const loading = ref(true)
const saving = ref(false)
const error = ref('')
const notice = ref('')
const newToken = reactive({ name: '', expiresInDays: 0 })
const createdToken = ref('')

const canManage = () => auth.hasPermission('system.manage')
const canUseApi = () => auth.hasPermission('api.access')

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3500)
}

function formatTime(seconds: number) {
  if (!seconds) return t('common.never')
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
    timeStyle: 'short',
  }).format(new Date(seconds * 1000))
}

async function load() {
  error.value = ''
  try {
    const [settingsResponse, summaryResponse, tokensResponse, startupResponse] = await Promise.all([
      api<Settings>('/api/v1/settings'),
      api<Summary>('/api/v1/system/overview'),
      canUseApi() ? api<TokenRow[]>('/api/v1/tokens') : Promise.resolve([]),
      api<StartupConfig>('/api/v1/system/config'),
    ])
    Object.assign(settings.security, settingsResponse.security)
    Object.assign(settings.audit, settingsResponse.audit)
    Object.assign(settings.system, settingsResponse.system)
    Object.assign(settings.smtp, settingsResponse.smtp)
    summary.value = summaryResponse
    tokens.value = tokensResponse
    startup.value = startupResponse
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

async function save() {
  if (!canManage()) return
  saving.value = true
  error.value = ''
  try {
    const updated = await api<Settings>('/api/v1/settings', {
      method: 'PUT',
      body: JSON.stringify({
        security: settings.security,
        audit: settings.audit,
        system: settings.system,
        smtp: {
          enabled: settings.smtp.enabled,
          host: settings.smtp.host,
          port: Number(settings.smtp.port),
          username: settings.smtp.username,
          from: settings.smtp.from,
          to: settings.smtp.to,
          password: smtpPassword.value || undefined,
        },
      }),
    })
    Object.assign(settings.security, updated.security)
    Object.assign(settings.audit, updated.audit)
    Object.assign(settings.system, updated.system)
    Object.assign(settings.smtp, updated.smtp)
    smtpPassword.value = ''
    flash(t('system.saved'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    saving.value = false
  }
}

async function createToken() {
  error.value = ''
  try {
    const response = await api<{ id: number; name: string; token: string }>('/api/v1/tokens', {
      method: 'POST',
      body: JSON.stringify({
        name: newToken.name,
        expiresInDays: Number(newToken.expiresInDays) || 0,
      }),
    })
    createdToken.value = response.token
    newToken.name = ''
    newToken.expiresInDays = 0
    flash(t('system.tokenCreated'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function revokeToken(token: TokenRow) {
  if (!window.confirm(`${t('system.revoke')}: ${token.name}?`)) return
  try {
    await api(`/api/v1/tokens/${token.id}`, { method: 'DELETE' })
    flash(t('system.tokenRevoked'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function copyToken() {
  if (!createdToken.value) return
  try {
    await navigator.clipboard.writeText(createdToken.value)
    flash(t('system.copy'))
  } catch {
    /* Clipboard may be blocked; the token remains visible for manual copy. */
  }
}

async function syncNtp() {
  error.value = ''
  try {
    const result = await api<{ offsetMs: number; applied: boolean; applyError?: string }>(
      '/api/v1/system/ntp/sync',
      { method: 'POST' },
    )
    settings.system.ntpOffsetMs = result.offsetMs
    flash(
      result.applied
        ? t('system.ntpSynced')
        : `${t('system.ntpOffset')}: ${result.offsetMs} ms${result.applyError ? ` (${result.applyError})` : ''}`,
    )
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function testEmail() {
  error.value = ''
  try {
    await api('/api/v1/system/email/test', { method: 'POST' })
    flash(t('system.emailTestSent'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

onMounted(load)
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.settings') }}</span>
      <h2>{{ t('system.title') }}</h2>
      <p>{{ t('system.subtitle') }}</p>
    </div>
    <button class="button" @click="load">
      <AppIcon name="refresh" :size="16" />
      {{ t('common.refresh') }}
    </button>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <template v-else>
    <div class="settings-grid">
      <form class="panel settings-card" @submit.prevent="save">
        <div class="section-title">
          <AppIcon name="shield" :size="20" />
          <h3>{{ t('system.security') }}</h3>
        </div>
        <div class="form-grid">
          <label class="field">
            <span>{{ t('system.webMaxFailures') }}</span>
            <input v-model.number="settings.security.webMaxFailures" class="input" type="number" min="1" max="1000" />
          </label>
          <label class="field">
            <span>{{ t('system.webLockMinutes') }}</span>
            <input v-model.number="settings.security.webLockMinutes" class="input" type="number" min="1" max="1440" />
          </label>
          <label class="field">
            <span>{{ t('system.apiMaxFailures') }}</span>
            <input v-model.number="settings.security.apiMaxFailures" class="input" type="number" min="1" max="100000" />
          </label>
          <label class="field">
            <span>{{ t('system.apiLockMinutes') }}</span>
            <input v-model.number="settings.security.apiLockMinutes" class="input" type="number" min="1" max="1440" />
          </label>
          <label class="field">
            <span>{{ t('system.auditRetention') }}</span>
            <input v-model.number="settings.audit.retentionDays" class="input" type="number" min="1" max="3650" />
          </label>
          <label class="field">
            <span>{{ t('system.timezone') }}</span>
            <input v-model="settings.system.timezone" class="input" />
          </label>
          <label class="field">
            <span>{{ t('system.language') }}</span>
            <select v-model="settings.system.language" class="input">
              <option value="auto">auto</option>
              <option value="zh-CN">中文</option>
              <option value="en-US">English</option>
            </select>
          </label>
          <label class="check-row">
            <input v-model="settings.system.gpuEnabled" type="checkbox" />
            <span>{{ t('system.gpuEnabled') }}</span>
          </label>
          <label class="check-row">
            <input v-model="settings.system.ntpEnabled" type="checkbox" />
            <span>{{ t('system.ntpEnabled') }}</span>
          </label>
          <label class="field">
            <span>{{ t('system.ntpServer') }}</span>
            <input v-model="settings.system.ntpServer" class="input" />
          </label>
          <label class="field">
            <span>{{ t('system.ntpOffset') }}</span>
            <div class="date-nav">
              <input :value="`${settings.system.ntpOffsetMs} ms`" class="input" readonly />
              <button type="button" class="button small" @click="syncNtp">
                {{ t('system.ntpSync') }}
              </button>
            </div>
          </label>
        </div>
        <div class="modal-actions">
          <button class="button primary" type="submit" :disabled="saving || !canManage()">
            <AppIcon name="save" :size="16" />
            {{ saving ? t('common.loading') : t('system.save') }}
          </button>
        </div>
      </form>

      <form class="panel settings-card" @submit.prevent="save">
        <div class="section-title">
          <AppIcon name="bell" :size="20" />
          <h3>{{ t('system.email') }}</h3>
        </div>
        <label class="check-row">
          <input v-model="settings.smtp.enabled" type="checkbox" />
          <span>{{ t('system.emailEnabled') }}</span>
        </label>
        <div class="form-grid">
          <label class="field">
            <span>{{ t('system.emailHost') }}</span>
            <input v-model="settings.smtp.host" class="input" />
          </label>
          <label class="field">
            <span>{{ t('system.emailPort') }}</span>
            <input v-model.number="settings.smtp.port" class="input" type="number" min="1" max="65535" />
          </label>
          <label class="field">
            <span>{{ t('system.emailUsername') }}</span>
            <input v-model="settings.smtp.username" class="input" autocomplete="off" />
          </label>
          <label class="field">
            <span>{{ t('system.emailPassword') }}</span>
            <input
              v-model="smtpPassword"
              class="input"
              type="password"
              autocomplete="new-password"
              :placeholder="settings.smtp.hasPassword ? t('system.emailKeepPassword') : ''"
            />
          </label>
          <label class="field">
            <span>{{ t('system.emailFrom') }}</span>
            <input v-model="settings.smtp.from" class="input" type="email" />
          </label>
          <label class="field">
            <span>{{ t('system.emailTo') }}</span>
            <input v-model="settings.smtp.to" class="input" type="email" />
          </label>
        </div>
        <p class="small-note">{{ t('system.emailQQHint') }}</p>
        <div class="modal-actions">
          <button type="button" class="button" @click="testEmail">
            <AppIcon name="bell" :size="15" />
            {{ t('system.emailTest') }}
          </button>
          <button class="button primary" type="submit" :disabled="saving || !canManage()">
            <AppIcon name="save" :size="15" />
            {{ t('system.save') }}
          </button>
        </div>
      </form>

      <article class="panel settings-card">
        <div class="section-title">
          <AppIcon name="info" :size="20" />
          <h3>{{ t('system.systemInfo') }}</h3>
        </div>
        <dl v-if="summary" class="meta-list">
          <div><dt>{{ t('system.version') }}</dt><dd>{{ summary.version }}</dd></div>
          <div><dt>{{ t('common.commit') }}</dt><dd>{{ summary.commit }}</dd></div>
          <div><dt>{{ t('system.serial') }}</dt><dd>{{ summary.serial }}</dd></div>
          <div><dt>{{ t('system.hostname') }}</dt><dd>{{ summary.hostname }}</dd></div>
          <div><dt>{{ t('system.os') }}</dt><dd>{{ summary.os }}</dd></div>
          <div><dt>{{ t('system.disk') }}</dt><dd>{{ summary.disk.path }}</dd></div>
          <div><dt>{{ t('dashboard.users') }}</dt><dd>{{ summary.counts.users }}</dd></div>
          <div><dt>{{ t('dashboard.auditEvents') }}</dt><dd>{{ summary.counts.audit }}</dd></div>
          <div><dt>{{ t('system.maintenance') }}</dt><dd>{{ summary.maintenance ? t('common.yes') : t('common.no') }}</dd></div>
          <div v-if="summary.performance"><dt>{{ t('system.performance') }}</dt><dd>{{ summary.performance.tier }}</dd></div>
          <div v-if="summary.performance"><dt>{{ t('system.gpu') }}</dt><dd>{{ summary.performance.gpu.available ? summary.performance.gpu.vendor : t('common.none') }}</dd></div>
          <div v-if="summary.cache">
            <dt>{{ t('system.cacheStats') }}</dt>
            <dd>{{ summary.cache.hits }} / {{ summary.cache.misses }}</dd>
          </div>
        </dl>
      </article>

      <article v-if="startup" class="panel settings-card">
        <div class="section-title">
          <AppIcon name="settings" :size="20" />
          <h3>{{ t('system.startupConfig') }}</h3>
        </div>
        <dl class="meta-list">
          <div><dt>{{ t('system.httpPort') }}</dt><dd>{{ startup.http.port }}</dd></div>
          <div><dt>{{ t('system.rtspPort') }}</dt><dd>{{ startup.rtsp.port }}</dd></div>
          <div><dt>{{ t('system.httpHost') }}</dt><dd>{{ startup.http.host }}</dd></div>
          <div><dt>{{ t('system.dataDir') }}</dt><dd>{{ startup.dataDir }}</dd></div>
          <div v-if="startup.storage"><dt>{{ t('system.cacheSize') }}</dt><dd>{{ startup.storage.cacheMb }} MB</dd></div>
          <div v-if="startup.storage"><dt>{{ t('system.readahead') }}</dt><dd>{{ startup.storage.readaheadMb }} MB</dd></div>
          <div>
            <dt>{{ t('system.configPath') }}</dt>
            <dd>{{ startup.config.loaded ? startup.config.path : t('system.configDefaults') }}</dd>
          </div>
        </dl>
        <p class="small-note">{{ t('system.startupConfigHint') }}</p>
      </article>
    </div>

    <section v-if="canUseApi()" class="panel tokens-card">
      <div class="section-title">
        <AppIcon name="key" :size="20" />
        <h3>{{ t('system.tokens') }}</h3>
      </div>
      <p class="small-note">{{ t('system.tokenWarning') }}</p>

      <div v-if="createdToken" class="token-once">
        <strong>{{ t('system.tokenOnce') }}</strong>
        <div class="token-value">
          <code>{{ createdToken }}</code>
          <button class="button small" @click="copyToken">
            <AppIcon name="save" :size="14" />
            {{ t('system.copy') }}
          </button>
        </div>
      </div>

      <form class="token-form" @submit.prevent="createToken">
        <label class="field compact-field grow">
          <span>{{ t('system.tokenName') }}</span>
          <input v-model="newToken.name" class="input" required maxlength="64" />
        </label>
        <label class="field compact-field">
          <span>{{ t('system.expiresInDays') }}</span>
          <input v-model.number="newToken.expiresInDays" class="input" type="number" min="0" max="3650" />
        </label>
        <button class="button primary" type="submit">
          <AppIcon name="plus" :size="16" />
          {{ t('system.createToken') }}
        </button>
      </form>

      <table class="data-table">
        <thead>
          <tr>
            <th>{{ t('system.tokenName') }}</th>
            <th>{{ t('users.created') }}</th>
            <th>{{ t('system.tokenExpires') }}</th>
            <th>{{ t('users.lastLogin') }}</th>
            <th>{{ t('common.actions') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="token in tokens" :key="token.id">
            <td><strong>{{ token.name }}</strong></td>
            <td class="muted">{{ formatTime(token.createdAt) }}</td>
            <td class="muted">{{ token.expiresAt ? formatTime(token.expiresAt) : t('common.never') }}</td>
            <td class="muted">{{ formatTime(token.lastUsedAt) }}</td>
            <td>
              <button class="button small danger" @click="revokeToken(token)">
                <AppIcon name="trash" :size="14" />
                {{ t('system.revoke') }}
              </button>
            </td>
          </tr>
          <tr v-if="tokens.length === 0">
            <td colspan="5" class="empty-cell">{{ t('system.noTokens') }}</td>
          </tr>
        </tbody>
      </table>
    </section>
  </template>
</template>
