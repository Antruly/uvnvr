import { createRouter, createWebHistory } from 'vue-router'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    {
      path: '/',
      name: 'dashboard',
      component: () => import('@/views/DashboardView.vue'),
      meta: { titleKey: 'nav.dashboard' },
    },
    {
      path: '/preview',
      name: 'preview',
      component: () => import('@/views/PreviewView.vue'),
      meta: { titleKey: 'nav.preview' },
    },
    {
      path: '/about',
      name: 'about',
      component: () => import('@/views/AboutView.vue'),
      meta: { titleKey: 'nav.about' },
    },
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
})

export default router
