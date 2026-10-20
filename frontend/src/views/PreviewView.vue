<script setup lang="ts">

defineOptions({ name: 'PreviewView' })

import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'

const { t } = useI18n()

const features = [
  t('preview.f1'),
  t('preview.f2'),
  t('preview.f3'),
  t('preview.f4'),
]
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill">{{ t('app.status') }}</span>
      <h2>{{ t('preview.title') }}</h2>
      <p>{{ t('preview.subtitle') }}</p>
    </div>
    <RouterLink class="button" to="/">
      <AppIcon name="arrow" :size="16" />
      {{ t('common.back') }}
    </RouterLink>
  </section>

  <section class="preview-layout">
    <article class="panel preview-stage">
      <div class="preview-grid">
        <div v-for="index in 4" :key="index" class="preview-cell">
          <span class="preview-cell-mark">
            <AppIcon name="camera" :size="28" />
          </span>
          <strong>{{ t('preview.placeholder') }} {{ index }}</strong>
          <span>{{ t('preview.placeholderBody') }}</span>
        </div>
      </div>
    </article>

    <aside class="panel preview-side">
      <h3>{{ t('preview.features') }}</h3>
      <ul class="feature-list">
        <li v-for="feature in features" :key="feature">
          <AppIcon name="check" :size="16" />
          <span>{{ feature }}</span>
        </li>
      </ul>
    </aside>
  </section>
</template>
