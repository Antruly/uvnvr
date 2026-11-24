<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'
import { useChannelStatusStore } from '@/stores/channelStatus'

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
  protocol?: string
  httpPort?: number
  rtspPort?: number
  name?: string
  model?: string
  brand?: string
  source?: string
}

interface DetectAttempt {
  protocol: string
  port: number
  ok: boolean
  error: string
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
    deviceName?: string
  }
  deviceName?: string
  nameSource?: string
  profiles: OnvifProfile[]
}

interface ChannelGroup {
  id: number
  name: string
  parentId: number
  sort: number
  channels: number
}

interface ChannelOnboarding {
  defaultUsername: string
  hasDefaultPassword: boolean
  maxChannels: number
  channelCount: number
}

const { t } = useI18n()
const auth = useAuthStore()
const channelStatus = useChannelStatusStore()
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
const onboarding = ref<ChannelOnboarding | null>(null)
const useSavedCredentials = ref(false)
const formUseSavedCredentials = ref(false)
const atChannelLimit = computed(
  () => onboarding.value !== null && onboarding.value.channelCount >= onboarding.value.maxChannels,
)
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
const detectResult = ref<DetectResult | null>(null)
const saving = ref(false)
// A plain RTSP URL needs nothing else; ONVIF uses host/HTTP port plus the
// profile URIs, and Hikvision/Dahua generate the RTSP path from the host.
const isOnvif = computed(() => modal.protocol === 'onvif')
const isBrandProtocol = computed(
  () => modal.protocol === 'hikvision' || modal.protocol === 'dahua',
)
const isRtsp = computed(() => modal.protocol === 'rtsp')
const isGeneric = computed(() => modal.protocol === 'generic')
// Both plain RTSP and URL-template channels need a URL, but only RTSP can
// hide every camera field; generic templates may still use {host}/{port}.
const isUrlProtocol = computed(() => isRtsp.value || isGeneric.value)
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
  detected: string
  protocol: string
  name: string
  model: string
  brand: string
  vendor?: string
  firmware?: string
  deviceName?: string
  nameSource?: string
  mainUrl: string
  subUrl: string
  subEnabled: boolean
  eventsEnabled: boolean
  httpPort: number
  rtspPort?: number
  streamUris?: string[]
  tried?: DetectAttempt[]
  message?: string
}

function onvifHttpPort(xaddr: string) {
  try {
    return Number(new URL(xaddr).port) || 80
  } catch {
    return 80
  }
}

function applyProbeResult(host: string, httpPort: number, result: OnvifProbeResult) {
  const profile = result.profiles[0]
  const subProfile = result.profiles[1]
  // A device answered ONVIF, so keep the ONVIF transport and use the stream
  // URIs it returned. The manufacturer still goes into `brand` for vendor
  // time/encoder fallbacks, but it must not switch the channel to the
  // Hikvision/Dahua RTSP template.
  Object.assign(modal, {
    groupId: 0,
    name: result.deviceName || result.device.deviceName || result.device.model || host,
    protocol: 'onvif',
    host,
    port: httpPort,
    httpPort,
    mainUrl: profile?.streamUri ?? '',
    subUrl: subProfile?.streamUri ?? '',
    subEnabled: Boolean(subProfile),
    enabled: true,
    brand: result.device.manufacturer,
    eventsEnabled: true,
  })
}

function applyDetectResult(result: DetectResult) {
  Object.assign(modal, {
    protocol: result.protocol || 'onvif',
    name: modal.name.trim() || result.deviceName || result.name || modal.host.trim(),
    brand: result.vendor || result.brand,
    port: result.rtspPort || 554,
    httpPort: result.httpPort || (Number(modal.httpPort) || 80),
    mainUrl: result.mainUrl || (result.protocol === 'rtsp' ? modal.mainUrl : ''),
    subUrl: result.subUrl || (result.protocol === 'rtsp' ? modal.subUrl : ''),
    subEnabled: result.subEnabled,
    eventsEnabled: result.eventsEnabled,
  })
}

