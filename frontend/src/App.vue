<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { RouterLink, RouterView, useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useNow } from '@vueuse/core'
import AppIcon from '@/components/AppIcon.vue'
import { useAppStore } from '@/stores/app'

const app = useAppStore()
const route = useRoute()
const { t, locale } = useI18n()
const now = useNow({ interval: 1000 })

const primaryNav = [
  { key: 'dashboard', to: '/', icon: 'dashboard' },
  { key: 'preview', to: '/preview', icon: 'camera' },
]

const plannedNav = [
  { key: 'channels', icon: 'camera' },
  { key: 'playback', icon: 'playback' },
  { key: 'storage', icon: 'storage' },
  { key: 'alerts', icon: 'bell' },
  { key: 'users', icon: 'users' },
  { key: 'logs', icon: 'logs' },
  { key: 'settings', icon: 'settings' },
]

const aboutNav = { key: 'about', to: '/about', icon: 'help' }

const localeOptions = [
  { value: 'zh-CN', label: '中文' },
  { value: 'en-US', label: 'EN' },
] as const

function switchLocale(value: 'zh-CN' | 'en-US') {
  locale.value = value
  localStorage.setItem('uvnvr.locale', value)
  document.documentElement.lang = value
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

onMounted(() => {
  document.documentElement.lang = locale.value
  void app.fetchServerInfo()
  setInterval(() => void app.fetchServerInfo(), 30_000)
})
</script>

<template>
  <div class="app-shell" :class="{ 'sidebar-collapsed': app.sidebarCollapsed }">
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
          v-for="item in primaryNav"
          :key="item.key"
          :to="item.to"
          class="nav-item"
          :class="{ active: isActive(item.to) }"
        >
          <AppIcon :name="item.icon" />
          <span class="nav-label">{{ t(`nav.${item.key}`) }}</span>
        </RouterLink>

        <div class="nav-section">
          <span class="nav-section-label">{{ t('app.status') }}</span>
        </div>

        <div v-for="item in plannedNav" :key="item.key" class="nav-item disabled">
          <AppIcon :name="item.icon" />
          <span class="nav-label">{{ t(`nav.${item.key}`) }}</span>
          <span class="nav-soon">{{ t('nav.soon') }}</span>
        </div>

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

      <main class="content">
        <RouterView v-slot="{ Component }">
          <Transition name="page" mode="out-in">
            <component :is="Component" />
          </Transition>
        </RouterView>
      </main>
    </div>
  </div>
</template>
