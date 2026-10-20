<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'SetupView' })

const auth = useAuthStore()
const router = useRouter()
const { t, locale } = useI18n()

const form = reactive({
  password: '',
  confirmPassword: '',
  timezone: Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC',
})
const error = ref('')
const submitting = ref(false)
const showPassword = ref(false)

const zones = [
  'UTC',
  'Asia/Shanghai',
  'Asia/Tokyo',
  'Asia/Singapore',
  'Asia/Kolkata',
  'Europe/London',
  'Europe/Berlin',
  'America/New_York',
  'America/Chicago',
  'America/Los_Angeles',
  'Australia/Sydney',
]

const passwordStrength = computed(() => {
  const value = form.password
  let classes = 0
  if (/[a-z]/.test(value)) classes += 1
  if (/[A-Z]/.test(value)) classes += 1
  if (/\d/.test(value)) classes += 1
  if (/[^A-Za-z0-9]/.test(value)) classes += 1
  return { classes, length: value.length }
})

function switchLocale(value: 'zh-CN' | 'en-US') {
  locale.value = value
  localStorage.setItem('uvnvr.locale', value)
  document.documentElement.lang = value
}

async function submit() {
  error.value = ''
  if (form.password !== form.confirmPassword) {
    error.value = t('auth.passwordMismatch')
    return
  }
  if (form.password.length < 10 || passwordStrength.value.classes < 3) {
    error.value = t('auth.passwordHint')
    return
  }
  submitting.value = true
  try {
    await auth.setup(form.password, form.confirmPassword, form.timezone)
    await router.push('/')
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    submitting.value = false
  }
}

onMounted(async () => {
  try {
    await auth.ensureInitialized()
    if (!auth.setupRequired) await router.replace('/login')
  } catch {
    /* The guard will route to login if the service is unreachable. */
  }
})
</script>

<template>
  <div class="auth-shell">
    <section class="auth-brand">
      <img src="/icon-512.png" alt="UvNVR" class="auth-logo" />
      <span class="pill accent">{{ t('auth.firstRun') }}</span>
      <h1>{{ t('app.name') }}</h1>
      <p class="auth-tagline">{{ t('app.tagline') }} — {{ t('app.subtitle') }}</p>
      <p class="auth-copy">{{ t('auth.firstRunBody') }}</p>
      <div class="auth-meta">
        <span>{{ t('auth.setupSerial') }}</span>
        <strong>{{ auth.serial || '—' }}</strong>
        <span>{{ t('common.version') }} {{ auth.version || '0.1.0' }}</span>
      </div>
      <div class="auth-locales">
        <button
          class="locale-button"
          :class="{ active: locale === 'zh-CN' }"
          @click="switchLocale('zh-CN')"
        >
          中文
        </button>
        <button
          class="locale-button"
          :class="{ active: locale === 'en-US' }"
          @click="switchLocale('en-US')"
        >
          EN
        </button>
      </div>
    </section>

    <section class="auth-panel">
      <form class="auth-card" @submit.prevent="submit">
        <div class="auth-card-head">
          <span class="pill">{{ t('auth.adminAccount') }}</span>
          <h2>{{ t('auth.setupTitle') }}</h2>
          <p>{{ t('auth.setupSubtitle') }}</p>
        </div>

        <label class="field">
          <span>{{ t('auth.password') }}</span>
          <input
            v-model="form.password"
            class="input"
            :type="showPassword ? 'text' : 'password'"
            autocomplete="new-password"
            minlength="10"
            required
          />
        </label>

        <label class="field">
          <span>{{ t('auth.confirmPassword') }}</span>
          <input
            v-model="form.confirmPassword"
            class="input"
            :type="showPassword ? 'text' : 'password'"
            autocomplete="new-password"
            minlength="10"
            required
          />
        </label>

        <label class="check-row">
          <input v-model="showPassword" type="checkbox" />
          <span>{{ t('auth.showPassword') }}</span>
        </label>

        <div class="password-meter">
          <span
            v-for="index in 4"
            :key="index"
            class="password-segment"
            :class="{ filled: passwordStrength.classes >= index }"
          ></span>
        </div>
        <p class="hint">{{ t('auth.passwordHint') }}</p>

        <label class="field">
          <span>{{ t('auth.timezone') }}</span>
          <select v-model="form.timezone" class="input">
            <option v-for="zone in zones" :key="zone" :value="zone">{{ zone }}</option>
          </select>
        </label>

        <p v-if="error" class="form-error">{{ error }}</p>

        <button class="button primary auth-submit" type="submit" :disabled="submitting">
          <AppIcon name="shield" :size="18" />
          {{ submitting ? t('common.loading') : t('auth.setupAction') }}
        </button>

        <p class="auth-note">{{ t('auth.securityNote') }}</p>
      </form>
    </section>
  </div>
</template>
