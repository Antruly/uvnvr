<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import UsageMeter from '@/components/UsageMeter.vue'
import { api } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'DashboardView' })

interface Summary {
  version: string
  commit: string
  serial: string
  hostname: string
  os: string
  uptimeSeconds: number
  setupRequired: boolean
  maintenance: boolean
  cpu: { cores: number; usagePercent: number }
  memory: { totalBytes: number; usedBytes: number; freeBytes: number; usagePercent: number }
  disk: {
    path: string
    totalBytes: number
    usedBytes: number
    freeBytes: number
    usagePercent: number
  }
  counts: {
    users: number
    sessions: number
    audit: number
    channels: number
    recordings: number
  }
}

const { t } = useI18n()
const auth = useAuthStore()
const summary = ref<Summary | null>(null)
const error = ref('')
const loading = ref(true)
const updatedAt = ref<Date | null>(null)
let timer: number | undefined

function formatBytes(value: number) {
  if (!Number.isFinite(value) || value <= 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB']
  const index = Math.min(units.length - 1, Math.floor(Math.log(value) / Math.log(1024)))
  const amount = value / 1024 ** index
  return `${amount.toFixed(amount >= 100 ? 0 : amount >= 10 ? 1 : 2)} ${units[index]}`
}

function formatDuration(totalSeconds: number) {
  const seconds = Math.max(0, Math.floor(totalSeconds))
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  if (days > 0) return `${days}d ${hours}h ${minutes}m`
  if (hours > 0) return `${hours}h ${minutes}m`
  return `${minutes}m ${seconds % 60}s`
}

function tone(percent: number): 'ok' | 'warn' | 'danger' {
  if (percent >= 90) return 'danger'
  if (percent >= 70) return 'warn'
  return 'ok'
}

const memoryDetail = computed(() => {
  if (!summary.value) return ''
  return `${formatBytes(summary.value.memory.usedBytes)} / ${formatBytes(summary.value.memory.totalBytes)}`
})

const diskDetail = computed(() => {
  if (!summary.value) return ''
  return `${formatBytes(summary.value.disk.usedBytes)} / ${formatBytes(summary.value.disk.totalBytes)}`
})

const updatedText = computed(() =>
  updatedAt.value
    ? new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit', second: '2-digit' }).format(updatedAt.value)
    : '—',
)

async function load() {
  error.value = ''
  try {
    summary.value = await api<Summary>('/api/v1/dashboard/summary')
    updatedAt.value = new Date()
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  void load()
  timer = window.setInterval(load, 15_000)
})

onBeforeUnmount(() => {
  if (timer) window.clearInterval(timer)
})
</script>

<template>
  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else-if="error" class="panel empty-state error-state">
    <AppIcon name="info" :size="26" />
    <p>{{ error }}</p>
    <button class="button" @click="load">
      <AppIcon name="refresh" :size="16" />
      {{ t('common.retry') }}
    </button>
  </section>

  <template v-else-if="summary">
    <section class="dash-hero panel">
      <div>
        <span class="pill accent">{{ t('dashboard.title') }}</span>
        <h2>{{ auth.user?.username }} · {{ summary.hostname }}</h2>
        <p class="dash-subtitle">{{ t('dashboard.intro') }}</p>
        <div class="dash-meta">
          <span><AppIcon name="shield" :size="14" /> {{ summary.serial }}</span>
          <span>{{ t('common.version') }} {{ summary.version }}</span>
          <span>{{ summary.os }}</span>
          <span>{{ t('dashboard.uptime') }} {{ formatDuration(summary.uptimeSeconds) }}</span>
        </div>
      </div>
      <div class="dash-side">
        <div class="dash-status-grid">
          <div>
            <span>{{ t('dashboard.channels') }}</span>
            <strong>{{ summary.counts.channels }}</strong>
          </div>
          <div>
            <span>{{ t('dashboard.recordings') }}</span>
            <strong>{{ summary.counts.recordings }}</strong>
          </div>
          <div>
            <span>{{ t('dashboard.users') }}</span>
            <strong>{{ summary.counts.users }}</strong>
          </div>
          <div>
            <span>{{ t('dashboard.storageUsage') }}</span>
            <strong>{{ Math.round(summary.disk.usagePercent) }}%</strong>
          </div>
        </div>
        <div class="dash-actions">
          <span class="dash-updated">{{ t('dashboard.updated') }} {{ updatedText }}</span>
          <button class="button" @click="load">
            <AppIcon name="refresh" :size="16" />
            {{ t('common.refresh') }}
          </button>
        </div>
      </div>
    </section>

    <section class="metric-grid">
      <article class="panel metric-card">
        <div class="metric-head">
          <span class="feature-icon"><AppIcon name="settings" :size="20" /></span>
          <span class="metric-title">{{ t('dashboard.cpu') }}</span>
        </div>
        <UsageMeter
          :label="`${summary.cpu.cores} ${t('dashboard.cores')}`"
          :percent="summary.cpu.usagePercent"
          :tone="tone(summary.cpu.usagePercent)"
        />
      </article>

      <article class="panel metric-card">
        <div class="metric-head">
          <span class="feature-icon"><AppIcon name="storage" :size="20" /></span>
          <span class="metric-title">{{ t('dashboard.memory') }}</span>
        </div>
        <UsageMeter
          :label="t('dashboard.memory')"
          :percent="summary.memory.usagePercent"
          :detail="memoryDetail"
          :tone="tone(summary.memory.usagePercent)"
        />
      </article>

      <article class="panel metric-card">
        <div class="metric-head">
          <span class="feature-icon"><AppIcon name="storage" :size="20" /></span>
          <span class="metric-title">{{ t('dashboard.storageUsage') }}</span>
        </div>
        <UsageMeter
          :label="summary.disk.path"
          :percent="summary.disk.usagePercent"
          :detail="diskDetail"
          :tone="tone(summary.disk.usagePercent)"
        />
      </article>

      <article class="panel metric-card">
        <div class="metric-head">
          <span class="feature-icon"><AppIcon name="users" :size="20" /></span>
          <span class="metric-title">{{ t('dashboard.users') }}</span>
        </div>
        <div class="count-row">
          <div>
            <strong>{{ summary.counts.users }}</strong>
            <span>{{ t('dashboard.users') }}</span>
          </div>
          <div>
            <strong>{{ summary.counts.sessions }}</strong>
            <span>{{ t('dashboard.sessions') }}</span>
          </div>
          <div>
            <strong>{{ summary.counts.audit }}</strong>
            <span>{{ t('dashboard.auditEvents') }}</span>
          </div>
        </div>
      </article>
    </section>

    <section class="two-column">
      <article class="panel info-card">
        <h3>{{ t('dashboard.runtime') }}</h3>
        <dl class="meta-list">
          <div>
            <dt>{{ t('dashboard.hostname') }}</dt>
            <dd>{{ summary.hostname }}</dd>
          </div>
          <div>
            <dt>{{ t('dashboard.operatingSystem') }}</dt>
            <dd>{{ summary.os }}</dd>
          </div>
          <div>
            <dt>{{ t('dashboard.storagePath') }}</dt>
            <dd>{{ summary.disk.path }}</dd>
          </div>
          <div>
            <dt>{{ t('dashboard.channels') }}</dt>
            <dd>{{ summary.counts.channels }}</dd>
          </div>
          <div>
            <dt>{{ t('dashboard.recordings') }}</dt>
            <dd>{{ summary.counts.recordings }}</dd>
          </div>
          <div>
            <dt>{{ t('dashboard.maintenance') }}</dt>
            <dd>{{ summary.maintenance ? t('common.yes') : t('common.no') }}</dd>
          </div>
        </dl>
        <p class="small-note">{{ t('dashboard.recordingsEmpty') }}</p>
      </article>

      <article class="panel info-card">
        <h3>{{ t('dashboard.m0') }}</h3>
        <p class="muted">{{ t('dashboard.m0Body') }}</p>
        <ul class="check-list compact">
          <li><AppIcon name="check" :size="16" /> {{ t('dashboard.securityBody') }}</li>
          <li><AppIcon name="check" :size="16" /> {{ t('dashboard.runtimeBody') }}</li>
          <li><AppIcon name="check" :size="16" /> {{ t('dashboard.storageBody') }}</li>
        </ul>
        <h3 class="next-heading">{{ t('dashboard.next') }}</h3>
        <p class="muted">{{ t('dashboard.nextBody') }}</p>
        <RouterLink class="inline-link" to="/preview">
          {{ t('nav.preview') }}
          <AppIcon name="arrow" :size="16" />
        </RouterLink>
      </article>
    </section>
  </template>
</template>
