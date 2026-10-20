<script setup lang="ts">
import { computed } from 'vue'

const props = withDefaults(
  defineProps<{
    label: string
    percent: number
    detail?: string
    tone?: 'ok' | 'warn' | 'danger'
  }>(),
  { detail: '', tone: 'ok' },
)

const clamped = computed(() => Math.min(100, Math.max(0, props.percent)))
const tone = computed(() => {
  if (props.tone) return props.tone
  if (clamped.value >= 90) return 'danger'
  if (clamped.value >= 70) return 'warn'
  return 'ok'
})
</script>

<template>
  <div class="usage-meter">
    <div class="usage-meter-head">
      <span>{{ label }}</span>
      <strong>{{ Math.round(clamped) }}%</strong>
    </div>
    <div
      class="usage-track"
      role="progressbar"
      :aria-label="label"
      :aria-valuenow="Math.round(clamped)"
      aria-valuemin="0"
      aria-valuemax="100"
    >
      <span class="usage-fill" :class="tone" :style="{ width: `${clamped}%` }"></span>
    </div>
    <span v-if="detail" class="usage-detail">{{ detail }}</span>
  </div>
</template>
