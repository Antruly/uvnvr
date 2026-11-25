<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'

defineOptions({ name: 'AiEventsView' })

interface AiEvent {
  id: number
  type: 'plate' | 'face'
  channelId: number
  channelName: string
  ts: number
  plate?: string
  quality?: number
  confidence?: number
  imageUrl: string
  cropUrl: string
}

interface EventPage {
  total: number
  items: AiEvent[]
}

const { t } = useI18n()
const events = ref<AiEvent[]>([])
const loading = ref(true)
const error = ref('')
let refreshTimer: number | undefined

function formatTime(seconds: number) {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'short',
    timeStyle: 'medium',
  }).format(new Date(seconds * 1000))
}

function summary(event: AiEvent) {
  if (event.type === 'plate') return event.plate ?? ''
  const confidence = Math.round((event.confidence ?? 0) * 100)
  return t('ai.faceSummary', { confidence })
}

async function load() {
  loading.value = events.value.length === 0
  error.value = ''
  try {
    const [plate, face] = await Promise.all([
      api<EventPage>('/api/v1/ai/events?type=plate&limit=50'),
      api<EventPage>('/api/v1/ai/events?type=face&limit=50'),
    ])
    events.value = [...plate.items, ...face.items]
      .sort((left, right) => right.ts - left.ts)
      .slice(0, 100)
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  void load()
  refreshTimer = window.setInterval(() => void load(), 10000)
})

onBeforeUnmount(() => {
  if (refreshTimer !== undefined) window.clearInterval(refreshTimer)
})
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.aiEvents') }}</span>
      <h2>{{ t('ai.title') }}</h2>
      <p>{{ t('ai.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <button class="button" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
    </div>
  </section>

  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else class="alert-list">
    <article v-for="event in events" :key="`${event.type}-${event.id}`" class="panel alert-card">
      <div class="ai-snapshot">
        <img
          v-if="event.imageUrl"
          :src="event.imageUrl"
          :alt="summary(event)"
          loading="lazy"
        />
        <span v-else class="muted small-note">{{ t('ai.snapshotSuppressed') }}</span>
      </div>
      <div class="alert-body">
        <div class="alert-head">
          <strong>{{ t(`ai.types.${event.type}`) }}</strong>
          <span class="muted">{{ formatTime(event.ts) }}</span>
          <span v-if="event.channelName" class="pill">{{ event.channelName }}</span>
        </div>
        <p>{{ summary(event) }}</p>
      </div>
    </article>
    <div v-if="events.length === 0" class="panel empty-state">
      <AppIcon name="camera" :size="28" />
      <span>{{ t('ai.empty') }}</span>
    </div>
  </section>
</template>

<style scoped>
.ai-snapshot {
  width: 120px;
  min-width: 120px;
  height: 90px;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  border-radius: 8px;
  background: var(--surface-2, rgba(127, 127, 127, 0.1));
  text-align: center;
  padding: 0.25rem;
}

.ai-snapshot img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
</style>
