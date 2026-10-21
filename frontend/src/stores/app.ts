import { defineStore } from 'pinia'

export interface ServerInfo {
  status: string
  name: string
  version: string
  commit: string
  maintenance: boolean
}

export const useAppStore = defineStore('app', {
  state: () => ({
    sidebarCollapsed: localStorage.getItem('uvnvr.sidebar') === 'collapsed',
    server: null as ServerInfo | null,
    serverError: false,
  }),
  actions: {
    toggleSidebar() {
      this.sidebarCollapsed = !this.sidebarCollapsed
      localStorage.setItem('uvnvr.sidebar', this.sidebarCollapsed ? 'collapsed' : 'expanded')
    },
    async fetchServerInfo() {
      try {
        const response = await fetch('/healthz', { headers: { Accept: 'application/json' } })
        if (!response.ok) throw new Error(`HTTP ${response.status}`)
        this.server = (await response.json()) as ServerInfo
        this.serverError = false
      } catch {
        this.serverError = true
      }
    },
  },
})
