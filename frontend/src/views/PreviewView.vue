<script setup lang="ts">
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import LiveTile, { type LiveChannel } from '@/components/LiveTile.vue'
import { api } from '@/api/client'

defineOptions({ name: 'PreviewView' })

const { t } = useI18n()

interface ChannelGroup {
  id: number
  name: string
  parentId: number
  sort: number
  channels: number
}

interface Layout {
  id: string
  serverId?: number
  name: string
  grid: string
  cols: number
  rows: number
  cells: (number | null)[]
}

const gridOptions = [
  { key: '1', cols: 1, rows: 1 },
  { key: '4', cols: 2, rows: 2 },
  { key: '8', cols: 4, rows: 2 },
  { key: '16', cols: 4, rows: 4 },
  { key: '32', cols: 8, rows: 4 },
]

const storageKey = 'uvnvr.preview.layouts'
const channels = ref<LiveChannel[]>([])
const groups = ref<ChannelGroup[]>([])
const collapsedGroups = ref<Set<number>>(new Set())
const loading = ref(true)
const error = ref('')
const search = ref('')
const selectedChannelId = ref<number | null>(null)
const streamMode = ref<'main' | 'sub'>('sub')
const layouts = ref<Layout[]>([])
const activeLayoutId = ref('')
const draggingId = ref<number | null>(null)

function defaultLayout(): Layout {
  return {
    id: 'default',
    name: t('preview.defaultLayout'),
    grid: '4',
    cols: 2,
    rows: 2,
    cells: [null, null, null, null],
  }
}

interface ServerLayout {
  id: number
  name: string
  grid: { grid?: string; cols?: number; rows?: number; cells?: (number | null)[] }
  isDefault: boolean
}

function fromServer(row: ServerLayout): Layout {
  const cols = Math.max(1, Math.min(8, Number(row.grid.cols) || 2))
  const rows = Math.max(1, Math.min(8, Number(row.grid.rows) || 2))
  const cells = Array.isArray(row.grid.cells) ? [...row.grid.cells] : []
  while (cells.length < cols * rows) cells.push(null)
  return {
    id: `server-${row.id}`,
    serverId: row.id,
    name: row.name,
    grid: typeof row.grid.grid === 'string' ? row.grid.grid : 'custom',
    cols,
    rows,
    cells: cells.slice(0, cols * rows),
  }
}

function loadLocalLayouts() {
  try {
    const raw = localStorage.getItem(storageKey)
    if (raw) {
      const parsed = JSON.parse(raw) as Layout[]
      if (Array.isArray(parsed) && parsed.length > 0) {
        layouts.value = parsed
        activeLayoutId.value = parsed[0].id
        return
      }
    }
  } catch {
    /* Corrupt or blocked storage: fall back to the default layout. */
  }
  const layout = defaultLayout()
  layouts.value = [layout]
  activeLayoutId.value = layout.id
}

async function loadLayouts() {
  loadLocalLayouts()
  try {
    const server = await api<ServerLayout[]>('/api/v1/layouts')
    if (server.length > 0) {
      layouts.value = server.map(fromServer)
      activeLayoutId.value = layouts.value[0].id
      saveLayouts()
    }
  } catch {
    /* Offline or unsupported: keep the local layouts. */
  }
}

const syncTimers = new Map<string, number>()

async function syncLayout(layout: Layout) {
  const grid = { grid: layout.grid, cols: layout.cols, rows: layout.rows, cells: layout.cells }
  try {
    if (layout.serverId) {
      await api(`/api/v1/layouts/${layout.serverId}`, {
        method: 'PUT',
        body: JSON.stringify({
          name: layout.name,
          grid,
          isDefault: layouts.value[0]?.id === layout.id,
        }),
      })
    } else {
      const created = await api<{ id: number }>('/api/v1/layouts', {
        method: 'POST',
        body: JSON.stringify({
          name: layout.name,
          grid,
          isDefault: layouts.value[0]?.id === layout.id,
        }),
      })
      layout.serverId = created.id
      saveLayouts()
    }
  } catch {
    /* Keep the local copy; the next change retries. */
  }
}

