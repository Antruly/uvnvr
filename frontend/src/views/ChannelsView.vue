<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'ChannelsView' })

interface Channel {
  id: number
  groupId: number
  name: string
  protocol: string
  host: string
  port: number
  httpPort: number
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
const auth = useAuthStore()
const canSnapshot = computed(() => auth.hasPermission('api.snapshot'))
const failedThumbs = ref<Set<number>>(new Set())
const thumbVersion = reactive<Record<number, number>>({})
const thumbLoading = ref<Set<number>>(new Set())
const previewImage = ref('')
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
  httpPort: 80,
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
const detecting = ref(false)
const scanSelection = ref<Set<string>>(new Set())
const scanStatus = reactive<Record<string, string>>({})
const scanErrors = reactive<Record<string, string>>({})
const scanError = ref('')
const scanNotice = ref('')
const scanAdding = ref(false)
const encoder = reactive({
  token: '',
  encoding: 'H264',
  width: 0,
  height: 0,
  fps: 0,
  bitrateKbps: 0,
  quality: 0,
})
const encoderBusy = ref(false)
const encoderError = ref('')
const encoderNotice = ref('')
const importing = ref(false)
const importTotal = ref(0)
const importDone = ref(0)
const importInput = ref<HTMLInputElement | null>(null)

function parseCsvLine(line: string) {
  const cells: string[] = []
  let current = ''
  let quoted = false
  for (let index = 0; index < line.length; index += 1) {
    const char = line[index]
    if (char === '"') {
      if (quoted && line[index + 1] === '"') {
        current += '"'
        index += 1
      } else {
        quoted = !quoted
      }
    } else if (char === ',' && !quoted) {
      cells.push(current.trim())
      current = ''
    } else {
      current += char
    }
  }
  cells.push(current.trim())
  return cells
}

async function importCsv(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file || importing.value) return
  importing.value = true
  importDone.value = 0
  importTotal.value = 0
  error.value = ''
  notice.value = ''
  const failures: string[] = []
  let succeeded = 0
  try {
    const lines = (await file.text())
      .replace(/^\uFEFF/, '')
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line && !line.startsWith('#'))
    if (lines.length === 0) {
      error.value = t('channels.importEmpty')
      return
    }
    let rows = lines.map(parseCsvLine)
    let columns = ['host', 'username', 'password', 'brand', 'name', 'httpport']
    const first = rows[0].map((cell) => cell.toLowerCase())
    if (first.includes('host') || first.includes('ip')) {
      columns = first
      rows = rows.slice(1)
    }
    const column = (row: string[], name: string) => {
      const index = columns.indexOf(name)
      return index >= 0 ? (row[index] ?? '') : ''
    }
    if (rows.length === 0) {
      error.value = t('channels.importEmpty')
      return
    }
    const knownHosts = new Set(channels.value.map((channel) => channel.host))
    importTotal.value = rows.length
    for (const row of rows) {
      const host = column(row, 'host') || column(row, 'ip')
      if (!host) {
        importDone.value += 1
        continue
      }
      try {
        if (knownHosts.has(host)) {
          failures.push(`${host}: ${t('channels.importDuplicate')}`)
          continue
        }
        knownHosts.add(host)
        const username = column(row, 'username') || column(row, 'user')
        const password = column(row, 'password') || column(row, 'pass')
        const brand = column(row, 'brand').toLowerCase()
        const httpPort = Number(column(row, 'httpport') || column(row, 'http_port') || 80) || 80
        const detected = await api<DetectResult>('/api/v1/channels/detect', {
          method: 'POST',
          body: JSON.stringify({ host, httpPort, username, password, brand }),
        })
        const payload = {
          name: column(row, 'name') || detected.name || detected.model || host,
          protocol: detected.protocol,
          host,
          port: detected.protocol === 'onvif' ? 80 : 554,
          httpPort: detected.httpPort ?? httpPort,
          username,
          password,
          mainUrl: detected.mainUrl,
          subUrl: detected.subUrl,
          subEnabled: detected.subEnabled,
          enabled: true,
          brand: detected.brand,
          eventsEnabled: detected.eventsEnabled,
          watermark: { text: '', position: 'bottom-right' },
        }
        await api('/api/v1/channels', { method: 'POST', body: JSON.stringify(payload) })
        succeeded += 1
      } catch (caught) {
        failures.push(`${host}: ${caught instanceof Error ? caught.message : t('common.error')}`)
      } finally {
        importDone.value += 1
      }
    }
    await load()
    notice.value = t('channels.importDone', { count: succeeded })
    if (failures.length > 0) {
      error.value = t('channels.importFailed', { count: failures.length }) + ' ' + failures.slice(0, 5).join('; ')
    }
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    importing.value = false
    input.value = ''
  }
}

