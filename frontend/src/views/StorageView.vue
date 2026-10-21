<script setup lang="ts">
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import UsageMeter from '@/components/UsageMeter.vue'
import { api, ApiError } from '@/api/client'

defineOptions({ name: 'StorageView' })

interface Volume {
  id: number
  path: string
  label: string
  mode: string
  maxUsagePct: number
  reserveGb: number
  retentionDays: number
  enabled: boolean
  totalBytes: number
  freeBytes: number
  usagePercent: number
}

interface ActiveRecording {
  channelId: number
  stream: string
  recordType: string
  active: boolean
  path: string
  bytes: number
  startMs: number
  volumeId: number
  volumePath: string
}

interface RecordingStatus {
  recordings: ActiveRecording[]
  volume: { id?: number; path?: string; mode?: string }
}

const { t } = useI18n()
const volumes = ref<Volume[]>([])
const recordingStatus = ref<RecordingStatus | null>(null)
let statusTimer: number | undefined
const loading = ref(true)
const error = ref('')
const notice = ref('')
const modal = reactive({
  open: false,
  path: '',
  label: 'Recordings',
  mode: 'loop',
  maxUsagePct: 90,
  reserveGb: 5,
  retentionDays: 30,
})

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3500)
}

function formatBytes(value: number) {
  if (!value) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  const index = Math.min(units.length - 1, Math.floor(Math.log(value) / Math.log(1024)))
  return `${(value / 1024 ** index).toFixed(index === 0 ? 0 : 1)} ${units[index]}`
}

function typeLabel(type: string) {
  return type === 'event' ? t('storage.recordTypeEvent') : t('storage.recordTypeTimed')
}

