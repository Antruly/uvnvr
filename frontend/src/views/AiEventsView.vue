<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { RouterLink } from 'vue-router'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'AiEventsView' })

interface AiBox {
  x: number
  y: number
  w: number
  h: number
}

interface AiEvent {
  id: number
  type: 'plate' | 'face'
  channelId: number
  channelName: string
  ts: number
  plate?: string
  plateNorm?: string
  color?: string
  quality?: number
  confidence?: number
  box?: AiBox
  engine?: string
  matchedFaceId?: number | null
  matchedScore?: number
  imageUrl: string
  cropUrl: string
  createdAt?: number
}

interface EventPage {
  total: number
  items: AiEvent[]
}

interface Channel {
  id: number
  name: string
}

interface FaceRecord {
  id: number
  name: string
}

interface FilterState {
  type: '' | 'plate' | 'face'
  channelId: string
  plate: string
  matchedFaceId: string
  from: string
  to: string
}

const { t } = useI18n()
const auth = useAuthStore()
const canManage = computed(() => auth.hasPermission('channel.manage'))

const events = ref<AiEvent[]>([])
const loading = ref(true)
const error = ref('')
const total = ref(0)
const offset = ref(0)
const limit = ref(50)
const channels = ref<Channel[]>([])
const faces = ref<FaceRecord[]>([])
const filters = reactive<FilterState>({
  type: '',
  channelId: '',
  plate: '',
  matchedFaceId: '',
  from: '',
  to: '',
})

const detail = ref<AiEvent | null>(null)
const trail = ref<AiEvent[] | null>(null)
const trailLoading = ref(false)
const trailError = ref('')

let refreshTimer: number | undefined
let inFlight = false

const faceNames = computed(() => new Map(faces.value.map((face) => [face.id, face.name])))
const paged = computed(() => filters.type !== '')
const page = computed(() => Math.floor(offset.value / limit.value) + 1)
const pages = computed(() => (total.value > 0 ? Math.ceil(total.value / limit.value) : 1))
const hasPrev = computed(() => offset.value > 0)
const hasNext = computed(() => paged.value && offset.value + limit.value < total.value)
const allModeShown = computed(() => events.value.length)

function formatTime(seconds: number) {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'short',
    timeStyle: 'medium',
  }).format(new Date(seconds * 1000))
}

function toEpoch(value: string) {
  if (!value) return 0
  const time = new Date(value).getTime()
  return Number.isFinite(time) ? Math.floor(time / 1000) : 0
}

function faceName(id?: number | null) {
  return id ? faceNames.value.get(id) ?? `#${id}` : ''
}

function summary(event: AiEvent) {
  if (event.type === 'plate') return event.plate ?? ''
  const confidence = Math.round((event.confidence ?? 0) * 100)
  const matched = faceName(event.matchedFaceId)
  if (matched) return t('ai.faceMatched', { name: matched, confidence })
  return t('ai.faceSummary', { confidence })
}

function confidenceText(value?: number) {
  return value === undefined || value === null ? '—' : `${Math.round(value * 100)}%`
}

function buildQuery(type: 'plate' | 'face', pagedQuery: boolean) {
  const params = new URLSearchParams({
    type,
    limit: String(pagedQuery ? limit.value : 100),
    offset: String(pagedQuery ? offset.value : 0),
  })
  if (filters.channelId) params.set('channelId', filters.channelId)
  const from = toEpoch(filters.from)
  if (from > 0) params.set('from', String(from))
  const to = toEpoch(filters.to)
  if (to > 0) params.set('to', String(to))
  if (type === 'plate' && filters.plate.trim()) {
    params.set('plate', filters.plate.trim())
  }
  if (type === 'face' && filters.matchedFaceId) {
    params.set('matchedFaceId', filters.matchedFaceId)
  }
  return params.toString()
}

async function load() {
  if (inFlight) return
  inFlight = true
  loading.value = events.value.length === 0
  error.value = ''
  try {
    if (filters.type === 'plate' || filters.type === 'face') {
      const result = await api<EventPage>(`/api/v1/ai/events?${buildQuery(filters.type, true)}`)
      events.value = result.items
      total.value = result.total
    } else {
      const [plate, face] = await Promise.all([
        api<EventPage>(`/api/v1/ai/events?${buildQuery('plate', false)}`),
        api<EventPage>(`/api/v1/ai/events?${buildQuery('face', false)}`),
      ])
      total.value = plate.total + face.total
      events.value = [...plate.items, ...face.items]
        .sort((left, right) => right.ts - left.ts)
        .slice(0, 100)
    }
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    inFlight = false
    loading.value = false
  }
}

