import { defineStore } from 'pinia'

export interface ChannelStatusItem {
  id: number
  name?: string
  enabled?: boolean
  mainState: string
  mainError: string
  subState: string
  subError: string
  lastOnlineAt?: number
  lastOfflineAt?: number
}

interface StatusMessage {
  type?: string
  items?: ChannelStatusItem[]
  interval?: number
  ts?: number
}

/**
 * Shared channel-state subscription. The socket itself is owned by App.vue
 * (one connection for the whole tab); pages read `items` and watch `seq`.
 * Items are merged field-by-field so a row keeps its identity and Vue does
 * not re-render or re-key the surrounding list.
 */
export const useChannelStatusStore = defineStore('channelStatus', {
  state: () => ({
    connected: false,
    // Bumped for every message. Pages watch this scalar, not `items`, so a
    // deep watcher does not run for unrelated fields.
    seq: 0,
    // Bumped after the post-reconnect GET /api/v1/channels snapshot so pages
    // can reload metadata without touching the live DOM in between.
    resync: 0,
    lastTs: 0,
    items: {} as Record<number, ChannelStatusItem>,
  }),
  actions: {
    setConnected(value: boolean) {
      this.connected = value
    },
    apply(message: StatusMessage | unknown) {
      if (!message || typeof message !== 'object') return
      const payload = message as StatusMessage
      if (payload.type !== 'channel.status' || !Array.isArray(payload.items)) return
      for (const item of payload.items) {
        if (!item || typeof item.id !== 'number') continue
        const existing = this.items[item.id]
        if (existing) {
          existing.mainState = item.mainState
          existing.mainError = item.mainError ?? ''
          existing.subState = item.subState
          existing.subError = item.subError ?? ''
          if (item.name !== undefined) existing.name = item.name
          if (item.enabled !== undefined) existing.enabled = item.enabled
          if (item.lastOnlineAt !== undefined) existing.lastOnlineAt = item.lastOnlineAt
          if (item.lastOfflineAt !== undefined) existing.lastOfflineAt = item.lastOfflineAt
        } else {
          this.items[item.id] = { ...item }
        }
      }
      if (typeof payload.ts === 'number') this.lastTs = payload.ts
      this.seq += 1
    },
    bumpResync() {
      this.resync += 1
    },
    /** Aligns the store with a full GET /api/v1/channels after reconnect. */
    replaceFromChannels(rows: Array<Record<string, unknown>>) {
      for (const row of rows) {
        const id = Number(row.id)
        if (!Number.isFinite(id)) continue
        this.items[id] = {
          id,
          name: typeof row.name === 'string' ? row.name : undefined,
          enabled: typeof row.enabled === 'boolean' ? row.enabled : undefined,
          mainState: String(row.mainState ?? 'unknown'),
          mainError: String(row.mainError ?? ''),
          subState: String(row.subState ?? 'disabled'),
          subError: String(row.subError ?? ''),
          lastOnlineAt: Number(row.lastOnlineAt ?? 0),
          lastOfflineAt: Number(row.lastOfflineAt ?? 0),
        }
      }
      this.seq += 1
      this.resync += 1
    },
  },
})
