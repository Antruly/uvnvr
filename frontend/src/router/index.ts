import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { useTabsStore } from '@/stores/tabs'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    {
      path: '/setup',
      name: 'SetupView',
      component: () => import('@/views/SetupView.vue'),
      meta: { public: true },
    },
    {
      path: '/login',
      name: 'LoginView',
      component: () => import('@/views/LoginView.vue'),
      meta: { public: true },
    },
    {
      path: '/',
      name: 'DashboardView',
      component: () => import('@/views/DashboardView.vue'),
      meta: { titleKey: 'nav.dashboard', permission: 'dashboard.view' },
    },
    {
      path: '/preview',
      name: 'PreviewView',
      component: () => import('@/views/PreviewView.vue'),
      meta: { titleKey: 'nav.preview' },
    },
    {
      path: '/users',
      name: 'UsersView',
      component: () => import('@/views/UsersView.vue'),
      meta: { titleKey: 'nav.users', permission: 'user.view' },
    },
    {
      path: '/audit',
      name: 'AuditView',
      component: () => import('@/views/AuditView.vue'),
      meta: { titleKey: 'nav.logs', permission: 'log.view' },
    },
    {
      path: '/system',
      name: 'SystemView',
      component: () => import('@/views/SystemView.vue'),
      meta: { titleKey: 'nav.settings', permission: 'system.view' },
    },
    {
      path: '/about',
      name: 'AboutView',
      component: () => import('@/views/AboutView.vue'),
      meta: { titleKey: 'nav.about' },
    },
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
})

router.beforeEach(async (to) => {
  const auth = useAuthStore()
  await auth.ensureInitialized()

  if (to.meta.public) {
    if (auth.user && (to.name === 'LoginView' || to.name === 'SetupView')) {
      return { name: 'DashboardView' }
    }
    if (to.name === 'LoginView' && auth.setupRequired) return { name: 'SetupView' }
    if (to.name === 'SetupView' && !auth.setupRequired) {
      return { name: auth.user ? 'DashboardView' : 'LoginView' }
    }
    return true
  }

  if (auth.setupRequired) return { name: 'SetupView' }

  if (!auth.user) {
    const ok = await auth.fetchMe()
    if (!ok) {
      return { name: 'LoginView', query: { redirect: to.fullPath } }
    }
  }

  if (to.meta.permission && !auth.hasPermission(String(to.meta.permission))) {
    return { name: 'DashboardView' }
  }

  return true
})

router.afterEach((to) => {
  useTabsStore().open(to)
})

export default router
