<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import mpegts from 'mpegts.js'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api/client'

const props = defineProps<{
  channelId: number
  stream: 'main' | 'sub'
  startMs: number
  endMs: number
  speed: number
  title: string
}>()

const emit = defineEmits<{
  (event: 'ended'): void
  (event: 'error', message: string): void
  (event: 'seek', targetMs: number): void
  (event: 'speed-change', speed: number): void
  (event: 'position', positionMs: number): void
}>()

const { t } = useI18n()
const video = ref<HTMLVideoElement | null>(null)
const state = ref<'idle' | 'loading' | 'playing' | 'error'>('idle')
const message = ref('')
const activeSpeed = ref(props.speed || 1)
const position = ref(props.startMs)
let player: mpegts.Player | null = null
let destroyed = false
let failed = false
let lastStart = props.startMs
let anchorWall = 0
let accumulated = 0
let running = false
let ticker: number | undefined

function snapshotMs() {
  return accumulated + (running ? (performance.now() - anchorWall) * activeSpeed.value : 0)
}

function positionMs() {
  return props.startMs + snapshotMs()
}

const clockFormat = new Intl.DateTimeFormat(undefined, {
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
})

function resetPosition() {
  accumulated = 0
  anchorWall = performance.now()
  running = false
  position.value = props.startMs
}

function markRunning(value: boolean) {
  if (running === value) return
  if (value) {
    anchorWall = performance.now()
  } else {
    accumulated += (performance.now() - anchorWall) * activeSpeed.value
  }
  running = value
}

function formatClock(ms: number) {
  return clockFormat.format(new Date(ms))
}

function destroy() {
  if (player) {
    try {
      player.pause()
      player.unload()
      player.detachMediaElement()
      player.destroy()
    } catch {
      /* Already torn down. */
    }
    player = null
  }
}

async function start() {
  if (!video.value) return
  markRunning(false)
  destroy()
  failed = false
  state.value = 'loading'
  message.value = ''
  lastStart = props.startMs
  resetPosition()
  try {
    const ticket = await api<{ wsPath: string }>('/api/v1/playback/ticket', {
      method: 'POST',
      body: JSON.stringify({
        channelId: props.channelId,
        stream: props.stream,
        startMs: props.startMs,
        endMs: props.endMs,
        speed: activeSpeed.value,
      }),
    })
    if (destroyed || !video.value) return
    const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    player = mpegts.createPlayer(
      {
        type: 'mse',
        isLive: false,
        url: `${scheme}//${window.location.host}${ticket.wsPath}`,
      },
      { enableWorker: true, lazyLoad: false, enableStashBuffer: true },
    )
    player.on(mpegts.Events.ERROR, (_type, detail) => {
      failed = true
      state.value = 'error'
      markRunning(false)
      message.value = typeof detail === 'string' ? detail : t('playback.streamError')
      emit('error', message.value)
    })
    player.on(mpegts.Events.LOADING_COMPLETE, () => {
      state.value = 'idle'
      markRunning(false)
      emit('ended')
    })
    player.attachMediaElement(video.value)
    player.load()
    if (video.value) video.value.playbackRate = activeSpeed.value
    const result = player.play()
    if (result && typeof result.catch === 'function') await result.catch(() => undefined)
    if (!failed) {
      state.value = 'playing'
      resetPosition()
      markRunning(true)
    }
  } catch (caught) {
    state.value = 'error'
    message.value = caught instanceof Error ? caught.message : t('playback.streamError')
    emit('error', message.value)
  }
}

function togglePlay() {
  if (!player) {
    void start()
    return
  }
  if (state.value === 'playing') {
    player.pause()
    state.value = 'idle'
    markRunning(false)
  } else {
    void player.play()
    state.value = 'playing'
    markRunning(true)
  }
}

function skip(deltaSeconds: number) {
  emit('seek', positionMs() + deltaSeconds * 1000)
}

function setSpeed(value: number) {
  if (running) {
    accumulated += (performance.now() - anchorWall) * activeSpeed.value
    anchorWall = performance.now()
  }
  activeSpeed.value = value
  if (video.value) video.value.playbackRate = value
  emit('speed-change', value)
}

watch(
  () => props.startMs,
  (value) => {
    if (value !== lastStart) void start()
  },
)

watch(
  () => props.speed,
  (value) => {
    if (!value || value === activeSpeed.value) return
    if (running) {
      accumulated += (performance.now() - anchorWall) * activeSpeed.value
      anchorWall = performance.now()
    }
    activeSpeed.value = value
    if (video.value) video.value.playbackRate = value
  },
)

defineExpose({ reload: start, togglePlay, positionMs, skip, setSpeed })

onMounted(() => {
  ticker = window.setInterval(() => {
    position.value = positionMs()
    emit('position', position.value)
  }, 500)
  void start()
})
onBeforeUnmount(() => {
  destroyed = true
  if (ticker !== undefined) window.clearInterval(ticker)
  destroy()
})
</script>

<template>
  <div class="playback-player">
    <video ref="video" class="playback-video" playsinline></video>
    <div class="playback-title">{{ title }}</div>
    <div v-if="state === 'loading'" class="live-overlay">
      <span class="spinner"></span>
      <span>{{ t('common.loading') }}</span>
    </div>
    <div v-else-if="state === 'error'" class="live-overlay error">
      <AppIcon name="info" :size="24" />
      <strong>{{ t('playback.streamError') }}</strong>
      <span>{{ message }}</span>
    </div>
    <div class="playback-controls">
      <button class="icon-button small" :title="t('playback.rewind30')" @click="skip(-30)">
        <span class="transport-label">«30</span>
      </button>
      <button class="icon-button small" :title="t('playback.rewind10')" @click="skip(-10)">
        <span class="transport-label">«10</span>
      </button>
      <button class="icon-button small" :title="t('playback.playPause')" @click="togglePlay">
        <AppIcon :name="state === 'playing' ? 'playback' : 'arrow'" :size="15" />
      </button>
      <button class="icon-button small" :title="t('playback.forward10')" @click="skip(10)">
        <span class="transport-label">10»</span>
      </button>
      <button class="icon-button small" :title="t('playback.forward30')" @click="skip(30)">
        <span class="transport-label">30»</span>
      </button>
      <span class="transport-clock">{{ formatClock(position) }}</span>
      <label class="transport-speed">
        <span>{{ t('playback.speed') }}</span>
        <select :value="activeSpeed" @change="setSpeed(Number(($event.target as HTMLSelectElement).value))">
          <option :value="0.5">0.5×</option>
          <option :value="1">1×</option>
          <option :value="2">2×</option>
          <option :value="4">4×</option>
          <option :value="8">8×</option>
        </select>
      </label>
    </div>
  </div>
</template>