async function readEncoder() {
  if (modal.mode !== 'edit' || modal.id <= 0) return
  encoderBusy.value = true
  encoderError.value = ''
  encoderNotice.value = ''
  try {
    const config = await api<typeof encoder>(`/api/v1/channels/${modal.id}/encoder`)
    Object.assign(encoder, config)
    encoderNotice.value = t('channels.encoderReadDone')
  } catch (caught) {
    encoderError.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    encoderBusy.value = false
  }
}

async function applyEncoder() {
  if (modal.mode !== 'edit' || modal.id <= 0) return
  encoderBusy.value = true
  encoderError.value = ''
  encoderNotice.value = ''
  try {
    const config = await api<typeof encoder>(`/api/v1/channels/${modal.id}/encoder`, {
      method: 'PUT',
      body: JSON.stringify({
        token: encoder.token,
        encoding: encoder.encoding,
        width: Number(encoder.width),
        height: Number(encoder.height),
        fps: Number(encoder.fps),
        bitrateKbps: Number(encoder.bitrateKbps),
        quality: Number(encoder.quality),
      }),
    })
    Object.assign(encoder, config)
    encoderNotice.value = t('channels.encoderApplied')
  } catch (caught) {
    encoderError.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    encoderBusy.value = false
  }
}

interface DetectResult {
  protocol: string
  name: string
  model: string
  brand: string
  mainUrl: string
  subUrl: string
  subEnabled: boolean
  eventsEnabled: boolean
  httpPort: number
}

function detectedProtocol(manufacturer: string) {
  const value = manufacturer.toLowerCase()
  if (value.includes('hikvision'))
    return 'hikvision'
  if (value.includes('dahua'))
    return 'dahua'
  return 'onvif'
}

function applyProbeResult(host: string, result: OnvifProbeResult) {
  const protocol = detectedProtocol(result.device.manufacturer)
  const profile = result.profiles[0]
  const subProfile = result.profiles[1]
  Object.assign(modal, {
    groupId: 0,
    name: profile?.name || result.device.model || host,
    protocol,
    host,
    port: protocol === 'onvif' ? 80 : 554,
    httpPort: 80,
    mainUrl: protocol === 'onvif' ? (profile?.streamUri ?? '') : '',
    subUrl: protocol === 'onvif' ? (subProfile?.streamUri ?? '') : '',
    subEnabled: protocol !== 'onvif' || Boolean(subProfile),
    enabled: true,
    brand: result.device.manufacturer,
    eventsEnabled: true,
  })
}

