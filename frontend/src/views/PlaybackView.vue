<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useRoute, useRouter } from 'vue-router'
import AppIcon from '@/components/AppIcon.vue'
import PlaybackPlayer from '@/components/PlaybackPlayer.vue'
import { api, getCsrfToken } from '@/api/client'
import { useAppStore } from '@/stores/app'
import { useAuthStore } from '@/stores/auth'
import { useChannelStatusStore } from '@/stores/channelStatus'

defineOptions({ name: 'PlaybackView' })

interface Channel {
  id: number
  name: string
  subEnabled: boolean
  enabled?: boolean
  mainState?: string
  lastStatus?: string
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

interface DaysResponse {
  days: string[]
}

interface CalendarCell {
  key: string
  day: string
  number: number
  inMonth: boolean
  hasRecording: boolean
  isToday: boolean
  isSelected: boolean
}

const { t, tm, locale } = useI18n()
const route = useRoute()
const router = useRouter()
const auth = useAuthStore()
const app = useAppStore()
const channelStatus = useChannelStatusStore()
const canDownload = computed(() => auth.hasPermission('playback.download'))
const canMaintain = computed(() => app.server?.maintenance === true)
const channels = ref<Channel[]>([])
const channelId = ref(0)
const stream = ref<'main' | 'sub'>('main')
function localDay(date: Date) {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`
}

const day = ref(localDay(new Date()))
const today = ref(localDay(new Date()))
const calendarMonth = ref(day.value.slice(0, 7))
// Cache keyed by "<channelId>:<YYYY-MM>" so switching back to a month or a
// channel never issues the same days query twice.
const monthCache = ref<Record<string, string[]>>({})
const monthPending = new Set<string>()
const monthLoading = ref(false)
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
const calendarOpen = ref(false)
const rangeOpen = ref(false)
const dateWrapRef = ref<HTMLElement | null>(null)
const rangeWrapRef = ref<HTMLElement | null>(null)
const clipsCollapsed = ref(false)
let reverseTimer: number | undefined
let recordingsTimer: number | undefined
let loadSeq = 0

const dayStart = computed(() => new Date(`${day.value}T00:00:00`).getTime())
const dayEnd = computed(() => dayStart.value + 24 * 60 * 60 * 1000)

const zoomLevels = [5, 10, 30, 60, 180, 360, 720, 1440].map((minutes) => minutes * 60 * 1000)

const frozenWindow = ref<{ start: number; end: number } | null>(null)

const naturalWindowStart = computed(() => {
  const span = Math.min(windowMs.value, dayEnd.value - dayStart.value)
  if (span >= dayEnd.value - dayStart.value) return dayStart.value
  const center = positionMs.value > 0 ? positionMs.value : dayStart.value + span / 2
  return Math.max(dayStart.value, Math.min(dayEnd.value - span, center - span / 2))
})

const naturalWindowEnd = computed(() =>
  Math.min(
    dayEnd.value,
    naturalWindowStart.value + Math.min(windowMs.value, dayEnd.value - dayStart.value),
  ),
)

// While dragging, pin the visible window. Otherwise setting positionMs moves
// the auto-centered window under the pointer on every move, which changes the
// pixel→time mapping mid-drag and makes the cursor drift / land wrong.
const windowStart = computed(() => frozenWindow.value?.start ?? naturalWindowStart.value)
const windowEnd = computed(() => frozenWindow.value?.end ?? naturalWindowEnd.value)

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

const weekdays = computed(() => tm('playback.weekdays') as unknown as string[])
const recordedDays = computed(
  () => new Set(monthCache.value[`${channelId.value}:${calendarMonth.value}`] ?? []),
)

const calendarTitle = computed(() => {
  const [year, monthNumber] = calendarMonth.value.split('-').map(Number)
  return new Intl.DateTimeFormat(locale.value, { year: 'numeric', month: 'long' }).format(
    new Date(year, monthNumber - 1, 1, 12),
  )
})

const calendarCells = computed<CalendarCell[]>(() => {
  const [year, monthNumber] = calendarMonth.value.split('-').map(Number)
  const first = new Date(year, monthNumber - 1, 1, 12)
  // Monday is the first column, matching the weekday labels above the grid.
  const offset = (first.getDay() + 6) % 7
  const cells: CalendarCell[] = []
  for (let index = 0; index < 42; index += 1) {
    const date = new Date(year, monthNumber - 1, 1 - offset + index, 12)
    const iso = localDay(date)
    const inMonth = date.getFullYear() === year && date.getMonth() === monthNumber - 1
    cells.push({
      key: iso,
      day: iso,
      number: date.getDate(),
      inMonth,
      hasRecording: inMonth && recordedDays.value.has(iso),
      isToday: iso === today.value,
      isSelected: iso === day.value,
    })
  }
  return cells
})

function channelOnline(channel: Channel) {
  if (channel.mainState === 'online') return true
  const idle = channel.mainState === undefined || channel.mainState === 'disabled'
  return idle && channel.lastStatus === 'online'
}

function selectChannel(id: number) {
  if (id !== channelId.value) channelId.value = id
}

function monthRange(month: string) {
  const [year, monthNumber] = month.split('-').map(Number)
  const last = new Date(year, monthNumber, 0, 12).getDate()
  return { from: `${month}-01`, to: `${month}-${String(last).padStart(2, '0')}` }
}

async function ensureMonth() {
  const id = channelId.value
  const month = calendarMonth.value
  if (id <= 0) return
  const key = `${id}:${month}`
  if (key in monthCache.value || monthPending.has(key)) return
  monthPending.add(key)
  monthLoading.value = true
  const { from, to } = monthRange(month)
  try {
    const result = await api<DaysResponse>(
      `/api/v1/recordings/days?channelId=${id}&from=${from}&to=${to}`,
    )
    monthCache.value = { ...monthCache.value, [key]: result.days ?? [] }
  } catch {
    // The calendar only highlights days; a failed lookup must not replace the
    // recording list's own, more actionable error message.
  } finally {
    monthPending.delete(key)
    monthLoading.value = monthPending.size > 0
  }
}

function invalidateMonthCache() {
  const prefix = `${channelId.value}:`
  const next: Record<string, string[]> = {}
  for (const [key, value] of Object.entries(monthCache.value)) {
    if (!key.startsWith(prefix)) next[key] = value
  }
  monthCache.value = next
}

function shiftMonth(delta: number) {
  const [year, monthNumber] = calendarMonth.value.split('-').map(Number)
  const date = new Date(year, monthNumber - 1 + delta, 1, 12)
  calendarMonth.value = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}`
}

function selectCalendarDay(cell: CalendarCell) {
  if (!cell.inMonth) calendarMonth.value = cell.day.slice(0, 7)
  day.value = cell.day
  // Picking a date commits the choice; the popover has no "apply" step.
  calendarOpen.value = false
}

function goToday() {
  const now = new Date()
  today.value = localDay(now)
  day.value = today.value
  calendarMonth.value = day.value.slice(0, 7)
  calendarOpen.value = false
}

function toggleCalendar() {
  rangeOpen.value = false
  calendarOpen.value = !calendarOpen.value
  if (calendarOpen.value) {
    // Reopen on the month of the selected day, not on whatever month was
    // browsed last time, so the highlighted day is in view.
    calendarMonth.value = day.value.slice(0, 7)
    void ensureMonth()
  }
}

function toggleRange() {
  calendarOpen.value = false
  rangeOpen.value = !rangeOpen.value
}

function closePopovers() {
  calendarOpen.value = false
  rangeOpen.value = false
}

function onDocumentPointerDown(event: PointerEvent) {
  const target = event.target as Node | null
  if (!target) return
  if (calendarOpen.value && !dateWrapRef.value?.contains(target)) calendarOpen.value = false
  if (rangeOpen.value && !rangeWrapRef.value?.contains(target)) rangeOpen.value = false
}

function onDocumentKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') closePopovers()
}

