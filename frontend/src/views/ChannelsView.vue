<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'

defineOptions({ name: 'ChannelsView' })

interface Channel {
  id: number
  groupId: number
  name: string
  protocol: string
  host: string
  port: number
  username: string
  hasPassword: boolean
  mainUrl: string
  subUrl: string
  subEnabled: boolean
  enabled: boolean
  eventsEnabled: boolean
  brand: string
  watermark: { text?: string; position?: string }
  lastStatus: string
  lastError: string
  mainState: string
  mainError: string
  lastOnlineAt: number
  lastOfflineAt: number
  subState: string
  subError: string
}

interface OnvifDevice {
  xaddr: string
  ip: string
}

interface OnvifProfile {
  token: string
  name: string
  streamUri: string
}

interface OnvifProbeResult {
  device: {
    manufacturer: string
    model: string
    firmware: string
    serial: string
  }
  profiles: OnvifProfile[]
}

interface ChannelGroup {
  id: number
  name: string
  parentId: number
  sort: number
  channels: number
}

const { t } = useI18n()
const channels = ref<Channel[]>([])
const groups = ref<ChannelGroup[]>([])
const groupsOpen = ref(false)
const groupModal = reactive({ open: false, name: '', parentId: 0 })
const loading = ref(true)
const error = ref('')
const notice = ref('')
const modal = reactive({
  open: false,
  mode: 'create' as 'create' | 'edit',
  id: 0,
  groupId: 0,
  name: '',
  protocol: 'rtsp',
  host: '',
  port: 554,
  username: '',
  password: '',
  mainUrl: '',
  subUrl: '',
  subEnabled: false,
  enabled: true,
  eventsEnabled: false,
  brand: '',
  watermarkText: '',
  watermarkPosition: 'bottom-right',
})

const scan = reactive({
  open: false,
  scanning: false,
  username: '',
  password: '',
  devices: [] as OnvifDevice[],
  probing: '',
  result: null as OnvifProbeResult | null,
})

const protocols = ['rtsp', 'onvif', 'hikvision', 'dahua', 'generic']

const onlineCount = computed(
  () => channels.value.filter((channel) => channel.mainState === 'online').length,
)

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3500)
}

function groupName(id: number) {
  return groups.value.find((group) => group.id === id)?.name ?? ''
}

function stateLabel(channel: Channel) {
  if (!channel.enabled) return t('common.disabled')
  if (channel.mainState === 'online') return t('preview.online')
  if (channel.mainState === 'reconnecting' || channel.mainState === 'connecting') {
    return t('preview.connecting')
  }
  if (channel.mainState === 'error') return t('common.error')
  return t('preview.offline')
}

function formatTime(value: number) {
  if (!value) return '—'
  return new Intl.DateTimeFormat(undefined, {
    month: 'short',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  }).format(new Date(value * 1000))
}

function stateClass(channel: Channel) {
  if (!channel.enabled) return 'muted'
  if (channel.mainState === 'online') return 'ok'
  if (channel.mainState === 'reconnecting' || channel.mainState === 'connecting') return 'warn'
  return 'danger'
}

