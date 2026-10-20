<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { api, ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'UsersView' })

interface UserRow {
  id: number
  username: string
  roleId: number
  role: string
  enabled: boolean
  createdAt: number
  lastLoginAt: number
}

interface RoleRow {
  id: number
  name: string
  displayName: string
  permissions: string[]
  builtin: boolean
}

const { t } = useI18n()
const auth = useAuthStore()

const tab = ref<'users' | 'roles'>('users')
const users = ref<UserRow[]>([])
const roles = ref<RoleRow[]>([])
const allPermissions = ref<string[]>([])
const loading = ref(true)
const error = ref('')
const notice = ref('')

const userModal = reactive({
  open: false,
  mode: 'create' as 'create' | 'edit',
  id: 0,
  username: '',
  password: '',
  roleId: 0,
  enabled: true,
})

const roleModal = reactive({
  open: false,
  mode: 'create' as 'create' | 'edit',
  id: 0,
  name: '',
  displayName: '',
  permissions: [] as string[],
  builtin: false,
})

const groups = computed(() => [
  {
    key: 'web',
    label: t('users.permissionGroups.web'),
    items: allPermissions.value.filter(
      (permission) =>
        permission.startsWith('dashboard.') ||
        permission.startsWith('preview.') ||
        permission.startsWith('playback.') ||
        permission.startsWith('channel.') ||
        permission.startsWith('plan.') ||
        permission.startsWith('storage.') ||
        permission.startsWith('alert.'),
    ),
  },
  {
    key: 'api',
    label: t('users.permissionGroups.api'),
    items: allPermissions.value.filter(
      (permission) =>
        permission.startsWith('api.') || permission.startsWith('rtsp.'),
    ),
  },
  {
    key: 'system',
    label: t('users.permissionGroups.system'),
    items: allPermissions.value.filter(
      (permission) =>
        permission.startsWith('user.') ||
        permission.startsWith('log.') ||
        permission.startsWith('system.') ||
        permission === 'maintenance',
    ),
  },
])

function flash(message: string) {
  notice.value = message
  window.setTimeout(() => {
    if (notice.value === message) notice.value = ''
  }, 3500)
}

function formatTime(value: number) {
  if (!value) return t('common.never')
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
    timeStyle: 'short',
  }).format(new Date(value * 1000))
}

function roleLabel(id: number) {
  const role = roles.value.find((entry) => entry.id === id)
  return role?.displayName || role?.name || `#${id}`
}

async function load() {
  error.value = ''
  try {
    const [userList, roleList, permissionList] = await Promise.all([
      api<UserRow[]>('/api/v1/users'),
      api<RoleRow[]>('/api/v1/roles'),
      api<string[]>('/api/v1/permissions'),
    ])
    users.value = userList
    roles.value = roleList
    allPermissions.value = permissionList
  } catch (caught) {
    error.value = caught instanceof Error ? caught.message : t('common.error')
  } finally {
    loading.value = false
  }
}

function openCreateUser() {
  userModal.mode = 'create'
  userModal.id = 0
  userModal.username = ''
  userModal.password = ''
  userModal.roleId = roles.value.find((role) => role.name === 'viewer')?.id ?? roles.value[0]?.id ?? 0
  userModal.enabled = true
  userModal.open = true
}

function openEditUser(user: UserRow) {
  userModal.mode = 'edit'
  userModal.id = user.id
  userModal.username = user.username
  userModal.password = ''
  userModal.roleId = user.roleId
  userModal.enabled = user.enabled
  userModal.open = true
}

