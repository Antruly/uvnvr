<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'

defineOptions({ name: 'PlansView' })

interface Channel {
  id: number
  name: string
  subEnabled: boolean
}

interface Plan {
  weekday: number
  startMin: number
  endMin: number
  mode: string
  stream: string
}

type CellMode = '' | 'timed' | 'event' | 'both'
type Brush = 'timed' | 'event' | 'both' | 'erase'

const { t } = useI18n()
const channels = ref<Channel[]>([])
const channelId = ref(0)
const stream = ref<'main' | 'sub'>('main')
const brush = ref<Brush>('timed')
const grid = ref<CellMode[][]>(Array.from({ length: 7 }, () => Array<CellMode>(48).fill('')))
const loading = ref(true)
const saving = ref(false)
const error = ref('')
const notice = ref('')
const selection = reactive({ active: false, day1: 0, slot1: 0, day2: 0, slot2: 0 })
const syncOpen = ref(false)
const syncTargets = ref<number[]>([])

const weekdays = computed(() => [
  t('plans.days.sun'),
  t('plans.days.mon'),
  t('plans.days.tue'),
  t('plans.days.wed'),
  t('plans.days.thu'),
  t('plans.days.fri'),
  t('plans.days.sat'),
])

const paintedCount = computed(() => grid.value.flat().filter(Boolean).length)

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3500)
}

function clearGrid() {
  grid.value = Array.from({ length: 7 }, () => Array<CellMode>(48).fill(''))
}

function applyPlans(plans: Plan[]) {
  clearGrid()
  for (const plan of plans) {
    const mode = (plan.mode === 'timed' || plan.mode === 'event' || plan.mode === 'both'
      ? plan.mode
      : 'timed') as CellMode
    const start = Math.max(0, Math.min(48, Math.floor(plan.startMin / 30)))
    const end = Math.max(0, Math.min(48, Math.ceil(plan.endMin / 30)))
    if (plan.weekday < 0 || plan.weekday > 6) continue
    for (let slot = start; slot < end; slot += 1) {
      grid.value[plan.weekday][slot] = mode
    }
  }
}

async function loadChannels() {
  channels.value = await api<Channel[]>('/api/v1/channels')
  if (channels.value.length > 0 && channelId.value === 0) {
    channelId.value = channels.value[0].id
  }
}

