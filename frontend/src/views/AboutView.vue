<script setup lang="ts">
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { useAppStore } from '@/stores/app'

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
    <RouterLink class="button" to="/">
      <AppIcon name="arrow" :size="16" />
      {{ t('common.back') }}
    </RouterLink>
  </section>

  <section class="about-grid">
    <article class="panel about-card">
      <h3>{{ t('about.version') }}</h3>
      <dl class="meta-list">
        <div>
          <dt>{{ t('common.version') }}</dt>
          <dd>{{ app.server?.version ?? '0.1.0' }}</dd>
        </div>
        <div>
          <dt>{{ t('common.commit') }}</dt>
          <dd>{{ app.server?.commit ?? 'unknown' }}</dd>
        </div>
        <div>
          <dt>{{ t('common.source') }}</dt>
          <dd>
            <a href="https://github.com/Antruly/uvnvr" target="_blank" rel="noreferrer">
              github.com/Antruly/uvnvr
            </a>
          </dd>
        </div>
      </dl>
    </article>

    <article class="panel about-card">
      <h3>{{ t('about.licenseTitle') }}</h3>
      <p>{{ t('about.licenseBody') }}</p>
    </article>

    <article class="panel about-card">
      <h3>{{ t('about.ffmpegTitle') }}</h3>
      <p>{{ t('about.ffmpegBody') }}</p>
    </article>

    <article class="panel about-card danger-card">
      <h3>{{ t('about.patentTitle') }}</h3>
      <p>{{ t('about.patentBody') }}</p>
    </article>

    <article class="panel about-card about-card-wide">
      <h3>{{ t('about.m0Title') }}</h3>
      <p>{{ t('about.m0Body') }}</p>
    </article>
  </section>
</template>