function formatTime(ms: number) {
  if (!ms) return '—'
  return new Intl.DateTimeFormat(undefined, {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).format(new Date(ms))
}

function detail(volume: Volume) {
  return `${formatBytes(volume.totalBytes - volume.freeBytes)} / ${formatBytes(volume.totalBytes)}`
}

async function loadStatus() {
  try {
    recordingStatus.value = await api<RecordingStatus>('/api/v1/recordings/status')
  } catch {
    /* Status is informational; the volume list still works. */
  }
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const [volumeRows] = await Promise.all([
      api<Volume[]>('/api/v1/storage/volumes'),
      loadStatus(),
    ])
    volumes.value = volumeRows
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

async function save(volume: Volume) {
  try {
    await api(`/api/v1/storage/volumes/${volume.id}`, {
      method: 'PUT',
      body: JSON.stringify({
        label: volume.label,
        mode: volume.mode,
        maxUsagePct: Number(volume.maxUsagePct),
        reserveGb: Number(volume.reserveGb),
        retentionDays: Number(volume.retentionDays),
        enabled: volume.enabled,
      }),
    })
    flash(t('storage.saved'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function remove(volume: Volume) {
  if (!window.confirm(t('storage.deleteConfirm', { path: volume.path }))) return
  try {
    await api(`/api/v1/storage/volumes/${volume.id}`, { method: 'DELETE' })
    flash(t('storage.deleted'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function add() {
  try {
    await api('/api/v1/storage/volumes', {
      method: 'POST',
      body: JSON.stringify(modal),
    })
    modal.open = false
    flash(t('storage.created'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

onMounted(() => {
  void load()
  statusTimer = window.setInterval(loadStatus, 5000)
})

onBeforeUnmount(() => {
  if (statusTimer !== undefined) window.clearInterval(statusTimer)
})
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.storage') }}</span>
      <h2>{{ t('storage.title') }}</h2>
      <p>{{ t('storage.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <button class="button" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <button class="button primary" @click="modal.open = true">
        <AppIcon name="plus" :size="16" />
        {{ t('storage.addVolume') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else class="volume-grid">
    <article v-for="volume in volumes" :key="volume.id" class="panel volume-card">
      <div class="volume-head">
        <div>
          <h3>{{ volume.label || t('storage.volume') }}</h3>
          <span class="muted mono">{{ volume.path }}</span>
        </div>
        <span class="status-pill" :class="volume.enabled ? 'ok' : 'muted'">
          {{ volume.enabled ? t('common.enabled') : t('common.disabled') }}
        </span>
      </div>

      <UsageMeter
        :label="t('dashboard.storageUsage')"
        :percent="volume.usagePercent"
        :detail="detail(volume)"
      />

      <div class="form-grid">
        <label class="field">
          <span>{{ t('storage.mode') }}</span>
          <select v-model="volume.mode" class="input">
            <option value="loop">{{ t('storage.loop') }}</option>
            <option value="alert">{{ t('storage.alert') }}</option>
          </select>
        </label>
        <label class="field">
          <span>{{ t('storage.maxUsage') }}</span>
          <input v-model.number="volume.maxUsagePct" class="input" type="number" min="50" max="99" />
        </label>
        <label class="field">
          <span>{{ t('storage.reserveGb') }}</span>
          <input v-model.number="volume.reserveGb" class="input" type="number" min="0" max="1000" />
        </label>
        <label class="field">
          <span>{{ t('storage.retentionDays') }}</span>
          <input v-model.number="volume.retentionDays" class="input" type="number" min="0" max="3650" />
        </label>
      </div>

      <label class="check-row">
        <input v-model="volume.enabled" type="checkbox" />
        <span>{{ t('common.enabled') }}</span>
      </label>

      <div class="modal-actions">
        <button class="button danger" @click="remove(volume)">
          <AppIcon name="trash" :size="15" />
          {{ t('common.delete') }}
        </button>
        <button class="button primary" @click="save(volume)">
          <AppIcon name="save" :size="15" />
          {{ t('common.save') }}
        </button>
      </div>
    </article>
  </section>

  <section class="panel recordings-status">
    <div class="section-title">
      <AppIcon name="playback" :size="18" />
      <h3>{{ t('storage.activeRecordings') }}</h3>
    </div>
    <p class="small-note">
      {{ t('storage.activeVolume', { path: recordingStatus?.volume?.path || '—' }) }}
    </p>
    <table class="data-table">
      <thead>
        <tr>
          <th>{{ t('playback.channel') }}</th>
          <th>{{ t('playback.stream') }}</th>
          <th>{{ t('storage.type') }}</th>
          <th>{{ t('storage.state') }}</th>
          <th>{{ t('storage.currentFile') }}</th>
          <th>{{ t('playback.size') }}</th>
          <th>{{ t('storage.startedAt') }}</th>
        </tr>
      </thead>
      <tbody>
        <tr
          v-for="item in recordingStatus?.recordings ?? []"
          :key="`${item.channelId}:${item.stream}:${item.recordType}`"
        >
          <td>#{{ item.channelId }}</td>
          <td>{{ item.stream }}</td>
          <td>{{ typeLabel(item.recordType) }}</td>
          <td>
            <span class="status-pill" :class="item.active ? 'ok' : 'muted'">
              {{ item.active ? t('storage.recordingNow') : t('storage.recordingIdle') }}
            </span>
          </td>
          <td class="mono file-path">{{ item.path || '—' }}</td>
          <td>{{ formatBytes(item.bytes) }}</td>
          <td>{{ formatTime(item.startMs) }}</td>
        </tr>
        <tr v-if="!recordingStatus?.recordings?.length">
          <td colspan="7" class="empty-cell">{{ t('storage.noActiveRecordings') }}</td>
        </tr>
      </tbody>
    </table>
  </section>

  <Teleport to="body">
    <div v-if="modal.open" class="modal-backdrop" @click.self="modal.open = false">
      <form class="modal panel" @submit.prevent="add">
        <div class="modal-head">
          <h3>{{ t('storage.addVolume') }}</h3>
          <button type="button" class="icon-button small" @click="modal.open = false">
            <AppIcon name="close" :size="16" />
          </button>
        </div>
        <label class="field">
          <span>{{ t('storage.path') }}</span>
          <input v-model="modal.path" class="input" required placeholder="/mnt/recordings" />
        </label>
        <label class="field">
          <span>{{ t('storage.label') }}</span>
          <input v-model="modal.label" class="input" />
        </label>
        <div class="form-grid">
          <label class="field">
            <span>{{ t('storage.mode') }}</span>
            <select v-model="modal.mode" class="input">
              <option value="loop">{{ t('storage.loop') }}</option>
              <option value="alert">{{ t('storage.alert') }}</option>
            </select>
          </label>
          <label class="field">
            <span>{{ t('storage.maxUsage') }}</span>
            <input v-model.number="modal.maxUsagePct" class="input" type="number" min="50" max="99" />
          </label>
          <label class="field">
            <span>{{ t('storage.reserveGb') }}</span>
            <input v-model.number="modal.reserveGb" class="input" type="number" min="0" max="1000" />
          </label>
          <label class="field">
            <span>{{ t('storage.retentionDays') }}</span>
            <input v-model.number="modal.retentionDays" class="input" type="number" min="0" max="3650" />
          </label>
        </div>
        <div class="modal-actions">
          <button type="button" class="button" @click="modal.open = false">{{ t('common.cancel') }}</button>
          <button type="submit" class="button primary">{{ t('common.create') }}</button>
        </div>
      </form>
    </div>
  </Teleport>
</template>