watch([calendarOpen, rangeOpen], ([calendar, range]) => {
  if (calendar || range) {
    document.addEventListener('pointerdown', onDocumentPointerDown, true)
    document.addEventListener('keydown', onDocumentKeydown)
  } else {
    document.removeEventListener('pointerdown', onDocumentPointerDown, true)
    document.removeEventListener('keydown', onDocumentKeydown)
  }
})

function refreshAll() {
  invalidateMonthCache()
  void loadRecordings(true)
  void ensureMonth()
}

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
  // While the pointer is on the timeline the drag owns the cursor; the
  // player's 500 ms clock tick must not pull it back to the play position.
  if (!timelineDragging.value) positionMs.value = value
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
      if (!reverse.value || !selected.value || timelineDragging.value) return
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
  frozenWindow.value = { start: windowStart.value, end: windowEnd.value }
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
  const result = timelineHit(event)
  timelineDragging.value = false
  frozenWindow.value = null
  if (!result) return
  selected.value = result.hit
  onSeek(result.target)
}

function matchRecording(rows: Recording[], previous: Recording) {
  if (previous.active) {
    const active = rows.find((row) => row.active)
    if (active) return active
  }
  return (
    rows.find((row) => row.id === previous.id) ??
    rows.find((row) => row.startMs === previous.startMs) ??
    null
  )
}

