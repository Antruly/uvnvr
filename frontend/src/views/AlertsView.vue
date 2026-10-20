<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'

defineOptions({ name: 'AlertsView' })

interface Alert {
  id: number
  channelId: number
  channelName: string
  type: string
  level: string
  message: string
  snapshotPath: string
  acknowledged: boolean
  createdAt: number
}

const { t } = useI18n()
const alerts = ref<Alert[]>([])
const loading = ref(true)
const error = ref('')
const notice = ref('')
const filters = reactive({ level: '', acknowledged: '' })

const unacknowledged = computed(() => alerts.value.filter((alert) => !alert.acknowledged).length)

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3000)
}

function formatTime(seconds: number) {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'short',
    timeStyle: 'medium',
  }).format(new Date(seconds * 1000))
}

function typeLabel(type: string) {
  const key = `alerts.types.${type}`
  const translated = t(key)
  return translated === key ? type : translated
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const params = new URLSearchParams()
    if (filters.level) params.set('level', filters.level)
    if (filters.acknowledged) params.set('acknowledged', filters.acknowledged)
    alerts.value = await api<Alert[]>(`/api/v1/alerts?${params.toString()}`)
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

async function acknowledge(alert: Alert) {
  try {
    await api(`/api/v1/alerts/${alert.id}/ack`, { method: 'POST' })
    alert.acknowledged = true
    flash(t('alerts.acknowledged'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

onMounted(load)
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.alerts') }}</span>
      <h2>{{ t('alerts.title') }}</h2>
      <p>{{ t('alerts.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <span class="muted">{{ t('alerts.unread', { count: unacknowledged }) }}</span>
      <select v-model="filters.level" class="input filter-select" @change="load">
        <option value="">{{ t('alerts.allLevels') }}</option>
        <option value="info">info</option>
        <option value="warning">warning</option>
        <option value="error">error</option>
      </select>
      <select v-model="filters.acknowledged" class="input filter-select" @change="load">
        <option value="">{{ t('alerts.allStates') }}</option>
        <option value="0">{{ t('alerts.unacknowledged') }}</option>
        <option value="1">{{ t('alerts.acknowledged') }}</option>
      </select>
      <button class="button" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else class="alert-list">
    <article v-for="alert in alerts" :key="alert.id" class="panel alert-card" :class="[alert.level, { ack: alert.acknowledged }]">
      <div class="alert-icon">
        <AppIcon :name="alert.type === 'motion' ? 'camera' : alert.type === 'storage_full' ? 'storage' : 'bell'" :size="20" />
      </div>
      <div class="alert-body">
        <div class="alert-head">
          <strong>{{ typeLabel(alert.type) }}</strong>
          <span class="status-pill" :class="alert.level">{{ alert.level }}</span>
          <span class="muted">{{ formatTime(alert.createdAt) }}</span>
          <span v-if="alert.channelName" class="pill">{{ alert.channelName }}</span>
        </div>
        <p>{{ alert.message }}</p>
        <div class="alert-actions">
          <a v-if="alert.snapshotPath" class="inline-link" :href="`/api/v1/alerts/${alert.id}/snapshot`" target="_blank">
            <AppIcon name="camera" :size="14" />
            {{ t('alerts.snapshot') }}
          </a>
          <button v-if="!alert.acknowledged" class="button small" @click="acknowledge(alert)">
            <AppIcon name="check" :size="14" />
            {{ t('alerts.acknowledge') }}
          </button>
          <span v-else class="muted small-note">{{ t('alerts.acknowledged') }}</span>
        </div>
      </div>
    </article>
    <div v-if="alerts.length === 0" class="panel empty-state">
      <AppIcon name="bell" :size="28" />
      <span>{{ t('alerts.empty') }}</span>
    </div>
  </section>
</template>
