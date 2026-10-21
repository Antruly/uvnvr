<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api/client'
import { useAppStore } from '@/stores/app'

defineOptions({ name: 'AuditView' })

interface AuditRow {
  id: number
  ts: number
  category: string
  level: string
  actor: string
  ip: string
  userAgent: string
  objectType: string
  objectId: string
  action: string
  result: string
  errorCode: string
  detail: string
}

interface AuditResponse {
  items: AuditRow[]
  total: number
  page: number
  pageSize: number
}

const { t } = useI18n()
const app = useAppStore()
const canMaintain = computed(() => app.server?.maintenance === true)
const notice = ref('')
const items = ref<AuditRow[]>([])
const total = ref(0)
const page = ref(1)
const pageSize = ref(25)
const loading = ref(false)
const error = ref('')
const filters = reactive({
  category: '',
  level: '',
  q: '',
  from: '',
  to: '',
})

const categories = [
  'auth',
  'user',
  'permission',
  'system',
  'api',
  'channel',
  'record',
  'storage',
  'alert',
  'maintenance',
]
const levels = ['info', 'warning', 'error']

const totalPages = computed(() => Math.max(1, Math.ceil(total.value / pageSize.value)))

function categoryLabel(category: string) {
  const key = `audit.categories.${category}`
  const translated = t(key)
  return translated === key ? category : translated
}

function formatTime(seconds: number) {
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'short',
    timeStyle: 'medium',
  }).format(new Date(seconds * 1000))
}

function prettyDetail(detail: string) {
  if (!detail) return '—'
  try {
    return JSON.stringify(JSON.parse(detail), null, 2)
  } catch {
    return detail
  }
}

function buildQuery() {
  const params = new URLSearchParams()
  params.set('page', String(page.value))
  params.set('pageSize', String(pageSize.value))
  if (filters.category) params.set('category', filters.category)
  if (filters.level) params.set('level', filters.level)
  if (filters.q) params.set('q', filters.q)
  if (filters.from) {
    const seconds = Math.floor(new Date(filters.from).getTime() / 1000)
    if (Number.isFinite(seconds)) params.set('from', String(seconds))
  }
  if (filters.to) {
    const seconds = Math.floor(new Date(filters.to).getTime() / 1000)
    if (Number.isFinite(seconds)) params.set('to', String(seconds))
  }
  return params
}