async function detectFromModal() {
  if (!modal.host.trim()) return
  detecting.value = true
  error.value = ''
  try {
    const preferred = modal.protocol === 'hikvision' || modal.protocol === 'dahua'
      ? modal.protocol
      : ''
    const result = await api<DetectResult>('/api/v1/channels/detect', {
      method: 'POST',
      body: JSON.stringify({
        host: modal.host.trim(),
        httpPort: Number(modal.httpPort) || 80,
        username: modal.username,
        password: modal.password,
        brand: preferred,
      }),
    })
    Object.assign(modal, {
      protocol: result.protocol,
      name: result.name || modal.name || result.model,
      brand: result.brand,
      port: result.protocol === 'onvif' ? 80 : 554,
      httpPort: result.httpPort ?? (Number(modal.httpPort) || 80),
      mainUrl: result.mainUrl,
      subUrl: result.subUrl,
      subEnabled: result.subEnabled,
      eventsEnabled: result.eventsEnabled,
    })
    flash(t('channels.detected'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    detecting.value = false
  }
}

async function addDevice(device: OnvifDevice): Promise<boolean> {
  if (scanStatus[device.xaddr] === 'added' || scanStatus[device.xaddr] === 'adding') return false
  if (channels.value.some((channel) => channel.host === device.ip)) {
    scanStatus[device.xaddr] = 'failed'
    scanErrors[device.xaddr] = t('channels.importDuplicate')
    return false
  }
  scanStatus[device.xaddr] = 'adding'
  scan.probing = device.xaddr
  try {
    const result = await api<OnvifProbeResult>('/api/v1/channels/onvif/probe', {
      method: 'POST',
      body: JSON.stringify({
        xaddr: device.xaddr,
        username: scan.username,
        password: scan.password,
      }),
    })
    const protocol = detectedProtocol(result.device.manufacturer)
    const profile = result.profiles[0]
    const subProfile = result.profiles[1]
    const payload: Record<string, unknown> = {
      name: profile?.name || result.device.model || device.ip,
      protocol,
      host: device.ip,
      port: protocol === 'onvif' ? 80 : 554,
      httpPort: 80,
      username: scan.username,
      password: scan.password,
      mainUrl: protocol === 'onvif' ? (profile?.streamUri ?? '') : '',
      subUrl: protocol === 'onvif' ? (subProfile?.streamUri ?? '') : '',
      subEnabled: protocol !== 'onvif' || Boolean(subProfile),
      enabled: true,
      brand: result.device.manufacturer,
      eventsEnabled: true,
      watermark: { text: '', position: 'bottom-right' },
    }
    await api('/api/v1/channels', { method: 'POST', body: JSON.stringify(payload) })
    scanStatus[device.xaddr] = 'added'
    delete scanErrors[device.xaddr]
    scanSelection.value = new Set([...scanSelection.value].filter((key) => key !== device.xaddr))
    return true
  } catch (caught) {
    scanStatus[device.xaddr] = 'failed'
    scanErrors[device.xaddr] =
      caught instanceof ApiError ? caught.message : t('common.error')
    return false
  } finally {
    scan.probing = ''
  }
}

async function quickAdd(device: OnvifDevice) {
  scanError.value = ''
  scanNotice.value = ''
  if (await addDevice(device)) {
    flash(t('channels.created'))
    await load()
  } else if (scanErrors[device.xaddr]) {
    scanError.value = scanErrors[device.xaddr]
  }
}

function toggleDevice(xaddr: string) {
  const next = new Set(scanSelection.value)
  if (next.has(xaddr)) next.delete(xaddr)
  else next.add(xaddr)
  scanSelection.value = next
}

function allDevicesSelected() {
  return scan.devices.length > 0 && scan.devices.every((device) => scanSelection.value.has(device.xaddr))
}

function toggleAllDevices() {
  scanSelection.value = allDevicesSelected()
    ? new Set()
    : new Set(scan.devices.map((device) => device.xaddr))
}

async function addSelected() {
  if (scanAdding.value) return
  const devices = scan.devices.filter(
    (device) => scanSelection.value.has(device.xaddr) && scanStatus[device.xaddr] !== 'added',
  )
  if (devices.length === 0) return
  scanAdding.value = true
  scanError.value = ''
  scanNotice.value = ''
  let added = 0
  let failed = 0
  for (const device of devices) {
    if (await addDevice(device)) added += 1
    else failed += 1
  }
  await load()
  scanAdding.value = false
  if (added > 0) scanNotice.value = t('channels.addSelectedDone', { count: added })
  if (failed > 0) {
    const detail = devices
      .map((device) => (scanErrors[device.xaddr] ? `${device.ip}: ${scanErrors[device.xaddr]}` : ''))
      .filter(Boolean)
      .slice(0, 4)
      .join('; ')
    scanError.value = t('channels.addSelectedFailed', { count: failed }) + (detail ? ` ${detail}` : '')
  }
}

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

function snapshotUrl(channel: Channel) {
  return `/api/v1/channels/${channel.id}/snapshot?t=${thumbVersion[channel.id] ?? 0}`
}

function thumbFailed(channel: Channel) {
  failedThumbs.value = new Set([...failedThumbs.value, channel.id])
  const next = new Set(thumbLoading.value)
  next.delete(channel.id)
  thumbLoading.value = next
}

function thumbLoaded(channel: Channel) {
  const next = new Set(thumbLoading.value)
  next.delete(channel.id)
  thumbLoading.value = next
}

function capture(channel: Channel) {
  const version = Date.now()
  thumbLoading.value = new Set([...thumbLoading.value, channel.id])
  thumbVersion[channel.id] = version
  failedThumbs.value = new Set([...failedThumbs.value].filter((id) => id !== channel.id))
  previewImage.value = `/api/v1/channels/${channel.id}/snapshot?t=${version}`
}

function closePreview() {
  previewImage.value = ''
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
    httpPort: 80,
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
    httpPort: channel.httpPort ?? 80,
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
    if (!modal.name.trim())
      modal.name = modal.host.trim() || 'Camera'
    const payload: Record<string, unknown> = {
      name: modal.name,
      protocol: modal.protocol,
      host: modal.host,
      port: Number(modal.port) || 554,
      httpPort: Number(modal.httpPort) || 80,
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
  scanSelection.value = new Set()
  scanStatus
  Object.keys(scanStatus).forEach((key) => delete scanStatus[key])
  Object.keys(scanErrors).forEach((key) => delete scanErrors[key])
  scanError.value = ''
  scanNotice.value = ''
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
    scanSelection.value = new Set(result.devices.map((device) => device.xaddr))
    Object.keys(scanStatus).forEach((key) => delete scanStatus[key])
    Object.keys(scanErrors).forEach((key) => delete scanErrors[key])
    scanError.value = ''
    scanNotice.value = ''
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
        password: scan.password,
        username: scan.username,
        watermarkText: '',
        watermarkPosition: 'bottom-right',
      })
      applyProbeResult(device.ip, scan.result)
      scan.open = false
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
      <input
        ref="importInput"
        class="hidden-input"
        type="file"
        accept=".csv,text/csv"
        @change="importCsv"
      />
      <button
        class="button"
        :disabled="importing"
        :title="t('channels.csvHint')"
        @click="importInput?.click()"
      >
        <AppIcon name="plus" :size="16" />
        {{ importing ? `${importDone}/${importTotal}` : t('channels.importCsv') }}
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
              <span class="channel-thumb" @dblclick="capture(channel)">
                <img
                  v-if="canSnapshot && !failedThumbs.has(channel.id)"
                  :src="snapshotUrl(channel)"
                  loading="lazy"
                  alt=""
                  @load="thumbLoaded(channel)"
                  @error="thumbFailed(channel)"
                />
                <span v-if="thumbLoading.has(channel.id) && !failedThumbs.has(channel.id)" class="thumb-spinner"></span>
                <AppIcon v-else-if="!canSnapshot" name="camera" :size="15" />
              </span>
              <div>
                <strong>{{ channel.name }}</strong>
                <small class="block muted">#{{ channel.id }}</small>
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
              <button
                class="icon-button small"
                :title="t('channels.snapshot')"
                :disabled="!canSnapshot"
                @click="capture(channel)"
              >
                <AppIcon name="camera" :size="15" />
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
    <div v-if="previewImage" class="modal-backdrop" @click.self="closePreview">
      <div class="modal panel snapshot-modal">
        <div class="modal-head">
          <h3>{{ t('channels.snapshot') }}</h3>
          <button type="button" class="icon-button small" @click="closePreview">
            <AppIcon name="close" :size="16" />
          </button>
        </div>
        <img :src="previewImage" class="snapshot-large" alt="" />
      </div>
    </div>

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
            <span>{{ t('channels.autoDetect') }}</span>
            <button
              type="button"
              class="button"
              :disabled="detecting || !modal.host.trim()"
              @click="detectFromModal"
            >
              <AppIcon name="search" :size="15" />
              {{ detecting ? t('common.loading') : t('channels.autoDetectAction') }}
            </button>
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
            <span>{{ t('channels.httpPort') }}</span>
            <input v-model.number="modal.httpPort" class="input" type="number" min="1" max="65535" />
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

        <div v-if="modal.mode === 'edit' && modal.protocol === 'onvif'" class="encoder-section">
          <div class="section-title">
            <AppIcon name="settings" :size="18" />
            <h3>{{ t('channels.encoderTitle') }}</h3>
          </div>
          <p class="small-note">{{ t('channels.encoderHint') }}</p>
          <div class="modal-columns">
            <label class="field">
              <span>{{ t('channels.encoderEncoding') }}</span>
              <select v-model="encoder.encoding" class="input">
                <option value="H264">H.264</option>
                <option value="H265">H.265</option>
              </select>
            </label>
            <label class="field">
              <span>{{ t('channels.encoderWidth') }}</span>
              <input v-model.number="encoder.width" class="input" type="number" min="0" max="8192" />
            </label>
            <label class="field">
              <span>{{ t('channels.encoderHeight') }}</span>
              <input v-model.number="encoder.height" class="input" type="number" min="0" max="8192" />
            </label>
            <label class="field">
              <span>{{ t('channels.encoderFps') }}</span>
              <input v-model.number="encoder.fps" class="input" type="number" min="0" max="120" />
            </label>
            <label class="field">
              <span>{{ t('channels.encoderBitrate') }}</span>
              <input
                v-model.number="encoder.bitrateKbps"
                class="input"
                type="number"
                min="0"
                max="102400"
              />
            </label>
            <label class="field">
              <span>{{ t('channels.encoderQuality') }}</span>
              <input v-model.number="encoder.quality" class="input" type="number" min="0" max="100" />
            </label>
          </div>
          <div class="modal-actions">
            <button type="button" class="button" :disabled="encoderBusy" @click="readEncoder">
              <AppIcon name="refresh" :size="15" />
              {{ t('channels.encoderRead') }}
            </button>
            <button type="button" class="button primary" :disabled="encoderBusy" @click="applyEncoder">
              <AppIcon name="save" :size="15" />
              {{ t('channels.encoderApply') }}
            </button>
          </div>
          <p v-if="encoderNotice" class="notice">{{ encoderNotice }}</p>
          <p v-if="encoderError" class="form-error">{{ encoderError }}</p>
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
          <button
            v-if="scan.devices.length > 0"
            class="button primary"
            :disabled="scanAdding || scanSelection.size === 0"
            @click="addSelected"
          >
            <AppIcon name="plus" :size="16" />
            {{
              scanAdding
                ? t('channels.adding')
                : t('channels.addSelected', { count: scanSelection.size })
            }}
          </button>
        </div>
        <p v-if="scanNotice" class="notice">{{ scanNotice }}</p>
        <p v-if="scanError" class="form-error">{{ scanError }}</p>
        <table class="data-table">
          <thead>
            <tr>
              <th class="check-cell">
                <input
                  type="checkbox"
                  :checked="allDevicesSelected()"
                  :aria-label="t('channels.selectAll')"
                  @change="toggleAllDevices"
                />
              </th>
              <th>{{ t('channels.ip') }}</th>
              <th>XAddr</th>
              <th>{{ t('common.actions') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="device in scan.devices" :key="device.xaddr">
              <td class="check-cell">
                <input
                  type="checkbox"
                  :checked="scanSelection.has(device.xaddr)"
                  :disabled="scanStatus[device.xaddr] === 'added'"
                  @change="toggleDevice(device.xaddr)"
                />
              </td>
              <td>{{ device.ip }}</td>
              <td class="mono">{{ device.xaddr }}</td>
              <td>
                <div class="scan-actions">
                  <button class="button small" :disabled="scan.probing === device.xaddr || scanStatus[device.xaddr] === 'added'" @click="probe(device)">
                    {{ scan.probing === device.xaddr ? t('common.loading') : t('channels.probe') }}
                  </button>
                  <button
                    class="button small primary"
                    :disabled="scan.probing === device.xaddr || scanAdding || scanStatus[device.xaddr] === 'added'"
                    @click="quickAdd(device)"
                  >
                    {{
                      scanStatus[device.xaddr] === 'added'
                        ? t('channels.added')
                        : scan.probing === device.xaddr
                          ? t('channels.adding')
                          : t('channels.addNow')
                    }}
                  </button>
                </div>
                <small v-if="scanErrors[device.xaddr]" class="block error-text">{{ scanErrors[device.xaddr] }}</small>
              </td>
            </tr>
            <tr v-if="scan.devices.length === 0">
              <td colspan="4" class="empty-cell">{{ t('channels.noDevicesYet') }}</td>
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
