<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useRoute } from 'vue-router'
import AppIcon from '@/components/AppIcon.vue'
import PlaybackPlayer from '@/components/PlaybackPlayer.vue'
import { api, getCsrfToken } from '@/api/client'
import { useAppStore } from '@/stores/app'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'PlaybackView' })

interface Channel {
  id: number
  name: string
  subEnabled: boolean
}

interface Recording {
  id: number
  channelId: number
  stream: string
  startMs: number
  endMs: number
  durationMs: number
  sizeBytes: number
  recordType: string
  active?: boolean
}

const { t } = useI18n()
const route = useRoute()
const auth = useAuthStore()
const app = useAppStore()
const canDownload = computed(() => auth.hasPermission('playback.download'))
const canMaintain = computed(() => app.server?.maintenance === true)
const channels = ref<Channel[]>([])
const channelId = ref(0)
const stream = ref<'main' | 'sub'>('main')
function localDay(date: Date) {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`
}

const day = ref(localDay(new Date()))
const recordings = ref<Recording[]>([])
const selected = ref<Recording | null>(null)
const playStartMs = ref(0)
const speed = ref(1)
const positionMs = ref(0)
const loading = ref(false)
const error = ref('')
const notice = ref('')
const exporting = ref(false)
const reverse = ref(false)
const timelineDragging = ref(false)
const rangeStart = ref('')
const rangeEnd = ref('')
const windowMs = ref(24 * 60 * 60 * 1000)
const playerRef = ref<InstanceType<typeof PlaybackPlayer> | null>(null)
let reverseTimer: number | undefined
let recordingsTimer: number | undefined

const dayStart = computed(() => new Date(`${day.value}T00:00:00`).getTime())
const dayEnd = computed(() => dayStart.value + 24 * 60 * 60 * 1000)

const zoomLevels = [5, 10, 30, 60, 180, 360, 720, 1440].map((minutes) => minutes * 60 * 1000)

const windowStart = computed(() => {
  const span = Math.min(windowMs.value, dayEnd.value - dayStart.value)
  if (span >= dayEnd.value - dayStart.value) return dayStart.value
  const center = positionMs.value > 0 ? positionMs.value : dayStart.value + span / 2
  return Math.max(dayStart.value, Math.min(dayEnd.value - span, center - span / 2))
})

const windowEnd = computed(() =>
  Math.min(dayEnd.value, windowStart.value + Math.min(windowMs.value, dayEnd.value - dayStart.value)),
)

const skipStepMs = computed(() =>
  Math.max(1000, Math.round((windowEnd.value - windowStart.value) / 24)),
)

const zoomLabel = computed(() => formatDuration(windowEnd.value - windowStart.value))

const rulerTicks = computed(() => {
  const span = Math.max(1, windowEnd.value - windowStart.value)
  return Array.from({ length: 12 }, (_, index) => ({
    left: (index / 12) * 100,
    label: formatTime(windowStart.value + (index / 12) * span).slice(0, 5),
  }))
})

function formatDuration(ms: number) {
  const minutes = Math.max(1, Math.round(ms / 60000))
  if (minutes >= 60 && minutes % 60 === 0) return `${minutes / 60}h`
  if (minutes >= 60) return `${Math.floor(minutes / 60)}h${minutes % 60}m`
  return `${minutes}m`
}

function zoom(direction: number) {
  const current = windowMs.value
  const sorted = [...zoomLevels].sort((a, b) => a - b)
  if (direction < 0) {
    windowMs.value = [...sorted].reverse().find((value) => value < current) ?? sorted[0]
  } else {
    windowMs.value = sorted.find((value) => value > current) ?? sorted[sorted.length - 1]
  }
}

const channelName = computed(
  () => channels.value.find((channel) => channel.id === channelId.value)?.name ?? '—',
)

const positionPercent = computed(() => {
  const span = windowEnd.value - windowStart.value
  if (span <= 0) return 0
  return Math.max(0, Math.min(100, ((positionMs.value - windowStart.value) / span) * 100))
})

function toLocalInput(ms: number) {
  const date = new Date(ms)
  const pad = (value: number) => String(value).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`
}