function scheduleSync(layout: Layout | undefined) {
  if (!layout) return
  const existing = syncTimers.get(layout.id)
  if (existing !== undefined) window.clearTimeout(existing)
  syncTimers.set(
    layout.id,
    window.setTimeout(() => void syncLayout(layout), 700),
  )
}

function saveLayouts() {
  try {
    localStorage.setItem(storageKey, JSON.stringify(layouts.value))
  } catch {
    /* Storage may be unavailable; the page still works for this session. */
  }
  scheduleSync(activeLayout.value)
}

const activeLayout = computed(
  () => layouts.value.find((layout) => layout.id === activeLayoutId.value) ?? layouts.value[0],
)

const gridStyle = computed(() => ({
  gridTemplateColumns: `repeat(${activeLayout.value?.cols ?? 2}, minmax(0, 1fr))`,
  gridTemplateRows: `repeat(${activeLayout.value?.rows ?? 2}, minmax(0, 1fr))`,
}))

const filteredChannels = computed(() => {
  const query = search.value.trim().toLowerCase()
  if (!query) return channels.value
  return channels.value.filter((channel) => channel.name.toLowerCase().includes(query))
})

const groupedChannels = computed(() => {
  if (groups.value.length === 0) return []
  const buckets = new Map<number, LiveChannel[]>()
  for (const channel of filteredChannels.value) {
    const groupId = groups.value.some((group) => group.id === channel.groupId)
      ? channel.groupId
      : 0
    const bucket = buckets.get(groupId) ?? []
    bucket.push(channel)
    buckets.set(groupId, bucket)
  }
  const result: { id: number; name: string; channels: LiveChannel[] }[] = []
  for (const group of groups.value) {
    const bucket = buckets.get(group.id)
    if (bucket?.length) result.push({ id: group.id, name: group.name, channels: bucket })
  }
  const ungrouped = buckets.get(0)
  if (ungrouped?.length) {
    result.push({ id: 0, name: t('channels.ungrouped'), channels: ungrouped })
  }
  return result
})

function toggleGroup(id: number) {
  const next = new Set(collapsedGroups.value)
  if (next.has(id)) next.delete(id)
  else next.add(id)
  collapsedGroups.value = next
}

const onlineCount = computed(
  () => channels.value.filter((channel) => channel.mainState === 'online').length,
)

function channelById(id: number | null) {
  if (id == null) return null
  return channels.value.find((channel) => channel.id === id) ?? null
}

function statusClass(channel: LiveChannel) {
  if (channel.mainState === 'online') return 'online'
  if (channel.mainState === 'reconnecting' || channel.mainState === 'connecting') return 'warn'
  return 'offline'
}

function setGrid(key: string) {
  const option = gridOptions.find((entry) => entry.key === key)
  if (!option || !activeLayout.value) return
  const cells = [...activeLayout.value.cells]
  const count = option.cols * option.rows
  while (cells.length < count) cells.push(null)
  activeLayout.value.grid = key
  activeLayout.value.cols = option.cols
  activeLayout.value.rows = option.rows
  activeLayout.value.cells = cells.slice(0, count)
  saveLayouts()
}

function setCustomGrid() {
  if (!activeLayout.value) return
  const cols = Math.min(8, Math.max(1, Number(activeLayout.value.cols) || 2))
  const rows = Math.min(8, Math.max(1, Number(activeLayout.value.rows) || 2))
  const cells = [...activeLayout.value.cells]
  const count = cols * rows
  while (cells.length < count) cells.push(null)
  activeLayout.value.grid = 'custom'
  activeLayout.value.cols = cols
  activeLayout.value.rows = rows
  activeLayout.value.cells = cells.slice(0, count)
  saveLayouts()
}