async function load(resetPage = false) {
  if (resetPage) page.value = 1
  loading.value = true
  error.value = ''
  try {
    const response = await api<AuditResponse>(`/api/v1/audit?${buildQuery().toString()}`)
    items.value = response.items
    total.value = response.total
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

function reset() {
  filters.category = ''
  filters.level = ''
  filters.q = ''
  filters.from = ''
  filters.to = ''
  void load(true)
}

function go(delta: number) {
  const next = page.value + delta
  if (next < 1 || next > totalPages.value) return
  page.value = next
  void load()
}

function exportCsv() {
  const params = buildQuery()
  params.delete('page')
  params.delete('pageSize')
  window.location.assign(`/api/v1/audit/export?${params.toString()}`)
}

async function purge() {
  if (!window.confirm(t('audit.purgeConfirm'))) return
  notice.value = ''
  error.value = ''
  try {
    const result = await api<{ deleted: number }>('/api/v1/audit/purge', {
      method: 'POST',
      body: JSON.stringify({ confirm: 'purge' }),
    })
    notice.value = t('audit.purged', { count: result.deleted })
    await load(true)
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  }
}

onMounted(() => void load())
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.logs') }}</span>
      <h2>{{ t('audit.title') }}</h2>
      <p>{{ t('audit.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <span class="muted">{{ t('audit.total', { total }) }}</span>
      <button class="button" @click="exportCsv">
        <AppIcon name="save" :size="16" />
        {{ t('audit.exportCsv') }}
      </button>
      <button v-if="canMaintain" class="button danger" @click="purge">
        <AppIcon name="trash" :size="16" />
        {{ t('audit.purge') }}
      </button>
      <button class="button" @click="load()">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>

  <form class="panel filter-bar" @submit.prevent="load(true)">
    <label class="field compact-field">
      <span>{{ t('audit.category') }}</span>
      <select v-model="filters.category" class="input">
        <option value="">{{ t('common.all') }}</option>
        <option v-for="category in categories" :key="category" :value="category">
          {{ categoryLabel(category) }}
        </option>
      </select>
    </label>
    <label class="field compact-field">
      <span>{{ t('audit.level') }}</span>
      <select v-model="filters.level" class="input">
        <option value="">{{ t('common.all') }}</option>
        <option v-for="level in levels" :key="level" :value="level">{{ level }}</option>
      </select>
    </label>
    <label class="field compact-field grow">
      <span>{{ t('audit.keyword') }}</span>
      <input v-model="filters.q" class="input" type="search" />
    </label>
    <label class="field compact-field">
      <span>{{ t('audit.from') }}</span>
      <input v-model="filters.from" class="input" type="datetime-local" />
    </label>
    <label class="field compact-field">
      <span>{{ t('audit.to') }}</span>
      <input v-model="filters.to" class="input" type="datetime-local" />
    </label>
    <div class="filter-actions">
      <button type="submit" class="button primary">
        <AppIcon name="search" :size="16" />
        {{ t('audit.filter') }}
      </button>
      <button type="button" class="button" @click="reset">{{ t('audit.reset') }}</button>
    </div>
  </form>

  <p v-if="error" class="form-error">{{ error }}</p>

  <section class="panel table-panel">
    <table class="data-table audit-table">
      <thead>
        <tr>
          <th>{{ t('audit.time') }}</th>
          <th>{{ t('audit.category') }}</th>
          <th>{{ t('audit.level') }}</th>
          <th>{{ t('audit.actor') }}</th>
          <th>{{ t('audit.ip') }}</th>
          <th>{{ t('audit.action') }}</th>
          <th>{{ t('audit.result') }}</th>
          <th>{{ t('audit.details') }}</th>
        </tr>
      </thead>
      <tbody>
        <template v-for="row in items" :key="row.id">
          <tr>
            <td class="muted nowrap">{{ formatTime(row.ts) }}</td>
            <td><span class="pill">{{ categoryLabel(row.category) }}</span></td>
            <td>
              <span class="status-pill" :class="row.level">{{ row.level }}</span>
            </td>
            <td>
              <strong>{{ row.actor || '—' }}</strong>
              <small class="block muted">{{ row.userAgent || '' }}</small>
            </td>
            <td class="muted mono">{{ row.ip || '—' }}</td>
            <td>
              <strong>{{ row.action }}</strong>
              <small v-if="row.errorCode" class="block error-text">{{ row.errorCode }}</small>
            </td>
            <td>
              <span class="status-pill" :class="row.result === 'success' ? 'ok' : 'danger'">
                {{ row.result }}
              </span>
            </td>
            <td>
              <details v-if="row.detail && row.detail !== '{}'" class="detail-box">
                <summary>{{ t('audit.details') }}</summary>
                <pre>{{ prettyDetail(row.detail) }}</pre>
              </details>
              <span v-else class="muted">—</span>
            </td>
          </tr>
        </template>
        <tr v-if="!loading && items.length === 0">
          <td colspan="8" class="empty-cell">{{ t('audit.empty') }}</td>
        </tr>
        <tr v-if="loading">
          <td colspan="8" class="empty-cell"><span class="spinner"></span></td>
        </tr>
      </tbody>
    </table>
    <div class="pagination">
      <button class="button small" :disabled="page <= 1" @click="go(-1)">
        {{ t('common.prev') }}
      </button>
      <span>{{ t('common.page') }} {{ page }} / {{ totalPages }}</span>
      <button class="button small" :disabled="page >= totalPages" @click="go(1)">
        {{ t('common.next') }}
      </button>
    </div>
  </section>
</template>
