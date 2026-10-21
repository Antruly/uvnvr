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
  event: {
    enabled: boolean
    preSeconds: number
    postSeconds: number
    maxSeconds: number
  }
  motion: {
    enabled: boolean
    sensitivity: number
    cooldownSeconds: number
    region: { x: number; y: number; width: number; height: number }
  }
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
  buildVersion?: string
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
  scopes?: string[]
}

const scopeGroups = [
  { key: 'preview', scopes: ['preview', 'snapshots'] },
  { key: 'channels', scopes: ['channels.read', 'channels.create', 'channels.delete', 'channels.manage'] },
  { key: 'playback', scopes: ['playback', 'download'] },
  { key: 'alerts', scopes: ['alerts'] },
  { key: 'plans', scopes: ['plans'] },
  { key: 'storage', scopes: ['storage'] },
  { key: 'time', scopes: ['time', 'system'] },
  { key: 'rtsp', scopes: ['rtsp'] },
  { key: 'audit', scopes: ['audit'] },
  { key: 'users', scopes: ['users'] },
] as const

const allScopes = scopeGroups.flatMap((group) => [...group.scopes])

interface StartupConfig {
  http: { port: number; host: string }
  rtsp: { port: number }
  dataDir: string
  maintenance: boolean
  storage: { cacheMb: number; readaheadMb: number }
  config: { path: string; loaded: boolean }
}