function assign(cellIndex: number, channelId: number | null) {
  if (!activeLayout.value) return
  activeLayout.value.cells[cellIndex] = channelId
  saveLayouts()
}

function onCellClick(index: number) {
  if (!activeLayout.value) return
  const existing = activeLayout.value.cells[index]
  if (existing == null && selectedChannelId.value != null) {
    assign(index, selectedChannelId.value)
  } else if (existing != null) {
    selectedChannelId.value = existing
  }
}

function onDrop(index: number) {
  if (draggingId.value != null) {
    assign(index, draggingId.value)
    selectedChannelId.value = draggingId.value
    draggingId.value = null
  }
}

function clearCell(index: number) {
  assign(index, null)
}

function addLayout() {
  if (!activeLayout.value) return
  const name = window.prompt(t('preview.layoutName'), `${t('preview.layout')} ${layouts.value.length + 1}`)
  if (!name) return
  const copy: Layout = {
    id: `layout-${Date.now()}`,
    name,
    grid: activeLayout.value.grid,
    cols: activeLayout.value.cols,
    rows: activeLayout.value.rows,
    cells: [...activeLayout.value.cells],
  }
  layouts.value.push(copy)
  activeLayoutId.value = copy.id
  saveLayouts()
}

function removeLayout() {
  if (layouts.value.length <= 1 || !activeLayout.value) return
  const removed = activeLayout.value
  const index = layouts.value.findIndex((layout) => layout.id === removed.id)
  layouts.value.splice(index, 1)
  activeLayoutId.value = layouts.value[Math.max(0, index - 1)].id
  if (removed.serverId) {
    void api(`/api/v1/layouts/${removed.serverId}`, { method: 'DELETE' }).catch(() => undefined)
  }
  saveLayouts()
}

function reloadAll() {
  // The tiles poll tickets themselves; re-keying forces a fresh player for
  // channels that recovered from an offline state.
  const layout = activeLayout.value
  if (!layout) return
  const cells = [...layout.cells]
  layout.cells = []
  window.setTimeout(() => {
    layout.cells = cells
  }, 50)
}