async function loadReferenceData() {
  try {
    const [channelRows, faceRows] = await Promise.all([
      api<Channel[] | { channels?: Channel[] }>('/api/v1/channels'),
      api<{ items: FaceRecord[] }>('/api/v1/ai/faces'),
    ])
    channels.value = Array.isArray(channelRows) ? channelRows : (channelRows.channels ?? [])
    faces.value = faceRows.items
  } catch {
    // Reference data is optional: the event list still works without filters.
  }
}

function search() {
  offset.value = 0
  void load()
}

function resetFilters() {
  filters.type = ''
  filters.channelId = ''
  filters.plate = ''
  filters.matchedFaceId = ''
  filters.from = ''
  filters.to = ''
  offset.value = 0
  void load()
}

function changeType() {
  offset.value = 0
  void load()
}

function nextPage() {
  if (!hasNext.value) return
  offset.value += limit.value
  void load()
}

function prevPage() {
  if (!hasPrev.value) return
  offset.value = Math.max(0, offset.value - limit.value)
  void load()
}

function openDetail(event: AiEvent) {
  detail.value = event
  trail.value = null
  trailError.value = ''
}

function closeDetail() {
  detail.value = null
  trail.value = null
  trailError.value = ''
}

async function loadTrail(event: AiEvent) {
  if (!event.plate) return
  trailLoading.value = true
  trailError.value = ''
  try {
    const result = await api<{ items: AiEvent[] }>(
      `/api/v1/ai/plates?plate=${encodeURIComponent(event.plate)}`,
    )
    trail.value = result.items
  } catch (caught) {
    trailError.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    trailLoading.value = false
  }
}

function searchPlate(event: AiEvent) {
  filters.type = 'plate'
  filters.plate = event.plate ?? ''
  filters.matchedFaceId = ''
  offset.value = 0
  closeDetail()
  void load()
}

function searchFace(event: AiEvent) {
  if (!event.matchedFaceId) return
  filters.type = 'face'
  filters.matchedFaceId = String(event.matchedFaceId)
  filters.plate = ''
  offset.value = 0
  closeDetail()
  void load()
}

function searchDetailPlate() {
  if (detail.value) searchPlate(detail.value)
}

function searchDetailFace() {
  if (detail.value) searchFace(detail.value)
}

function loadDetailTrail() {
  if (detail.value) void loadTrail(detail.value)
}

const detailRows = computed(() => {
  const event = detail.value
  if (!event) return []
  const rows = [
    { label: t('ai.fields.type'), value: t(`ai.types.${event.type}`) },
    { label: t('ai.fields.time'), value: formatTime(event.ts) },
    { label: t('ai.fields.channel'), value: event.channelName || `#${event.channelId}` },
  ]
  if (event.type === 'plate') {
    rows.push({ label: t('ai.fields.plate'), value: event.plate ?? '—' })
    if (event.plateNorm && event.plateNorm !== event.plate) {
      rows.push({ label: t('ai.fields.plateNorm'), value: event.plateNorm })
    }
    if (event.color) rows.push({ label: t('ai.fields.color'), value: event.color })
  }
  rows.push({ label: t('ai.fields.confidence'), value: confidenceText(event.confidence) })
  if (event.type === 'face') {
    rows.push({ label: t('ai.fields.quality'), value: confidenceText(event.quality) })
    rows.push({
      label: t('ai.fields.face'),
      value: event.matchedFaceId ? faceName(event.matchedFaceId) : t('ai.unmatchedFace'),
    })
    if (event.matchedScore !== undefined) {
      rows.push({ label: t('ai.fields.score'), value: confidenceText(event.matchedScore) })
    }
  }
  if (event.box) {
    const { x, y, w, h } = event.box
    rows.push({ label: t('ai.fields.box'), value: `${x}, ${y}, ${w} × ${h}` })
  }
  if (event.engine) rows.push({ label: t('ai.fields.engine'), value: event.engine })
  rows.push({ label: t('ai.fields.eventId'), value: `#${event.id}` })
  return rows
})