async function loadRecordings(preserve = false) {
  if (channelId.value <= 0) return
  // A background refresh must never fight a pointer drag on the timeline, and
  // there is no reason to poll while the tab is hidden.
  if (preserve && (timelineDragging.value || document.hidden)) return
  const seq = ++loadSeq
  const previous = selected.value
  const previousStart = playStartMs.value
  const previousPosition = positionMs.value
  // Refreshing an existing selection is a background operation: keep the
  // player and timeline mounted instead of swapping them for the spinner.
  const background = preserve && previous !== null
  if (!background) {
    loading.value = true
    if (!preserve) {
      // Changing channel/stream/day starts a new query; drop the old segment.
      selected.value = null
      playStartMs.value = 0
      positionMs.value = 0
      frozenWindow.value = null
    }
  }
  error.value = ''
  try {
    const params = new URLSearchParams({
      channelId: String(channelId.value),
      stream: stream.value,
      from: String(dayStart.value),
      to: String(dayEnd.value),
    })
    const rows = await api<Recording[]>(`/api/v1/recordings?${params.toString()}`)
    if (seq !== loadSeq) return
    recordings.value = rows
    if (background && previous) {
      // The user may have started dragging while the request was in flight.
      if (timelineDragging.value) return
      // Keep the stream (playStartMs) and the clock untouched. When the row
      // is briefly missing (rollover/retention) keep playing the previous
      // segment; never fall back to recordings[0] and yank the cursor away.
      selected.value = matchRecording(rows, previous) ?? previous
      playStartMs.value = previousStart
      positionMs.value = playerRef.value?.positionMs() ?? previousPosition
      return
    }
    const chosen = rows[0]
    if (chosen) {
      selected.value = chosen
      playStartMs.value = chosen.startMs
      positionMs.value = chosen.startMs
      rangeStart.value = toLocalInput(chosen.startMs)
      rangeEnd.value = toLocalInput(chosen.endMs)
    } else {
      selected.value = null
      playStartMs.value = 0
      positionMs.value = 0
    }
  } catch (caught) {
    if (seq === loadSeq) error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    if (seq === loadSeq && !background) loading.value = false
  }
}

