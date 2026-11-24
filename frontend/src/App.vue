<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useNow } from '@vueuse/core'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api/client'
import { useAppStore } from '@/stores/app'
import { useAuthStore } from '@/stores/auth'
import { useChannelStatusStore } from '@/stores/channelStatus'
import { useEventStore } from '@/stores/events'
import { useTabsStore } from '@/stores/tabs'
import { useTheme } from '@/stores/theme'

const app = useAppStore()
const auth = useAuthStore()
const events = useEventStore()
const channelStatus = useChannelStatusStore()
const tabs = useTabsStore()
const route = useRoute()
const router = useRouter()
const { t, locale } = useI18n()
const now = useNow({ interval: 1000 })

const isPublic = computed(() => Boolean(route.meta.public))

const primaryNav = [
  { key: 'dashboard', to: '/', icon: 'dashboard', permission: 'dashboard.view' },
  { key: 'preview', to: '/preview', icon: 'camera', permission: 'preview.view' },
  { key: 'channels', to: '/channels', icon: 'camera', permission: 'channel.view' },
  { key: 'playback', to: '/playback', icon: 'playback', permission: 'playback.view' },
  { key: 'plans', to: '/plans', icon: 'dashboard', permission: 'plan.view' },
  { key: 'storage', to: '/storage', icon: 'storage', permission: 'storage.view' },
  { key: 'alerts', to: '/alerts', icon: 'bell', permission: 'alert.view' },
]

const adminNav = [
  { key: 'users', to: '/users', icon: 'users', permission: 'user.view' },
  { key: 'logs', to: '/audit', icon: 'logs', permission: 'log.view' },
  { key: 'settings', to: '/system', icon: 'settings', permission: 'system.view' },
]

const aboutNav = { key: 'about', to: '/about', icon: 'help' }

const localeOptions = [
  { value: 'zh-CN', label: '中文' },
  { value: 'en-US', label: 'EN' },
] as const

const themeOptions = ['auto', 'light', 'dark'] as const
const { mode: themeMode, apply: applyTheme } = useTheme()

function switchLocale(value: 'zh-CN' | 'en-US') {
  locale.value = value
  localStorage.setItem('uvnvr.locale', value)
  document.documentElement.lang = value
}

function canSee(permission?: string) {
  return !permission || auth.hasPermission(permission)
}

const pageTitle = computed(() => {
  const key = route.meta.titleKey as string | undefined
  return key ? t(key) : t('nav.dashboard')
})