function deviceKey(device: OnvifDevice) {
  return device.xaddr || `${device.ip}:${device.httpPort ?? 80}:${device.protocol ?? 'onvif'}`
}

async function detectFromModal() {
  if (!modal.host.trim()) return
  detecting.value = true
  error.value = ''
  detectResult.value = null
  try {
    const result = await api<DetectResult>('/api/v1/channels/detect', {
      method: 'POST',
      body: JSON.stringify({
        host: modal.host.trim(),
        // Zero asks the server to probe 80, 8080 and 8000; a value the
        // operator typed is tried first and then the standard ports.
        port: Number(modal.httpPort) > 0 ? Number(modal.httpPort) : 0,
        username: modal.username,
        password: modal.password,
        protocol: 'auto',
        useDefaultCredentials: formUseSavedCredentials.value,
      }),
    })
    detectResult.value = result
    if (!result.detected) {
      error.value = result.message || t('channels.detectNone')
      return
    }
    applyDetectResult(result)
    flash(t('channels.detected'))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    detecting.value = false
  }
}

async function addDevice(device: OnvifDevice): Promise<boolean> {
  const key = deviceKey(device)
  if (scanStatus[key] === 'added' || scanStatus[key] === 'adding') return false
  if (channels.value.some((channel) => channel.host === device.ip)) {
    scanStatus[key] = 'failed'
    scanErrors[key] = t('channels.importDuplicate')
    return false
  }
  scanStatus[key] = 'adding'
  scan.probing = key
  try {
    const isOnvif = Boolean(device.xaddr) && (!device.protocol || device.protocol === 'onvif')
    const payload: Record<string, unknown> = {
      host: device.ip,
      username: scan.username,
      password: scan.password,
      useDefaultCredentials: useSavedCredentials.value,
      enabled: true,
      eventsEnabled: true,
      watermark: { text: '', position: 'bottom-right' },
    }
    if (isOnvif) {
      const result = await api<OnvifProbeResult>('/api/v1/channels/onvif/probe', {
        method: 'POST',
        body: JSON.stringify({
          xaddr: device.xaddr,
          username: scan.username,
          password: scan.password,
          useDefaultCredentials: useSavedCredentials.value,
        }),
      })
      const profile = result.profiles[0]
      const subProfile = result.profiles[1]
      const httpPort = onvifHttpPort(device.xaddr)
      Object.assign(payload, {
        name: result.deviceName || result.device.deviceName || device.name || result.device.model ||
          device.ip,
        protocol: 'onvif',
        port: httpPort,
        httpPort,
        mainUrl: profile?.streamUri ?? '',
        subUrl: subProfile?.streamUri ?? '',
        subEnabled: Boolean(subProfile),
        brand: result.device.manufacturer,
      })
    } else {
      Object.assign(payload, {
        // Vendor HTTP detection already found the device; the server derives
        // the vendor RTSP URL from host/port, so no stream URL is sent.
        name: device.name || device.model || device.ip,
        protocol: device.protocol || 'onvif',
        port: device.rtspPort || 554,
        httpPort: device.httpPort || 80,
        mainUrl: '',
        subUrl: '',
        subEnabled: true,
        brand: device.brand || '',
      })
    }
    await api('/api/v1/channels', { method: 'POST', body: JSON.stringify(payload) })
    scanStatus[key] = 'added'
    delete scanErrors[key]
    scanSelection.value = new Set([...scanSelection.value].filter((entry) => entry !== key))
    return true
  } catch (caught) {
    scanStatus[key] = 'failed'
    scanErrors[key] = caught instanceof ApiError ? caught.message : t('common.error')
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
  } else if (scanErrors[deviceKey(device)]) {
    scanError.value = scanErrors[deviceKey(device)]
  }
}