function formatTime(ms: number) {
  return new Intl.DateTimeFormat(undefined, {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).format(new Date(ms))
}

function formatBytes(value: number) {
  if (!value) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB']
  const index = Math.min(units.length - 1, Math.floor(Math.log(value) / Math.log(1024)))
  return `${(value / 1024 ** index).toFixed(index === 0 ? 0 : 1)} ${units[index]}`
}

function blockStyle(recording: Recording) {
  const start = Math.max(windowStart.value, recording.startMs)
  const end = Math.min(windowEnd.value, recording.endMs)
  if (end <= start) return { display: 'none' }
  const span = windowEnd.value - windowStart.value
  const left = ((start - windowStart.value) / span) * 100
  const width = Math.max(0.25, ((end - start) / span) * 100)
  return { left: `${left}%`, width: `${width}%` }
}

function selectSegment(recording: Recording) {
  selected.value = recording
  playStartMs.value = recording.startMs
  positionMs.value = recording.startMs
  rangeStart.value = toLocalInput(recording.startMs)
  rangeEnd.value = toLocalInput(recording.endMs)
}

function playFrom(recording: Recording) {
  selected.value = recording
  playStartMs.value = recording.startMs
  positionMs.value = recording.startMs
  if (playerRef.value) void playerRef.value.reload()
}

function onSeek(targetMs: number) {
  const target = Math.max(dayStart.value, Math.min(dayEnd.value - 1000, targetMs))
  playStartMs.value = target
  positionMs.value = target
}

function onSpeedChange(value: number) {
  const current = playerRef.value?.positionMs() ?? playStartMs.value
  const target = Math.max(dayStart.value, Math.min(dayEnd.value - 1000, current))
  speed.value = value
  if (target === playStartMs.value) {
    // Paused at the start (or a seek in progress): restart with the new rate.
    void playerRef.value?.reload()
  } else {
    playStartMs.value = target
  }
}

function onEnded() {
  // The server closes the stream at a codec boundary; continue with the next
  // segment in a fresh player so H.264/H.265 can switch automatically.
  const boundary = selected.value?.endMs ?? positionMs.value
  const next = recordings.value.find(
    (recording) => recording.startMs >= boundary - 1000 && recording.id !== selected.value?.id,
  )
  if (next) void playFrom(next)
}

function onPosition(value: number) {
  positionMs.value = value
}

function toggleReverse() {
  reverse.value = !reverse.value
  if (reverse.value) {
    if (playerRef.value) {
      onSeek(playerRef.value.positionMs())
      void playerRef.value.reload()
    }
    if (reverseTimer !== undefined) window.clearInterval(reverseTimer)
    reverseTimer = window.setInterval(() => {
      if (!reverse.value || !selected.value) return
      const current = playerRef.value?.positionMs() ?? positionMs.value
      const step = Math.max(1000, Math.round(skipStepMs.value * speed.value * 1.5))
      const target = current - step
      if (target <= dayStart.value) {
        reverse.value = false
        if (reverseTimer !== undefined) window.clearInterval(reverseTimer)
        reverseTimer = undefined
        return
      }
      onSeek(target)
    }, 1500)
  } else if (reverseTimer !== undefined) {
    window.clearInterval(reverseTimer)
    reverseTimer = undefined
  }
}

function timelineHit(event: PointerEvent | MouseEvent) {
  const track = event.currentTarget as HTMLElement
  const rect = track.getBoundingClientRect()
  if (rect.width <= 0) return null
  const target =
    windowStart.value +
    ((event.clientX - rect.left) / rect.width) * (windowEnd.value - windowStart.value)
  const hit = recordings.value.find(
    (recording) => target >= recording.startMs && target <= recording.endMs,
  )
  return hit ? { hit, target } : null
}

function startTimelineDrag(event: PointerEvent) {
  if (event.button !== 0) return
  const track = event.currentTarget as HTMLElement
  track.setPointerCapture(event.pointerId)
  timelineDragging.value = true
  const result = timelineHit(event)
  if (result) {
    selected.value = result.hit
    positionMs.value = result.target
  }
}

function moveTimelineDrag(event: PointerEvent) {
  if (!timelineDragging.value) return
  const result = timelineHit(event)
  if (!result) return
  selected.value = result.hit
  positionMs.value = result.target
}

function endTimelineDrag(event: PointerEvent) {
  if (!timelineDragging.value) return
  timelineDragging.value = false
  const result = timelineHit(event)
  if (!result) return
  selected.value = result.hit
  onSeek(result.target)
}

async function loadRecordings(preserve = false) {
  if (channelId.value <= 0) return
  loading.value = true
  error.value = ''
  try {
    const params = new URLSearchParams({
      channelId: String(channelId.value),
      stream: stream.value,
      from: String(dayStart.value),
      to: String(dayEnd.value),
    })
    const previous = selected.value
    recordings.value = await api<Recording[]>(`/api/v1/recordings?${params.toString()}`)
    const previousMatch =
      preserve && previous
        ? (previous.active
            ? recordings.value.find((row) => row.active)
            : recordings.value.find((row) => row.id === previous.id)) ??
          recordings.value.find((row) => row.startMs === previous.startMs)
        : undefined
    const chosen = previousMatch ?? recordings.value[0]
    if (chosen) {
      const keepPosition = preserve && previousMatch ? positionMs.value : chosen.startMs
      selected.value = chosen
      playStartMs.value = chosen.startMs
      positionMs.value = keepPosition
      rangeStart.value = toLocalInput(chosen.startMs)
      rangeEnd.value = toLocalInput(chosen.endMs)
    } else {
      selected.value = null
      playStartMs.value = 0
      positionMs.value = 0
    }
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

async function removeRecording(recording: Recording) {
  if (!window.confirm(t('playback.deleteConfirm', { time: formatTime(recording.startMs) }))) return
  error.value = ''
  try {
    await api(`/api/v1/recordings/${recording.id}`, { method: 'DELETE' })
    if (selected.value?.id === recording.id) selected.value = null
    await loadRecordings()
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  }
}

function shiftDay(delta: number) {
  const date = new Date(`${day.value}T12:00:00`)
  date.setDate(date.getDate() + delta)
  day.value = localDay(date)
}

function download() {
  if (!selected.value) return
  window.open(`/api/v1/recordings/${selected.value.id}/download`, '_blank')
}

async function exportMp4() {
  if (!selected.value) return
  await exportRange(selected.value.startMs, selected.value.endMs)
}

async function exportRangeFromForm() {
  const start = rangeStart.value ? new Date(rangeStart.value).getTime() : 0
  const end = rangeEnd.value ? new Date(rangeEnd.value).getTime() : 0
  if (start <= 0 || end <= start) {
    error.value = t('playback.invalidRange')
    return
  }
  await exportRange(start, end)
}

async function exportRange(startMs: number, endMs: number) {
  if (exporting.value) return
  exporting.value = true
  error.value = ''
  notice.value = ''
  try {
    const response = await fetch('/api/v1/playback/export', {
      method: 'POST',
      credentials: 'same-origin',
      headers: {
        'Content-Type': 'application/json',
        'X-CSRF-Token': getCsrfToken(),
      },
      body: JSON.stringify({
        channelId: channelId.value,
        stream: stream.value,
        startMs,
        endMs,
      }),
    })
    if (!response.ok) {
      let message = response.statusText || t('common.error')
      try {
        const body = (await response.json()) as { message?: string }
        if (body.message) message = body.message
      } catch {
        /* not JSON */
      }
      throw new Error(message)
    }
    const blob = await response.blob()
    const url = URL.createObjectURL(blob)
    const link = document.createElement('a')
    link.href = url
    link.download = `uvnvr-${channelId.value}-${startMs}.mp4`
    document.body.appendChild(link)
    link.click()
    link.remove()
    window.setTimeout(() => URL.revokeObjectURL(url), 10000)
    notice.value = t('playback.exported')
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    exporting.value = false
  }
}

watch([channelId, stream], () => void loadRecordings())
watch(day, () => void loadRecordings())

onBeforeUnmount(() => {
  if (reverseTimer !== undefined) window.clearInterval(reverseTimer)
  if (recordingsTimer !== undefined) window.clearInterval(recordingsTimer)
})

onMounted(async () => {
  try {
    channels.value = await api<Channel[]>('/api/v1/channels')
    if (channels.value.length > 0) {
      const requested = Number(route.query.channelId)
      const match = channels.value.find((channel) => channel.id === requested)
      channelId.value = match?.id ?? channels.value[0].id
      await loadRecordings()
      recordingsTimer = window.setInterval(() => {
        if (!loading.value) void loadRecordings(true)
      }, 5000)
    }
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  }
})
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.playback') }}</span>
      <h2>{{ t('playback.title') }}</h2>
      <p>{{ t('playback.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <button class="button" @click="loadRecordings()">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <button class="button" :class="{ primary: reverse }" :disabled="!selected" @click="toggleReverse">
        <AppIcon name="playback" :size="16" />
        {{ reverse ? t('playback.reverseOn') : t('playback.reverse') }}
      </button>
      <button
        class="button"
        :disabled="!selected || selected.active || !canDownload || exporting"
        @click="exportMp4"
      >
        <AppIcon name="save" :size="16" />
        {{ exporting ? t('playback.exporting') : t('playback.exportMp4') }}
      </button>
      <button class="button" :disabled="!selected || selected.active" @click="download">
        <AppIcon name="save" :size="16" />
        {{ t('playback.download') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section class="panel playback-filters">
    <label class="field compact-field">
      <span>{{ t('playback.channel') }}</span>
      <select v-model.number="channelId" class="input">
        <option v-for="channel in channels" :key="channel.id" :value="channel.id">
          {{ channel.name }}
        </option>
      </select>
    </label>
    <label class="field compact-field">
      <span>{{ t('playback.stream') }}</span>
      <select v-model="stream" class="input">
        <option value="main">MAIN</option>
        <option value="sub">SUB</option>
      </select>
    </label>
    <label class="field compact-field">
      <span>{{ t('playback.date') }}</span>
      <div class="date-nav">
        <button class="icon-button small" @click="shiftDay(-1)">‹</button>
        <input v-model="day" class="input" type="date" />
        <button class="icon-button small" @click="shiftDay(1)">›</button>
      </div>
    </label>
    <span class="muted playback-count">{{ t('playback.segments', { count: recordings.length }) }}</span>
    <div class="range-export">
      <label class="field compact-field">
        <span>{{ t('playback.rangeStart') }}</span>
        <input v-model="rangeStart" class="input" type="datetime-local" />
      </label>
      <label class="field compact-field">
        <span>{{ t('playback.rangeEnd') }}</span>
        <input v-model="rangeEnd" class="input" type="datetime-local" />
      </label>
      <button class="button" :disabled="!canDownload || exporting" @click="exportRangeFromForm">
        <AppIcon name="save" :size="15" />
        {{ t('playback.exportRange') }}
      </button>
    </div>
  </section>

  <div v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </div>

  <template v-else>
    <section class="panel playback-stage">
      <PlaybackPlayer
        v-if="selected"
        ref="playerRef"
        :channel-id="channelId"
        :stream="stream"
        :start-ms="playStartMs"
        :end-ms="dayEnd"
        :speed="speed"
        :step-ms="skipStepMs"
        :title="`${channelName} · ${formatTime(positionMs)}`"
        @seek="onSeek"
        @speed-change="onSpeedChange"
        @position="onPosition"
        @ended="onEnded"
      />
      <div v-else class="empty-state">
        <AppIcon name="playback" :size="28" />
        <span>{{ t('playback.noRecordings') }}</span>
      </div>
    </section>

    <section class="panel timeline-panel">
      <div class="timeline-header">
        <h3>{{ t('playback.timeline') }}</h3>
        <div class="timeline-zoom">
          <button class="icon-button small" :title="t('playback.zoomOut')" @click="zoom(1)">−</button>
          <span class="muted">{{ zoomLabel }}</span>
          <button class="icon-button small" :title="t('playback.zoomIn')" @click="zoom(-1)">+</button>
          <span class="muted">{{ day }}</span>
        </div>
      </div>
      <div class="timeline">
        <div class="timeline-hours">
          <span v-for="tick in rulerTicks" :key="tick.left" :style="{ left: `${tick.left}%` }">
            {{ tick.label }}
          </span>
        </div>
        <div
          class="timeline-track"
          :class="{ dragging: timelineDragging }"
          @pointerdown="startTimelineDrag"
          @pointermove="moveTimelineDrag"
          @pointerup="endTimelineDrag"
          @pointercancel="endTimelineDrag"
        >
          <button
            v-for="recording in recordings"
            :key="recording.id"
            class="timeline-block"
            :class="{ active: selected?.id === recording.id }"
            :style="blockStyle(recording)"
            :title="`${formatTime(recording.startMs)} – ${formatTime(recording.endMs)} · ${formatBytes(recording.sizeBytes)}`"
            @click.stop="selectSegment(recording)"
            @dblclick="playFrom(recording)"
          ></button>
          <div
            v-if="selected"
            class="timeline-cursor"
            :style="{ left: `${positionPercent}%` }"
            :title="formatTime(positionMs)"
          ></div>
        </div>
      </div>
    </section>

    <section class="panel table-panel">
      <table class="data-table">
        <thead>
          <tr>
            <th>{{ t('playback.start') }}</th>
            <th>{{ t('playback.end') }}</th>
            <th>{{ t('playback.duration') }}</th>
            <th>{{ t('playback.size') }}</th>
            <th>{{ t('playback.type') }}</th>
            <th>{{ t('common.actions') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="recording in recordings" :key="recording.id">
            <td>{{ formatTime(recording.startMs) }}</td>
            <td>{{ formatTime(recording.endMs) }}</td>
            <td>{{ Math.round(recording.durationMs / 1000) }}s</td>
            <td>{{ formatBytes(recording.sizeBytes) }}</td>
            <td>
              <span class="pill">{{ recording.recordType }}</span>
              <span v-if="recording.active" class="status-pill info">{{ t('playback.live') }}</span>
            </td>
            <td>
              <button class="button small" @click="playFrom(recording)">
                <AppIcon name="playback" :size="14" />
                {{ t('playback.play') }}
              </button>
              <button
                v-if="canMaintain && !recording.active"
                class="button small danger"
                @click="removeRecording(recording)"
              >
                <AppIcon name="trash" :size="14" />
                {{ t('common.delete') }}
              </button>
            </td>
          </tr>
          <tr v-if="recordings.length === 0">
            <td colspan="6" class="empty-cell">{{ t('playback.noRecordings') }}</td>
          </tr>
        </tbody>
      </table>
    </section>
  </template>
</template>