interface ServiceStatus {
  platform: string
  manager: string
  detail: string
  supported: boolean
  installed: boolean
  running: boolean
  autostart: boolean
  runningAsService: boolean
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
  event: {
    enabled: true,
    preSeconds: 10,
    postSeconds: 30,
    maxSeconds: 300,
  },
  motion: {
    enabled: true,
    sensitivity: 50,
    cooldownSeconds: 60,
    region: { x: 0, y: 0, width: 100, height: 100 },
  },
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
const service = ref<ServiceStatus | null>(null)
const serviceBusy = ref(false)
const tokens = ref<TokenRow[]>([])
const loading = ref(true)
const saving = ref(false)
const error = ref('')
const notice = ref('')
const newToken = reactive({ name: '', expiresInDays: 0, scopes: [] as string[] })
const createdToken = ref('')
const serialEdit = ref('')
const versionEdit = ref('')

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
    const [settingsResponse, summaryResponse, tokensResponse, startupResponse, serviceResponse] =
      await Promise.all([
        api<Settings>('/api/v1/settings'),
        api<Summary>('/api/v1/system/overview'),
        canUseApi() ? api<TokenRow[]>('/api/v1/tokens') : Promise.resolve([]),
        api<StartupConfig>('/api/v1/system/config'),
        api<ServiceStatus>('/api/v1/system/service'),
      ])
    Object.assign(settings.security, settingsResponse.security)
    Object.assign(settings.audit, settingsResponse.audit)
    Object.assign(settings.event, settingsResponse.event)
    Object.assign(settings.motion, settingsResponse.motion)
    if (settingsResponse.motion?.region) Object.assign(settings.motion.region, settingsResponse.motion.region)
    Object.assign(settings.system, settingsResponse.system)
    Object.assign(settings.smtp, settingsResponse.smtp)
    summary.value = summaryResponse
    serialEdit.value = summaryResponse.serial
    versionEdit.value = summaryResponse.version
    tokens.value = tokensResponse
    startup.value = startupResponse
    service.value = serviceResponse
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
        event: {
          enabled: settings.event.enabled,
          preSeconds: Number(settings.event.preSeconds),
          postSeconds: Number(settings.event.postSeconds),
          maxSeconds: Number(settings.event.maxSeconds),
        },
        motion: {
          enabled: settings.motion.enabled,
          sensitivity: Number(settings.motion.sensitivity),
          cooldownSeconds: Number(settings.motion.cooldownSeconds),
          region: {
            x: Number(settings.motion.region.x),
            y: Number(settings.motion.region.y),
            width: Number(settings.motion.region.width),
            height: Number(settings.motion.region.height),
          },
        },
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
    Object.assign(settings.event, updated.event)
    Object.assign(settings.motion, updated.motion)
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

async function saveSerial() {
  error.value = ''
  try {
    const result = await api<{ serial: string }>('/api/v1/system/serial', {
      method: 'PUT',
      body: JSON.stringify({ serial: serialEdit.value }),
    })
    if (summary.value) summary.value.serial = result.serial
    flash(t('system.maintenanceSaved'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function saveVersion() {
  error.value = ''
  try {
    await api('/api/v1/system/version', {
      method: 'PUT',
      body: JSON.stringify({ version: versionEdit.value }),
    })
    await load()
    flash(t('system.maintenanceSaved'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
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
        scopes: newToken.scopes,
      }),
    })
    createdToken.value = response.token
    newToken.name = ''
    newToken.expiresInDays = 0
    newToken.scopes = []
    flash(t('system.tokenCreated'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

function toggleScope(scope: string) {
  if (newToken.scopes.includes(scope)) {
    newToken.scopes = newToken.scopes.filter((item) => item !== scope)
  } else {
    newToken.scopes = [...newToken.scopes, scope]
  }
}

function groupSelected(scopes: readonly string[]) {
  return scopes.every((scope) => newToken.scopes.includes(scope))
}

function toggleGroup(scopes: readonly string[]) {
  if (groupSelected(scopes)) {
    newToken.scopes = newToken.scopes.filter((scope) => !scopes.includes(scope))
  } else {
    newToken.scopes = Array.from(new Set([...newToken.scopes, ...scopes]))
  }
}

function allSelected() {
  return allScopes.every((scope) => newToken.scopes.includes(scope))
}

function toggleAllScopes() {
  newToken.scopes = allSelected() ? [] : [...allScopes]
}

function tokenScopeLabel(token: TokenRow) {
  if (!token.scopes || token.scopes.length === 0) return t('system.scopesInherited')
  return token.scopes
    .map((scope) => t(`system.scopes.${scope}`))
    .join(', ')
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
    const result = await api<{
      offsetMs: number
      applied: boolean
      applyError?: string
      cameras?: { synced: number; failed: number; skipped: number }
    }>('/api/v1/system/ntp/sync', { method: 'POST' })
    settings.system.ntpOffsetMs = result.offsetMs
    const cameraText = result.cameras
      ? ` · ${t('system.cameraSyncResult', {
          synced: result.cameras.synced,
          failed: result.cameras.failed,
        })}`
      : ''
    flash(
      (result.applied
        ? t('system.ntpSynced')
        : `${t('system.ntpOffset')}: ${result.offsetMs} ms${result.applyError ? ` (${result.applyError})` : ''}`) +
        cameraText,
    )
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function syncBrowserTime() {
  error.value = ''
  try {
    const result = await api<{
      serverTimeApplied: boolean
      applyError?: string
      cameras?: { synced: number; failed: number; skipped: number }
    }>('/api/v1/system/time/sync', {
      method: 'POST',
      body: JSON.stringify({ epochMs: Date.now() }),
    })
    const cameraText = result.cameras
      ? ` · ${t('system.cameraSyncResult', {
          synced: result.cameras.synced,
          failed: result.cameras.failed,
        })}`
      : ''
    flash(
      (result.serverTimeApplied
        ? t('system.browserTimeSynced')
        : `${result.applyError ?? t('common.error')}`) + cameraText,
    )
    await load()
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

async function loadService() {
  try {
    service.value = await api<ServiceStatus>('/api/v1/system/service')
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

function waitForRestart(attempt = 0) {
  window.setTimeout(async () => {
    try {
      service.value = await api<ServiceStatus>('/api/v1/system/service')
      await load()
    } catch {
      if (attempt < 15) waitForRestart(attempt + 1)
      else error.value = t('system.serviceFailed')
    }
  }, attempt === 0 ? 5000 : 2000)
}

async function installService() {
  if (!canManage()) return
  if (!window.confirm(t('system.serviceInstallConfirm'))) return
  serviceBusy.value = true
  error.value = ''
  try {
    await api('/api/v1/system/service/install', { method: 'POST' })
    flash(t('system.serviceRestarting'))
    waitForRestart()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('system.serviceFailed')
  } finally {
    serviceBusy.value = false
  }
}

async function uninstallService() {
  if (!canManage()) return
  if (!window.confirm(t('system.serviceUninstallConfirm'))) return
  serviceBusy.value = true
  error.value = ''
  try {
    const wasService = service.value?.runningAsService ?? false
    await api('/api/v1/system/service/uninstall', { method: 'POST' })
    flash(wasService ? t('system.serviceRestarting') : t('system.serviceUninstalled'))
    if (wasService) waitForRestart()
    else window.setTimeout(() => { void load() }, 1200)
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('system.serviceFailed')
  } finally {
    serviceBusy.value = false
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
              <button type="button" class="button small" @click="syncBrowserTime">
                {{ t('system.browserSync') }}
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
          <AppIcon name="playback" :size="20" />
          <h3>{{ t('system.eventRecording') }}</h3>
        </div>
        <label class="check-row">
          <input v-model="settings.event.enabled" type="checkbox" />
          <span>{{ t('system.eventEnabled') }}</span>
        </label>
        <div class="form-grid">
          <label class="field">
            <span>{{ t('system.eventPre') }}</span>
            <input
              v-model.number="settings.event.preSeconds"
              class="input"
              type="number"
              min="0"
              max="60"
            />
          </label>
          <label class="field">
            <span>{{ t('system.eventPost') }}</span>
            <input
              v-model.number="settings.event.postSeconds"
              class="input"
              type="number"
              min="1"
              max="3600"
            />
          </label>
          <label class="field">
            <span>{{ t('system.eventMax') }}</span>
            <input
              v-model.number="settings.event.maxSeconds"
              class="input"
              type="number"
              min="1"
              max="3600"
            />
          </label>
        </div>
        <p class="small-note">{{ t('system.eventHint') }}</p>
        <div class="modal-actions">
          <button class="button primary" type="submit" :disabled="saving || !canManage()">
            <AppIcon name="save" :size="15" />
            {{ t('system.save') }}
          </button>
        </div>
      </form>

      <form class="panel settings-card" @submit.prevent="save">
        <div class="section-title">
          <AppIcon name="camera" :size="20" />
          <h3>{{ t('system.motionDetection') }}</h3>
        </div>
        <label class="check-row">
          <input v-model="settings.motion.enabled" type="checkbox" />
          <span>{{ t('system.motionEnabled') }}</span>
        </label>
        <div class="form-grid">
          <label class="field">
            <span>{{ t('system.motionSensitivity') }}</span>
            <input
              v-model.number="settings.motion.sensitivity"
              class="input"
              type="number"
              min="1"
              max="100"
            />
          </label>
          <label class="field">
            <span>{{ t('system.motionCooldown') }}</span>
            <input
              v-model.number="settings.motion.cooldownSeconds"
              class="input"
              type="number"
              min="10"
              max="3600"
            />
          </label>
          <label class="field">
            <span>{{ t('system.motionRegionX') }}</span>
            <input v-model.number="settings.motion.region.x" class="input" type="number" min="0" max="100" />
          </label>
          <label class="field">
            <span>{{ t('system.motionRegionY') }}</span>
            <input v-model.number="settings.motion.region.y" class="input" type="number" min="0" max="100" />
          </label>
          <label class="field">
            <span>{{ t('system.motionRegionWidth') }}</span>
            <input
              v-model.number="settings.motion.region.width"
              class="input"
              type="number"
              min="1"
              max="100"
            />
          </label>
          <label class="field">
            <span>{{ t('system.motionRegionHeight') }}</span>
            <input
              v-model.number="settings.motion.region.height"
              class="input"
              type="number"
              min="1"
              max="100"
            />
          </label>
        </div>
        <p class="small-note">{{ t('system.motionHint') }}</p>
        <div class="modal-actions">
          <button class="button primary" type="submit" :disabled="saving || !canManage()">
            <AppIcon name="save" :size="15" />
            {{ t('system.save') }}
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

      <article v-if="service" class="panel settings-card">
        <div class="section-title">
          <AppIcon name="shield" :size="20" />
          <h3>{{ t('system.serviceTitle') }}</h3>
        </div>
        <dl class="meta-list">
          <div>
            <dt>{{ t('system.serviceManager') }}</dt>
            <dd>{{ service.manager }}<span v-if="service.detail"> · {{ service.detail }}</span></dd>
          </div>
          <div>
            <dt>{{ t('system.serviceState') }}</dt>
            <dd>
              {{
                service.installed
                  ? service.running
                    ? t('system.serviceRunning')
                    : t('system.serviceStopped')
                  : t('system.serviceNotInstalled')
              }}
            </dd>
          </div>
          <div>
            <dt>{{ t('system.serviceAutostart') }}</dt>
            <dd>{{ service.autostart ? t('system.serviceEnabled') : t('system.serviceDisabled') }}</dd>
          </div>
          <div>
            <dt>{{ t('system.serviceProcess') }}</dt>
            <dd>{{ service.runningAsService ? t('system.serviceModeService') : t('system.serviceModeConsole') }}</dd>
          </div>
        </dl>
        <p class="small-note">
          {{ service.supported ? t('system.serviceHint') : t('system.serviceUnsupported') }}
        </p>
        <div class="modal-actions">
          <button
            v-if="service.supported && !service.installed"
            type="button"
            class="button primary"
            :disabled="serviceBusy || !canManage()"
            @click="installService"
          >
            <AppIcon name="shield" :size="15" />
            {{ serviceBusy ? t('system.serviceBusy') : t('system.serviceInstall') }}
          </button>
          <button
            v-else-if="service.supported"
            type="button"
            class="button danger"
            :disabled="serviceBusy || !canManage()"
            @click="uninstallService"
          >
            {{ serviceBusy ? t('system.serviceBusy') : t('system.serviceUninstall') }}
          </button>
          <button type="button" class="button" :disabled="serviceBusy" @click="loadService">
            <AppIcon name="refresh" :size="15" />
            {{ t('common.refresh') }}
          </button>
        </div>
      </article>

      <article v-if="summary?.maintenance" class="panel settings-card">
        <div class="section-title">
          <AppIcon name="shield" :size="20" />
          <h3>{{ t('system.maintenanceOps') }}</h3>
        </div>
        <div class="form-grid">
          <label class="field">
            <span>{{ t('system.maintenanceSerial') }}</span>
            <input v-model="serialEdit" class="input" />
          </label>
          <label class="field">
            <span>{{ t('system.maintenanceVersion') }}</span>
            <input v-model="versionEdit" class="input" :placeholder="summary.buildVersion" />
          </label>
        </div>
        <p class="small-note">
          {{ t('system.maintenanceHint', { version: summary.buildVersion || summary.version }) }}
        </p>
        <div class="modal-actions">
          <button class="button" @click="saveSerial">
            <AppIcon name="save" :size="15" />
            {{ t('system.maintenanceSaveSerial') }}
          </button>
          <button class="button primary" @click="saveVersion">
            <AppIcon name="save" :size="15" />
            {{ t('system.maintenanceSaveVersion') }}
          </button>
        </div>
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

      <div class="token-create">
        <div class="token-form">
          <label class="field compact-field grow">
            <span>{{ t('system.tokenName') }}</span>
            <input v-model="newToken.name" class="input" maxlength="64" />
          </label>
          <label class="field compact-field">
            <span>{{ t('system.expiresInDays') }}</span>
            <input v-model.number="newToken.expiresInDays" class="input" type="number" min="0" max="3650" />
          </label>
        </div>

        <div class="scope-tree">
          <label class="check-row scope-all">
            <input type="checkbox" :checked="allSelected()" @change="toggleAllScopes" />
            <span>{{ t('system.allScopes') }}</span>
          </label>
          <div v-for="group in scopeGroups" :key="group.key" class="scope-group">
            <label class="check-row">
              <input
                type="checkbox"
                :checked="groupSelected(group.scopes)"
                @change="toggleGroup(group.scopes)"
              />
              <strong>{{ t(`system.scopeGroups.${group.key}`) }}</strong>
            </label>
            <div class="scope-children">
              <label v-for="scope in group.scopes" :key="scope" class="check-row">
                <input
                  type="checkbox"
                  :checked="newToken.scopes.includes(scope)"
                  @change="toggleScope(scope)"
                />
                <span>{{ t(`system.scopes.${scope}`) }}</span>
              </label>
            </div>
          </div>
        </div>

        <p class="small-note">{{ t('system.scopesHint') }}</p>
        <div class="modal-actions">
          <button class="button primary" type="button" :disabled="!newToken.name" @click="createToken">
            <AppIcon name="plus" :size="16" />
            {{ t('system.createToken') }}
          </button>
        </div>
      </div>

      <table class="data-table">
        <thead>
          <tr>
            <th>{{ t('system.tokenName') }}</th>
            <th>{{ t('system.tokenScopes') }}</th>
            <th>{{ t('users.created') }}</th>
            <th>{{ t('system.tokenExpires') }}</th>
            <th>{{ t('users.lastLogin') }}</th>
            <th>{{ t('common.actions') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="token in tokens" :key="token.id">
            <td><strong>{{ token.name }}</strong></td>
            <td class="muted token-scopes">{{ tokenScopeLabel(token) }}</td>
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
            <td colspan="6" class="empty-cell">{{ t('system.noTokens') }}</td>
          </tr>
        </tbody>
      </table>
    </section>
  </template>
</template>
