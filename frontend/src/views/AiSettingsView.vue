<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { RouterLink } from 'vue-router'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'AiSettingsView' })

interface GlobalConfig {
  enabled: boolean
  plateEnabled: boolean
  faceEnabled: boolean
  intervalMs: number
  minPlateConfidence: number
  minFaceQuality: number
  faceMatchThreshold: number
  running?: boolean
}

interface ChannelAiSettings {
  channelId: number
  channelName: string
  plateEnabled: boolean
  faceEnabled: boolean
  intervalMs: number
  minPlateConfidence: number | null
  minFaceQuality: number | null
  effectivePlate: boolean
  effectiveFace: boolean
  effectiveIntervalMs: number
  effectiveMinPlateConfidence: number
  effectiveMinFaceQuality: number
}

interface StatsChannel {
  id: number
  name: string
}

interface AiStats {
  channels: StatsChannel[]
}

interface ChannelRow {
  id: number
  name: string
  selected: boolean
  saving: boolean
  plateEnabled: boolean
  faceEnabled: boolean
  intervalMs: number
  plateInherit: boolean
  plateSensitivity: number
  faceInherit: boolean
  faceSensitivity: number
  effectivePlate: boolean
  effectiveFace: boolean
  effectiveIntervalMs: number
  effectiveMinPlateConfidence: number
  effectiveMinFaceQuality: number
}

const { t } = useI18n()
const auth = useAuthStore()
const canManage = computed(() => auth.hasPermission('channel.manage'))

const loading = ref(true)
const error = ref('')
const notice = ref('')
const unavailable = ref(false)
const savingGlobal = ref(false)
const applyingBulk = ref(false)
const rows = ref<ChannelRow[]>([])

const globalForm = reactive({
  enabled: false,
  plateEnabled: true,
  faceEnabled: true,
  intervalMs: 1000,
  plateSensitivity: 60,
  faceSensitivity: 30,
  matchThreshold: 75,
})

const bulk = reactive({
  plateEnabled: false,
  faceEnabled: false,
})

const selectedRows = computed(() => rows.value.filter((row) => row.selected))
const allSelected = computed(
  () => rows.value.length > 0 && rows.value.every((row) => row.selected),
)

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3000)
}

function percent(value: number) {
  return `${Math.round(value * 100)}%`
}

function applyGlobal(config: GlobalConfig) {
  globalForm.enabled = config.enabled
  globalForm.plateEnabled = config.plateEnabled
  globalForm.faceEnabled = config.faceEnabled
  globalForm.intervalMs = config.intervalMs
  globalForm.plateSensitivity = Math.round(config.minPlateConfidence * 100)
  globalForm.faceSensitivity = Math.round(config.minFaceQuality * 100)
  globalForm.matchThreshold = Math.round(config.faceMatchThreshold * 100)
}

function applyChannelSettings(row: ChannelRow, settings: ChannelAiSettings) {
  row.plateEnabled = settings.plateEnabled
  row.faceEnabled = settings.faceEnabled
  row.intervalMs = settings.intervalMs
  row.plateInherit = settings.minPlateConfidence === null
  row.plateSensitivity = Math.round(
    (settings.minPlateConfidence ?? globalForm.plateSensitivity / 100) * 100,
  )
  row.faceInherit = settings.minFaceQuality === null
  row.faceSensitivity = Math.round(
    (settings.minFaceQuality ?? globalForm.faceSensitivity / 100) * 100,
  )
  row.effectivePlate = settings.effectivePlate
  row.effectiveFace = settings.effectiveFace
  row.effectiveIntervalMs = settings.effectiveIntervalMs
  row.effectiveMinPlateConfidence = settings.effectiveMinPlateConfidence
  row.effectiveMinFaceQuality = settings.effectiveMinFaceQuality
}