onMounted(() => {
  void loadReferenceData()
  void load()
  refreshTimer = window.setInterval(() => {
    if (!detail.value) void load()
  }, 10000)
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
      <RouterLink v-if="canManage" class="button" to="/ai-settings">
        <AppIcon name="settings" :size="16" />
        {{ t('nav.aiSettings') }}
      </RouterLink>
      <button class="button" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
    </div>
  </section>

  <form class="panel ai-filters" @submit.prevent="search">
    <label class="field">
      <span>{{ t('ai.typeFilter') }}</span>
      <select v-model="filters.type" class="input" @change="changeType">
        <option value="">{{ t('ai.allTypes') }}</option>
        <option value="plate">{{ t('ai.types.plate') }}</option>
        <option value="face">{{ t('ai.types.face') }}</option>
      </select>
    </label>
    <label class="field">
      <span>{{ t('ai.channelFilter') }}</span>
      <select v-model="filters.channelId" class="input">
        <option value="">{{ t('ai.allChannels') }}</option>
        <option v-for="channel in channels" :key="channel.id" :value="String(channel.id)">
          {{ channel.name }}
        </option>
      </select>
    </label>
    <label class="field">
      <span>{{ t('ai.plateFilter') }}</span>
      <input
        v-model="filters.plate"
        class="input"
        :placeholder="t('ai.platePlaceholder')"
        :disabled="filters.type === 'face'"
      />
    </label>
    <label class="field">
      <span>{{ t('ai.faceFilter') }}</span>
      <select v-model="filters.matchedFaceId" class="input" :disabled="filters.type === 'plate'">
        <option value="">{{ t('ai.allFaces') }}</option>
        <option v-for="face in faces" :key="face.id" :value="String(face.id)">
          {{ face.name }}
        </option>
      </select>
    </label>
    <label class="field">
      <span>{{ t('ai.from') }}</span>
      <input v-model="filters.from" class="input" type="datetime-local" />
    </label>
    <label class="field">
      <span>{{ t('ai.to') }}</span>
      <input v-model="filters.to" class="input" type="datetime-local" />
    </label>
    <label class="field">
      <span>{{ t('ai.limit') }}</span>
      <select v-model.number="limit" class="input" @change="search">
        <option :value="20">20</option>
        <option :value="50">50</option>
        <option :value="100">100</option>
        <option :value="200">200</option>
      </select>
    </label>
    <div class="ai-filter-actions">
      <button class="button primary" type="submit">
        <AppIcon name="search" :size="15" />
        {{ t('ai.search') }}
      </button>
      <button class="button" type="button" @click="resetFilters">{{ t('ai.reset') }}</button>
    </div>
  </form>

  <p v-if="error" class="form-error">{{ error }}</p>

  <div class="ai-results-head">
    <span class="muted">{{ t('ai.totalCount', { total }) }}</span>
    <span v-if="paged" class="muted">{{ t('ai.pageInfo', { page, pages }) }}</span>
    <span v-else class="muted small-note">
      {{ t('ai.allModeHint', { shown: allModeShown, total }) }}
    </span>
  </div>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else class="alert-list">
    <article
      v-for="event in events"
      :key="`${event.type}-${event.id}`"
      class="panel alert-card ai-event-card"
      :class="event.type"
    >
      <div class="ai-snapshot">
        <img v-if="event.imageUrl" :src="event.imageUrl" :alt="summary(event)" loading="lazy" />
        <span v-else class="muted small-note">{{ t('ai.snapshotSuppressed') }}</span>
      </div>
      <div class="alert-body">
        <div class="alert-head">
          <strong>{{ t(`ai.types.${event.type}`) }}</strong>
          <span class="muted">{{ formatTime(event.ts) }}</span>
          <span v-if="event.channelName" class="pill">{{ event.channelName }}</span>
        </div>
        <p>{{ summary(event) }}</p>
        <div class="alert-actions">
          <button class="button small" type="button" @click="openDetail(event)">
            <AppIcon name="info" :size="14" />
            {{ t('ai.details') }}
          </button>
          <span v-if="event.type === 'plate' && event.plate" class="chip">{{ event.plate }}</span>
          <span v-if="event.type === 'face' && event.matchedFaceId" class="chip">
            {{ faceName(event.matchedFaceId) }}
          </span>
        </div>
      </div>
    </article>
    <div v-if="events.length === 0" class="panel empty-state">
      <AppIcon name="search" :size="28" />
      <span>{{ t('ai.empty') }}</span>
    </div>
  </section>

  <div v-if="paged && total > 0" class="ai-pager">
    <button class="button small" type="button" :disabled="!hasPrev" @click="prevPage">
      {{ t('common.prev') }}
    </button>
    <span class="muted">{{ t('ai.pageInfo', { page, pages }) }}</span>
    <button class="button small" type="button" :disabled="!hasNext" @click="nextPage">
      {{ t('common.next') }}
    </button>
  </div>

  <div v-if="detail" class="modal-backdrop" @click.self="closeDetail">
    <div class="modal modal-wide ai-detail">
      <div class="modal-head">
        <h3>{{ t('ai.detailTitle') }}</h3>
        <button class="button small" type="button" @click="closeDetail">
          <AppIcon name="close" :size="14" />
          {{ t('common.close') }}
        </button>
      </div>

      <div class="ai-detail-images">
        <figure class="ai-detail-figure">
          <img v-if="detail.imageUrl" :src="detail.imageUrl" :alt="summary(detail)" />
          <span v-else class="muted small-note">{{ t('ai.snapshotSuppressed') }}</span>
          <figcaption>{{ t('ai.fields.snapshot') }}</figcaption>
        </figure>
        <figure class="ai-detail-figure crop">
          <img v-if="detail.cropUrl" :src="detail.cropUrl" :alt="summary(detail)" />
          <span v-else class="muted small-note">{{ t('ai.snapshotSuppressed') }}</span>
          <figcaption>{{ t('ai.fields.crop') }}</figcaption>
        </figure>
      </div>

      <dl class="ai-detail-meta">
        <div v-for="row in detailRows" :key="row.label">
          <dt>{{ row.label }}</dt>
          <dd>{{ row.value }}</dd>
        </div>
      </dl>

      <div class="modal-actions">
        <button
          v-if="detail.type === 'plate' && detail.plate"
          class="button"
          type="button"
          @click="searchDetailPlate"
        >
          <AppIcon name="search" :size="14" />
          {{ t('ai.searchThisPlate') }}
        </button>
        <button
          v-if="detail.type === 'face' && detail.matchedFaceId"
          class="button"
          type="button"
          @click="searchDetailFace"
        >
          <AppIcon name="search" :size="14" />
          {{ t('ai.searchThisFace') }}
        </button>
        <a
          v-if="detail.imageUrl"
          class="button"
          :href="detail.imageUrl"
          target="_blank"
          rel="noopener"
        >
          {{ t('ai.openSnapshot') }}
        </a>
      </div>

      <section v-if="detail.type === 'plate' && detail.plate" class="ai-trail">
        <div class="section-title">
          <AppIcon name="playback" :size="18" />
          <h3>{{ t('ai.trail') }}</h3>
        </div>
        <p class="small-note">{{ t('ai.trailHint') }}</p>
        <button class="button small" type="button" :disabled="trailLoading" @click="loadDetailTrail">
          <AppIcon name="search" :size="14" />
          {{ trailLoading ? t('common.loading') : t('ai.loadTrail') }}
        </button>
        <p v-if="trailError" class="form-error">{{ trailError }}</p>
        <div v-if="trail && trail.length > 0" class="table-panel">
          <table class="data-table">
            <thead>
              <tr>
                <th>{{ t('ai.fields.time') }}</th>
                <th>{{ t('ai.fields.channel') }}</th>
                <th>{{ t('ai.fields.confidence') }}</th>
                <th>{{ t('ai.fields.color') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="item in trail" :key="item.id">
                <td>{{ formatTime(item.ts) }}</td>
                <td>{{ item.channelName || `#${item.channelId}` }}</td>
                <td>{{ confidenceText(item.confidence) }}</td>
                <td>{{ item.color || '—' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <p v-else-if="trail" class="muted small-note">{{ t('ai.trailEmpty') }}</p>
      </section>
    </div>
  </div>
</template>

<style scoped>
.ai-filters {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 12px;
  padding: 18px;
  margin-bottom: 14px;
}

.ai-filter-actions {
  display: flex;
  align-items: flex-end;
  gap: 8px;
}

.ai-results-head {
  display: flex;
  align-items: center;
  gap: 14px;
  flex-wrap: wrap;
  margin-bottom: 10px;
}

.ai-event-card.plate {
  border-left-color: var(--accent);
}

.ai-event-card.face {
  border-left-color: var(--accent-2);
}

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

.ai-pager {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 14px;
  margin-top: 14px;
}

.ai-detail {
  gap: 18px;
}

.ai-detail-images {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 200px;
  gap: 12px;
}

.ai-detail-figure {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  min-height: 180px;
  margin: 0;
  padding: 10px;
  border: 1px solid var(--line);
  border-radius: 10px;
  background: var(--surface-2, rgba(127, 127, 127, 0.08));
}

.ai-detail-figure img {
  max-width: 100%;
  max-height: 360px;
  object-fit: contain;
  border-radius: 6px;
}

.ai-detail-figure.crop img {
  max-height: 220px;
}

.ai-detail-figure figcaption {
  color: var(--muted-2);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
}

.ai-detail-meta {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 10px 18px;
  margin: 0;
}

.ai-detail-meta > div {
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding-bottom: 7px;
  border-bottom: 1px solid rgba(120, 155, 195, 0.09);
}

.ai-detail-meta dt {
  color: var(--muted-2);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: 0.08em;
}

.ai-detail-meta dd {
  margin: 0;
  color: var(--text-soft);
  font-size: 12px;
  word-break: break-word;
}

.ai-trail {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding-top: 6px;
  border-top: 1px solid var(--line);
}

.ai-trail .section-title {
  margin-bottom: 0;
}

@media (max-width: 1100px) {
  .ai-filters {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 700px) {
  .ai-filters,
  .ai-detail-images,
  .ai-detail-meta {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