async function removeRecording(recording: Recording) {
  if (!window.confirm(t('playback.deleteConfirm', { time: formatTime(recording.startMs) }))) return
  error.value = ''
  try {
    await api(`/api/v1/recordings/${recording.id}`, { method: 'DELETE' })
    if (selected.value?.id === recording.id) selected.value = null
    invalidateMonthCache()
    await loadRecordings(true)
    await ensureMonth()
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  }
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
watch(channelId, (id) => {
  void ensureMonth()
  if (id > 0 && Number(route.query.channelId) !== id) {
    void router.replace({ query: { ...route.query, channelId: String(id) } })
  }
})
watch(calendarMonth, () => void ensureMonth())
watch(day, () => {
  const month = day.value.slice(0, 7)
  if (month !== calendarMonth.value) calendarMonth.value = month
  void loadRecordings()
})

// The selector only needs name/online state. Status frames mutate the
// existing channel objects, so the open player and its list are untouched.
function applyLiveStatus() {
  for (const channel of channels.value) {
    const item = channelStatus.items[channel.id]
    if (!item) continue
    channel.mainState = item.mainState
    channel.lastStatus = item.mainState === 'online' ? 'online' : channel.lastStatus
    if (item.name !== undefined) channel.name = item.name
  }
}

async function refreshChannels() {
  const rows = await api<Channel[]>('/api/v1/channels')
  const current = new Map(channels.value.map((channel) => [channel.id, channel]))
  channels.value = rows.map((row) => {
    const existing = current.get(row.id)
    if (existing) {
      Object.assign(existing, row)
      return existing
    }
    return row
  })
  applyLiveStatus()
}

let channelTimer: number | undefined
watch(() => channelStatus.seq, applyLiveStatus)
watch(
  () => channelStatus.resync,
  () => {
    if (channelStatus.resync > 0) {
      void refreshChannels().catch(() => undefined)
    }
  },
)

onBeforeUnmount(() => {
  loadSeq++
  if (reverseTimer !== undefined) window.clearInterval(reverseTimer)
  if (recordingsTimer !== undefined) window.clearInterval(recordingsTimer)
  if (channelTimer !== undefined) window.clearInterval(channelTimer)
  document.removeEventListener('pointerdown', onDocumentPointerDown, true)
  document.removeEventListener('keydown', onDocumentKeydown)
})

onMounted(async () => {
  try {
    channels.value = await api<Channel[]>('/api/v1/channels')
    applyLiveStatus()
    if (channels.value.length > 0) {
      const requested = Number(route.query.channelId)
      const match = channels.value.find((channel) => channel.id === requested)
      const firstOnline = channels.value.find((channel) => channelOnline(channel))
      // Opening playback from the live preview passes channelId; otherwise
      // start on the first camera that is actually online.
      channelId.value = match?.id ?? firstOnline?.id ?? channels.value[0].id
      recordingsTimer = window.setInterval(() => {
        if (!loading.value && !timelineDragging.value && !document.hidden) {
          void loadRecordings(true)
        }
      }, 5000)
    }
    // Fallback for the channel selector only; recordings keep their own 5 s
    // refresh. Quiet while the status socket is connected.
    channelTimer = window.setInterval(() => {
      if (!channelStatus.connected) void refreshChannels().catch(() => undefined)
    }, 30_000)
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  }
})
</script>

<template>
  <section class="panel playback-toolbar">
    <div class="playback-toolbar-id">
      <span class="pill accent">{{ t('nav.playback') }}</span>
      <div class="playback-toolbar-name">
        <h2>{{ channelName }}</h2>
        <small>{{ t('playback.segments', { count: recordings.length }) }}</small>
      </div>
    </div>

    <div class="playback-toolbar-controls">
      <div ref="dateWrapRef" class="playback-popover-wrap">
        <button
          class="input playback-date-trigger"
          type="button"
          :title="t('playback.chooseDate')"
          :aria-expanded="calendarOpen"
          aria-haspopup="dialog"
          @click="toggleCalendar"
        >
          <AppIcon name="calendar" :size="15" />
          <span>{{ day }}</span>
          <AppIcon name="chevron" :size="12" class="playback-date-caret" />
        </button>
        <Transition name="popover">
          <div
            v-if="calendarOpen"
            class="calendar-popover"
            role="dialog"
            :aria-label="t('playback.calendar')"
          >
            <div class="calendar-head">
              <div class="calendar-heading">
                <h3>{{ t('playback.calendar') }}</h3>
                <span class="calendar-title">{{ calendarTitle }}</span>
              </div>
              <div class="calendar-nav">
                <button
                  class="icon-button small"
                  :title="t('playback.calendarPrevMonth')"
                  @click="shiftMonth(-1)"
                >
                  ‹
                </button>
                <button class="button small calendar-today" @click="goToday">
                  {{ t('playback.calendarToday') }}
                </button>
                <button
                  class="icon-button small"
                  :title="t('playback.calendarNextMonth')"
                  @click="shiftMonth(1)"
                >
                  ›
                </button>
              </div>
            </div>
            <div class="calendar-grid calendar-weekdays">
              <span v-for="weekday in weekdays" :key="weekday" class="calendar-weekday">
                {{ weekday }}
              </span>
            </div>
            <div class="calendar-grid">
              <button
                v-for="cell in calendarCells"
                :key="cell.key"
                class="calendar-day"
                :class="{
                  outside: !cell.inMonth,
                  'has-recording': cell.hasRecording,
                  today: cell.isToday,
                  selected: cell.isSelected,
                }"
                @click="selectCalendarDay(cell)"
              >
                {{ cell.number }}
              </button>
            </div>
            <div class="calendar-foot">
              <span class="calendar-legend">
                <span class="calendar-dot"></span>
                {{ t('playback.calendarLegend') }}
              </span>
              <span v-if="monthLoading" class="spinner calendar-loading"></span>
            </div>
          </div>
        </Transition>
      </div>

      <div class="playback-stream" :title="t('playback.stream')">
        <span class="playback-stream-prefix" aria-hidden="true">{{ t('playback.stream') }}</span>
        <select v-model="stream" class="input" :aria-label="t('playback.stream')">
          <option value="main">MAIN</option>
          <option value="sub">SUB</option>
        </select>
      </div>

      <div ref="rangeWrapRef" class="playback-popover-wrap">
        <button
          class="button"
          type="button"
          :aria-expanded="rangeOpen"
          aria-haspopup="dialog"
          @click="toggleRange"
        >
          <AppIcon name="save" :size="16" />
          {{ t('playback.exportRange') }}
        </button>
        <Transition name="popover">
          <div
            v-if="rangeOpen"
            class="calendar-popover range-popover"
            role="dialog"
            :aria-label="t('playback.exportRange')"
          >
            <label class="field compact-field">
              <span>{{ t('playback.rangeStart') }}</span>
              <input v-model="rangeStart" class="input" type="datetime-local" />
            </label>
            <label class="field compact-field">
              <span>{{ t('playback.rangeEnd') }}</span>
              <input v-model="rangeEnd" class="input" type="datetime-local" />
            </label>
            <button
              class="button primary"
              :disabled="!canDownload || exporting"
              @click="exportRangeFromForm"
            >
              <AppIcon name="save" :size="15" />
              {{ exporting ? t('playback.exporting') : t('playback.exportRange') }}
            </button>
          </div>
        </Transition>
      </div>

      <div class="head-actions playback-actions">
        <button class="button" :title="t('common.refresh')" @click="refreshAll">
          <AppIcon name="refresh" :size="16" />
          <span class="button-label">{{ t('common.refresh') }}</span>
        </button>
        <button
          class="button"
          :class="{ primary: reverse }"
          :disabled="!selected"
          :title="reverse ? t('playback.reverseOn') : t('playback.reverse')"
          @click="toggleReverse"
        >
          <AppIcon name="playback" :size="16" />
          <span class="button-label">{{ reverse ? t('playback.reverseOn') : t('playback.reverse') }}</span>
        </button>
        <button
          class="button"
          :disabled="!selected || selected.active || !canDownload || exporting"
          :title="exporting ? t('playback.exporting') : t('playback.exportMp4')"
          @click="exportMp4"
        >
          <AppIcon name="save" :size="16" />
          <span class="button-label">{{ exporting ? t('playback.exporting') : t('playback.exportMp4') }}</span>
        </button>
        <button
          class="button"
          :disabled="!selected || selected.active"
          :title="t('playback.download')"
          @click="download"
        >
          <AppIcon name="save" :size="16" />
          <span class="button-label">{{ t('playback.download') }}</span>
        </button>
      </div>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <div class="playback-workspace">
    <aside class="panel channel-rail playback-channel-rail" :aria-label="t('playback.channelList')">
      <div class="channel-rail-head">
        <h3>{{ t('playback.channelList') }}</h3>
        <span class="count-badge">{{ channels.length }}</span>
      </div>
      <div class="channel-list playback-channel-list">
        <button
          v-for="channel in channels"
          :key="channel.id"
          class="channel-item"
          :class="{ active: channel.id === channelId }"
          @click="selectChannel(channel.id)"
        >
          <span class="status-dot" :class="channelOnline(channel) ? 'online' : 'offline'"></span>
          <span class="channel-item-copy">
            <strong>{{ channel.name }}</strong>
            <small>{{ channelOnline(channel) ? t('preview.online') : t('preview.offline') }}</small>
          </span>
        </button>
        <p v-if="channels.length === 0" class="small-note">{{ t('preview.noChannels') }}</p>
      </div>
    </aside>

    <div class="playback-main">
      <section v-if="loading" class="panel empty-state playback-loading">
        <span class="spinner"></span>
        {{ t('common.loading') }}
      </section>
      <section v-else class="panel playback-stage">
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

      <template v-if="!loading">

    <section class="panel playback-timeline">
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

    <section class="panel playback-clips" :class="{ collapsed: clipsCollapsed }">
      <div class="playback-clips-head">
        <h3>
          {{ t('playback.clips') }}
          <span class="count-badge">{{ recordings.length }}</span>
        </h3>
        <button
          class="button small"
          type="button"
          :aria-expanded="!clipsCollapsed"
          @click="clipsCollapsed = !clipsCollapsed"
        >
          <AppIcon
            name="chevron"
            :size="14"
            class="playback-clips-caret"
            :class="{ flipped: clipsCollapsed }"
          />
          {{ clipsCollapsed ? t('playback.expandList') : t('playback.collapseList') }}
        </button>
      </div>
      <div v-show="!clipsCollapsed" class="playback-clips-body">
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
      </div>
    </section>
      </template>
    </div>
  </div>
</template>