async function load() {
  loading.value = true
  error.value = ''
  unavailable.value = false
  try {
    const [config, stats] = await Promise.all([
      api<GlobalConfig>('/api/v1/ai/config'),
      api<AiStats>('/api/v1/ai/stats'),
    ])
    applyGlobal(config)
    const channels = stats.channels ?? []
    const settings = await Promise.all(
      channels.map(async (channel) => {
        try {
          return await api<ChannelAiSettings>(`/api/v1/channels/${channel.id}/ai`)
        } catch {
          return null
        }
      }),
    )
    rows.value = channels.map((channel, index) => {
      const row: ChannelRow = {
        id: channel.id,
        name: channel.name,
        selected: false,
        saving: false,
        plateEnabled: false,
        faceEnabled: false,
        intervalMs: 0,
        plateInherit: true,
        plateSensitivity: globalForm.plateSensitivity,
        faceInherit: true,
        faceSensitivity: globalForm.faceSensitivity,
        effectivePlate: false,
        effectiveFace: false,
        effectiveIntervalMs: config.intervalMs,
        effectiveMinPlateConfidence: config.minPlateConfidence,
        effectiveMinFaceQuality: config.minFaceQuality,
      }
      const current = settings[index]
      if (current) applyChannelSettings(row, current)
      return row
    })
  } catch (caught) {
    if (caught instanceof ApiError && caught.status === 503) {
      unavailable.value = true
    } else {
      error.value = caught instanceof ApiError ? caught.message : t('common.error')
    }
  } finally {
    loading.value = false
  }
}