async function saveUser() {
  error.value = ''
  try {
    if (userModal.mode === 'create') {
      await api('/api/v1/users', {
        method: 'POST',
        body: JSON.stringify({
          username: userModal.username,
          password: userModal.password,
          roleId: userModal.roleId,
          enabled: userModal.enabled,
        }),
      })
      flash(t('users.userCreated'))
    } else {
      const body: Record<string, unknown> = {
        roleId: userModal.roleId,
        enabled: userModal.enabled,
      }
      if (userModal.password) body.password = userModal.password
      await api(`/api/v1/users/${userModal.id}`, {
        method: 'PUT',
        body: JSON.stringify(body),
      })
      flash(t('users.userUpdated'))
    }
    userModal.open = false
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function deleteUser(user: UserRow) {
  if (!window.confirm(t('users.deleteUserConfirm', { name: user.username }))) return
  try {
    await api(`/api/v1/users/${user.id}`, { method: 'DELETE' })
    flash(t('users.userDeleted'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

function openCreateRole() {
  roleModal.mode = 'create'
  roleModal.id = 0
  roleModal.name = ''
  roleModal.displayName = ''
  roleModal.permissions = ['dashboard.view', 'preview.view']
  roleModal.builtin = false
  roleModal.open = true
}

function openEditRole(role: RoleRow) {
  if (role.builtin) return
  roleModal.mode = 'edit'
  roleModal.id = role.id
  roleModal.name = role.name
  roleModal.displayName = role.displayName
  roleModal.permissions = [...role.permissions]
  roleModal.builtin = role.builtin
  roleModal.open = true
}

function togglePermission(permission: string) {
  const index = roleModal.permissions.indexOf(permission)
  if (index >= 0) roleModal.permissions.splice(index, 1)
  else roleModal.permissions.push(permission)
}

async function saveRole() {
  error.value = ''
  try {
    if (roleModal.mode === 'create') {
      await api('/api/v1/roles', {
        method: 'POST',
        body: JSON.stringify({
          name: roleModal.name,
          displayName: roleModal.displayName || roleModal.name,
          permissions: roleModal.permissions,
        }),
      })
      flash(t('users.roleCreated'))
    } else {
      await api(`/api/v1/roles/${roleModal.id}`, {
        method: 'PUT',
        body: JSON.stringify({
          displayName: roleModal.displayName,
          permissions: roleModal.permissions,
        }),
      })
      flash(t('users.roleUpdated'))
    }
    roleModal.open = false
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

async function deleteRole(role: RoleRow) {
  if (role.builtin) return
  if (!window.confirm(t('users.deleteRoleConfirm', { name: role.displayName || role.name }))) return
  try {
    await api(`/api/v1/roles/${role.id}`, { method: 'DELETE' })
    flash(t('users.roleDeleted'))
    await load()
  } catch (caught) {
    error.value = caught instanceof ApiError ? caught.message : t('common.error')
  }
}

onMounted(load)
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('nav.administration') }}</span>
      <h2>{{ t('users.title') }}</h2>
      <p>{{ t('users.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <button class="button" @click="load">
        <AppIcon name="refresh" :size="16" />
        {{ t('common.refresh') }}
      </button>
      <button v-if="tab === 'users'" class="button primary" @click="openCreateUser">
        <AppIcon name="plus" :size="16" />
        {{ t('users.addUser') }}
      </button>
      <button v-else class="button primary" @click="openCreateRole">
        <AppIcon name="plus" :size="16" />
        {{ t('users.addRole') }}
      </button>
    </div>
  </section>

  <div class="segmented">
    <button :class="{ active: tab === 'users' }" @click="tab = 'users'">
      {{ t('users.usersTab') }} <span class="count-badge">{{ users.length }}</span>
    </button>
    <button :class="{ active: tab === 'roles' }" @click="tab = 'roles'">
      {{ t('users.rolesTab') }} <span class="count-badge">{{ roles.length }}</span>
    </button>
  </div>

  <p v-if="notice" class="notice">{{ notice }}</p>
  <p v-if="error" class="form-error">{{ error }}</p>

  <section v-if="loading" class="panel empty-state">
    <span class="spinner"></span>
    {{ t('common.loading') }}
  </section>

  <section v-else-if="tab === 'users'" class="panel table-panel">
    <table class="data-table">
      <thead>
        <tr>
          <th>{{ t('users.username') }}</th>
          <th>{{ t('users.role') }}</th>
          <th>{{ t('users.enabled') }}</th>
          <th>{{ t('users.lastLogin') }}</th>
          <th>{{ t('common.actions') }}</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="user in users" :key="user.id">
          <td>
            <div class="table-primary">
              <span class="user-avatar small">{{ user.username.slice(0, 1).toUpperCase() }}</span>
              <div>
                <strong>{{ user.username }}</strong>
                <small>{{ t('users.created') }} {{ formatTime(user.createdAt) }}</small>
              </div>
            </div>
          </td>
          <td><span class="pill">{{ roleLabel(user.roleId) }}</span></td>
          <td>
            <span class="status-pill" :class="user.enabled ? 'ok' : 'muted'">
              {{ user.enabled ? t('common.enabled') : t('common.disabled') }}
            </span>
          </td>
          <td class="muted">{{ formatTime(user.lastLoginAt) }}</td>
          <td>
            <div class="row-actions">
              <button class="icon-button small" :title="t('common.edit')" @click="openEditUser(user)">
                <AppIcon name="edit" :size="15" />
              </button>
              <button
                class="icon-button small danger"
                :title="t('common.delete')"
                :disabled="user.id === auth.user?.id"
                @click="deleteUser(user)"
              >
                <AppIcon name="trash" :size="15" />
              </button>
            </div>
          </td>
        </tr>
        <tr v-if="users.length === 0">
          <td colspan="5" class="empty-cell">{{ t('users.empty') }}</td>
        </tr>
      </tbody>
    </table>
  </section>

  <section v-else class="role-grid">
    <article v-for="role in roles" :key="role.id" class="panel role-card">
      <div class="role-head">
        <div>
          <h3>{{ role.displayName || role.name }}</h3>
          <span class="muted mono">{{ role.name }}</span>
        </div>
        <span v-if="role.builtin" class="pill">{{ t('users.builtin') }}</span>
      </div>
      <div class="permission-chips">
        <span v-for="permission in role.permissions.slice(0, 8)" :key="permission" class="chip">
          {{ permission }}
        </span>
        <span v-if="role.permissions.length > 8" class="chip">
          +{{ role.permissions.length - 8 }}
        </span>
      </div>
      <div class="role-actions">
        <button class="button small" :disabled="role.builtin" @click="openEditRole(role)">
          <AppIcon name="edit" :size="14" />
          {{ t('common.edit') }}
        </button>
        <button class="button small danger" :disabled="role.builtin" @click="deleteRole(role)">
          <AppIcon name="trash" :size="14" />
          {{ t('common.delete') }}
        </button>
      </div>
      <p v-if="role.builtin" class="small-note">{{ t('users.builtinHint') }}</p>
    </article>
  </section>

  <Teleport to="body">
    <div v-if="userModal.open" class="modal-backdrop" @click.self="userModal.open = false">
      <form class="modal panel" @submit.prevent="saveUser">
        <div class="modal-head">
          <h3>{{ userModal.mode === 'create' ? t('users.addUser') : t('users.editUser') }}</h3>
          <button type="button" class="icon-button small" @click="userModal.open = false">
            <AppIcon name="close" :size="16" />
          </button>
        </div>
        <label class="field">
          <span>{{ t('users.username') }}</span>
          <input
            v-model="userModal.username"
            class="input"
            :disabled="userModal.mode === 'edit'"
            required
          />
        </label>
        <label class="field">
          <span>{{ t('users.password') }}</span>
          <input
            v-model="userModal.password"
            class="input"
            type="password"
            :required="userModal.mode === 'create'"
            :placeholder="userModal.mode === 'edit' ? t('users.keepPassword') : ''"
          />
        </label>
        <label class="field">
          <span>{{ t('users.role') }}</span>
          <select v-model.number="userModal.roleId" class="input" required>
            <option v-for="role in roles" :key="role.id" :value="role.id">
              {{ role.displayName || role.name }}
            </option>
          </select>
        </label>
        <label class="check-row">
          <input v-model="userModal.enabled" type="checkbox" />
          <span>{{ t('users.enabled') }}</span>
        </label>
        <div class="modal-actions">
          <button type="button" class="button" @click="userModal.open = false">
            {{ t('common.cancel') }}
          </button>
          <button type="submit" class="button primary">
            <AppIcon name="save" :size="16" />
            {{ t('common.save') }}
          </button>
        </div>
      </form>
    </div>

    <div v-if="roleModal.open" class="modal-backdrop" @click.self="roleModal.open = false">
      <form class="modal panel modal-wide" @submit.prevent="saveRole">
        <div class="modal-head">
          <h3>{{ roleModal.mode === 'create' ? t('users.addRole') : t('users.editRole') }}</h3>
          <button type="button" class="icon-button small" @click="roleModal.open = false">
            <AppIcon name="close" :size="16" />
          </button>
        </div>
        <div class="modal-columns">
          <label class="field">
            <span>{{ t('users.roleName') }}</span>
            <input
              v-model="roleModal.name"
              class="input"
              :disabled="roleModal.mode === 'edit'"
              required
            />
          </label>
          <label class="field">
            <span>{{ t('users.displayName') }}</span>
            <input v-model="roleModal.displayName" class="input" />
          </label>
        </div>
        <div class="permission-groups">
          <section v-for="group in groups" :key="group.key" class="permission-group">
            <h4>{{ group.label }}</h4>
            <label v-for="permission in group.items" :key="permission" class="check-row">
              <input
                type="checkbox"
                :checked="roleModal.permissions.includes(permission)"
                @change="togglePermission(permission)"
              />
              <span class="mono">{{ permission }}</span>
            </label>
          </section>
        </div>
        <div class="modal-actions">
          <button type="button" class="button" @click="roleModal.open = false">
            {{ t('common.cancel') }}
          </button>
          <button type="submit" class="button primary">
            <AppIcon name="save" :size="16" />
            {{ t('common.save') }}
          </button>
        </div>
      </form>
    </div>
  </Teleport>
</template>
