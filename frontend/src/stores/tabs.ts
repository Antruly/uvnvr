import { defineStore } from 'pinia'
import type { RouteLocationNormalized } from 'vue-router'

export interface WorkspaceTab {
  name: string
  path: string
  titleKey: string
  closable: boolean
}

interface TabState {
  tabs: WorkspaceTab[]
  activePath: string
}

export const useTabsStore = defineStore('tabs', {
  state: (): TabState => ({
    tabs: [{ name: 'DashboardView', path: '/', titleKey: 'nav.dashboard', closable: false }],
    activePath: '/',
  }),
  getters: {
    cachedNames: (state) => state.tabs.map((tab) => tab.name),
  },
  actions: {
    open(route: RouteLocationNormalized) {
      if (route.meta.public || !route.meta.titleKey) return
      this.activePath = route.path
      if (this.tabs.some((tab) => tab.path === route.path)) return
      this.tabs.push({
        name: String(route.name ?? route.path),
        path: route.path,
        titleKey: String(route.meta.titleKey),
        closable: route.path !== '/',
      })
    },
    close(path: string): string {
      const index = this.tabs.findIndex((tab) => tab.path === path)
      if (index < 0 || !this.tabs[index].closable) return this.activePath
      const wasActive = this.activePath === path
      this.tabs.splice(index, 1)
      if (wasActive) {
        const next = this.tabs[Math.max(0, index - 1)] ?? this.tabs[0]
        this.activePath = next.path
        return next.path
      }
      return this.activePath
    },
  },
})
