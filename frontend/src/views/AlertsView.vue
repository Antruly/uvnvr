<script setup lang="ts">
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useEventStore } from '@/stores/events'

defineOptions({ name: 'AlertsView' })

interface Alert {
  id: number
  channelId: number
  channelName: string
  type: string
  level: string
  message: string
  snapshotPath: string
  snapshotUrl: string
  sourceType: string
  sourceId: number
  sourceUrl: string
  acknowledged: boolean
  acknowledgedAt: number
  acknowledgedBy: string
  note: string
  createdAt: number
}

interface AckResult {
  ok: boolean
  acknowledged: boolean
  acknowledgedAt: number
  acknowledgedBy: string
  note: string
}

const { t } = useI18n()
const alerts = ref<Alert[]>([])
const loading = ref(true)
const error = ref('')
const notice = ref('')
const filters = reactive({ level: '', type: '', acknowledged: '' })
const ackingId = ref(0)
const ackNote = ref('')
const ackBusy = ref(false)
const brokenThumbs = reactive(new Set<number>())

const unacknowledged = computed(() => alerts.value.filter((alert) => !alert.acknowledged).length)

const typeOptions = [
  'ai_plate',
  'ai_face',
  'motion',
  'channel_offline',
  'channel_online',
  'storage_full',
  'disk_error',
  'service_error',
]

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3000)
}

function formatTime(seconds: number) {
  if (!seconds) return '—'
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

function thumbUrl(alert: Alert) {
  return alert.sourceUrl || alert.snapshotUrl
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const params = new URLSearchParams()
    if (filters.level) params.set('level', filters.level)
    if (filters.type) params.set('type', filters.type)
    if (filters.acknowledged) params.set('acknowledged', filters.acknowledged)
    alerts.value = await api<Alert[]>(`/api/v1/alerts?${params.toString()}`)
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

function startAck(alert: Alert) {
  ackingId.value = alert.id
  ackNote.value = ''
}

function cancelAck() {
  ackingId.value = 0
  ackNote.value = ''
}

async function confirmAck(alert: Alert) {
  ackBusy.value = true
  try {
    const result = await api<AckResult>(`/api/v1/alerts/${alert.id}/ack`, {
      method: 'POST',
      body: JSON.stringify({ note: ackNote.value.trim() }),
    })
    alert.acknowledged = result.acknowledged
    alert.acknowledgedAt = result.acknowledgedAt
    alert.acknowledgedBy = result.acknowledgedBy
    alert.note = result.note
    ackingId.value = 0
    ackNote.value = ''
    flash(t('alerts.acknowledged'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    ackBusy.value = false
  }
}

const events = useEventStore()
watch(
  () => events.seq,
  () => void load(),
)

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
      <select v-model="filters.type" class="input filter-select" @change="load">
        <option value="">{{ t('alerts.allTypes') }}</option>
        <option v-for="type in typeOptions" :key="type" :value="type">{{ typeLabel(type) }}</option>
      </select>
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
      <a
        v-if="thumbUrl(alert) && !brokenThumbs.has(alert.id)"
        class="alert-thumb"
        :href="thumbUrl(alert)"
        target="_blank"
        rel="noopener"
      >
        <img :src="thumbUrl(alert)" :alt="alert.message" loading="lazy" @error="brokenThumbs.add(alert.id)" />
      </a>
      <div v-else class="alert-icon">
        <AppIcon :name="alert.type === 'motion' ? 'camera' : alert.type === 'storage_full' ? 'storage' : 'bell'" :size="20" />
      </div>
      <div class="alert-body">
        <div class="alert-head">
          <strong>{{ typeLabel(alert.type) }}</strong>
          <span class="status-pill" :class="alert.level">{{ alert.level }}</span>
          <span class="muted">{{ formatTime(alert.createdAt) }}</span>
          <span v-if="alert.channelName" class="pill">{{ alert.channelName }}</span>
          <a
            v-if="alert.sourceUrl"
            class="pill accent source-pill"
            :href="alert.sourceUrl"
            target="_blank"
            rel="noopener"
          >
            {{ t(`alerts.sources.${alert.sourceType}`, { id: alert.sourceId }) }}
          </a>
        </div>
        <p>{{ alert.message }}</p>
        <p v-if="alert.acknowledged && alert.note" class="ack-note">「{{ alert.note }}」</p>
        <div class="alert-actions">
          <a v-if="alert.snapshotUrl" class="inline-link" :href="alert.snapshotUrl" target="_blank">
            <AppIcon name="camera" :size="14" />
            {{ t('alerts.snapshot') }}
          </a>
          <template v-if="!alert.acknowledged">
            <button v-if="ackingId !== alert.id" class="button small" @click="startAck(alert)">
              <AppIcon name="check" :size="14" />
              {{ t('alerts.acknowledge') }}
            </button>
            <div v-else class="ack-form">
              <input
                v-model="ackNote"
                class="input ack-input"
                maxlength="2000"
                :placeholder="t('alerts.ackNotePlaceholder')"
                :disabled="ackBusy"
                @keyup.enter="confirmAck(alert)"
              />
              <button class="button small" :disabled="ackBusy" @click="confirmAck(alert)">
                {{ t('alerts.confirmAck') }}
              </button>
              <button class="button small" :disabled="ackBusy" @click="cancelAck">
                {{ t('common.cancel') }}
              </button>
            </div>
          </template>
          <span v-else class="muted small-note">
            {{ t('alerts.acknowledgedBy', { user: alert.acknowledgedBy || t('common.none') }) }}
            <template v-if="alert.acknowledgedAt">· {{ formatTime(alert.acknowledgedAt) }}</template>
          </span>
        </div>
      </div>
    </article>
    <div v-if="alerts.length === 0" class="panel empty-state">
      <AppIcon name="bell" :size="28" />
      <span>{{ t('alerts.empty') }}</span>
    </div>
  </section>
</template>

<style scoped>
.alert-thumb {
  width: 96px;
  min-width: 96px;
  height: 72px;
  border-radius: 8px;
  overflow: hidden;
  background: var(--surface-2, rgba(127, 127, 127, 0.1));
  display: block;
}

.alert-thumb img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.ack-form {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.ack-input {
  width: min(420px, 60vw);
  min-height: 32px;
}

.ack-note {
  color: var(--text, inherit);
}

.source-pill {
  text-decoration: none;
}
</style>