async function load() {
  error.value = ''
  try {
    const [channelRows, groupRows] = await Promise.all([
      api<Channel[]>('/api/v1/channels'),
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

function openCreate() {
  Object.assign(modal, {
    open: true,
    mode: 'create',
    id: 0,
    groupId: 0,
    name: '',
    protocol: 'rtsp',
    host: '',
    port: 554,
    username: '',
    password: '',
    mainUrl: '',
    subUrl: '',
    subEnabled: false,
    enabled: true,
    eventsEnabled: false,
    brand: '',
    watermarkText: '',
    watermarkPosition: 'bottom-right',
  })
}

function openEdit(channel: Channel) {
  Object.assign(modal, {
    open: true,
    mode: 'edit',
    id: channel.id,
    groupId: channel.groupId ?? 0,
    name: channel.name,
    protocol: channel.protocol,
    host: channel.host,
    port: channel.port,
    username: channel.username,
    password: '',
    mainUrl: channel.mainUrl,
    subUrl: channel.subUrl,
    subEnabled: channel.subEnabled,
    enabled: channel.enabled,
    eventsEnabled: channel.eventsEnabled ?? false,
    brand: channel.brand,
    watermarkText: channel.watermark?.text ?? '',
    watermarkPosition: channel.watermark?.position ?? 'bottom-right',
  })
}

async function save() {
  error.value = ''
  try {
    const payload: Record<string, unknown> = {
      name: modal.name,
      protocol: modal.protocol,
      host: modal.host,
      port: Number(modal.port) || 554,
      username: modal.username,
      mainUrl: modal.mainUrl,
      subUrl: modal.subUrl,
      subEnabled: modal.subEnabled,
      enabled: modal.enabled,
      eventsEnabled: modal.eventsEnabled,
      brand: modal.brand,
      groupId: modal.groupId,
      watermark: {
        text: modal.watermarkText,
        position: modal.watermarkPosition,
      },
    }
    if (modal.password) payload.password = modal.password
    if (modal.mode === 'create') {
      await api('/api/v1/channels', { method: 'POST', body: JSON.stringify(payload) })
      flash(t('channels.created'))
    } else {
      await api(`/api/v1/channels/${modal.id}`, {
        method: 'PUT',
        body: JSON.stringify(payload),
      })
      flash(t('channels.updated'))
    }
    modal.open = false
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function remove(channel: Channel) {
  if (!window.confirm(t('channels.deleteConfirm', { name: channel.name }))) return
  try {
    await api(`/api/v1/channels/${channel.id}`, { method: 'DELETE' })
    flash(t('channels.deleted'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function reconnect(channel: Channel) {
  try {
    await api(`/api/v1/channels/${channel.id}/reconnect`, { method: 'POST' })
    flash(t('channels.reconnecting'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

function openScan() {
  Object.assign(scan, {
    open: true,
    scanning: false,
    username: '',
    password: '',
    devices: [],
    probing: '',
    result: null,
  })
}

async function discover() {
  scan.scanning = true
  error.value = ''
  try {
    const result = await api<{ devices: OnvifDevice[] }>('/api/v1/channels/discover', {
      method: 'POST',
      body: '{}',
    })
    scan.devices = result.devices
    if (result.devices.length === 0) {
      error.value = t('channels.noDevices')
    }
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    scan.scanning = false
  }
}

async function probe(device: OnvifDevice) {
  scan.probing = device.xaddr
  scan.result = null
  error.value = ''
  try {
    scan.result = await api<OnvifProbeResult>('/api/v1/channels/onvif/probe', {
      method: 'POST',
      body: JSON.stringify({
        xaddr: device.xaddr,
        username: scan.username,
        password: scan.password,
      }),
    })
    if (scan.result?.profiles.length) {
      Object.assign(modal, {
        open: true,
        mode: 'create',
        groupId: 0,
        name: scan.result.device.model || device.ip,
        protocol: 'onvif',
        host: device.ip,
        port: 80,
        username: scan.username,
        password: scan.password,
        mainUrl: scan.result.profiles[0].streamUri,
        subUrl: scan.result.profiles[1]?.streamUri ?? '',
        subEnabled: scan.result.profiles.length > 1,
        enabled: true,
        eventsEnabled: true,
        brand: scan.result.device.manufacturer,
        watermarkText: '',
        watermarkPosition: 'bottom-right',
      })
    }
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    scan.probing = ''
  }
}

function openGroups() {
  groupsOpen.value = !groupsOpen.value
  groupModal.name = ''
  groupModal.parentId = 0
}

async function createGroup() {
  if (!groupModal.name.trim()) return
  try {
    await api('/api/v1/channel-groups', {
      method: 'POST',
      body: JSON.stringify({ name: groupModal.name.trim(), parentId: groupModal.parentId }),
    })
    groupModal.name = ''
    groupModal.parentId = 0
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function removeGroup(group: ChannelGroup) {
  if (!window.confirm(t('channels.deleteGroupConfirm', { name: group.name }))) return
  try {
    await api(`/api/v1/channel-groups/${group.id}`, { method: 'DELETE' })
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

onMounted(load)
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.channels') }}</span>
      <h2>{{ t('channels.title') }}</h2>
      <p>{{ t('channels.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <span class="muted">{{ t('preview.onlineCount', { online: onlineCount, total: channels.length }) }}</span>
      <button class="button" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <button class="button" @click="openScan">
        <AppIcon name="search" :size="16" />
        {{ t('channels.scanOnvif') }}
      </button>
      <button class="button" @click="openGroups">
        <AppIcon name="users" :size="16" />
        {{ t('channels.groups') }}
      </button>
      <button class="button primary" @click="openCreate">
        <AppIcon name="plus" :size="16" />
        {{ t('channels.addChannel') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="groupsOpen" class="panel groups-panel">
    <div class="section-title">
      <AppIcon name="users" :size="18" />
      <h3>{{ t('channels.groups') }}</h3>
    </div>
    <form class="group-form" @submit.prevent="createGroup">
      <input v-model="groupModal.name" class="input" :placeholder="t('channels.groupName')" maxlength="64" />
      <select v-model.number="groupModal.parentId" class="input">
        <option :value="0">{{ t('channels.ungrouped') }}</option>
        <option v-for="group in groups" :key="group.id" :value="group.id">
          {{ group.name }}
        </option>
      </select>
      <button class="button primary" type="submit" :disabled="!groupModal.name.trim()">
        <AppIcon name="plus" :size="15" />
        {{ t('channels.addGroup') }}
      </button>
    </form>
    <div v-if="groups.length === 0" class="small-note">{{ t('channels.noGroups') }}</div>
    <ul v-else class="group-list">
      <li v-for="group in groups" :key="group.id">
        <span><strong>{{ group.name }}</strong>
          <small class="muted"> · {{ t('channels.groupChannels', { count: group.channels }) }}</small>
        </span>
        <button class="button small danger" @click="removeGroup(group)">
          <AppIcon name="trash" :size="14" />
        </button>
      </li>
    </ul>
  </section>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else class="panel table-panel">
    <table class="data-table">
      <thead>
        <tr>
          <th>{{ t('channels.name') }}</th>
          <th>{{ t('channels.protocol') }}</th>
          <th>{{ t('channels.state') }}</th>
          <th>{{ t('channels.streams') }}</th>
          <th>{{ t('channels.lastError') }}</th>
          <th>{{ t('common.actions') }}</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="channel in channels" :key="channel.id">
          <td>
            <div class="table-primary">
              <span class="user-avatar small"><AppIcon name="camera" :size="15" /></span>
              <div>
                <strong>{{ channel.name }}</strong>
                <small v-if="channel.groupId" class="block muted">{{ groupName(channel.groupId) }}</small>
                <small>{{ channel.host || '—' }}<span v-if="channel.port">:{{ channel.port }}</span></small>
              </div>
            </div>
          </td>
          <td>
            <span class="pill">{{ channel.protocol }}</span>
            <small v-if="channel.brand" class="block muted">{{ channel.brand }}</small>
          </td>
          <td>
            <span class="status-pill" :class="stateClass(channel)">{{ stateLabel(channel) }}</span>
            <small v-if="channel.mainError" class="block error-text">{{ channel.mainError }}</small>
            <small class="block muted">
              {{ t('channels.lastOnline') }} {{ formatTime(channel.lastOnlineAt) }}
              · {{ t('channels.lastOffline') }} {{ formatTime(channel.lastOfflineAt) }}
            </small>
          </td>
          <td>
            <span class="status-pill" :class="channel.enabled ? 'ok' : 'muted'">
              {{ channel.enabled ? t('common.enabled') : t('common.disabled') }}
            </span>
            <small class="block muted">
              main<span v-if="channel.subEnabled"> · sub</span>
            </small>
          </td>
          <td class="muted">{{ channel.lastError || channel.mainError || '—' }}</td>
          <td>
            <div class="row-actions">
              <button class="icon-button small" :title="t('common.edit')" @click="openEdit(channel)">
                <AppIcon name="edit" :size="15" />
              </button>
              <button class="icon-button small" :title="t('channels.reconnect')" @click="reconnect(channel)">
                <AppIcon name="refresh" :size="15" />
              </button>
              <button class="icon-button small danger" :title="t('common.delete')" @click="remove(channel)">
                <AppIcon name="trash" :size="15" />
              </button>
            </div>
          </td>
        </tr>
        <tr v-if="channels.length === 0">
          <td colspan="6" class="empty-cell">{{ t('channels.empty') }}</td>
        </tr>
      </tbody>
    </table>
  </section>

  <Teleport to="body">
    <div v-if="modal.open" class="modal-backdrop" @click.self="modal.open = false">
      <form class="modal panel modal-wide" @submit.prevent="save">
        <div class="modal-head">
          <h3>{{ modal.mode === 'create' ? t('channels.addChannel') : t('channels.editChannel') }}</h3>
          <button type="button" class="icon-button small" @click="modal.open = false">
            <AppIcon name="close" :size="16" />
          </button>
        </div>

        <div class="modal-columns">
          <label class="field">
            <span>{{ t('channels.name') }}</span>
            <input v-model="modal.name" class="input" required maxlength="64" />
          </label>
          <label class="field">
            <span>{{ t('channels.protocol') }}</span>
            <select v-model="modal.protocol" class="input">
              <option v-for="protocol in protocols" :key="protocol" :value="protocol">
                {{ protocol }}
              </option>
            </select>
          </label>
          <label class="field">
            <span>{{ t('channels.group') }}</span>
            <select v-model.number="modal.groupId" class="input">
              <option :value="0">{{ t('channels.ungrouped') }}</option>
              <option v-for="group in groups" :key="group.id" :value="group.id">
                {{ group.name }}
              </option>
            </select>
          </label>
          <label class="field">
            <span>{{ t('channels.host') }}</span>
            <input v-model="modal.host" class="input" placeholder="192.168.1.64" />
          </label>
          <label class="field">
            <span>{{ t('channels.port') }}</span>
            <input v-model.number="modal.port" class="input" type="number" min="1" max="65535" />
          </label>
          <label class="field">
            <span>{{ t('channels.username') }}</span>
            <input v-model="modal.username" class="input" autocomplete="off" />
          </label>
          <label class="field">
            <span>{{ t('channels.password') }}</span>
            <input
              v-model="modal.password"
              class="input"
              type="password"
              autocomplete="new-password"
              :placeholder="modal.mode === 'edit' ? t('channels.keepPassword') : ''"
            />
          </label>
        </div>

        <label class="field">
          <span>{{ t('channels.mainUrl') }}</span>
          <input v-model="modal.mainUrl" class="input" placeholder="rtsp://user:pass@host:554/..." />
        </label>
        <label class="field">
          <span>{{ t('channels.subUrl') }}</span>
          <input v-model="modal.subUrl" class="input" placeholder="rtsp://..." />
        </label>

        <div class="modal-columns">
          <label class="check-row">
            <input v-model="modal.subEnabled" type="checkbox" />
            <span>{{ t('channels.subEnabled') }}</span>
          </label>
          <label class="check-row">
            <input v-model="modal.enabled" type="checkbox" />
            <span>{{ t('common.enabled') }}</span>
          </label>
          <label class="check-row">
            <input v-model="modal.eventsEnabled" type="checkbox" />
            <span>{{ t('channels.eventsEnabled') }}</span>
          </label>
          <label class="field">
            <span>{{ t('channels.watermark') }}</span>
            <input v-model="modal.watermarkText" class="input" />
          </label>
          <label class="field">
            <span>{{ t('channels.watermarkPosition') }}</span>
            <select v-model="modal.watermarkPosition" class="input">
              <option value="top-left">top-left</option>
              <option value="top-right">top-right</option>
              <option value="bottom-left">bottom-left</option>
              <option value="bottom-right">bottom-right</option>
            </select>
          </label>
        </div>

        <p class="small-note">{{ t('channels.hikvisionHint') }}</p>
        <p class="small-note">{{ t('channels.eventsHint') }}</p>

        <div class="modal-actions">
          <button type="button" class="button" @click="modal.open = false">
            {{ t('common.cancel') }}
          </button>
          <button type="submit" class="button primary">
            <AppIcon name="save" :size="16" />
            {{ t('common.save') }}
          </button>
        </div>
      </form>
    </div>

    <div v-if="scan.open" class="modal-backdrop" @click.self="scan.open = false">
      <div class="modal panel modal-wide">
        <div class="modal-head">
          <h3>{{ t('channels.scanOnvif') }}</h3>
          <button type="button" class="icon-button small" @click="scan.open = false">
            <AppIcon name="close" :size="16" />
          </button>
        </div>
        <p class="small-note">{{ t('channels.scanHint') }}</p>
        <div class="modal-columns">
          <label class="field">
            <span>{{ t('channels.username') }}</span>
            <input v-model="scan.username" class="input" autocomplete="off" />
          </label>
          <label class="field">
            <span>{{ t('channels.password') }}</span>
            <input v-model="scan.password" class="input" type="password" autocomplete="new-password" />
          </label>
        </div>
        <div class="head-actions">
          <button class="button primary" :disabled="scan.scanning" @click="discover">
            <AppIcon name="search" :size="16" />
            {{ scan.scanning ? t('channels.scanning') : t('channels.startScan') }}
          </button>
        </div>
        <table class="data-table">
          <thead>
            <tr>
              <th>{{ t('channels.ip') }}</th>
              <th>XAddr</th>
              <th>{{ t('common.actions') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="device in scan.devices" :key="device.xaddr">
              <td>{{ device.ip }}</td>
              <td class="mono">{{ device.xaddr }}</td>
              <td>
                <button class="button small" :disabled="scan.probing === device.xaddr" @click="probe(device)">
                  {{ scan.probing === device.xaddr ? t('common.loading') : t('channels.probe') }}
                </button>
              </td>
            </tr>
            <tr v-if="scan.devices.length === 0">
              <td colspan="3" class="empty-cell">{{ t('channels.noDevicesYet') }}</td>
            </tr>
          </tbody>
        </table>
        <div v-if="scan.result" class="notice">
          {{ scan.result.device.manufacturer }} {{ scan.result.device.model }} ·
          {{ scan.result.profiles.length }} profile(s)
        </div>
      </div>
    </div>
  </Teleport>
</template>
