import { defineStore } from 'pinia'
import { api, setCsrfToken } from '@/api/client'

export interface AuthUser {
  id: number
  username: string
  role: string
  permissions: string[]
}

interface AuthState {
  initialized: boolean
  setupRequired: boolean
  user: AuthUser | null
  csrfToken: string
  version: string
  serial: string
  loading: boolean
}

export const useAuthStore = defineStore('auth', {
  state: (): AuthState => ({
    initialized: false,
    setupRequired: false,
    user: null,
    csrfToken: '',
    version: '',
    serial: '',
    loading: false,
  }),
  getters: {
    isAuthenticated: (state) => state.user !== null,
    hasPermission: (state) => (permission: string) =>
      state.user?.permissions.includes('*') || state.user?.permissions.includes(permission) || false,
  },
  actions: {
    applySession(payload: { user: AuthUser; csrfToken: string }) {
      this.user = payload.user
      this.csrfToken = payload.csrfToken
      setCsrfToken(payload.csrfToken)
    },
    clearSession() {
      this.user = null
      this.csrfToken = ''
      setCsrfToken('')
    },
    async ensureInitialized() {
      if (this.initialized) return
      try {
        const status = await api<{ setupRequired: boolean; version: string; serial: string }>(
          '/api/v1/setup/status',
        )
        this.setupRequired = status.setupRequired
        this.version = status.version
        this.serial = status.serial
        if (!status.setupRequired) await this.fetchMe()
      } catch {
        this.clearSession()
      } finally {
        this.initialized = true
      }
    },
    async checkSetup() {
      const status = await api<{ setupRequired: boolean; version: string; serial: string }>(
        '/api/v1/setup/status',
      )
      this.setupRequired = status.setupRequired
      this.version = status.version
      this.serial = status.serial
    },
    async fetchMe() {
      try {
        const payload = await api<{ user: AuthUser; csrfToken: string }>('/api/v1/auth/me')
        this.applySession(payload)
        this.setupRequired = false
        return true
      } catch {
        this.clearSession()
        return false
      }
    },
    async login(username: string, password: string) {
      const payload = await api<{ user: AuthUser; csrfToken: string }>('/api/v1/auth/login', {
        method: 'POST',
        body: JSON.stringify({ username, password }),
      })
      this.applySession(payload)
      this.setupRequired = false
      this.initialized = true
    },
    async setup(password: string, confirmPassword: string, timezone: string) {
      const payload = await api<{ user: AuthUser; csrfToken: string }>('/api/v1/setup', {
        method: 'POST',
        body: JSON.stringify({ password, confirmPassword, timezone }),
      })
      this.applySession(payload)
      this.setupRequired = false
      this.initialized = true
    },
    async logout() {
      try {
        await api('/api/v1/auth/logout', { method: 'POST' })
      } finally {
        this.clearSession()
      }
    },
  },
})