async function saveGlobal() {
  if (!canManage.value) return
  savingGlobal.value = true
  error.value = ''
  try {
    const config = await api<GlobalConfig>('/api/v1/ai/config', {
      method: 'PUT',
      body: JSON.stringify({
        enabled: globalForm.enabled,
        plateEnabled: globalForm.plateEnabled,
        faceEnabled: globalForm.faceEnabled,
        intervalMs: Number(globalForm.intervalMs),
        minPlateConfidence: Number(globalForm.plateSensitivity) / 100,
        minFaceQuality: Number(globalForm.faceSensitivity) / 100,
        faceMatchThreshold: Number(globalForm.matchThreshold) / 100,
      }),
    })
    applyGlobal(config)
    flash(t('aiSettings.savedGlobal'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    savingGlobal.value = false
  }
}

async function saveChannel(row: ChannelRow) {
  if (!canManage.value) return
  row.saving = true
  error.value = ''
  try {
    const settings = await api<ChannelAiSettings>(`/api/v1/channels/${row.id}/ai`, {
      method: 'PUT',
      body: JSON.stringify({
        plateEnabled: row.plateEnabled,
        faceEnabled: row.faceEnabled,
        intervalMs: Number(row.intervalMs) || 0,
        minPlateConfidence: row.plateInherit ? null : Number(row.plateSensitivity) / 100,
        minFaceQuality: row.faceInherit ? null : Number(row.faceSensitivity) / 100,
      }),
    })
    applyChannelSettings(row, settings)
    flash(t('aiSettings.savedChannel', { channel: row.name }))
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    row.saving = false
  }
}

function toggleAll() {
  const value = !allSelected.value
  rows.value.forEach((row) => {
    row.selected = value
  })
}

async function applyBulk() {
  if (!canManage.value) return
  const channels = selectedRows.value
  if (channels.length === 0) return
  applyingBulk.value = true
  error.value = ''
  try {
    await api('/api/v1/ai/channels', {
      method: 'PUT',
      body: JSON.stringify({
        channelIds: channels.map((row) => row.id),
        plateEnabled: bulk.plateEnabled,
        faceEnabled: bulk.faceEnabled,
      }),
    })
    flash(t('aiSettings.bulkSaved', { count: channels.length }))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  } finally {
    applyingBulk.value = false
  }
}

onMounted(load)
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.aiSettings') }}</span>
      <h2>{{ t('aiSettings.title') }}</h2>
      <p>{{ t('aiSettings.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <RouterLink class="button" to="/ai-events">
        <AppIcon name="camera" :size="16" />
        {{ t('nav.aiEvents') }}
      </RouterLink>
      <button class="button" type="button" :disabled="loading" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
    </div>
  </section>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else-if="unavailable" class="panel empty-state">
    <AppIcon name="info" :size="28" />
    <span>{{ t('aiSettings.unavailable') }}</span>
  </section>

  <template v-else>
    <section class="panel settings-card">
      <div class="section-title">
        <AppIcon name="settings" :size="20" />
        <h3>{{ t('aiSettings.globalTitle') }}</h3>
      </div>
      <p class="small-note">{{ t('aiSettings.globalHint') }}</p>
      <div class="ai-switch-row">
        <label class="check-row">
          <input v-model="globalForm.enabled" type="checkbox" :disabled="!canManage" />
          <span>{{ t('aiSettings.masterEnabled') }}</span>
        </label>
        <label class="check-row">
          <input v-model="globalForm.plateEnabled" type="checkbox" :disabled="!canManage" />
          <span>{{ t('aiSettings.plateGlobal') }}</span>
        </label>
        <label class="check-row">
          <input v-model="globalForm.faceEnabled" type="checkbox" :disabled="!canManage" />
          <span>{{ t('aiSettings.faceGlobal') }}</span>
        </label>
      </div>
      <div class="form-grid">
        <label class="field">
          <span>{{ t('aiSettings.interval') }}</span>
          <input
            v-model.number="globalForm.intervalMs"
            class="input"
            type="number"
            min="250"
            max="10000"
            step="250"
            :disabled="!canManage"
          />
        </label>
        <label class="field">
          <span>{{ t('aiSettings.matchThreshold') }}</span>
          <div class="ai-range">
            <input
              v-model.number="globalForm.matchThreshold"
              type="range"
              min="0"
              max="100"
              step="1"
              :disabled="!canManage"
            />
            <span class="chip">{{ globalForm.matchThreshold }}%</span>
          </div>
        </label>
        <label class="field">
          <span>{{ t('aiSettings.plateSensitivity') }}</span>
          <div class="ai-range">
            <input
              v-model.number="globalForm.plateSensitivity"
              type="range"
              min="0"
              max="100"
              step="1"
              :disabled="!canManage"
            />
            <span class="chip">{{ globalForm.plateSensitivity }}%</span>
          </div>
          <span class="field-hint">{{ t('aiSettings.sensitivityHint') }}</span>
        </label>
        <label class="field">
          <span>{{ t('aiSettings.faceSensitivity') }}</span>
          <div class="ai-range">
            <input
              v-model.number="globalForm.faceSensitivity"
              type="range"
              min="0"
              max="100"
              step="1"
              :disabled="!canManage"
            />
            <span class="chip">{{ globalForm.faceSensitivity }}%</span>
          </div>
          <span class="field-hint">{{ t('aiSettings.sensitivityHint') }}</span>
        </label>
      </div>
      <div class="modal-actions">
        <button
          class="button primary"
          type="button"
          :disabled="savingGlobal || !canManage"
          @click="saveGlobal"
        >
          <AppIcon name="save" :size="16" />
          {{ savingGlobal ? t('common.loading') : t('aiSettings.saveGlobal') }}
        </button>
      </div>
    </section>

    <section class="panel ai-bulk">
      <div class="section-title">
        <AppIcon name="check" :size="18" />
        <h3>{{ t('aiSettings.bulkTitle') }}</h3>
      </div>
      <div class="ai-bulk-row">
        <span class="muted">{{ t('aiSettings.selectedCount', { count: selectedRows.length }) }}</span>
        <label class="check-row">
          <input v-model="bulk.plateEnabled" type="checkbox" :disabled="!canManage" />
          <span>{{ t('aiSettings.bulkPlate') }}</span>
        </label>
        <label class="check-row">
          <input v-model="bulk.faceEnabled" type="checkbox" :disabled="!canManage" />
          <span>{{ t('aiSettings.bulkFace') }}</span>
        </label>
        <button
          class="button small"
          type="button"
          :disabled="applyingBulk || !canManage || selectedRows.length === 0"
          @click="applyBulk"
        >
          {{ applyingBulk ? t('common.loading') : t('aiSettings.applyBulk') }}
        </button>
      </div>
      <p class="field-hint">{{ t('aiSettings.bulkHint') }}</p>
    </section>

    <section class="panel table-panel ai-channel-table">
      <div class="ai-channel-head">
        <div class="section-title">
          <AppIcon name="camera" :size="18" />
          <h3>{{ t('aiSettings.channelsTitle') }}</h3>
        </div>
        <p class="small-note">{{ t('aiSettings.channelsHint') }}</p>
      </div>
      <table class="data-table">
        <thead>
          <tr>
            <th class="ai-check-cell">
              <input
                type="checkbox"
                :checked="allSelected"
                :disabled="rows.length === 0"
                :aria-label="t('aiSettings.selectAll')"
                @change="toggleAll"
              />
            </th>
            <th>{{ t('aiSettings.channel') }}</th>
            <th>{{ t('aiSettings.plateSwitch') }}</th>
            <th>{{ t('aiSettings.faceSwitch') }}</th>
            <th>{{ t('aiSettings.intervalOverride') }}</th>
            <th>{{ t('aiSettings.plateSensitivityShort') }}</th>
            <th>{{ t('aiSettings.faceSensitivityShort') }}</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="row in rows" :key="row.id">
            <td class="ai-check-cell">
              <input v-model="row.selected" type="checkbox" :aria-label="row.name" />
            </td>
            <td>
              <div class="table-primary">
                <div>
                  <strong>{{ row.name }}</strong>
                  <small>
                    {{ t('aiSettings.effective') }}:
                    {{ row.effectivePlate ? t('aiSettings.on') : t('aiSettings.off') }} /
                    {{ row.effectiveFace ? t('aiSettings.on') : t('aiSettings.off') }}
                  </small>
                </div>
              </div>
            </td>
            <td>
              <input v-model="row.plateEnabled" type="checkbox" :disabled="!canManage" />
            </td>
            <td>
              <input v-model="row.faceEnabled" type="checkbox" :disabled="!canManage" />
            </td>
            <td>
              <input
                v-model.number="row.intervalMs"
                class="input ai-option"
                type="number"
                min="0"
                max="10000"
                step="250"
                :disabled="!canManage"
              />
              <small class="field-hint">
                {{
                  t('aiSettings.intervalInheritHint', {
                    value: row.effectiveIntervalMs,
                  })
                }}
              </small>
            </td>
            <td>
              <label class="check-row ai-inherit">
                <input v-model="row.plateInherit" type="checkbox" :disabled="!canManage" />
                <span>{{ t('aiSettings.useGlobal') }}</span>
              </label>
              <input
                v-model.number="row.plateSensitivity"
                class="input ai-option"
                type="number"
                min="0"
                max="100"
                step="1"
                :disabled="!canManage || row.plateInherit"
              />
              <small class="field-hint">
                {{ percent(row.effectiveMinPlateConfidence) }}
              </small>
            </td>
            <td>
              <label class="check-row ai-inherit">
                <input v-model="row.faceInherit" type="checkbox" :disabled="!canManage" />
                <span>{{ t('aiSettings.useGlobal') }}</span>
              </label>
              <input
                v-model.number="row.faceSensitivity"
                class="input ai-option"
                type="number"
                min="0"
                max="100"
                step="1"
                :disabled="!canManage || row.faceInherit"
              />
              <small class="field-hint">
                {{ percent(row.effectiveMinFaceQuality) }}
              </small>
            </td>
            <td>
              <button
                class="button small"
                type="button"
                :disabled="row.saving || !canManage"
                @click="saveChannel(row)"
              >
                <AppIcon name="save" :size="14" />
                {{ row.saving ? t('common.loading') : t('aiSettings.saveChannel') }}
              </button>
            </td>
          </tr>
        </tbody>
      </table>
      <p v-if="rows.length === 0" class="empty-state">{{ t('aiSettings.noChannels') }}</p>
    </section>
  </template>
</template>

<style scoped>
.ai-switch-row {
  display: flex;
  align-items: center;
  gap: 22px;
  flex-wrap: wrap;
  margin-bottom: 16px;
}

.ai-range {
  display: flex;
  align-items: center;
  gap: 10px;
}

.ai-range input[type='range'] {
  flex: 1 1 auto;
  accent-color: var(--accent);
}

.ai-bulk {
  padding: 18px;
  margin-top: 14px;
}

.ai-bulk-row {
  display: flex;
  align-items: center;
  gap: 18px;
  flex-wrap: wrap;
}

.ai-channel-table {
  margin-top: 14px;
  padding: 18px 0 0;
}

.ai-channel-head {
  padding: 0 18px;
}

.ai-channel-head .section-title {
  margin-bottom: 4px;
}

.ai-check-cell {
  width: 34px;
  text-align: center;
}

.ai-check-cell input,
tbody td > input[type='checkbox'] {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
}

.ai-option {
  width: 96px;
  min-height: 32px;
  margin: 4px 0 2px;
  font-size: 12px;
}

.ai-inherit {
  font-size: 11px;
}
</style>
