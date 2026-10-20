<script setup lang="ts">
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'LoginView' })

const auth = useAuthStore()
const route = useRoute()
const router = useRouter()
const { t, locale } = useI18n()

const form = reactive({ username: 'admin', password: '' })
const error = ref('')
const submitting = ref(false)
const lockedSeconds = ref(0)
let timer: number | undefined

function switchLocale(value: 'zh-CN' | 'en-US') {
  locale.value = value
  localStorage.setItem('uvnvr.locale', value)
  document.documentElement.lang = value
}

function startLockCountdown(seconds: number) {
  lockedSeconds.value = Math.max(0, seconds)
  if (timer) window.clearInterval(timer)
  timer = window.setInterval(() => {
    lockedSeconds.value = Math.max(0, lockedSeconds.value - 1)
    if (lockedSeconds.value === 0 && timer) {
      window.clearInterval(timer)
      timer = undefined
    }
  }, 1000)
}

async function submit() {
  if (lockedSeconds.value > 0) return
  error.value = ''
  submitting.value = true
  try {
    await auth.login(form.username.trim(), form.password)
    const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : '/'
    await router.push(redirect)
  } catch (caught) {
    if (caught instanceof ApiError) {
      if (caught.status === 429) {
        startLockCountdown(caught.retryAfter || 600)
        error.value = t('auth.locked')
      } else if (caught.status === 401) {
        error.value = t('auth.loginFailed')
      } else {
        error.value = caught.message
      }
    } else {
      error.value = t('common.error')
    }
  } finally {
    submitting.value = false
  }
}

onMounted(async () => {
  await auth.ensureInitialized()
  if (auth.user) await router.replace('/')
  if (auth.setupRequired) await router.replace('/setup')
})

onBeforeUnmount(() => {
  if (timer) window.clearInterval(timer)
})
</script>

<template>
  <div class="auth-shell">
    <section class="auth-brand">
      <img src="/icon-512.png" alt="UvNVR" class="auth-logo" />
      <span class="pill accent">{{ t('auth.adminAccount') }}</span>
      <h1>{{ t('app.name') }}</h1>
      <p class="auth-tagline">{{ t('app.tagline') }} — {{ t('app.subtitle') }}</p>
      <p class="auth-copy">{{ t('auth.loginSubtitle') }}</p>
      <div class="auth-meta">
        <span>{{ t('dashboard.serial') }}</span>
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
          <span class="pill">{{ t('auth.signInPrompt') }}</span>
          <h2>{{ t('auth.loginTitle') }}</h2>
          <p>{{ t('auth.loginSubtitle') }}</p>
        </div>

        <label class="field">
          <span>{{ t('common.username') }}</span>
          <input
            v-model="form.username"
            class="input"
            autocomplete="username"
            required
            autofocus
          />
        </label>

        <label class="field">
          <span>{{ t('auth.password') }}</span>
          <input
            v-model="form.password"
            class="input"
            type="password"
            autocomplete="current-password"
            required
          />
        </label>

        <p v-if="lockedSeconds > 0" class="form-error">
          {{ t('auth.locked') }} {{ t('auth.lockedRetry', { seconds: lockedSeconds }) }}
        </p>
        <p v-else-if="error" class="form-error">{{ error }}</p>

        <button
          class="button primary auth-submit"
          type="submit"
          :disabled="submitting || lockedSeconds > 0"
        >
          <AppIcon name="shield" :size="18" />
          {{ submitting ? t('common.loading') : t('auth.signIn') }}
        </button>

        <p class="auth-note">
          {{ t('common.version') }} {{ auth.version || '0.1.0' }} · {{ auth.serial || '—' }}
        </p>
      </form>
    </section>
  </div>
</template>
