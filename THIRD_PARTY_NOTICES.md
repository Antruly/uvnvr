# Third-party notices

UvNVR's own source code is MIT-licensed. Release packages also contain third-party
components under their own licenses. This file records the policy; a release package must
ship the exact license texts and source offer for the versions it bundles.

## Components

| Component | License | How it is used |
|---|---|---|
| [libuvcpp](https://github.com/Antruly/libuvcpp) | MIT | Web/file framework; shared library |
| [libuv](https://github.com/libuv/libuv) | MIT | Event loop (through libuvcpp) |
| [FFmpeg](https://ffmpeg.org/) | LGPL-2.1-or-later; bundled builds use the LGPLv3 option | CLI subprocess, not linked into UvNVR |
| [SQLite](https://sqlite.org/) | Public domain | Configuration, index, audit database |
| [OpenSSL](https://www.openssl.org/) | Apache-2.0 | TLS and cryptography |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Application logging (introduced with the backend modules) |
| [Vue 3](https://vuejs.org/) | MIT | Frontend |
| [Vue Router](https://router.vuejs.org/) | MIT | Frontend routing |
| [Pinia](https://pinia.vuejs.org/) | MIT | Frontend state |
| [vue-i18n](https://vue-i18n.intlify.dev/) | MIT | Chinese/English UI |
| [Naive UI](https://www.naiveui.com/) | MIT | UI components |
| [ECharts](https://echarts.apache.org/) | Apache-2.0 | Dashboard charts |
| [mpegts.js](https://github.com/xqq/mpegts.js) | Apache-2.0 | Browser MPEG-TS/MSE live playback |
| [@vueuse/core](https://vueuse.org/) | MIT | Frontend utilities |
| [Vite](https://vite.dev/) | MIT | Frontend build |
| [TypeScript](https://www.typescriptlang.org/) | Apache-2.0 | Frontend language |
| [Swagger UI](https://github.com/swagger-api/swagger-ui) (swagger-ui-dist) | Apache-2.0 | Self-hosted API reference at `/api/docs` |

The authoritative list for a given release is the SBOM attached to that release.

## FFmpeg policy

UvNVR does not link FFmpeg into its own binary. It executes a bundled FFmpeg CLI as a
separate process. Release builds follow these rules:

1. FFmpeg is configured **without** `--enable-gpl` and **without** `--enable-nonfree`.
2. The FFmpeg executable is a separate program, not a linked library. Users can replace it
   with another compatible LGPL build.
3. The complete corresponding FFmpeg source, including any modifications, is distributed
   with the release, together with the exact `configure` line.
4. The source is hosted alongside the binary release (GitHub release assets or an equivalent
   stable URL), and the download page links to it.
5. The About page and this notice state: *This software uses a program from the FFmpeg
   project under the LGPL.*
6. FFmpeg's own license text is included in the release package.

CI fails the release if `ffmpeg -buildconf` contains `--enable-gpl` or `--enable-nonfree`, or
if the FFmpeg source and license assets are missing.

If you prefer not to distribute FFmpeg at all, remove the bundled executable and require
the operator to install a compatible LGPL FFmpeg build. Document that requirement clearly
for your users.

## Patent notice

**This project does not grant any patent license.** The MIT license covers copyright only.

No patent license is granted for H.264/AVC, H.265/HEVC, AAC, or any other codec, regardless
of whether the codec is implemented by FFmpeg, a camera, a browser, or another component.

**x264 and x265 are GPL-licensed and may be subject to third-party patent licensing.** If
you build UvNVR or an installer with x264/x265 (or any other GPL/patent-bearing component),
the resulting distribution is subject to GPL obligations and to any applicable patent
licenses. You are responsible for obtaining those licenses and for complying with the GPL.
Official UvNVR release builds do not include x264/x265.

Patent law differs by jurisdiction and by how a product is distributed. This notice is
informational, not legal advice; consult qualified counsel before commercial distribution.
