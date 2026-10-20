# Contributing to UvNVR

Thanks for helping. UvNVR is built milestone by milestone; please keep pull requests focused
and describe which milestone/requirement they serve.

## Before you start

- Read [`docs/development-plan.md`](docs/development-plan.md) and the relevant requirements
  in [`docs/plan.md`](docs/plan.md).
- For a large feature, open an issue first so the design can be agreed before code.
- Follow the [Code of Conduct](CODE_OF_CONDUCT.md).
- Report security issues privately via [SECURITY.md](SECURITY.md), not in a public issue.

## Development setup

### Backend

```bash
# libuvcpp 1.8.1 release archive
curl -fL -o /tmp/libuvcpp.zip \
  https://github.com/Antruly/libuvcpp/releases/download/v1.8.1/libuvcpp-1.8.1-linux-x64.zip
mkdir -p "$HOME/.local/uvcpp-1.8.1"
unzip -q /tmp/libuvcpp.zip -d "$HOME/.local/uvcpp-1.8.1"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DUVNVR_LIBUVCPP_ROOT="$HOME/.local/uvcpp-1.8.1/libuvcpp-1.8.1-linux-x64"
cmake --build build -j
./build/uvnvr --port 8080
```

C++20 is required. Keep the public module boundaries in `include/uvnvr/` and avoid blocking
the libuv event loop: FFmpeg RTSP reads, disk writes and database work belong in their
dedicated workers.

### Frontend

```bash
cd frontend
npm ci
npm run dev        # http://127.0.0.1:5173, proxies to :8080
npm run build      # typecheck + production build
npm run typecheck
```

The frontend is Vue 3 + TypeScript. Keep UI strings in `src/i18n` (Chinese and English), not
hard-coded in components.

## Style

- Format C++ with `clang-format` (`.clang-format` in the repo).
- Keep lines within 100 columns; use 2-space indentation.
- Prefer `std::string_view`, `std::span`, `std::chrono` and RAII.
- Do not add a dependency with a license incompatible with MIT/LGPL distribution. GPL
  codecs must stay optional and must not enter official release builds.
- Frontend: strict TypeScript; no `any` unless justified; keep components small.

## Commits and pull requests

- Use [Conventional Commits](https://www.conventionalcommits.org/):
  `feat:`, `fix:`, `docs:`, `build:`, `ci:`, `refactor:`, `test:`, `chore:`.
- Sign off every commit (Developer Certificate of Origin):
  `git commit -s -m "feat: add ..."`.
- Keep the PR limited to one concern. Include:
  - what changed and why,
  - which milestone/requirement it addresses,
  - how it was tested (commands, hardware/camera models if relevant),
  - screenshots for UI changes,
  - the license of any new dependency.
- CI must pass. Do not force-push over review comments; add commits and let the reviewer
  request a squash if needed.
- By contributing, you agree that your contribution is licensed under the MIT License and
  can be reused in the UVP template ecosystem under MIT.

## Upstream collaboration

UvNVR uses [uvp](https://github.com/Antruly/uvp) and
[libuvcpp](https://github.com/Antruly/libuvcpp). Reusable modules and fixes are welcome
upstream:

- Backend modules: authentication/RBAC, audit log, system settings, channel CRUD skeleton.
- Frontend modules: collapsible sidebar + cached tabs, channel tree, video grid, permission
  matrix.
- libuvcpp gaps: streaming response backpressure, WebSocket binary broadcast, HTTP Range,
  async file helpers. Add a local adapter first, then open an upstream issue/PR with the
  evidence.

Please do not vendor a private fork of libuvcpp into this repository.
