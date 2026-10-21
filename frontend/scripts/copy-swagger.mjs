// Copy the self-hosted Swagger UI assets into the Vite public directory.
// The API docs page at /api/docs loads them from /api-docs/.
import { copyFile, mkdir } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const source = join(root, 'node_modules', 'swagger-ui-dist')
const target = join(root, 'public', 'api-docs')

await mkdir(target, { recursive: true })
for (const name of [
  'swagger-ui.css',
  'swagger-ui-bundle.js',
  'swagger-ui-standalone-preset.js',
  'LICENSE',
]) {
  await copyFile(join(source, name), join(target, name))
}