const timeText = computed(() =>
  new Intl.DateTimeFormat(locale.value, {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).format(now.value),
)

const dateText = computed(() =>
  new Intl.DateTimeFormat(locale.value, {
    year: 'numeric',
    month: 'short',
    day: '2-digit',
    weekday: 'short',
  }).format(now.value),
)

const statusText = computed(() => {
  if (app.server) return t('app.online')
  if (app.serverError) return t('app.offline')
  return '…'
})

function isActive(to: string) {
  return route.path === to
}

function openTab(path: string) {
  if (route.path !== path) void router.push(path)
}

function closeTab(path: string) {
  const next = tabs.close(path)
  if (route.path === path) void router.push(next)
}

async function logout() {
  disconnectEvents()
  disconnectStatus()
  statusConnectedBefore = false
  await auth.logout()
  tabs.$reset()
  await router.push('/login')
}

const toast = ref<{ message: string } | null>(null)
let toastTimer: number | undefined
let socket: WebSocket | null = null
let reconnectTimer: number | undefined
let reconnectDelay = 1000

function showToast(message: string) {
  toast.value = { message }
  if (toastTimer !== undefined) window.clearTimeout(toastTimer)
  toastTimer = window.setTimeout(() => {
    toast.value = null
  }, 6000)
}

function scheduleReconnect() {
  if (reconnectTimer !== undefined || !auth.user) return
  reconnectTimer = window.setTimeout(() => {
    reconnectTimer = undefined
    void connectEvents()
  }, reconnectDelay)
  reconnectDelay = Math.min(30000, reconnectDelay * 2)
}

async function connectEvents() {
  if (!auth.user || socket) return
  try {
    const ticket = await api<{ wsPath: string }>('/api/v1/events/ticket', { method: 'POST' })
    if (!auth.user) return
    const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws'
    const current = new WebSocket(`${scheme}://${window.location.host}${ticket.wsPath}`)
    socket = current
    current.onopen = () => {
      events.setConnected(true)
      reconnectDelay = 1000
    }
    current.onmessage = (message) => {
      try {
        const payload = JSON.parse(String(message.data)) as {
          type?: string
          alert?: { message?: string }
        }
        events.apply(payload)
        if (payload.type === 'alert' && payload.alert?.message) showToast(payload.alert.message)
      } catch {
        /* Ignore malformed events. */
      }
    }
    current.onclose = () => {
      events.setConnected(false)
      if (socket === current) socket = null
      scheduleReconnect()
    }
    current.onerror = () => current.close()
  } catch {
    scheduleReconnect()
  }
}

function disconnectEvents() {
  if (reconnectTimer !== undefined) {
    window.clearTimeout(reconnectTimer)
    reconnectTimer = undefined
  }
  if (socket) {
    const current = socket
    socket = null
    current.close()
  }
  events.setConnected(false)
}

// Channel-state socket. It is separate from /ws/events: viewers without
// alert.view (preview and playback) still need online/offline changes.
let statusSocket: WebSocket | null = null
let statusReconnectTimer: number | undefined
let statusReconnectDelay = 1000
let statusConnectedBefore = false

function scheduleStatusReconnect() {
  if (statusReconnectTimer !== undefined || !auth.user) return
  statusReconnectTimer = window.setTimeout(() => {
    statusReconnectTimer = undefined
    void connectStatus()
  }, statusReconnectDelay)
  statusReconnectDelay = Math.min(30000, statusReconnectDelay * 2)
}

async function alignStatusAfterReconnect() {
  // The server streams changes only, so a reconnect needs one full snapshot
  // before the diff stream can be trusted again.
  try {
    const rows = await api<Array<Record<string, unknown>>>('/api/v1/channels')
    if (!auth.user) return
    channelStatus.replaceFromChannels(rows)
  } catch {
    // A preview-only account cannot read /channels; the page's own fallback
    // refresh still aligns it.
    channelStatus.bumpResync()
  }
}

async function connectStatus() {
  if (!auth.user || statusSocket) return
  try {
    const ticket = await api<{ wsPath: string }>('/api/v1/status/ticket', { method: 'POST' })
    if (!auth.user) return
    const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws'
    const current = new WebSocket(`${scheme}://${window.location.host}${ticket.wsPath}`)
    statusSocket = current
    current.onopen = () => {
      channelStatus.setConnected(true)
      statusReconnectDelay = 1000
      if (statusConnectedBefore) void alignStatusAfterReconnect()
      statusConnectedBefore = true
    }
    current.onmessage = (message) => {
      try {
        channelStatus.apply(JSON.parse(String(message.data)))
      } catch {
        /* Ignore malformed status frames. */
      }
    }
    current.onclose = () => {
      channelStatus.setConnected(false)
      if (statusSocket === current) statusSocket = null
      scheduleStatusReconnect()
    }
    current.onerror = () => current.close()
  } catch {
    scheduleStatusReconnect()
  }
}

function disconnectStatus() {
  if (statusReconnectTimer !== undefined) {
    window.clearTimeout(statusReconnectTimer)
    statusReconnectTimer = undefined
  }
  if (statusSocket) {
    const current = statusSocket
    statusSocket = null
    current.close()
  }
  channelStatus.setConnected(false)
}

watch(
  () => auth.user,
  (user) => {
    if (user) {
      void connectEvents()
      void connectStatus()
    } else {
      statusConnectedBefore = false
      disconnectEvents()
      disconnectStatus()
    }
  },
  { immediate: true },
)

onMounted(() => {
  document.documentElement.lang = locale.value
  void app.fetchServerInfo()
  setInterval(() => void app.fetchServerInfo(), 30_000)
})

onBeforeUnmount(() => {
  disconnectEvents()
  disconnectStatus()
  if (toastTimer !== undefined) window.clearTimeout(toastTimer)
})
</script>

<template>
  <div v-if="isPublic" class="public-shell">
    <RouterView />
  </div>

  <div v-else class="app-shell" :class="{ 'sidebar-collapsed': app.sidebarCollapsed }">
    <RouterLink v-if="toast" to="/alerts" class="event-toast" @click="toast = null">
      <AppIcon name="bell" :size="18" />
      <div>
        <strong>{{ t('events.newAlert') }}</strong>
        <span>{{ toast.message }}</span>
      </div>
    </RouterLink>
    <aside class="sidebar">
      <div class="brand">
        <img src="/icon-512.png" alt="" class="brand-mark" />
        <div class="brand-copy">
          <strong>{{ t('app.name') }}</strong>
          <span>{{ t('app.tagline') }}</span>
        </div>
      </div>

      <nav class="nav">
        <RouterLink
          v-for="item in primaryNav.filter((entry) => canSee(entry.permission))"
          :key="item.key"
          :to="item.to"
          class="nav-item"
          :class="{ active: isActive(item.to) }"
        >
          <AppIcon :name="item.icon" />
          <span class="nav-label">{{ t(`nav.${item.key}`) }}</span>
        </RouterLink>


        <div v-if="adminNav.some((entry) => canSee(entry.permission))" class="nav-section">
          <span class="nav-section-label">{{ t('nav.administration') }}</span>
        </div>

        <RouterLink
          v-for="item in adminNav.filter((entry) => canSee(entry.permission))"
          :key="item.key"
          :to="item.to"
          class="nav-item"
          :class="{ active: isActive(item.to) }"
        >
          <AppIcon :name="item.icon" />
          <span class="nav-label">{{ t(`nav.${item.key}`) }}</span>
        </RouterLink>

        <div class="nav-section nav-section-bottom"></div>
        <RouterLink
          :to="aboutNav.to"
          class="nav-item"
          :class="{ active: isActive(aboutNav.to) }"
        >
          <AppIcon :name="aboutNav.icon" />
          <span class="nav-label">{{ t(`nav.${aboutNav.key}`) }}</span>
        </RouterLink>
      </nav>

      <div class="sidebar-footer">
        <span class="status-dot" :class="{ online: app.server, offline: app.serverError }"></span>
        <span class="sidebar-footer-text">{{ statusText }}</span>
        <button class="icon-button sidebar-toggle" :title="t('app.name')" @click="app.toggleSidebar">
          <AppIcon name="menu" :size="18" />
        </button>
      </div>
    </aside>

    <div class="main">
      <header class="topbar">
        <button class="icon-button" :aria-label="t('nav.dashboard')" @click="app.toggleSidebar">
          <AppIcon name="menu" />
        </button>
        <div class="topbar-title">
          <span class="eyebrow">{{ t('app.subtitle') }}</span>
          <h1>{{ pageTitle }}</h1>
        </div>
        <div class="topbar-spacer"></div>

        <div class="clock">
          <span class="clock-time">{{ timeText }}</span>
          <span class="clock-date">{{ dateText }}</span>
        </div>

        <div class="locale-switch" role="group" :aria-label="t('common.language')">
          <button
            v-for="option in localeOptions"
            :key="option.value"
            class="locale-button"
            :class="{ active: locale === option.value }"
            @click="switchLocale(option.value)"
          >
            {{ option.label }}
          </button>
        </div>

        <div class="locale-switch theme-switch" role="group" :aria-label="t('common.theme')">
          <button
            v-for="option in themeOptions"
            :key="option"
            class="locale-button"
            :class="{ active: themeMode === option }"
            :title="t(`common.theme${option[0].toUpperCase()}${option.slice(1)}`)"
            @click="applyTheme(option)"
          >
            {{ t(`common.theme${option[0].toUpperCase()}${option.slice(1)}`) }}
          </button>
        </div>

        <div v-if="auth.user" class="user-chip">
          <span class="user-avatar">{{ auth.user.username.slice(0, 1).toUpperCase() }}</span>
          <span class="user-meta">
            <strong>{{ auth.user.username }}</strong>
            <small>{{ auth.user.role }}</small>
          </span>
          <button class="icon-button" :title="t('common.logout')" @click="logout">
            <AppIcon name="logout" :size="18" />
          </button>
        </div>

        <a
          class="icon-button"
          href="https://github.com/Antruly/uvnvr"
          target="_blank"
          rel="noreferrer"
          aria-label="GitHub"
        >
          <AppIcon name="github" />
        </a>
      </header>

      <div class="tabbar">
        <button
          v-for="tab in tabs.tabs"
          :key="tab.path"
          class="tab"
          :class="{ active: route.path === tab.path }"
          @click="openTab(tab.path)"
        >
          <span class="tab-label">{{ t(tab.titleKey) }}</span>
          <span
            v-if="tab.closable"
            class="tab-close"
            role="button"
            :aria-label="t('common.close')"
            @click.stop="closeTab(tab.path)"
          >
            ×
          </span>
        </button>
      </div>

      <main class="content">
        <RouterView v-slot="{ Component, route: current }">
          <KeepAlive :include="tabs.cachedNames">
            <component :is="Component" :key="String(current.name)" />
          </KeepAlive>
        </RouterView>
      </main>
    </div>
  </div>
</template>
