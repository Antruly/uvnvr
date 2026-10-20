<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import mpegts from 'mpegts.js'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api/client'

const props = defineProps<{
  channelId: number
  stream: 'main' | 'sub'
  startMs: number
  endMs: number
  title: string
}>()

const emit = defineEmits<{
  (event: 'ended'): void
  (event: 'error', message: string): void
}>()

const { t } = useI18n()
const video = ref<HTMLVideoElement | null>(null)
const state = ref<'idle' | 'loading' | 'playing' | 'error'>('idle')
const message = ref('')
let player: mpegts.Player | null = null
let destroyed = false
let failed = false

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
  destroy()
  failed = false
  state.value = 'loading'
  message.value = ''
  try {
    const ticket = await api<{ wsPath: string }>('/api/v1/playback/ticket', {
      method: 'POST',
      body: JSON.stringify({
        channelId: props.channelId,
        stream: props.stream,
        startMs: props.startMs,
        endMs: props.endMs,
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
      message.value = typeof detail === 'string' ? detail : t('playback.streamError')
      emit('error', message.value)
    })
    player.on(mpegts.Events.LOADING_COMPLETE, () => {
      state.value = 'idle'
      emit('ended')
    })
    player.attachMediaElement(video.value)
    player.load()
    const result = player.play()
    if (result && typeof result.catch === 'function') await result.catch(() => undefined)
    if (!failed) state.value = 'playing'
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
  if (video.value?.paused) {
    void player.play()
    state.value = 'playing'
  } else {
    player.pause()
    state.value = 'idle'
  }
}

defineExpose({ reload: start, togglePlay })

onMounted(() => void start())
onBeforeUnmount(() => {
  destroyed = true
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
      <button class="icon-button small" :title="t('playback.playPause')" @click="togglePlay">
        <AppIcon :name="state === 'playing' ? 'playback' : 'arrow'" :size="15" />
      </button>
    </div>
  </div>
</template>
