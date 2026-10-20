<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import PlaybackPlayer from '@/components/PlaybackPlayer.vue'
import { api } from '@/api/client'

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
}

const { t } = useI18n()
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
const playerRef = ref<InstanceType<typeof PlaybackPlayer> | null>(null)

const dayStart = computed(() => new Date(`${day.value}T00:00:00`).getTime())
const dayEnd = computed(() => dayStart.value + 24 * 60 * 60 * 1000)

const channelName = computed(
  () => channels.value.find((channel) => channel.id === channelId.value)?.name ?? '—',
)

const positionPercent = computed(() => {
  const span = dayEnd.value - dayStart.value
  if (span <= 0) return 0
  return Math.max(0, Math.min(100, ((positionMs.value - dayStart.value) / span) * 100))
})

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
  const start = Math.max(dayStart.value, recording.startMs)
  const end = Math.min(dayEnd.value, recording.endMs)
  const left = ((start - dayStart.value) / (dayEnd.value - dayStart.value)) * 100
  const width = Math.max(0.25, ((end - start) / (dayEnd.value - dayStart.value)) * 100)
  return { left: `${left}%`, width: `${width}%` }
}

function selectSegment(recording: Recording) {
  selected.value = recording
  playStartMs.value = recording.startMs
  positionMs.value = recording.startMs
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
  speed.value = value
}

function onPosition(value: number) {
  positionMs.value = value
}

function seekTimeline(event: MouseEvent) {
  const track = event.currentTarget as HTMLElement
  const rect = track.getBoundingClientRect()
  if (rect.width <= 0) return
  const target =
    dayStart.value + ((event.clientX - rect.left) / rect.width) * (dayEnd.value - dayStart.value)
  const hit = recordings.value.find(
    (recording) => target >= recording.startMs && target <= recording.endMs,
  )
  if (!hit) return
  selected.value = hit
  onSeek(target)
}

async function loadRecordings() {
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
    recordings.value = await api<Recording[]>(`/api/v1/recordings?${params.toString()}`)
    if (recordings.value.length > 0) {
      selected.value = recordings.value[0]
      playStartMs.value = recordings.value[0].startMs
      positionMs.value = recordings.value[0].startMs
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

function shiftDay(delta: number) {
  const date = new Date(`${day.value}T12:00:00`)
  date.setDate(date.getDate() + delta)
  day.value = localDay(date)
}

function download() {
  if (!selected.value) return
  window.open(`/api/v1/recordings/${selected.value.id}/download`, '_blank')
}

watch([channelId, stream], () => void loadRecordings())
watch(day, () => void loadRecordings())

onMounted(async () => {
  try {
    channels.value = await api<Channel[]>('/api/v1/channels')
    if (channels.value.length > 0) {
      channelId.value = channels.value[0].id
      await loadRecordings()
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
      <button class="button" @click="loadRecordings">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <button class="button" :disabled="!selected" @click="download">
        <AppIcon name="save" :size="16" />
        {{ t('playback.download') }}
      </button>
    </div>
  </section>

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
        :title="`${channelName} · ${formatTime(positionMs)}`"
        @seek="onSeek"
        @speed-change="onSpeedChange"
        @position="onPosition"
      />
      <div v-else class="empty-state">
        <AppIcon name="playback" :size="28" />
        <span>{{ t('playback.noRecordings') }}</span>
      </div>
    </section>

    <section class="panel timeline-panel">
      <div class="timeline-header">
        <h3>{{ t('playback.timeline') }}</h3>
        <span class="muted">{{ day }}</span>
      </div>
      <div class="timeline">
        <div class="timeline-hours">
          <span v-for="hour in 24" :key="hour" :style="{ left: `${((hour - 1) / 24) * 100}%` }">
            {{ String(hour - 1).padStart(2, '0') }}
          </span>
        </div>
        <div class="timeline-track" @click="seekTimeline">
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
            <td><span class="pill">{{ recording.recordType }}</span></td>
            <td>
              <button class="button small" @click="playFrom(recording)">
                <AppIcon name="playback" :size="14" />
                {{ t('playback.play') }}
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
