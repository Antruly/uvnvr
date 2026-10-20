let csrfToken = ''

export function setCsrfToken(token: string) {
  csrfToken = token
}

export function getCsrfToken() {
  return csrfToken
}

export class ApiError extends Error {
  status: number
  code: string
  retryAfter: number

  constructor(message: string, status: number, code = 'request_failed', retryAfter = 0) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.retryAfter = retryAfter
  }
}

interface RequestOptions extends RequestInit {
  skipCsrf?: boolean
}

export async function api<T>(path: string, options: RequestOptions = {}): Promise<T> {
  const method = (options.method ?? 'GET').toUpperCase()
  const headers = new Headers(options.headers)

  if (options.body !== undefined && !headers.has('Content-Type')) {
    headers.set('Content-Type', 'application/json')
  }
  if (!['GET', 'HEAD', 'OPTIONS'].includes(method) && !options.skipCsrf && csrfToken) {
    headers.set('X-CSRF-Token', csrfToken)
  }

  const response = await fetch(path, {
    credentials: 'same-origin',
    ...options,
    method,
    headers,
  })

  const text = await response.text()
  let data: unknown = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      data = text
    }
  }

  if (!response.ok) {
    const record = data && typeof data === 'object' ? (data as Record<string, unknown>) : {}
    const message =
      typeof record.message === 'string' ? record.message : response.statusText || 'Request failed'
    const code = typeof record.error === 'string' ? record.error : 'request_failed'
    const retryAfter = Number(response.headers.get('Retry-After') ?? 0) || 0
    throw new ApiError(message, response.status, code, retryAfter)
  }

  return data as T
}
