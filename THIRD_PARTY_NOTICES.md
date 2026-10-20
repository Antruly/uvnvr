# Third-party notices

UvNVR's own source code is MIT-licensed. Release packages also contain third-party
components under their own licenses. This file records the policy; a release package must
ship the exact license texts and source offer for the versions it bundles.

## Components

| Component | License | How it is used |
|---|---|---|
| [libuvcpp](https://github.com/Antruly/libuvcpp) | MIT | Web/file framework; shared library |
| [libuv](https://github.com/libuv/libuv) | MIT | Event loop (through libuvcpp) |
| [FFmpeg](https://ffmpeg.org/) | LGPL-2.1-or-later (official builds) | Shared libraries, loaded dynamically at runtime |
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

The authoritative list for a given release is the SBOM attached to that release.

## FFmpeg policy

Official UvNVR release builds follow the FFmpeg project's LGPL compliance checklist:

1. FFmpeg is configured **without** `--enable-gpl` and **without** `--enable-nonfree`.
2. FFmpeg libraries are linked **dynamically** (`.dll` / `.so` / `.dylib`); UvNVR does not
   statically link FFmpeg and does not prevent users from replacing the shared libraries.
3. The complete corresponding FFmpeg source, including any modifications, is distributed
   with the release, together with the exact `configure` line.
4. The source is hosted alongside the binary release (GitHub release assets or an equivalent
   stable URL), and the download page links to it.
5. The About page and this notice state: *This software uses libraries from the FFmpeg
   project under the LGPLv2.1.*
6. FFmpeg's own license text is included in the release package.

When UvNVR starts shipping FFmpeg, CI will fail the release if `ffmpeg -buildconf` contains
`--enable-gpl` or `--enable-nonfree`, or if the source/SBOM assets are missing.

If you prefer not to distribute FFmpeg at all, remove the bundled shared libraries and
require the operator to install a compatible LGPL FFmpeg build. Document that requirement
clearly for your users.

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
