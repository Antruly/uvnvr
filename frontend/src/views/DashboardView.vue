<script setup lang="ts">
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { useAppStore } from '@/stores/app'

const { t } = useI18n()
const app = useAppStore()

const runtimeStatus = computed(() => {
  if (app.server) return `${app.server.name} ${app.server.version} · ${app.server.commit}`
  if (app.serverError) return t('app.offline')
  return '…'
})

const cards = computed(() => [
  { icon: 'settings', title: t('dashboard.runtime'), body: t('dashboard.runtimeBody') },
  { icon: 'camera', title: t('dashboard.scope'), body: t('dashboard.scopeBody') },
  { icon: 'storage', title: t('dashboard.storage'), body: t('dashboard.storageBody') },
  { icon: 'info', title: t('dashboard.license'), body: t('dashboard.licenseBody') },
])
</script>

<template>
  <section class="hero panel">
    <div class="hero-main">
      <span class="pill accent">{{ t('app.status') }}</span>
      <h2>{{ t('app.name') }} <span class="hero-version">0.1.0</span></h2>
      <p class="hero-tagline">{{ t('app.tagline') }} — {{ t('app.subtitle') }}</p>
      <p class="hero-intro">{{ t('dashboard.intro') }}</p>
      <div class="hero-actions">
        <a
          class="button primary"
          href="https://github.com/Antruly/uvnvr"
          target="_blank"
          rel="noreferrer"
        >
          <AppIcon name="github" :size="18" />
          {{ t('dashboard.openGithub') }}
        </a>
        <a
          class="button"
          href="https://github.com/Antruly/uvnvr/blob/main/docs/development-plan.md"
          target="_blank"
          rel="noreferrer"
        >
          <AppIcon name="arrow" :size="18" />
          {{ t('dashboard.readPlan') }}
        </a>
      </div>
    </div>
    <div class="hero-side">
      <div class="runtime-card">
        <span class="runtime-label">{{ t('dashboard.runtime') }}</span>
        <span class="runtime-value">{{ runtimeStatus }}</span>
        <span class="runtime-state" :class="{ online: app.server, offline: app.serverError }">
          <span class="status-dot" :class="{ online: app.server, offline: app.serverError }"></span>
          {{ app.server ? t('app.online') : app.serverError ? t('app.offline') : '…' }}
        </span>
      </div>
      <div class="hero-mark">
        <img src="/icon-512.png" alt="UvNVR" />
      </div>
    </div>
  </section>

  <section class="card-grid">
    <article v-for="card in cards" :key="card.title" class="panel feature-card">
      <span class="feature-icon"><AppIcon :name="card.icon" :size="22" /></span>
      <h3>{{ card.title }}</h3>
      <p>{{ card.body }}</p>
    </article>
  </section>

  <section class="two-column">
    <article class="panel milestone-card">
      <div class="section-heading">
        <span class="pill">{{ t('dashboard.m0') }}</span>
        <h3>{{ t('dashboard.m0Body') }}</h3>
      </div>
      <ul class="check-list">
        <li><AppIcon name="check" :size="16" /> GitHub repository, MIT license, contribution & security rules</li>
        <li><AppIcon name="check" :size="16" /> libuvcpp 1.8.1 service with /healthz and version metadata</li>
        <li><AppIcon name="check" :size="16" /> Vue 3 + TypeScript shell with zh-CN / en-US</li>
        <li><AppIcon name="check" :size="16" /> Unified brand icon for web, Windows and macOS</li>
        <li><AppIcon name="check" :size="16" /> Linux CI smoke build</li>
      </ul>
    </article>

    <article class="panel next-card">
      <div class="section-heading">
        <span class="pill accent">{{ t('dashboard.next') }}</span>
        <h3>{{ t('dashboard.nextBody') }}</h3>
      </div>
      <RouterLink class="inline-link" to="/preview">
        {{ t('nav.preview') }}
        <AppIcon name="arrow" :size="16" />
      </RouterLink>
    </article>
  </section>
</template>
