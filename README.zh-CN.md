<div align="center">

<img src="resources/branding/uvnvr-256.png" alt="UvNVR" width="120" />

# UvNVR

**面向 ONVIF 与 RTSP 摄像头的开源软件录像机。**

ONVIF 发现 · RTSP 接入 · 计划录像 · 无缝回放 · RTSP 输出 · Web 管理端

[![CI](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![libuvcpp](https://img.shields.io/badge/libuvcpp-1.8.1-2c3e50)](https://github.com/Antruly/libuvcpp)
[![Vue 3](https://img.shields.io/badge/Vue-3-42b883)](https://vuejs.org/)
[![Status](https://img.shields.io/badge/status-0.2.0-22c55e)](#状态)

[English](README.md) · [简体中文](README.zh-CN.md)

</div>

---

## 状态

**0.2.0 — 可配置部署与更快回放。** 支持 ONVIF 发现/探测、通道管理、RTSP
接入与自动重连、多客户端实时预览、计划录像、MPEG-TS 切片与 SQLite 索引、回放、RTSP
输出、抓拍、预警、邮件通知、用户/角色、审计与中英文 Web 界面。

0.2.0 新增启动 JSON 配置文件（HTTP/RTSP 端口、绑定地址、数据目录、TLS）、Windows
安装器端口选择、`/api/v1/system/config`、时间轴点击跳转与 0.5×–8× 倍速 /
−30s、−10s、+10s、+30s 快退快进。WebRTC、云平台与高级分析仍不在范围内。里程碑见
[`docs/development-plan.md`](docs/development-plan.md)。

## UvNVR 要做什么

UvNVR 是一套面向中小规模摄像头部署的软件 NVR。单进程完成 ONVIF 发现、RTSP 拉流、一路摄像头
向多客户端分发、关键帧对齐的 MPEG-TS 切片录像、SQLite 索引、无缝回放以及 RTSP 对外输出。

### 当前已实现

- 首次初始化、scrypt 密码、会话、CSRF、用户/角色与 API Token。
- ONVIF WS-Discovery/探测与手动 RTSP；海康/大华路径模板。
- 通道树、主/子码流、AES-256-GCM 摄像头凭据加密。
- 1/4/8/16/32 与自定义预览网格、布局保存、水印、离线覆盖。
- 每通道每码流一个 FFmpeg 拉流进程；TS 共享分发给 WebSocket 客户端。
- 计划录像、关键帧对齐切片、按时间/大小切片、循环/预警存储卷。
- 回放时间轴、跨切片 WebSocket 播放、预读与切片下载。
- RTSP 输出（`/live/{channel}/{main|sub}`，RTP/MP2T，TCP/UDP）与 Basic 鉴权。
- 抓拍、移动/离线/存储预警、SMTP 邮件（QQ 预设）、审计日志。
- 性能档位、GPU 探测与开关、系统时间 API。
- 中英文界面、多标签缓存、仪表盘、帮助手册与 API Token。
- 启动 `uvnvr.json`（端口/绑定地址/数据目录/TLS），命令行/环境变量/配置文件优先级与
  `--print-config`；Windows 安装器端口页，Linux/macOS 服务配置。
- 回放 0.5×–8× 倍速、±10s/±30s 快退快进、时间轴点击跳转与播放位置游标。

### 已交付范围

- ONVIF WS-Discovery 与一键添加；手动 RTSP；海康/大华/URL 模板。
- 通道树、分组、主/子码流、掉线重连、在线/离线历史、配置同步。
- 1 / 4 / 8 / 16 / 32 与自定义预览布局、拖动缩放、布局保存、水印。
- 定时 + 预警录像，按时间/大小切片，每卷循环或录满预警。
- MPEG-TS 切片 + SQLite 索引；环形缓存写入；Linux io_uring；崩溃恢复。
- 回放时间轴、拖动定位、预读、LRU 缓存、跨切片无缝播放、下载。
- RTSP Server（`/live/{channel}/{main|sub}`）、REST API、抓拍、预警与邮件。
- 用户/角色/权限、首次安装设置 admin、登录/API 锁定、审计日志。
- 性能档位、GPU 探测、NTP/浏览器时间同步、运维模式。
- Windows x64/arm64（MinGW）、Linux x64/arm64（deb/rpm）、macOS x64/arm64 安装包。
- 中英文界面，自动识别浏览器语言，可手动切换。

完整需求见 [`docs/plan.md`](docs/plan.md)，架构、里程碑与发布计划见
[`docs/development-plan.md`](docs/development-plan.md)。

## 架构概览

```
浏览器（Vue 3）
   │ HTTPS / REST / WebSocket(MPEG-TS)
libuvcpp 1.8.1 服务
   ├─ HTTP/2、WebSocket、REST API、鉴权、审计
   ├─ RTSP Server（对外输出）
   ├─ ONVIF 发现/客户端
   ├─ MediaCore：每通道一路 RTSP 拉流 → 时间戳归一化 → TS Muxer
   │    ├─ LiveHub（拥塞时丢到关键帧）
   │    ├─ RecordingSpool（可靠队列 + 背压）
   │    └─ 解码分支（抓拍 / 移动侦测）
   └─ StorageCore：切片索引、环形缓存、io_uring/uv_fs、LRU、预读
SQLite（WAL）
```

录像路径坚持 `stream copy`，录像本身不转码。FFmpeg 用于 RTSP 拉流、解复用、抓拍、可选的
兼容性转码与 MP4 导出。

## 从源码构建（M0）

### 依赖

- CMake 3.24+、C++20 编译器、`pkg-config`、`ninja` 或 `make`
- **libuvcpp 1.8.1**（Release 归档或已安装的 CMake 包）
- Node.js 22.19+ / 24.11+ / 26+ 与 npm（仅前端）
- Linux：`libssl-dev`、`libuv1-dev`、`libsqlite3-dev`、`zlib1g-dev`（libuvcpp Release
  归档已自带大部分依赖；UvNVR 自身调用 OpenSSL/libuv 时需要这些开发包，从源码构建
  libuvcpp 时也需要）

UvNVR **不链接** FFmpeg 开发库，而是把 FFmpeg 可执行文件作为隔离子进程运行。通过
`UVNVR_FFMPEG` 指定二进制，或把 `ffmpeg` 放在可执行文件旁边。发布包内置 LGPL 静态 FFmpeg。

### Linux x64 快速开始

```bash
git clone https://github.com/Antruly/uvnvr.git
cd uvnvr

# 前端
cd frontend
npm ci
npm run build
cd ..

# libuvcpp 1.8.1 Release 归档
curl -fL -o /tmp/libuvcpp.zip \
  https://github.com/Antruly/libuvcpp/releases/download/v1.8.1/libuvcpp-1.8.1-linux-x64.zip
mkdir -p "$HOME/.local/uvcpp-1.8.1"
unzip -q /tmp/libuvcpp.zip -d "$HOME/.local/uvcpp-1.8.1"

# 后端
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT="$HOME/.local/uvcpp-1.8.1/libuvcpp-1.8.1-linux-x64"
cmake --build build -j

UVNVR_FFMPEG=/path/to/ffmpeg ./build/uvnvr --port 8080 --rtsp-port 554
```

浏览器打开 <http://127.0.0.1:8080>。首次启动会跳转到 `/setup` 创建管理员账户。
健康检查 <http://127.0.0.1:8080/healthz>。

本地 HTTPS 调试可运行 `./build/uvnvr --port 8080 --tls-self-signed`，使用自动生成的本机证书。
生产环境应通过 `UVNVR_TLS_CERT` 与 `UVNVR_TLS_KEY` 提供正式证书，不要使用开发证书。

前端开发时在 `frontend/` 执行 `npm run dev`，Vite 会把 `/api`、`/healthz`、`/ws` 代理到
`127.0.0.1:8080`。

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `UVNVR_LIBUVCPP_ROOT` | 空 | libuvcpp Release 归档目录（走 pkg-config） |
| `UVNVR_BUILD_TESTS` | `OFF` | 构建单元测试 |
| `UVNVR_EMBED_WEB` | `OFF` | 将 `frontend/dist` 嵌入二进制（后续里程碑） |

## 0.2.0 已知限制

- ONVIF 探测目前只支持 `http://` 设备服务。
- 移动侦测是实验性的低分辨率帧差算法，不是 AI 识别。
- 浏览器回放使用 MSE；H.265 依赖浏览器/硬件，建议使用 H.264 子码流。
- RTSP 输出使用 RTP/MP2T（payload type 33），部分客户端可能只接受 H.264 RTP；VLC/ffplay 可用。
- macOS 安装包为 ad-hoc 签名；未配置 Developer ID 时 Gatekeeper 可能要求右键打开。
- 回退通过连续向后跳转实现（浏览器无法反向解码 GOP）；快进使用服务端限速与播放器倍速。
- 审计日志与录像共用数据盘；独立审计路径为后续增强。

## 许可证

UvNVR 源代码使用 [MIT License](LICENSE)。

官方构建内置 **LGPL FFmpeg 命令行程序**，以独立进程调用，不把 FFmpeg 代码链接进 UvNVR 二进制。FFmpeg 不得使用
`--enable-gpl` 或 `--enable-nonfree` 构建。所有随包分发 FFmpeg 的版本都会附带对应源码、
configure 参数与许可证声明，详见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
在满足 LGPL 条件的前提下，你的应用代码可以继续保持 MIT，分发包中同时包含 LGPL 的 FFmpeg。

> [!WARNING]
> **本项目不授予任何专利许可。**
>
> UvNVR 不授予、也无法授予 H.264、H.265/HEVC、AAC 或其他任何编解码格式的专利许可。MIT
> 许可证只覆盖版权，不授予专利权。
>
> 如果你自行构建或打包 **x264、x265** 或其他 GPL/涉及专利的组件，由此产生的 GPL 义务与
> 专利许可责任由你自行承担。官方 UvNVR 发布包**不包含** x264/x265，只使用 LGPL FFmpeg
> 并以独立进程运行。

## 安装包

请从 [最新 Release](https://github.com/Antruly/uvnvr/releases/latest) 下载：

| 平台 | 安装包 |
|---|---|
| Windows x64 | NSIS `setup.exe`、便携 `.zip` |
| Windows arm64 | NSIS `setup.exe`、便携 `.zip` |
| Linux x64 / arm64 | `.deb`、`.rpm`、`.tar.gz` |
| macOS x64 / arm64 | `.pkg`、`.dmg`、`.zip` |

Linux 安装到 `/opt/uvnvr`，systemd 服务以 `uvnvr` 用户运行并使用 `/var/lib/uvnvr`。
Web 默认端口 8080，首次访问完成初始化。端口、数据目录与服务细节见
[`docs/deployment.md`](docs/deployment.md)。

## 仓库结构

```
src/               C++ 服务（libuvcpp）
frontend/          Vue 3 + TypeScript + Vite
resources/         品牌图标与前端资源
docs/              需求、开发计划、架构、使用手册
packaging/         Windows/Linux/macOS 打包（M5）
scripts/           图标与发布脚本
tests/             单元/集成/E2E 测试
.github/           CI、Issue/PR 模板、仓库规则
```

## 里程碑

| 阶段 | 内容 |
|---|---|
| M0 | 仓库与规则、服务脚手架、前端壳层、图标、CI 冒烟测试 |
| M1 | ✅ 配置、SQLite、首次安装、鉴权/RBAC、锁定、审计、仪表盘 |
| M2 | ✅ 通道、ONVIF 发现、RTSP 拉流、实时预览、抓拍 |
| M3 | ✅ 录像、切片、索引、存储策略、回放、预读 |
| M4 | ✅ RTSP Server、API、预警、GPU/性能、系统设置 |
| M5 | ✅ 六平台安装包与 CI 矩阵 |
| M6 | ✅ 发布加固、中英文手册、`v0.1.0` |
| 0.2.0 | ✅ 启动配置文件与端口、安装器端口页、回放倍速/快进快退 |

## 贡献与安全

- 提交 PR 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。
- 安全漏洞请通过 GitHub Security Advisories 私下报告，见 [SECURITY.md](SECURITY.md)。
- 参与贡献即表示同意 [Code of Conduct](CODE_OF_CONDUCT.md)，并使用 DCO 签名
  （`git commit -s`）。

## 致谢

- [libuvcpp](https://github.com/Antruly/libuvcpp) — MIT，事件驱动 C++ Web 框架。
- [uvp](https://github.com/Antruly/uvp) — MIT，libuvcpp 服务脚手架与验证工具。
- [FFmpeg](https://ffmpeg.org/) — LGPL 媒体工具链，以独立进程调用。
- [Vue 3](https://vuejs.org/)、[Vite](https://vite.dev/)、[Naive UI](https://www.naiveui.com/)、
  [ECharts](https://echarts.apache.org/) — 前端技术栈。
- [Desk2ONVIF](https://github.com/Antruly/Desk2ONVIF) — ONVIF Profile S 行为与虚拟摄像头测试的
  重要参考。
