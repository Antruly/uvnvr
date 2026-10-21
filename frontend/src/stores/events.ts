import { defineStore } from 'pinia'

export interface AlertEvent {
  channelId: number
  type: string
  level: string
  message: string
  snapshotPath: string
  acknowledged: boolean
  createdAt: number
}

export interface ChannelEvent {
  channelId: number
  name?: string
  state: string
  error?: string
}

export const useEventStore = defineStore('events', {
  state: () => ({
    connected: false,
    seq: 0,
    lastAlert: null as AlertEvent | null,
    lastChannel: null as ChannelEvent | null,
  }),
  actions: {
    setConnected(value: boolean) {
      this.connected = value
    },
    apply(message: unknown) {
      if (!message || typeof message !== 'object') return
      const event = message as { type?: string; alert?: AlertEvent; channelId?: number }
      if (event.type === 'alert' && event.alert) {
        this.lastAlert = event.alert
        this.seq += 1
      } else if (event.type === 'channel') {
        this.lastChannel = event as unknown as ChannelEvent
      }
    },
  },
})