function toggleDevice(key: string) {
  const next = new Set(scanSelection.value)
  if (next.has(key)) next.delete(key)
  else next.add(key)
  scanSelection.value = next
}

function allDevicesSelected() {
  return scan.devices.length > 0 && scan.devices.every((device) => scanSelection.value.has(deviceKey(device)))
}

function toggleAllDevices() {
  scanSelection.value = allDevicesSelected()
    ? new Set()
    : new Set(scan.devices.map(deviceKey))
}

async function addSelected() {
  if (scanAdding.value) return
  const devices = scan.devices.filter(
    (device) =>
      scanSelection.value.has(deviceKey(device)) && scanStatus[deviceKey(device)] !== 'added',
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
      .map((device) => {
        const key = deviceKey(device)
        return scanErrors[key] ? `${device.ip}: ${scanErrors[key]}` : ''
      })
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
  // Thumbnails only need a small frame: a 1080p decode costs about a second
  // on the appliance, while 480x270 comes back in roughly 0.2 s.
  return `/api/v1/channels/${channel.id}/snapshot?width=480&height=270&quality=6&t=${thumbVersion[channel.id] ?? 0}`
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
  previewImage.value = `/api/v1/channels/${channel.id}/snapshot?width=1280&height=720&quality=4&t=${version}`
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

// Merge by id so the rows (and their <tr> elements) keep their identity.
// A status tick only mutates the fields it carries; this path is used for the
// initial load, the post-reconnect alignment and the 30 s fallback.
function mergeChannels(rows: Channel[]) {
  const current = new Map(channels.value.map((channel) => [channel.id, channel]))
  const merged = rows.map((row) => {
    const existing = current.get(row.id)
    if (existing) {
      Object.assign(existing, row)
      return existing
    }
    return row
  })
  channels.value = merged
}

function applyLiveStatus() {
  for (const channel of channels.value) {
    const item = channelStatus.items[channel.id]
    if (!item) continue
    channel.mainState = item.mainState
    channel.mainError = item.mainError
    channel.subState = item.subState
    channel.subError = item.subError
    if (item.name !== undefined) channel.name = item.name
    if (item.enabled !== undefined) channel.enabled = item.enabled
    if (item.lastOnlineAt !== undefined) channel.lastOnlineAt = item.lastOnlineAt
    if (item.lastOfflineAt !== undefined) channel.lastOfflineAt = item.lastOfflineAt
  }
}

async function load() {
  error.value = ''
  try {
    const [channelRows, groupRows] = await Promise.all([
      api<Channel[]>('/api/v1/channels'),
      api<ChannelGroup[]>('/api/v1/channel-groups'),
    ])
    mergeChannels(channelRows)
    groups.value = groupRows
    applyLiveStatus()
    try {
      onboarding.value = await api<ChannelOnboarding>('/api/v1/channels/onboarding')
    } catch {
      onboarding.value = null
    }
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

let fallbackTimer: number | undefined
watch(() => channelStatus.seq, applyLiveStatus)
watch(
  () => channelStatus.resync,
  () => {
    if (channelStatus.resync > 0) void load()
  },
)

function openCreate() {
  error.value = ''
  detectResult.value = null
  formUseSavedCredentials.value = false
  if (atChannelLimit.value) {
    error.value = t('channels.limitReached', {
      limit: onboarding.value?.maxChannels ?? 128,
    })
    return
  }
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
  error.value = ''
  detectResult.value = null
  formUseSavedCredentials.value = false
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

function defaultChannelName() {
  if (modal.host.trim()) return modal.host.trim()
  if (modal.mainUrl) {
    try {
      const host = new URL(modal.mainUrl).hostname
      if (host) return host
    } catch {
      /* The backend also derives a name when the URL is unusual. */
    }
  }
  return t('channels.defaultName')
}

async function save() {
  error.value = ''
  if (isUrlProtocol.value && !modal.mainUrl.trim()) {
    error.value = t('channels.rtspUrlRequired')
    return
  }
  if (isOnvif.value && !modal.mainUrl.trim() && !modal.host.trim()) {
    error.value = t('channels.onvifNeedHost')
    return
  }
  if (isBrandProtocol.value && !modal.host.trim()) {
    error.value = t('channels.hostRequired')
    return
  }
  // ONVIF/Hikvision/Dahua channels are named by the server from the device
  // (scope, ISAPI deviceName, getMachineName); plain URLs still get a name
  // derived from the address so the list is never blank.
  if (!modal.name.trim() && isUrlProtocol.value) modal.name = defaultChannelName()
  saving.value = true
  try {
    const payload: Record<string, unknown> = {
      name: modal.name.trim(),
      protocol: modal.protocol,
      host: modal.host.trim(),
      // ONVIF selects profiles over the HTTP service; avoid showing its
      // HTTP port as if it were the RTSP port.
      port: isOnvif.value ? Number(modal.httpPort) || 80 : Number(modal.port) || 554,
      httpPort: Number(modal.httpPort) || 80,
      username: modal.username,
      useDefaultCredentials:
        formUseSavedCredentials.value && (isOnvif.value || isBrandProtocol.value),
      mainUrl: modal.mainUrl.trim(),
      subUrl: modal.subUrl.trim(),
      subEnabled: isRtsp.value ? Boolean(modal.subUrl.trim()) : modal.subEnabled,
      enabled: modal.enabled,
      eventsEnabled: isOnvif.value ? modal.eventsEnabled : false,
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
  } finally {
    saving.value = false
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

async function openScan() {
  error.value = ''
  useSavedCredentials.value = false
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
  if (onboarding.value?.defaultUsername && onboarding.value.hasDefaultPassword) {
    useSavedCredentials.value = true
    scan.username = onboarding.value.defaultUsername
  }
  try {
    const info = await api<ChannelOnboarding>('/api/v1/channels/onboarding')
    onboarding.value = info
    if (info.defaultUsername && info.hasDefaultPassword) {
      useSavedCredentials.value = true
      scan.username = info.defaultUsername
    }
  } catch {
    // No saved credentials: the operator enters them for this scan.
  }
}

async function discover() {
  scan.scanning = true
  error.value = ''
  try {
    const result = await api<{ devices: OnvifDevice[] }>('/api/v1/channels/discover', {
      method: 'POST',
      body: JSON.stringify({
        brandScan: true,
        username: useSavedCredentials.value ? '' : scan.username,
        password: useSavedCredentials.value ? '' : scan.password,
        useDefaultCredentials: useSavedCredentials.value,
      }),
    })
    scan.devices = result.devices
    scanSelection.value = new Set(result.devices.map(deviceKey))
    Object.keys(scanStatus).forEach((key) => delete scanStatus[key])
    Object.keys(scanErrors).forEach((key) => delete scanErrors[key])
    scanError.value = ''
    scanNotice.value = ''
    if (result.devices.length === 0) {
      scanError.value = t('channels.noDevices')
    }
  } catch (caught) {
    scanError.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    scan.scanning = false
  }
}

function selectHikvision() {
  scanSelection.value = new Set(
    scan.devices
      .filter((device) => device.protocol === 'hikvision' && scanStatus[deviceKey(device)] !== 'added')
      .map(deviceKey),
  )
}

async function probe(device: OnvifDevice) {
  const key = deviceKey(device)
  scan.probing = key
  scan.result = null
  scanError.value = ''
  try {
    if (device.xaddr) {
      const result = await api<OnvifProbeResult>('/api/v1/channels/onvif/probe', {
        method: 'POST',
        body: JSON.stringify({
          xaddr: device.xaddr,
          username: scan.username,
          password: scan.password,
          useDefaultCredentials: useSavedCredentials.value,
        }),
      })
      scan.result = result
      if (result.profiles.length) {
        Object.assign(modal, {
          open: true,
          mode: 'create',
          password: scan.password,
          username: scan.username,
          name: result.deviceName || result.device.deviceName || '',
          watermarkText: '',
          watermarkPosition: 'bottom-right',
        })
        applyProbeResult(device.ip, onvifHttpPort(device.xaddr), result)
        scan.open = false
      }
      return
    }
    // Brand-only hit from the LAN probe: ask detect for the hit details so
    // the form shows the same vendor/model/name information.
    const result = await api<DetectResult>('/api/v1/channels/detect', {
      method: 'POST',
      body: JSON.stringify({
        host: device.ip,
        port: device.httpPort ?? 80,
        username: scan.username,
        password: scan.password,
        protocol: device.protocol || 'auto',
        useDefaultCredentials: useSavedCredentials.value,
      }),
    })
    detectResult.value = result
    if (!result.detected) {
      scanError.value = result.message || t('channels.detectNone')
      return
    }
    Object.assign(modal, {
      open: true,
      mode: 'create',
      groupId: 0,
      host: device.ip,
      username: scan.username,
      password: scan.password,
      name: result.deviceName || device.name || '',
      watermarkText: '',
      watermarkPosition: 'bottom-right',
    })
    formUseSavedCredentials.value = useSavedCredentials.value
    applyDetectResult(result)
    scan.open = false
  } catch (caught) {
    scanError.value = caught instanceof ApiError ? caught.message : t('common.error')
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

onMounted(() => {
  void load()
  // Fallback only: while the status socket is healthy the 1 s diff stream
  // keeps the table current and this timer stays quiet.
  fallbackTimer = window.setInterval(() => {
    if (!channelStatus.connected) void load()
  }, 30_000)
})

onBeforeUnmount(() => {
  if (fallbackTimer !== undefined) window.clearInterval(fallbackTimer)
})
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.channels') }}</span>
      <h2>{{ t('channels.title') }}</h2>
      <p>{{ t('channels.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <span v-if="onboarding" class="pill">
        {{ t('channels.channelCount', { count: onboarding.channelCount, limit: onboarding.maxChannels }) }}
      </span>
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
        :disabled="importing || atChannelLimit"
        :title="atChannelLimit ? t('channels.limitReached', { limit: onboarding?.maxChannels ?? 128 }) : t('channels.csvHint')"
        @click="importInput?.click()"
      >
        <AppIcon name="plus" :size="16" />
        {{ importing ? `${importDone}/${importTotal}` : t('channels.importCsv') }}
      </button>
      <button class="button primary" :disabled="atChannelLimit" @click="openCreate">
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
            <input v-model="modal.name" class="input" maxlength="64" :placeholder="t('channels.defaultName')" />
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
          <label v-if="!isGeneric" class="field">
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
          <div v-if="detectResult" class="notice detect-result">
            <template v-if="detectResult.detected">
              <strong>
                {{
                  t('channels.detectHit', {
                    protocol: detectResult.detected,
                    port: detectResult.httpPort || detectResult.rtspPort,
                  })
                }}
              </strong>
              <div class="small-note">
                {{ t('channels.detectVendor') }}: {{ detectResult.vendor || '—' }} ·
                {{ t('channels.detectModel') }}: {{ detectResult.model || '—' }} ·
                {{ t('channels.detectName') }}: {{ detectResult.deviceName || detectResult.name || '—' }}
              </div>
              <div v-if="detectResult.mainUrl" class="mono small-note">{{ detectResult.mainUrl }}</div>
            </template>
            <div v-else>{{ detectResult.message || t('channels.detectNone') }}</div>
            <ul class="detect-attempts small-note">
              <li
                v-for="(attempt, index) in detectResult.tried || []"
                :key="`${attempt.protocol}-${attempt.port}-${index}`"
              >
                <span :class="attempt.ok ? 'ok' : 'danger'">{{ attempt.ok ? '✓' : '✕' }}</span>
                {{ attempt.protocol }} :{{ attempt.port }}
                <span v-if="!attempt.ok && attempt.error"> — {{ attempt.error }}</span>
              </li>
            </ul>
          </div>
          <label
            v-if="(isOnvif || isBrandProtocol) && onboarding?.defaultUsername && onboarding?.hasDefaultPassword"
            class="check-row"
          >
            <input v-model="formUseSavedCredentials" type="checkbox" />
            <span>{{ t('channels.useSavedCredentials') }}</span>
          </label>
          <label v-if="!isRtsp" class="field">
            <span>{{ t('channels.host') }}</span>
            <input v-model="modal.host" class="input" placeholder="192.168.1.64" />
          </label>
          <label v-if="isBrandProtocol || isGeneric" class="field">
            <span>{{ t('channels.port') }}</span>
            <input v-model.number="modal.port" class="input" type="number" min="1" max="65535" />
          </label>
          <label v-if="isOnvif || isBrandProtocol" class="field">
            <span>{{ isOnvif ? t('channels.onvifPort') : t('channels.httpPort') }}</span>
            <input v-model.number="modal.httpPort" class="input" type="number" min="1" max="65535" />
          </label>
          <label v-if="!isRtsp" class="field">
            <span>{{ t('channels.username') }}</span>
            <input
              v-model="modal.username"
              class="input"
              autocomplete="off"
              :disabled="formUseSavedCredentials"
            />
          </label>
          <label v-if="!isRtsp" class="field">
            <span>{{ t('channels.password') }}</span>
            <input
              v-model="modal.password"
              class="input"
              type="password"
              autocomplete="new-password"
              :disabled="formUseSavedCredentials"
              :placeholder="modal.mode === 'edit' ? t('channels.keepPassword') : ''"
            />
          </label>
        </div>

        <p v-if="isRtsp" class="small-note">{{ t('channels.rtspHint') }}</p>
        <label v-if="!isBrandProtocol" class="field">
          <span>{{ t('channels.mainUrl') }}</span>
          <input v-model="modal.mainUrl" class="input" placeholder="rtsp://user:pass@host:554/..." />
        </label>
        <label v-if="!isBrandProtocol" class="field">
          <span>{{ t('channels.subUrl') }}</span>
          <input v-model="modal.subUrl" class="input" placeholder="rtsp://..." />
        </label>
        <p v-if="isOnvif" class="small-note">{{ t('channels.onvifUrlHint') }}</p>

        <div class="modal-columns">
          <label v-if="!isRtsp" class="check-row">
            <input v-model="modal.subEnabled" type="checkbox" />
            <span>{{ t('channels.subEnabled') }}</span>
          </label>
          <label class="check-row">
            <input v-model="modal.enabled" type="checkbox" />
            <span>{{ t('common.enabled') }}</span>
          </label>
          <label v-if="isOnvif" class="check-row">
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

        <p v-if="isBrandProtocol" class="small-note">{{ t('channels.hikvisionHint') }}</p>
        <p v-if="isOnvif" class="small-note">{{ t('channels.eventsHint') }}</p>
        <p v-if="error" class="form-error">{{ error }}</p>

        <div class="modal-actions">
          <button type="button" class="button" @click="modal.open = false">
            {{ t('common.cancel') }}
          </button>
          <button type="submit" class="button primary" :disabled="saving">
            <AppIcon name="save" :size="16" />
            {{ saving ? t('common.loading') : t('common.save') }}
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
        <p v-if="scanNotice" class="notice">{{ scanNotice }}</p>
        <p v-if="scanError" class="form-error">{{ scanError }}</p>
        <template v-if="onboarding?.defaultUsername && onboarding?.hasDefaultPassword">
          <label class="check-row">
            <input v-model="useSavedCredentials" type="checkbox" />
            <span>{{ t('channels.useSavedCredentials') }}</span>
          </label>
          <p class="small-note">
            {{ t('channels.savedCredentialsHint', { username: onboarding.defaultUsername }) }}
          </p>
        </template>
        <p v-else class="small-note">{{ t('channels.noSavedCredentials') }}</p>
        <div class="modal-columns">
          <label class="field">
            <span>{{ t('channels.username') }}</span>
            <input
              v-model="scan.username"
              class="input"
              autocomplete="off"
              :disabled="useSavedCredentials"
            />
          </label>
          <label class="field">
            <span>{{ t('channels.password') }}</span>
            <input
              v-model="scan.password"
              class="input"
              type="password"
              autocomplete="new-password"
              :disabled="useSavedCredentials"
              :placeholder="useSavedCredentials ? t('channels.savedPasswordPlaceholder') : ''"
            />
          </label>
        </div>
        <div class="head-actions">
          <button class="button primary" :disabled="scan.scanning" @click="discover">
            <AppIcon name="search" :size="16" />
            {{ scan.scanning ? t('channels.scanning') : t('channels.startScan') }}
          </button>
          <button
            v-if="scan.devices.some((device) => device.protocol === 'hikvision')"
            class="button"
            :disabled="scanAdding || atChannelLimit"
            @click="selectHikvision"
          >
            {{ t('channels.selectHikvision') }}
          </button>
          <button
            v-if="scan.devices.length > 0"
            class="button primary"
            :disabled="scanAdding || scanSelection.size === 0 || atChannelLimit"
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
        <p v-if="atChannelLimit" class="form-error">
          {{ t('channels.limitReached', { limit: onboarding?.maxChannels ?? 128 }) }}
        </p>
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
              <th>{{ t('channels.protocol') }}</th>
              <th>{{ t('channels.port') }}</th>
              <th>{{ t('channels.name') }}</th>
              <th>{{ t('common.actions') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="device in scan.devices" :key="deviceKey(device)">
              <td class="check-cell">
                <input
                  type="checkbox"
                  :checked="scanSelection.has(deviceKey(device))"
                  :disabled="scanStatus[deviceKey(device)] === 'added'"
                  @change="toggleDevice(deviceKey(device))"
                />
              </td>
              <td>{{ device.ip }}</td>
              <td>
                <span class="pill">{{ device.protocol || 'onvif' }}</span>
                <small v-if="device.brand" class="muted"> {{ device.brand }}</small>
              </td>
              <td class="mono">{{ device.httpPort ?? 80 }}</td>
              <td>
                {{ device.name || device.model || device.ip }}
                <small v-if="device.xaddr && (device.protocol === 'onvif' || !device.protocol)" class="mono block muted">
                  {{ device.xaddr }}
                </small>
              </td>
              <td>
                <div class="scan-actions">
                  <button class="button small" :disabled="scan.probing === deviceKey(device) || scanStatus[deviceKey(device)] === 'added'" @click="probe(device)">
                    {{ scan.probing === deviceKey(device) ? t('common.loading') : t('channels.probe') }}
                  </button>
                  <button
                    class="button small primary"
                    :disabled="scan.probing === deviceKey(device) || scanAdding || scanStatus[deviceKey(device)] === 'added' || atChannelLimit"
                    @click="quickAdd(device)"
                  >
                    {{
                      scanStatus[deviceKey(device)] === 'added'
                        ? t('channels.added')
                        : scan.probing === deviceKey(device)
                          ? t('channels.adding')
                          : t('channels.addNow')
                    }}
                  </button>
                </div>
                <small v-if="scanErrors[deviceKey(device)]" class="block error-text">{{ scanErrors[deviceKey(device)] }}</small>
              </td>
            </tr>
            <tr v-if="scan.devices.length === 0">
              <td colspan="6" class="empty-cell">{{ t('channels.noDevicesYet') }}</td>
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
