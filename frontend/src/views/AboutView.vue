<script setup lang="ts">
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { useAppStore } from '@/stores/app'

defineOptions({ name: 'AboutView' })

const { t } = useI18n()
const app = useAppStore()
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.about') }}</span>
      <h2>{{ t('about.title') }}</h2>
      <p>{{ t('about.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <RouterLink class="button" to="/help">
        <AppIcon name="help" :size="16" />
        {{ t('help.openManual') }}
      </RouterLink>
      <RouterLink class="button" to="/">
        <AppIcon name="arrow" :size="16" />
        {{ t('common.back') }}
      </RouterLink>
    </div>
  </section>

  <section class="about-grid">
    <article class="panel about-card about-card-wide">
      <div class="section-title">
        <AppIcon name="info" :size="18" />
        <h3>{{ t('about.projectTitle') }}</h3>
      </div>
      <p>{{ t('about.projectBody') }}</p>
      <div class="head-actions about-links">
        <a class="button" href="https://github.com/Antruly/uvnvr" target="_blank" rel="noreferrer">
          <AppIcon name="github" :size="16" />
          {{ t('about.openRepo') }}
        </a>
        <a class="button" href="https://github.com/Antruly/uvnvr/issues" target="_blank" rel="noreferrer">
          <AppIcon name="help" :size="16" />
          {{ t('about.openIssues') }}
        </a>
        <a class="button" href="https://github.com/Antruly/uvnvr/tree/main/docs" target="_blank" rel="noreferrer">
          <AppIcon name="logs" :size="16" />
          {{ t('about.openDocs') }}
        </a>
      </div>
    </article>

    <article class="panel about-card">
      <div class="section-title">
        <AppIcon name="settings" :size="18" />
        <h3>{{ t('about.buildTitle') }}</h3>
      </div>
      <dl class="meta-list">
        <div>
          <dt>{{ t('common.version') }}</dt>
          <dd>{{ app.server?.version ?? '0.1.0' }}</dd>
        </div>
        <div>
          <dt>{{ t('common.commit') }}</dt>
          <dd class="mono">{{ app.server?.commit ?? 'unknown' }}</dd>
        </div>
        <div>
          <dt>{{ t('about.repository') }}</dt>
          <dd>
            <a href="https://github.com/Antruly/uvnvr" target="_blank" rel="noreferrer">
              github.com/Antruly/uvnvr
            </a>
          </dd>
        </div>
        <div>
          <dt>{{ t('about.issues') }}</dt>
          <dd>
            <a href="https://github.com/Antruly/uvnvr/issues" target="_blank" rel="noreferrer">
              github.com/Antruly/uvnvr/issues
            </a>
          </dd>
        </div>
      </dl>
      <p class="small-note">{{ t('about.buildBody') }}</p>
    </article>

    <article class="panel about-card">
      <div class="section-title">
        <AppIcon name="shield" :size="18" />
        <h3>{{ t('about.licenseTitle') }}</h3>
      </div>
      <p>{{ t('about.licenseBody') }}</p>
      <p>{{ t('about.ffmpegBody') }}</p>
    </article>

    <article class="panel about-card about-card-wide danger-card">
      <div class="section-title">
        <AppIcon name="info" :size="18" />
        <h3>{{ t('about.riskTitle') }}</h3>
      </div>
      <p>{{ t('about.riskBody') }}</p>
      <p>{{ t('about.patentBody') }}</p>
    </article>
  </section>
</template>