async function loadPlans() {
  if (channelId.value <= 0) {
    clearGrid()
    return
  }
  loading.value = true
  error.value = ''
  try {
    const plans = await api<Plan[]>(`/api/v1/plans?channelId=${channelId.value}`)
    applyPlans(plans)
    const sub = channels.value.find((channel) => channel.id === channelId.value)?.subEnabled
    if (!sub && stream.value === 'sub') stream.value = 'main'
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

function onDown(day: number, slot: number) {
  Object.assign(selection, {
    active: true,
    day1: day,
    slot1: slot,
    day2: day,
    slot2: slot,
  })
}

function onEnter(day: number, slot: number) {
  if (!selection.active) return
  selection.day2 = day
  selection.slot2 = slot
}

function isSelected(day: number, slot: number) {
  if (!selection.active) return false
  const firstDay = Math.min(selection.day1, selection.day2)
  const lastDay = Math.max(selection.day1, selection.day2)
  const firstSlot = Math.min(selection.slot1, selection.slot2)
  const lastSlot = Math.max(selection.slot1, selection.slot2)
  return day >= firstDay && day <= lastDay && slot >= firstSlot && slot <= lastSlot
}

function finishSelection() {
  if (!selection.active) return
  const value: CellMode = brush.value === 'erase' ? '' : brush.value
  const firstDay = Math.min(selection.day1, selection.day2)
  const lastDay = Math.max(selection.day1, selection.day2)
  const firstSlot = Math.min(selection.slot1, selection.slot2)
  const lastSlot = Math.max(selection.slot1, selection.slot2)
  for (let day = firstDay; day <= lastDay; day += 1) {
    for (let slot = firstSlot; slot <= lastSlot; slot += 1) {
      grid.value[day][slot] = value
    }
  }
  selection.active = false
}

function collectPlans(): Plan[] {
  const plans: Plan[] = []
  for (let day = 0; day < 7; day += 1) {
    let slot = 0
    while (slot < 48) {
      const mode = grid.value[day][slot]
      if (!mode) {
        slot += 1
        continue
      }
      let end = slot + 1
      while (end < 48 && grid.value[day][end] === mode) end += 1
      plans.push({
        weekday: day,
        startMin: slot * 30,
        endMin: end * 30,
        mode,
        stream: stream.value,
      })
      slot = end
    }
  }
  return plans
}

async function save() {
  if (channelId.value <= 0) return
  saving.value = true
  error.value = ''
  try {
    await api('/api/v1/plans', {
      method: 'PUT',
      body: JSON.stringify({ channelId: channelId.value, plans: collectPlans() }),
    })
    flash(t('plans.saved'))
    await loadPlans()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    saving.value = false
  }
}

function openSync() {
  syncTargets.value = []
  syncOpen.value = true
}

async function syncPlans() {
  if (channelId.value <= 0 || syncTargets.value.length === 0) return
  try {
    await api('/api/v1/plans/sync', {
      method: 'POST',
      body: JSON.stringify({ sourceId: channelId.value, targetIds: syncTargets.value }),
    })
    syncOpen.value = false
    flash(t('plans.synced'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

function toggleTarget(id: number) {
  const index = syncTargets.value.indexOf(id)
  if (index >= 0) syncTargets.value.splice(index, 1)
  else syncTargets.value.push(id)
}

watch(channelId, () => void loadPlans())

onMounted(async () => {
  window.addEventListener('mouseup', finishSelection)
  try {
    await loadChannels()
    await loadPlans()
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
    loading.value = false
  }
})

onBeforeUnmount(() => window.removeEventListener('mouseup', finishSelection))
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('plans.title') }}</span>
      <h2>{{ t('plans.title') }}</h2>
      <p>{{ t('plans.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <button class="button" @click="loadPlans">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <button class="button" :disabled="channelId <= 0" @click="openSync">
        <AppIcon name="users" :size="16" />
        {{ t('plans.sync') }}
      </button>
      <button class="button primary" :disabled="saving || channelId <= 0" @click="save">
        <AppIcon name="save" :size="16" />
        {{ saving ? t('common.loading') : t('common.save') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section class="panel plan-toolbar">
    <label class="field compact-field">
      <span>{{ t('plans.channel') }}</span>
      <select v-model.number="channelId" class="input">
        <option v-for="channel in channels" :key="channel.id" :value="channel.id">
          {{ channel.name }}
        </option>
      </select>
    </label>
    <label class="field compact-field">
      <span>{{ t('plans.stream') }}</span>
      <select v-model="stream" class="input">
        <option value="main">MAIN</option>
        <option value="sub">SUB</option>
      </select>
    </label>
    <div class="field compact-field">
      <span>{{ t('plans.brush') }}</span>
      <div class="brush-group">
        <button
          v-for="option in ['timed', 'event', 'both', 'erase'] as const"
          :key="option"
          class="brush-button"
          :class="[option, { active: brush === option }]"
          @click="brush = option"
        >
          {{ t(`plans.modes.${option}`) }}
        </button>
      </div>
    </div>
    <span class="muted plan-count">{{ t('plans.painted', { count: paintedCount }) }}</span>
  </section>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else class="panel plan-panel">
    <div class="plan-scroll">
      <div class="plan-ruler">
        <span class="plan-day-spacer"></span>
        <div class="plan-hours">
          <span v-for="hour in 24" :key="hour">{{ String(hour - 1).padStart(2, '0') }}</span>
        </div>
      </div>
      <div v-for="(name, day) in weekdays" :key="day" class="plan-row">
        <span class="plan-day">{{ name }}</span>
        <div class="plan-cells">
          <button
            v-for="slot in 48"
            :key="slot"
            class="plan-cell"
            :class="[grid[day][slot - 1], { selected: isSelected(day, slot - 1) }]"
            :title="`${name} ${String(Math.floor((slot - 1) / 2)).padStart(2, '0')}:${(slot - 1) % 2 === 0 ? '00' : '30'}`"
            @mousedown.prevent="onDown(day, slot - 1)"
            @mouseenter="onEnter(day, slot - 1)"
          ></button>
        </div>
      </div>
    </div>
    <div class="plan-legend">
      <span><i class="legend-dot timed"></i>{{ t('plans.modes.timed') }}</span>
      <span><i class="legend-dot event"></i>{{ t('plans.modes.event') }}</span>
      <span><i class="legend-dot both"></i>{{ t('plans.modes.both') }}</span>
      <span class="muted">{{ t('plans.hint') }}</span>
    </div>
  </section>

  <Teleport to="body">
    <div v-if="syncOpen" class="modal-backdrop" @click.self="syncOpen = false">
      <div class="modal panel">
        <div class="modal-head">
          <h3>{{ t('plans.sync') }}</h3>
          <button class="icon-button small" @click="syncOpen = false">
            <AppIcon name="close" :size="16" />
          </button>
        </div>
        <p class="small-note">{{ t('plans.syncHint') }}</p>
        <label v-for="channel in channels.filter((entry) => entry.id !== channelId)" :key="channel.id" class="check-row">
          <input
            type="checkbox"
            :checked="syncTargets.includes(channel.id)"
            @change="toggleTarget(channel.id)"
          />
          <span>{{ channel.name }}</span>
        </label>
        <div class="modal-actions">
          <button class="button" @click="syncOpen = false">{{ t('common.cancel') }}</button>
          <button class="button primary" :disabled="syncTargets.length === 0" @click="syncPlans">
            {{ t('plans.applySync') }}
          </button>
        </div>
      </div>
    </div>
  </Teleport>
</template>