async function loadChannels() {
  error.value = ''
  try {
    const [channelRows, groupRows] = await Promise.all([
      api<LiveChannel[]>('/api/v1/channels'),
      api<ChannelGroup[]>('/api/v1/channel-groups'),
    ])
    channels.value = channelRows
    groups.value = groupRows
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

watch(
  () => activeLayout.value?.cells,
  () => saveLayouts(),
  { deep: true },
)

onMounted(() => {
  void loadLayouts()
  void loadChannels()
  window.setInterval(loadChannels, 10_000)
})
</script>

<template>
  <section class="panel page-head preview-head">
    <div>
      <span class="pill accent">{{ t('nav.preview') }}</span>
      <h2>{{ t('preview.title') }}</h2>
      <p>{{ t('preview.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <span class="muted">{{ t('preview.onlineCount', { online: onlineCount, total: channels.length }) }}</span>
      <div class="locale-switch" role="group">
        <button
          class="locale-button"
          :class="{ active: streamMode === 'main' }"
          @click="streamMode = 'main'"
        >
          MAIN
        </button>
        <button
          class="locale-button"
          :class="{ active: streamMode === 'sub' }"
          @click="streamMode = 'sub'"
        >
          SUB
        </button>
      </div>
      <button class="button" @click="reloadAll">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <RouterLink class="button" to="/channels">
        <AppIcon name="camera" :size="16" />
        {{ t('nav.channels') }}
      </RouterLink>
    </div>
  </section>

  <p v-if="error" class="form-error">{{ error }}</p>

  <div v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </div>

  <div v-else class="preview-workspace">
    <aside class="panel channel-rail">
      <div class="channel-rail-head">
        <h3>{{ t('preview.channelList') }}</h3>
        <span class="count-badge">{{ filteredChannels.length }}</span>
      </div>
      <input v-model="search" class="input" type="search" :placeholder="t('common.search')" />
      <div class="channel-list">
        <template v-if="groups.length === 0">
          <button
            v-for="channel in filteredChannels"
            :key="channel.id"
            class="channel-item"
            :class="{ active: selectedChannelId === channel.id }"
            draggable="true"
            @click="selectedChannelId = channel.id"
            @dragstart="draggingId = channel.id"
          >
            <span class="status-dot" :class="statusClass(channel)"></span>
            <span class="channel-item-copy">
              <strong>{{ channel.name }}</strong>
              <small>{{ channel.protocol }} · {{ channel.host || '—' }}</small>
            </span>
          </button>
        </template>
        <template v-else>
          <div v-for="group in groupedChannels" :key="group.id" class="channel-group">
            <button class="channel-group-head" @click="toggleGroup(group.id)">
              <span>{{ group.name }}</span>
              <span class="count-badge">{{ group.channels.length }}</span>
            </button>
            <template v-if="!collapsedGroups.has(group.id)">
              <button
                v-for="channel in group.channels"
                :key="channel.id"
                class="channel-item"
                :class="{ active: selectedChannelId === channel.id }"
                draggable="true"
                @click="selectedChannelId = channel.id"
                @dragstart="draggingId = channel.id"
              >
                <span class="status-dot" :class="statusClass(channel)"></span>
                <span class="channel-item-copy">
                  <strong>{{ channel.name }}</strong>
                  <small>{{ channel.protocol }} · {{ channel.host || '—' }}</small>
                </span>
              </button>
            </template>
          </div>
        </template>
        <p v-if="filteredChannels.length === 0" class="small-note">
          {{ t('preview.noChannels') }}
          <RouterLink to="/channels">{{ t('preview.addChannel') }}</RouterLink>
        </p>
      </div>
      <p class="small-note drag-hint">{{ t('preview.dragHint') }}</p>
    </aside>

    <section class="preview-main">
      <div class="layout-bar">
        <div class="layout-tabs">
          <button
            v-for="layout in layouts"
            :key="layout.id"
            class="layout-tab"
            :class="{ active: layout.id === activeLayoutId }"
            @click="activeLayoutId = layout.id"
          >
            {{ layout.name }}
          </button>
          <button class="layout-tab add" @click="addLayout">+</button>
          <button
            v-if="layouts.length > 1"
            class="layout-tab remove"
            :title="t('common.delete')"
            @click="removeLayout"
          >
            ×
          </button>
        </div>
        <div class="grid-picker">
          <button
            v-for="option in gridOptions"
            :key="option.key"
            class="small-button"
            :class="{ active: activeLayout?.grid === option.key }"
            @click="setGrid(option.key)"
          >
            {{ option.key }}
          </button>
          <input
            v-model.number="activeLayout!.cols"
            class="grid-number"
            type="number"
            min="1"
            max="8"
            :title="t('preview.columns')"
            @change="setCustomGrid"
          />
          <span class="muted">×</span>
          <input
            v-model.number="activeLayout!.rows"
            class="grid-number"
            type="number"
            min="1"
            max="8"
            :title="t('preview.rows')"
            @change="setCustomGrid"
          />
        </div>
      </div>

      <div class="preview-grid" :style="gridStyle">
        <div
          v-for="(cell, index) in activeLayout?.cells ?? []"
          :key="index"
          class="preview-cell-wrap"
          @dragover.prevent
          @drop.prevent="onDrop(index)"
          @click="onCellClick(index)"
        >
          <LiveTile
            v-if="channelById(cell)"
            :channel="channelById(cell)!"
            :stream="streamMode"
          />
          <div v-else class="preview-empty">
            <AppIcon name="camera" :size="24" />
            <span>{{ t('preview.emptyCell') }}</span>
          </div>
          <button
            v-if="cell != null"
            class="cell-clear"
            :title="t('preview.clearCell')"
            @click.stop="clearCell(index)"
          >
            ×
          </button>
        </div>
      </div>
    </section>
  </div>
</template>
