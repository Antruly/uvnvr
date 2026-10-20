<div align="center">

<img src="resources/branding/uvnvr-256.png" alt="UvNVR" width="120" />

# UvNVR

**面向 ONVIF 与 RTSP 摄像头的开源软件录像机。**

ONVIF 发现 · RTSP 接入 · 计划录像 · 无缝回放 · RTSP 输出 · Web 管理端

[![CI](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![libuvcpp](https://img.shields.io/badge/libuvcpp-1.8.1-2c3e50)](https://github.com/Antruly/libuvcpp)
[![Vue 3](https://img.shields.io/badge/Vue-3-42b883)](https://vuejs.org/)
[![Status](https://img.shields.io/badge/status-M0%20scaffolding-fbbf24)](#状态)

[English](README.md) · [简体中文](README.zh-CN.md)

</div>

---

## 状态

**M0 — 工程底座。** 仓库、许可证/专利政策、libuvcpp 服务、Vue 3 壳层、品牌图标和 Linux CI
冒烟测试已经就绪；摄像头、录像与回放功能尚未实现。里程碑见
[`docs/development-plan.md`](docs/development-plan.md)。

## UvNVR 要做什么

UvNVR 是一套面向中小规模摄像头部署的软件 NVR。单进程完成 ONVIF 发现、RTSP 拉流、一路摄像头
向多客户端分发、关键帧对齐的 MPEG-TS 切片录像、SQLite 索引、无缝回放以及 RTSP 对外输出。

### 0.1.0 规划范围

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
- Linux：`libssl-dev`、`libsqlite3-dev`、`zlib1g-dev`（Release 归档已自带依赖；从源码构建
  libuvcpp 时需要）

M0 暂不需要 FFmpeg 开发库，M2 接入媒体核心时再加入。

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

./build/uvnvr --port 8080
```

浏览器打开 <http://127.0.0.1:8080>；健康检查 <http://127.0.0.1:8080/healthz>。

前端开发时在 `frontend/` 执行 `npm run dev`，Vite 会把 `/api`、`/healthz`、`/ws` 代理到
`127.0.0.1:8080`。

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `UVNVR_LIBUVCPP_ROOT` | 空 | libuvcpp Release 归档目录（走 pkg-config） |
| `UVNVR_BUILD_TESTS` | `OFF` | 构建单元测试 |
| `UVNVR_EMBED_WEB` | `OFF` | 将 `frontend/dist` 嵌入二进制（后续里程碑） |

## 许可证

UvNVR 源代码使用 [MIT License](LICENSE)。

官方构建动态加载 **LGPL-2.1-or-later 的 FFmpeg 共享库**。FFmpeg 不得使用
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
> 并在运行时动态加载其共享库。

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
| M1 | 配置、SQLite、首次安装、鉴权/RBAC、锁定、审计、仪表盘 |
| M2 | 通道、ONVIF 发现、RTSP 拉流、实时预览、抓拍 |
| M3 | 录像、切片、索引、存储策略、回放、预读 |
| M4 | RTSP Server、API、预警、GPU/性能、系统设置 |
| M5 | 六平台安装包与 CI 矩阵 |
| M6 | 稳定性/安全加固、文档、发布 `v0.1.0` |

## 贡献与安全

- 提交 PR 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。
- 安全漏洞请通过 GitHub Security Advisories 私下报告，见 [SECURITY.md](SECURITY.md)。
- 参与贡献即表示同意 [Code of Conduct](CODE_OF_CONDUCT.md)，并使用 DCO 签名
  （`git commit -s`）。

## 致谢

- [libuvcpp](https://github.com/Antruly/libuvcpp) — MIT，事件驱动 C++ Web 框架。
- [uvp](https://github.com/Antruly/uvp) — MIT，libuvcpp 服务脚手架与验证工具。
- [FFmpeg](https://ffmpeg.org/) — LGPL-2.1-or-later，媒体工具链。
- [Vue 3](https://vuejs.org/)、[Vite](https://vite.dev/)、[Naive UI](https://www.naiveui.com/)、
  [ECharts](https://echarts.apache.org/) — 前端技术栈。
- [Desk2ONVIF](https://github.com/Antruly/Desk2ONVIF) — ONVIF Profile S 行为与虚拟摄像头测试的
  重要参考。
