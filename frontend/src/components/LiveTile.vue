<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import mpegts from 'mpegts.js'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api/client'

export interface LiveChannel {
  id: number
  name: string
  host: string
  port: number
  protocol: string
  enabled: boolean
  subEnabled: boolean
  watermark: { text?: string; position?: string }
  mainState?: string
  mainError?: string
  subState?: string
}

const props = defineProps<{
  channel: LiveChannel
  stream: 'main' | 'sub'
}>()

const emit = defineEmits<{ (event: 'fullscreen', channel: LiveChannel): void }>()

const { t } = useI18n()
const video = ref<HTMLVideoElement | null>(null)
const tile = ref<HTMLElement | null>(null)
const state = ref<'connecting' | 'playing' | 'error'>('connecting')
const message = ref('')
const retryTimer = ref<number | undefined>()
let player: mpegts.Player | null = null
let destroyed = false

const effectiveStream = computed(() => (props.stream === 'sub' && props.channel.subEnabled ? 'sub' : 'main'))
const streamLabel = computed(() => (effectiveStream.value === 'sub' ? 'SUB' : 'MAIN'))
const watermark = computed(() => props.channel.watermark ?? {})
const offline = computed(() => {
  const value = effectiveStream.value === 'sub' ? props.channel.subState : props.channel.mainState
  return value === 'offline' || value === 'error'
})

function clearRetry() {
  if (retryTimer.value) {
    window.clearTimeout(retryTimer.value)
    retryTimer.value = undefined
  }
}

function destroyPlayer() {
  if (player) {
    try {
      player.pause()
      player.unload()
      player.detachMediaElement()
      player.destroy()
    } catch {
      /* The player may already be torn down. */
    }
    player = null
  }
  if (video.value) {
    video.value.removeAttribute('src')
    video.value.load()
  }
}

async function start() {
  if (destroyed || !video.value) return
  clearRetry()
  destroyPlayer()
  state.value = 'connecting'
  message.value = ''
  if (!mpegts.getFeatureList().mseLivePlayback) {
    state.value = 'error'
    message.value = t('preview.mseUnsupported')
    return
  }
  try {
    const ticket = await api<{ wsPath: string }>('/api/v1/streams/ticket', {
      method: 'POST',
      body: JSON.stringify({ channelId: props.channel.id, stream: effectiveStream.value }),
    })
    if (destroyed || !video.value) return
    const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const url = `${scheme}//${window.location.host}${ticket.wsPath}`
    player = mpegts.createPlayer(
      { type: 'mse', isLive: true, url },
      {
        enableWorker: true,
        enableStashBuffer: false,
        stashInitialSize: 128,
        lazyLoad: false,
        liveBufferLatencyChasing: true,
        liveBufferLatencyMaxLatency: 2.5,
        liveBufferLatencyMinRemain: 0.5,
      },
    )
    let failed = false
    player.on(mpegts.Events.ERROR, (_type, detail) => {
      failed = true
      state.value = 'error'
      message.value = typeof detail === 'string' ? detail : t('preview.streamError')
      if (!destroyed) {
        retryTimer.value = window.setTimeout(() => void start(), 5000)
      }
    })
    player.attachMediaElement(video.value)
    player.load()
    const playResult = player.play()
    if (playResult && typeof playResult.catch === 'function') {
      await playResult.catch(() => undefined)
    }
    if (!failed) state.value = 'playing'
  } catch (caught) {
    state.value = 'error'
    message.value = caught instanceof Error ? caught.message : t('preview.streamError')
    if (!destroyed) {
      retryTimer.value = window.setTimeout(() => void start(), 5000)
    }
  }
}

function fullscreen() {
  if (offline.value) {
    const target = props.channel.host ? `http://${props.channel.host}` : ''
    if (target) window.open(target, '_blank', 'noopener')
    return
  }
  if (tile.value?.requestFullscreen) {
    void tile.value.requestFullscreen()
  } else {
    emit('fullscreen', props.channel)
  }
}

onMounted(() => void start())
onBeforeUnmount(() => {
  destroyed = true
  clearRetry()
  destroyPlayer()
})

watch(
  () => [props.channel.id, effectiveStream.value],
  () => {
    destroyed = false
    void start()
  },
)

defineExpose({ reload: start })
</script>

<template>
  <div ref="tile" class="live-tile" @dblclick="fullscreen">
    <video ref="video" class="live-video" muted playsinline autoplay></video>

    <div class="live-top">
      <span class="live-name">{{ channel.name }}</span>
      <span class="live-stream">{{ streamLabel }}</span>
    </div>

    <div
      v-if="watermark.text"
      class="live-watermark"
      :class="`wm-${watermark.position || 'bottom-right'}`"
    >
      {{ watermark.text }}
    </div>

    <div v-if="state === 'connecting'" class="live-overlay">
      <span class="spinner"></span>
      <span>{{ t('preview.connecting') }}</span>
    </div>
    <div v-else-if="state === 'error'" class="live-overlay error">
      <AppIcon name="info" :size="24" />
      <strong>{{ t('preview.streamError') }}</strong>
      <span>{{ message }}</span>
    </div>
    <div v-if="offline && state === 'playing'" class="live-offline">
      <AppIcon name="info" :size="20" />
      <strong>{{ t('preview.deviceOffline') }}</strong>
      <span>{{ t('preview.offlineHint') }}</span>
    </div>
  </div>
</template>
