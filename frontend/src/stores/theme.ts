import { ref } from 'vue'

export type ThemeMode = 'auto' | 'light' | 'dark'

const mode = ref<ThemeMode>('auto')

function apply(value: ThemeMode) {
  mode.value = value
  try {
    localStorage.setItem('uvnvr.theme', value)
  } catch {
    /* Storage may be unavailable; keep the in-memory choice. */
  }
  const root = document.documentElement
  if (value === 'auto') root.removeAttribute('data-theme')
  else root.setAttribute('data-theme', value)
}

export function useTheme() {
  return { mode, apply }
}

export function initTheme() {
  let saved: string | null = null
  try {
    saved = localStorage.getItem('uvnvr.theme')
  } catch {
    saved = null
  }
  apply(saved === 'light' || saved === 'dark' ? saved : 'auto')
}
