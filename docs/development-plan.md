# UvNVR 软录像机 — 开发计划与技术方案（v0.1.0）

- 文档状态：**待确认（Approval Required）**
- 日期：2026-10-20
- 需求依据：`docs/plan.md` + 2026-10-20 补充要求
- 目标版本：`0.1.0`
- 目标平台：Windows x64 / arm64（MinGW）、Linux x64 / arm64（deb + rpm）、macOS x64 / arm64

> 本文只做方案与排期确认，不开始写业务代码。确认后按里程碑顺序开发，每个里程碑都必须“可构建、可运行、可验收”。

---

## 0. 决策摘要（需要你确认）

| 项目 | 建议方案 | 备选 / 说明 |
|---|---|---|
| 产品名 / 仓库 | **UvNVR** / `Antruly/uvnvr`（public） | 当前 GitHub 账号为 `Antruly`，同名仓库尚不存在 |
| 开源协议 | **MIT**，运行时动态加载 LGPL FFmpeg 共享库，不打包 GPL 编解码器 | 若必须内置 x264/x265 做纯 CPU 转码，则整体改为 **GPL-3.0**；本项目不授予任何编解码专利许可 |
| 0.1.0 实时预览 | **WebSocket + MPEG-TS + mpegts.js/MSE**，同时提供 **RTSP 对外输出** | WebRTC 放 0.2，0.1.0 先保证兼容性与多画面能力 |
| 录像文件格式 | 内部统一 **MPEG-TS 切片** + SQLite 索引；下载/导出时转 MP4 | TS 崩溃恢复好、可流式拼接、适合无缝回放 |
| 存储 I/O | 内存环形缓存 + 批量顺序写；Linux 用 **io_uring**，其余平台用 libuvcpp 异步文件接口 | 控制面文件操作仍走 libuvcpp；录像热路径单独优化 |
| 前端 | **Vue 3 + TypeScript + Vite + Pinia + Naive UI + ECharts + vue-i18n** | 深色为主，桌面优先，中英文自动/手动切换 |
| 默认首页 | **仪表盘 Dashboard**，第二页为实时预览 | 按你最新补充要求，覆盖原需求文档中的“默认预览页” |
| 默认端口 | HTTP `8080`、HTTPS `8443`、RTSP `554`（无权限时回退 `8554`） | 安装后可改 |
| 0.1.0 范围 | 核心功能全部可用；人脸/车牌、本地人形 AI、GPU 转码、HTTP/3 作为实验特性 | 详见第 1 节 |

---

## 1. v0.1.0 交付范围

### 1.1 必须交付（Definition of Done）

1. **摄像头接入**
   - ONVIF 自动发现、设备信息/Profiles/StreamUri 获取、一键添加。
   - 手动 RTSP 接入；海康、大华、通用 RTSP 路径模板；可配置 URL 模板。
   - 主码流/子码流；子码流可关闭；通道增删改查、启停、分组、树形展示。
   - 掉线检测、指数退避重连、在线/离线时间与历史、最后错误、码率/帧率/分辨率。
   - 通道配置一键同步；录像计划一键同步。

2. **实时预览**
   - 1/4/8/16/32 画面 + 自定义网格；网格拖动调整大小；布局保存、多布局标签切换。
   - 通道树拖入画面；在线双击全屏；离线显示通道名和“设备已掉线”，双击打开摄像头管理页。
   - 每通道水印文字与拖动位置（预览/抓拍叠加；烧录进录像为可选增强）。
   - 多客户端并发预览：通道级共享分发、每客户端独立背压、慢客户端丢帧/断流不影响他人。
   - 实时性优先：拥塞时丢旧帧、对齐关键帧、必要时降帧/降码率。

3. **录像与存储**
   - 一周录像计划（7 × 48 格，15/30 分钟粒度），拖动绘制定时录像/预警录像；计划同步。
   - 按时间或按文件大小切片；关键帧对齐；每个切片写 SQLite 索引。
   - 多存储卷/路径；每卷独立配置循环录制或录满预警；空间阈值、保留天数。
   - 存储满 Web 提示 + 邮件预警；循环录制自动清理最旧切片。
   - 写入：异步 I/O + 内存环形缓存 + 批量顺序写；Linux io_uring 可开关；崩溃后可重建索引。
   - 视频文件默认不可手动删除；仅运维模式开放删除、日志删除、序列号/版本号修改、整盘格式化。

4. **回放**
   - 按通道/日期/时间范围查询录像时间轴；拖动进度条优先加载对应时间范围。
   - 独立预读模块：当前切片接近末尾时提前加载下一片；跨文件无缝衔接。
   - 回放/下载缓存命中直接返回，不回盘；LRU + 顺序预读；可配置缓存大小与预读阈值。
   - 倍速、暂停、逐帧（可选）、抓拍、片段下载；下载支持 Range。

5. **对外能力**
   - RTSP Server：`rtsp://host:554/live/{channel}/{main|sub}`，Basic/Digest 鉴权，主/子码流。
   - REST API：登录、通道、抓拍、预览地址、录像查询、下载、预警、系统信息等；OpenAPI 文档。
   - API 权限与 Web 功能权限分离；API 失败锁定独立配置。
   - WebSocket 实时流/状态推送；短期 stream ticket，避免把密码放进 URL。
   - 抓拍：JPEG，支持宽高/质量参数；优先硬件解码，失败回退 CPU。

6. **系统与安全**
   - 首次启动 Web 设置 admin 密码；多用户、角色、细粒度权限勾选。
   - Web 登录失败 10 次锁定 NVR 10 分钟（均可配置）；API 失败 100 次锁定（可单独配置）。
   - 审计日志：分类、筛选、分页、导出；默认保留 180 天；路径可配置；自动清理。
   - 版本号、序列号、构建信息、系统时间；NTP 同步；浏览器时间同步；页面时钟逐秒更新。
   - 启动时按 CPU 核心数/内存自动定性能档位；低配默认关闭子码流。
   - GPU 自动识别（NVIDIA/Intel/AMD/Apple）与 FFmpeg 命令自适应；可显式关闭，回退 CPU。
   - 运维模式（启动参数）开放高危操作；所有高危操作二次确认 + 审计。
   - 中英文 UI，浏览器语言自动识别 + 手动切换；左侧可收起菜单、多标签缓存页、切换不丢状态。

7. **预警**
   - 画面移动侦测（子码流低帧率采样，支持灵敏度/区域/冷却时间）。
   - 预警录像（预录 + 后录）、Web 实时提示、预警中心、邮件通知（含 QQ 邮箱预设）。
   - 摄像头离线、存储将满、磁盘异常、服务异常通知。

8. **交付物**
   - Windows x64 / arm64：MinGW 构建，NSIS `.exe` 安装包 + 便携 `.zip`。
   - Linux x64 / arm64：`.deb`、`.rpm`、`.tar.gz`。
   - macOS x64 / arm64：`.pkg` / `.dmg` + `.zip`。
   - GitHub Actions 全矩阵构建通过；`v0.1.0` Release 附带全部安装包、二进制包、`SHA256SUMS`、SBOM。
   - README（中英）、架构/API/部署/使用手册/FAQ、贡献指南、安全策略、行为准则、Issue/PR 模板。
   - 统一品牌图标：前端 favicon、页面 Logo、安装包图标一致。

### 1.2 实验性能力（0.1.0 带开关，不阻塞发布）

- 本地人形检测（ONNX Runtime 可选模块）；0.1.0 先支持 ONVIF 摄像头自带的移动/人形事件订阅。
- 人脸识别、车牌识别：预留分析插件接口与 UI 开关，0.1.0 不承诺识别准确率。
- H.265 浏览器播放不可用时的服务端 H.264 转码（有硬件编码器时启用；纯 CPU 转码取决于构建选项）。
- HTTP/3（libuvcpp 已支持）：编译进来，运行时默认关闭，可配置开启。
- 多路自定义网格模板导入/导出。

### 1.3 0.1.0 明确不做

- WebRTC 超低延迟输出（0.2 规划）。
- 手机 App、云平台、P2P 穿透、多机集群。
- 越界/区域入侵、人流统计等高级视频分析。
- 自动更新、远程升级、云备份。
- 非本机存储（NFS/SMB 作为录像主存储不推荐，仅允许本地盘/挂载良好的块设备）。

---

## 2. 技术选型

| 层次 | 选择 | 说明 |
|---|---|---|
| 语言 | C++20 | 兼顾现代特性与三大平台编译器；libuvcpp 为 C++11 ABI，兼容 |
| Web / 文件框架 | **libuvcpp 1.8.1** | HTTP/1.1、HTTP/2、HTTP/3、WebSocket、TLS、SOAP/WSDL、SQLite DB、多 loop |
| 开发脚手架 | **uvp-agents 0.4.0** | `uvp` CLI + MCP；生成 rest-api + vue3 工作区，构建/运行/验证 |
| 流媒体 | FFmpeg 7.x（libavformat/libavcodec/libavutil/libswscale） | RTSP 拉流、解复用、TS 封装、解码、抓拍、转码、MP4 导出 |
| 数据库 | SQLite 3.45+（WAL） | 配置、通道、计划、录像索引、用户、审计；热路径单独写线程 |
| 加密 | OpenSSL 3.x | TLS、AES-GCM 配置加密、scrypt 口令哈希、RTSP Digest |
| 高性能文件 I/O | liburing（Linux 可选） | io_uring 批量写；其他平台 uv_fs/平台异步 I/O |
| JSON / 日志 | nlohmann/json、spdlog | 配置、API、结构化日志、轮转 |
| 测试 | GoogleTest + Playwright + MediaMTX（仅测试夹具） | 单元/集成/E2E；MediaMTX 不作为发布依赖 |
| 前端 | Vue 3.5 + TypeScript + Vite + Pinia + Vue Router + Naive UI | 管理端 SPA；深色主题；组件按需引入 |
| 前端图表 | ECharts | 仪表盘 CPU/内存/磁盘/码率曲线 |
| 前端视频 | **mpegts.js**（MSE） | WebSocket 拉 MPEG-TS；H.264 稳定，H.265 依赖浏览器/硬件 |
| 国际化 | vue-i18n | `zh-CN` / `en-US`；错误码由后端返回、前端翻译 |
| 打包 | CMake + CPack + NSIS + systemd + pkgbuild | 6 个目标平台 |
| CI/CD | GitHub Actions | 构建、测试、打包、Release、校验和、SBOM |

### 2.1 为什么这样选

- **libuvcpp + uvp** 满足“全异步事件驱动 + 快速搭建标准 Web 框架”的约束，且已有 REST/WebSocket/SOAP/DB 能力。
- **FFmpeg 只做流处理，不自己做 RTSP 协议栈**：摄像头兼容性最好；录像始终 `stream copy`，不转码，CPU 开销可控。
- **MPEG-TS 切片**：天然适合边写边播、断电恢复、跨文件拼接；浏览器用 MSE 播放，VLC/ffplay 也能直接播放。
- **通道级共享流 + 每客户端独立队列**：同一摄像头只拉一路，多客户端不重复占用摄像头带宽。
- **时间戳归一化**：把摄像头 PTS 映射到通道单调时间轴，切片文件可直接首尾拼接，回放不跳变。
- **自研 RTSP Server**：对外输出是 NVR 的硬需求；libuvcpp 提供 TCP/UDP，RTP/RTCP 自行实现，避免引入第二套事件循环。

---

## 3. 总体架构

```
┌──────────────────────────────── Browser (Vue 3 SPA) ────────────────────────────────┐
│ 仪表盘 / 预览网格 / 回放 / 通道 / 计划 / 存储 / 预警 / 用户 / 日志 / 系统 / 关于帮助  │
│ HTTPS REST + WebSocket(MPEG-TS) + 下载(HTTP Range)                                   │
└───────────────────────────────────────┬──────────────────────────────────────────────┘
                                        │
┌───────────────────────────────────────▼──────────────────────────────────────────────┐
│ libuvcpp 1.8.1 进程（事件循环，多 loop）                                              │
│  ├─ HTTP/1.1 + HTTP/2 + HTTPS + 可选 HTTP/3 + 静态资源/SPA                           │
│  ├─ REST API / 鉴权 / RBAC / 审计 / 系统信息 / 计划调度                              │
│  ├─ WebSocket：实时流、回放流、状态/预警推送                                          │
│  ├─ RTSP Server :554（DESCRIBE/SETUP/PLAY/TEARDOWN，TCP interleaved + UDP）           │
│  ├─ ONVIF：WS-Discovery + SOAP 客户端                                                │
│  └─ 存储服务：卷管理、索引查询、LRU 缓存、预读、下载                                  │
└───────────────┬───────────────────────────────┬──────────────────────────────────────┘
                │ 线程安全队列 / uv_async       │ 异步 SQLite / io_uring
┌───────────────▼──────────────────┐   ┌────────▼─────────────────────────────────────┐
│ MediaCore（每通道/每码流）        │   │ StorageCore                                   │
│  FFmpeg RTSP 拉流线程（阻塞隔离）  │   │  SegmentWriter：环形缓存 + 批量顺序写         │
│   → AVPacket 分发                 │   │  SegmentIndex：SQLite 索引                    │
│   → 时间戳归一化                  │   │  VolumeManager：容量/循环/预警                │
│   → 通道级 TS Muxer               │   │  PlaybackReader：LRU + 预读 + 跨文件拼接      │
│      ├─ LiveHub 环形缓存（可丢帧） │   │  io_uring（Linux）/ uv_fs（Win/macOS）        │
│      ├─ RecordingSpool（不丢）     │   └──────────────────────────────────────────────┘
│      └─ Decoder Tap（抓拍/移动侦测）│
└───────────────┬──────────────────┘
                │
┌───────────────▼──────────────────┐
│ SQLite（WAL）：配置、索引、用户、审计 │
└──────────────────────────────────┘
```

### 3.1 线程与异步模型

| 执行单元 | 数量 | 职责 |
|---|---|---|
| libuv loop | `min(4, max(1, cores/2))` | HTTP/API/WebSocket/RTSP 信令、定时器、状态推送 |
| 媒体拉流线程 | 每通道每码流 1 个 | FFmpeg RTSP 阻塞读；通过中断回调实现超时/取消 |
| 通道处理 | 跟随拉流线程或独立 1 个 | 时间戳归一化、包分发、TS 封装 |
| 磁盘写线程 | 每存储卷 1 个 | 消费环形缓存、批量写、fsync、切片索引提交 |
| 解码工作池 | 2~4 个（按性能档位） | 抓拍、移动侦测、转码；不阻塞事件循环 |
| SQLite 写线程 | 1 个 | 索引/审计批量事务；读走连接池 |
| 定时任务 | loop timer | 计划评估、重连、保留清理、NTP、健康检查 |

**关键约束**：libuv 事件循环内绝不调用阻塞的 FFmpeg/文件/数据库 API。FFmpeg 的 RTSP 读放在隔离线程中，这是“全异步控制面 + 隔离媒体线程”的务实实现；API、WebSocket、存储、回放路径本身均为异步。

### 3.2 媒体数据流（核心）

```
RTSP 摄像头
  │  FFmpeg av_read_frame（线程内阻塞）
  ▼
AVPacket（引用计数）
  ├─ 时间戳归一化：camera PTS → channel monotonic PTS/DTS
  ├─ 关键帧/SPS/PPS 跟踪
  ▼
通道级 MPEG-TS Muxer（每路一路，输出连续 TS 字节流）
  ├─ LiveHub：环形缓存，保留最近 GOP + PAT/PMT；新客户端从关键帧接入
  ├─ RecordingSpool：可靠队列，不丢帧；磁盘慢时反压拉流
  └─ Decoder Tap：按需解码最新关键帧（抓拍/移动侦测）
```

- **一路摄像头只拉一次**，所有预览客户端、录像、抓拍、RTSP 输出共享。
- **直播可丢、录像不丢**：LiveHub 每个客户端独立有界队列，拥塞时丢到下一个关键帧；RecordingSpool 不丢，满了就反压摄像头读取并告警。
- **时间戳归一化**：摄像头断线重连后，新的 PTS 接续上一段末尾，保证跨切片连续；原始 PTS 仅用于排查。
- **通道级 TS Muxer**：录像与直播使用同一份 TS 字节，避免重复封装；切片边界由关键帧和字节偏移决定。

### 3.3 存储抽象（应用层不关心缓存/磁盘）

```cpp
// 应用层只依赖这些接口；缓存、预读、io_uring 均在 StorageCore 内部
class IStorageReader {
public:
  virtual void read_range(SegmentId id, uint64_t offset, size_t len,
                          ReadCallback cb) = 0;
  virtual void open_timeline(ChannelId ch, int64_t start_ms, int64_t end_ms,
                             TimelineCallback cb) = 0;
  virtual ~IStorageReader() = default;
};

class IStorageWriter {
public:
  virtual AppendResult append(ChannelId ch, StreamKind kind,
                              std::span<const uint8_t> ts_bytes,
                              int64_t pts_ms, bool keyframe) = 0;
  virtual void flush(FlushCallback cb) = 0;
  virtual ~IStorageWriter() = default;
};
```

回放/下载/预览统一走 `IStorageReader`；命中 LRU 块缓存直接回调，未命中才走 io_uring/uv_fs。预读模块对上层同样透明。

---

## 4. 仓库结构

```
uvnvr/
├─ CMakeLists.txt                 # 顶层构建
├─ CMakePresets.json              # linux/windows/macos 预设
├─ cmake/                         # 依赖查找、打包、图标生成
├─ include/uvnvr/                 # 公共头文件（模块边界）
├─ src/
│  ├─ main.cpp                    # 启动、CLI、服务模式、运维模式
│  ├─ app/                        # 生命周期、配置、依赖装配
│  ├─ common/                     # Result/Error、时间、UUID、序列化、加密
│  ├─ web/                        # uvcpp 路由、中间件、静态资源、WebSocket
│  ├─ api/                        # REST v1 处理器、DTO、OpenAPI
│  ├─ auth/                       # 用户、角色、会话、API Token、锁定
│  ├─ audit/                      # 审计日志、保留清理、导出
│  ├─ system/                     # 系统信息、NTP、性能档位、GPU 探测
│  ├─ channels/                   # 通道模型、CRUD、同步、健康/重连
│  ├─ onvif/                      # WS-Discovery、SOAP 客户端、Profiles
│  ├─ media/                      # RTSP 拉流、包分发、TS Muxer、解码、抓拍
│  ├─ rtsp/                       # RTSP Server、SDP、RTP/RTCP、鉴权
│  ├─ recording/                  # 计划引擎、切片器、索引、保留策略
│  ├─ storage/                    # 卷管理、环形缓存、io_uring、LRU/预读
│  ├─ playback/                   # 时间轴、回放会话、MP4 导出
│  ├─ alerts/                     # 移动侦测、事件、邮件、通知
│  └─ db/                         # 迁移、Repository、事务
├─ frontend/                      # Vue 3 + Vite
│  ├─ src/
│  │  ├─ layouts/                 # 左菜单、顶栏、多 Tab
│  │  ├─ views/                   # dashboard/live/playback/channels/...
│  │  ├─ components/              # VideoGrid、Timeline、ChannelTree、...
│  │  ├─ stores/                  # Pinia
│  │  ├─ api/                     # REST/WS 客户端、错误码
│  │  ├─ i18n/                    # zh-CN、en-US
│  │  └─ styles/
│  └─ package.json
├─ resources/
│  ├─ branding/                   # SVG/PNG/ICO/ICNS 源与生成物
│  ├─ web/                        # 前端构建产物（Release 嵌入二进制）
│  └─ help/                       # 使用手册、FAQ（中英）
├─ packaging/
│  ├─ windows/uvnvr.nsi
│  ├─ linux/uvnvr.service、postinst、prerm
│  └─ macos/                      # pkg/launchd/dmg 脚本
├─ scripts/                       # 依赖构建、图标生成、发布校验
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  ├─ fixtures/                   # ONVIF XML、TS 样本、假摄像头配置
│  └─ e2e/                        # Playwright
├─ docs/
│  ├─ plan.md                     # 需求
│  ├─ development-plan.md         # 本文
│  ├─ architecture.md
│  ├─ api.md
│  ├─ deployment.md
│  ├─ user-guide.md / user-guide.zh-CN.md
│  └─ faq.md
├─ .github/
│  ├─ workflows/
│  ├─ ISSUE_TEMPLATE/
│  ├─ PULL_REQUEST_TEMPLATE.md
│  └─ dependabot.yml
├─ .clang-format / .clang-tidy / .editorconfig / .gitignore / .gitattributes
├─ LICENSE / README.md / README.zh-CN.md
├─ CHANGELOG.md / CONTRIBUTING.md / CODE_OF_CONDUCT.md / SECURITY.md
└─ VERSION
```

---

## 5. 核心模块设计

### 5.1 通道、ONVIF 与重连

- **发现**：WS-Discovery `Probe`（UDP 239.255.255.250:3702）→ 解析 `XAddrs` → 去重；可选局域网 RTSP 端口探测（需用户确认，限制并发，避免扫描器行为）。
- **接入**：ONVIF `GetCapabilities` → `GetDeviceInformation` → `GetProfiles` → `GetStreamUri`；抓拍地址 `GetSnapshotUri`。
- **品牌模板**：海康 `Streaming/Channels/101|102`，大华 `cam/realmonitor?channel=1&subtype=0|1`，通用模板占位符 `{user}/{pass}/{ip}/{port}/{channel}`；凭据 URL 编码。
- **状态机**：`disabled → connecting → online → reconnecting → offline/error`；记录每次上线/离线时间、原因、重连次数。
- **重连**：指数退避（1s → 2s → 5s → 10s → 30s 上限）+ 抖动；看门狗检测“连接在但无数据”；可手动立即重连。
- **配置同步**：选择字段级同步（协议/码流/媒体参数/水印/计划），先 diff 预览再提交。
- **媒体参数**：通过 ONVIF `SetVideoEncoderConfiguration`（支持时）设置分辨率/帧率/码率/编码；不支持时只读并提示。

### 5.2 媒体核心与多客户端分发

- 每通道主/子码流各一个 `StreamSession`；一路 RTSP 连接。
- `PacketHub`：引用计数 AVPacket，订阅者（录像、直播、RTSP、解码）各自有界队列。
- `LiveHub`：最近 GOP + PAT/PMT + SPS/PPS；新客户端从最近关键帧接入，无需等待下一个 GOP。
- **背压与隔离**：单客户端队列超过阈值 → 丢到关键帧；持续过慢 → 主动断开并审计；不阻塞通道。
- **实时性策略**：
  - `realtime`（默认）：直播拥塞丢弃旧帧，取最新可解码帧。
  - `balanced`：允许小幅缓冲，降低卡顿。
  - 性能不足时可配置“仅转发关键帧 + 低帧率子码流”。
- **抓拍**：解码最新关键帧为 JPEG；优先 GPU 解码，失败回退 CPU；同一通道并发抓拍合并请求。
- **移动侦测**：子码流 1–5 fps 解码 → 灰度缩小 → 分块帧差 → 阈值/区域/冷却；事件进入告警引擎。低配档位可关闭。

### 5.3 录像、切片与索引

- **切片策略**：
  - 按时间：默认 5 分钟，范围 1–60 分钟；在超过阈值后的下一个关键帧切片。
  - 按大小：默认 256 MB，范围 32–2048 MB；超过阈值后的下一个关键帧切片。
  - 每片独立文件，内含 PAT/PMT/SPS/PPS；文件不可变。
- **写入链路**：
  - `RecordingSpool` → 每卷 `SegmentWriter` → SPSC 环形缓存（容量减 1 识别头尾）→ 批量（默认 8 MB 或 500 ms）→ io_uring/uv_fs 顺序写。
  - 切片完成：写入尾部 → fsync（可配置组提交）→ 原子 rename `.part → .ts` → 批量写 SQLite 索引。
  - 崩溃恢复：启动扫描 `.part`/孤立文件，校验 TS 尾部，重建/修正索引；数据库 WAL。
- **索引字段**：通道、码流、起止时间、文件路径、字节数、时长、首/末 PTS、关键帧偏移表、编码、分辨率、帧率、录像类型（定时/预警）、卷 ID、创建时间。
- **文件命名**：`{volume}/uvnvr/{channel_id}/{yyyy}/{MM}/{dd}/{channel}_{stream}_{start_unix_ms}_{seq}.ts`。
- **保留/容量**：
  - 每卷独立策略：`loop`（循环覆盖）或 `alert`（录满停止 + 告警）。
  - 使用率阈值默认 90%，保留空间默认 5%；保留天数默认 30 天（可配）。
  - 循环清理只删除最旧切片，不删除正在写入/正在回放的切片；操作写审计。
- **磁盘管理**：枚举卷/盘符、文件系统、容量、可写性；路径必须存在且可写；拒绝系统盘根目录/关键目录。整盘格式化仅在运维模式 + 管理员 + 输入设备名确认后执行。

### 5.4 回放、预读与缓存

- 时间轴查询：`recordings` 按 `(channel_id, start_ms)` 索引；返回合并后的连续区间与空档。
- 回放会话：`open_timeline(channel, start, end)` → 定位起始切片 → 跳到不晚于起点的关键帧 → 顺序读。
- **预读模块**（`PreloadReader`，独立封装）：
  - 当前切片剩余 < 阈值（默认 2 MB 或 10 秒）时，预取下一片头部（默认 4 MB）。
  - 用户拖动进度条：取消旧预读，查询目标时间索引，优先只加载目标切片及其前一片头部。
  - 跨片边界：TS 连续，字节流直接拼接；预读确保边界无等待。
  - 下载/导出同样走预读缓存。
- **缓存**：`LruBlockCache`，块大小 64 KB；总容量按性能档位（低 64 MB / 中 256 MB / 高 1 GB），可配置；正在播放的块可 pin；缓存命中率/回源次数暴露到仪表盘。
- **导出**：TS 片段直接拼接或 FFmpeg remux 为 MP4（不转码），支持按时间裁剪；下载走 HTTP Range。

### 5.5 RTSP Server

- URL：`rtsp://host:554/live/{channel}/{main|sub}`；也支持 `/live/{channel}` 默认主码流。
- 方法：`OPTIONS`、`DESCRIBE`、`SETUP`（TCP interleaved 优先，UDP unicast 次之）、`PLAY`、`PAUSE`、`TEARDOWN`、`GET_PARAMETER`、`SET_PARAMETER`。
- 鉴权：Basic + Digest（Digest 使用单独 RTSP HA1）；权限 `rtsp.access`；失败计入 API 锁定策略。
- SDP：H.264/H.265，`sprop-parameter-sets`、`packetization-mode=1`；AAC 音频可选（0.1.0 默认只转发视频，音频后续补）。
- RTP：H.264（RFC 6184）、H.265（RFC 7798）；90 kHz 时间戳；RTCP SR 周期发送；会话超时与 keepalive。
- 多客户端：复用 `PacketHub`；新客户端从关键帧开始；慢客户端独立丢包/断开。
- 兼容性验收：VLC、ffplay、GStreamer、ONVIF Device Manager 至少三种客户端播放通过。

### 5.6 告警与邮件

- 告警规则：移动侦测、人形事件（ONVIF 事件优先）、摄像头离线、存储将满、磁盘异常、服务异常。
- 预警录像：预录 5 秒（内存环形）+ 后录 10 秒（可配）；事件切片独立索引，避免与定时录像混淆。
- 通知：WebSocket 实时推送、预警中心（未读/确认）、邮件。
- 邮件：内置 SMTP 客户端（SMTPS/STARTTLS）；QQ 邮箱预设 `smtp.qq.com:465`；支持测试发送、重试队列、频率限制、附件抓拍。
- 告警去重与冷却：同通道同类事件默认 60 秒冷却，可配。

### 5.7 用户、角色、权限与锁定

- 首次启动：`admin` 密码设置向导（复杂度校验、确认、写入审计）；未完成前其他 API 返回 `setup_required`。
- 口令：scrypt（OpenSSL）加盐哈希；会话 Cookie `HttpOnly + SameSite=Strict + Secure(HTTPS)`；CSRF Token。
- API：Basic 或 Bearer Token（Token 仅显示一次，数据库存哈希）；短期 stream ticket（30 秒）用于 WS/下载。
- 权限分组：
  - Web：仪表盘、预览、回放、下载、通道查看/管理、计划查看/管理、存储查看/管理、预警查看/管理、用户/角色、日志、系统查看/管理、运维。
  - API：`api.access`、`api.snapshot`、`api.stream`、`api.download`、`api.alerts`、`rtsp.access`。
- 锁定：
  - Web：默认 10 次失败 → 全局登录锁定 10 分钟（按需求）；同时记录来源 IP/账号。实现时增加“按 IP+账号锁定”作为更细粒度选项，避免单一来源恶意锁死全站。
  - API：默认 100 次失败 → 锁定 10 分钟（阈值/时长独立配置）。
  - 所有锁定/解锁写审计；管理员可在系统页查看剩余时间（不提供任意解锁，防绕过）。
- 高危操作：二次确认 + 重新输入密码（可选）+ 审计；运维模式外一律 403。

### 5.8 审计与日志

- 审计库：独立 `audit.db`（可配置路径），表 `audit_log`；分类：auth/channel/record/storage/api/system/alert/permission/maintenance。
- 字段：时间、类别、级别、操作人、来源 IP、User-Agent、对象类型/ID、动作、结果、错误码、详情 JSON、请求 ID。
- 保留：默认 180 天，每日 03:00 清理；可配置；清理本身写审计；手动删除仅运维模式。
- 应用日志：spdlog 轮转（单文件 50 MB × 10），路径可配置，默认数据目录 `logs/`；敏感信息（密码、Token、摄像头凭据）脱敏。
- 查询：分类/时间/用户/结果/关键字；分页；CSV 导出；导出操作审计。

### 5.9 系统信息、时间、性能档位与 GPU

- 系统信息：版本（CMake + git SHA）、构建时间、序列号、操作系统、CPU 型号/核心、内存、GPU、FFmpeg/libuvcpp 版本、运行时长。
- 序列号：首次启动生成 `UVNVR-XXXXXXXXXXXX`，持久化；运维模式可修改并审计。
- NTP：SNTP 客户端，启动同步 + 每 6 小时；服务器可配；状态（偏移、上次成功）；时间跳变时记录并调整录像时间轴。
- 浏览器同步：页面显示服务器时间（本地每秒递增，定期校准偏移）；管理员可点击“用浏览器时间校准 NVR”，调用受权接口设置系统时间；失败时提示权限/服务账户限制。
- 性能档位（启动自动检测，可覆盖）：

| 档位 | 参考环境 | 默认策略 |
|---|---|---|
| Low | ≤4 核或 ≤8 GB | 子码流默认关闭；最多 8 画面；缓存 64 MB；解码池 2；移动侦测默认关 |
| Medium | 6–8 核 / 16 GB | 最多 16 画面；缓存 256 MB；解码池 3；移动侦测开 |
| High | ≥12 核 / 32 GB | 最多 32 画面；缓存 1 GB；解码池 4；可多路转码 |

- GPU 探测：FFmpeg `av_hwdevice_iterate_types` + 编码器枚举 + 厂商探测（`nvidia-smi`、`/dev/dri`、Windows DXGI、macOS VideoToolbox）；显示设备/可用编码器；`--no-gpu` 或配置关闭后全走 CPU；运行期失败自动回退并告警一次。
- 转码仅用于：浏览器不支持的编码、低带宽子码流、烧录水印；录像永远不转码。

### 5.10 API 设计

- 前缀 `/api/v1`；JSON；统一错误结构 `{code, message, requestId, details}`；HTTP 状态语义化。
- 鉴权：Cookie 会话或 Bearer Token；写操作校验 CSRF；速率限制；所有失败计入锁定策略。
- OpenAPI 3.1：`/api/openapi.json`；Swagger UI 自托管在 `/api/docs`（仅授权用户）。
- 关键端点：

| 方法 | 路径 | 说明 | 权限 |
|---|---|---|---|
| GET | `/api/v1/setup/status` | 是否首次安装 | 公开 |
| POST | `/api/v1/setup` | 设置 admin 密码 | 首次安装 |
| POST | `/api/v1/auth/login` | 登录 | 公开 |
| POST | `/api/v1/auth/logout` | 登出 | 登录 |
| GET | `/api/v1/dashboard/summary` | 仪表盘概览 | dashboard.view |
| GET | `/api/v1/system/info` | 版本/序列号/运行信息 | system.view |
| GET/PUT | `/api/v1/system/time` | 时间/时区 | system.view/manage |
| POST | `/api/v1/system/ntp/sync` | 立即 NTP | system.manage |
| GET/PUT | `/api/v1/system/performance` | 性能档位/GPU | system.manage |
| GET | `/api/v1/channels` | 通道列表/树 | channel.view |
| POST/PUT/DELETE | `/api/v1/channels/{id}` | 通道管理 | channel.manage |
| POST | `/api/v1/channels/discover` | ONVIF 发现 | channel.manage |
| POST | `/api/v1/channels/{id}/sync` | 配置同步 | channel.manage |
| POST | `/api/v1/channels/{id}/reconnect` | 手动重连 | channel.manage |
| GET | `/api/v1/channels/{id}/snapshot.jpg` | 抓拍 | api.snapshot |
| POST | `/api/v1/streams/ticket` | 获取 WS/RTSP 临时地址 | preview.view / rtsp.access |
| WS | `/ws/live?ticket=...` | 实时 TS | preview.view |
| WS | `/ws/playback?ticket=...` | 回放 TS | playback.view |
| GET | `/api/v1/recordings` | 录像查询 | playback.view |
| GET | `/api/v1/recordings/{id}/download` | 单片下载 | playback.download |
| POST | `/api/v1/playback/export` | 时间范围导出 | playback.download |
| GET/PUT | `/api/v1/plans` | 录像计划 | plan.view/manage |
| GET/PUT | `/api/v1/storage/volumes` | 存储卷/策略 | storage.view/manage |
| POST | `/api/v1/storage/format` | 整盘格式化（运维） | maintenance |
| GET | `/api/v1/alerts` | 预警列表/确认 | alert.view |
| GET/PUT | `/api/v1/users`、`/roles` | 用户/角色 | user.manage |
| GET | `/api/v1/audit` | 审计查询/导出 | log.view |
| GET | `/healthz` | 健康检查 | 公开（最小信息） |

- RTSP 地址：`rtsp://user:pass@host:554/live/{channel}/{main|sub}`；API 返回地址时默认隐藏密码，由客户端填入。
- 抓拍/预览/下载/预警均写审计；下载支持 `Range` 与断点续传。

### 5.11 Web UI 设计

- **整体布局**：左侧可收起菜单（宽 240 / 64 px）+ 顶部栏（系统时间、语言、告警铃、用户菜单）+ 多 Tab 工作区；Tab 使用 `KeepAlive` 缓存，切换不丢滚动位置/表单/播放状态。
- **页面**：
  1. 仪表盘（默认首页）：CPU/内存/磁盘/网络、通道在线数、录像状态、存储预测、最近告警、码率曲线。
  2. 实时预览：通道树 + 网格 + 布局标签 + 工具栏；支持拖拽通道到格子、拖动分界线调整大小、自定义布局保存。
  3. 回放：通道树 + 日历 + 24 小时时间轴 + 播放器 + 倍速/抓拍/下载。
  4. 通道管理：树/表视图，新增/编辑抽屉（ONVIF/RTSP/品牌模板），发现弹窗，状态与历史。
  5. 录像计划：7 × 48 格拖动绘制；定时/预警/关闭；复制到其他通道。
  6. 存储管理：卷列表、容量条、策略、路径选择、格式化（运维）、循环/预警模式。
  7. 预警中心：列表、筛选、确认、详情（含抓拍）、规则配置。
  8. 用户与角色：用户列表、角色权限矩阵（Web/API 分组勾选）。
  9. 审计日志：筛选、分页、详情、导出。
  10. 系统设置：基础、时间/NTP、性能/GPU、安全锁定、邮件、日志、网络、运维。
  11. 关于：版本、序列号、构建信息、开源地址、依赖许可。
  12. 帮助：完整使用手册、配置引导、FAQ、全文搜索（离线索引）。
- **首次安装**：设置向导（admin 密码、时区、存储路径、是否启用 HTTPS/RTSP 端口）。
- **视觉**：深色默认、浅色可选；统一设计 token；图标与安装包一致；图表使用 ECharts；空状态/加载/错误态完整。
- **国际化**：`zh-CN`、`en-US`；默认读 `navigator.language`，手动切换持久化；后端返回错误码，前端翻译；日期/时区按用户设置。
- **播放器抽象**：`LivePlayer` 接口，0.1.0 实现 MSE/mpegts.js；预留 WebRTC/WASM 后端，便于 0.2 切换。
- **性能**：32 画面默认子码流；非可视区域延迟加载；可配置最大并发解码数；标签页隐藏时暂停/降帧。

---

## 6. 数据模型（核心表）

| 表 | 关键字段 | 说明 |
|---|---|---|
| `schema_migrations` | version, applied_at | 迁移版本 |
| `settings` | key, value_json, updated_at | 系统/安全/邮件/日志/性能配置 |
| `users` | id, username, password_hash, role_id, enabled, created_at | 用户 |
| `roles` | id, name, permissions_json, builtin | 角色/权限 |
| `sessions` | id, user_id, token_hash, ip, ua, expires_at | Web 会话 |
| `api_tokens` | id, user_id, token_hash, name, expires_at, last_used_at | API Token |
| `login_attempts` | id, kind(web/api), username, ip, success, at | 锁定统计 |
| `channels` | id, name, group_id, protocol, host, port, username, password_enc, enabled, brand, main_url, sub_url, sub_enabled, watermark_json, media_json, created_at | 通道 |
| `channel_groups` | id, name, parent_id, sort | 通道树 |
| `channel_status_events` | id, channel_id, state, reason, at | 在线/离线历史 |
| `layouts` | id, user_id, name, grid_json, is_default | 预览布局 |
| `record_plans` | id, channel_id, weekday, start_min, end_min, mode(timed/event/both), stream, enabled | 录像计划 |
| `storage_volumes` | id, path, label, fs_type, total_bytes, free_bytes, mode(loop/alert), max_usage_pct, reserve_bytes, retention_days, enabled | 存储卷 |
| `recordings` | id, channel_id, stream, volume_id, path, start_ms, end_ms, duration_ms, size_bytes, first_pts, last_pts, keyframe_index_json, codec, width, height, fps, record_type, created_at | 切片索引 |
| `recording_events` | id, channel_id, type, start_ms, end_ms, recording_id, detail_json | 预警录像/事件 |
| `alert_rules` | id, channel_id, type, enabled, config_json | 告警规则 |
| `alerts` | id, channel_id, rule_id, type, level, message, snapshot_path, acknowledged, created_at | 告警 |
| `audit_log` | id, ts, category, level, actor, ip, ua, object_type, object_id, action, result, error_code, detail_json, request_id | 审计（独立库） |
| `system_meta` | key, value | 序列号、安装 ID 等 |

索引：`recordings(channel_id, start_ms)`、`recordings(volume_id, start_ms)`、`audit_log(ts)`、`audit_log(category, ts)`、`login_attempts(kind, ip, at)`。

敏感字段（摄像头密码、SMTP 密码）使用 AES-256-GCM 加密，密钥文件 `data/secret.key`（0600）首次生成；日志与 API 永不回显明文。

---

## 7. 跨平台构建、打包与 CI

### 7.1 CMake 与依赖

- CMake ≥ 3.24，C++20；`CMakePresets.json` 提供 `linux-release`、`windows-mingw-release`、`macos-release` 等。
- 依赖查找：优先 `find_package(uvcpp CONFIG)`，回退 `pkg-config`；FFmpeg 用 `pkg-config` 或自定义 `FindFFmpeg`；SQLite/OpenSSL/zlib 同样。
- 关键开关：
  - `UVNVR_EMBED_WEB=ON`（Release 把前端嵌入二进制）
  - `UVNVR_ENABLE_IO_URING=ON`（Linux，运行时探测）
  - `UVNVR_ENABLE_HTTP3=ON`（默认编译，运行时开关）
  - `UVNVR_ENABLE_GPU=ON`
  - `UVNVR_ENABLE_X264=OFF`（默认关闭；开启则许可证切换为 GPL）
  - `UVNVR_BUILD_TESTS=ON`
- 依赖固定版本并记录 SHA256；CI 缓存；不使用系统里漂移的 FFmpeg 做发布包。

### 7.2 六目标构建矩阵

| 目标 | Runner | 编译器 | libuvcpp 获取 | 产物 |
|---|---|---|---|---|
| Windows x64 | `windows-2022` + MSYS2 | MinGW-w64 GCC (UCRT64) | 1.8.1 `mingw-x64` 归档 | NSIS `.exe` + `.zip` |
| Windows arm64 | `ubuntu-24.04` 交叉 | llvm-mingw (clang + libc++) | 1.8.1 `mingw-arm64` 归档 | NSIS `.exe` + `.zip` |
| Linux x64 | `ubuntu-24.04` | GCC 13 | 1.8.1 `linux-x64` 归档 | `.deb` + `.rpm` + `.tar.gz` |
| Linux arm64 | `ubuntu-24.04-arm` | GCC 13 | 1.8.1 `linux-arm64` 归档 | `.deb` + `.rpm` + `.tar.gz` |
| macOS x64 | `macos-15-intel` | Apple Clang | **源码构建 1.8.1**（官方无 macOS 归档） | `.pkg` + `.dmg` + `.zip` |
| macOS arm64 | `macos-15` | Apple Clang | **源码构建 1.8.1** | `.pkg` + `.dmg` + `.zip` |

- FFmpeg：Linux 源码构建 LGPL 共享库；Windows 使用固定版本 LGPL 共享构建；macOS 源码构建（VideoToolbox 开启）。所有产物随包携带或声明依赖。
- macOS libuvcpp 源码构建需 OpenSSL、nghttp2、zlib、SQLite；用 Homebrew 安装后固定版本，或 vcpkg；CI 缓存。
- macOS 签名：无 Apple Developer ID 时做 ad-hoc 签名，文档说明 Gatekeeper 右键打开；提供 secrets 后可接入签名/公证。

### 7.3 安装包行为

- **Windows（NSIS）**：安装到 `Program Files\UvNVR`；数据 `%ProgramData%\UvNVR`；注册 Windows 服务（`uvnvr.exe --service`）；防火墙放行 HTTP/HTTPS/RTSP；开始菜单/桌面快捷方式；卸载保留数据（可选删除）。
- **Linux（deb/rpm）**：`/opt/uvnvr`、`/etc/uvnvr`、`/var/lib/uvnvr`、`/var/log/uvnvr`；`uvnvr.service`（systemd）；`AmbientCapabilities=CAP_NET_BIND_SERVICE` 绑定 554；创建 `uvnvr` 系统用户；卸载保留数据，`purge` 删除。
- **macOS**：`/usr/local/uvnvr` 或 `/Applications/UvNVR.app` 启动器；launchd `com.antruly.uvnvr`；`.pkg` 安装，`.dmg` 内含 pkg 与说明；数据 `~/Library/Application Support/UvNVR` 或 `/Library/Application Support/UvNVR`（按安装方式）。
- 首次访问 `http://host:8080` 进入设置向导；安装日志打印访问地址与 RTSP 地址。
- 端口冲突：启动失败给出明确提示；RTSP 无 554 权限时回退 8554 并在 UI 提示。

### 7.4 图标与品牌

- 单一 SVG 源：镜头 + 录制点 + 盾牌意象；由脚本生成 `favicon.ico`、`icon-512.png`、Windows `.ico`、macOS `.icns`、Linux PNG。
- 前端登录页、侧边栏、关于页、安装包、浏览器标签使用同一图形。
- 生成脚本纳入 `scripts/`，CI 校验产物与源一致。

### 7.5 GitHub 仓库与规则

- 创建 `Antruly/uvnvr`（public），描述与 topics：`nvr, onvif, rtsp, video-surveillance, libuvcpp, vue3, cpp`。
- `main` 分支保护：必须 PR、至少 1 次 review、CI 必过、线性历史、禁止强推。
- 文件：`README.md`（英文）+ `README.zh-CN.md`、`LICENSE`、`CONTRIBUTING.md`、`CODE_OF_CONDUCT.md`、`SECURITY.md`、`CHANGELOG.md`、Issue/PR 模板、`dependabot.yml`。
- 提交规范：Conventional Commits；DCO 签名（不用 CLA）。
- Workflows：
  - `ci.yml`：格式化/静态检查、前端构建、C++ 构建、单元/集成测试（Linux x64、Windows x64、macOS arm64）。
  - `build-all.yml`：六目标构建 + 打包，上传 artifact（PR 也跑，保证不回归）。
  - `release.yml`：`v*` tag 触发，汇总产物，生成 `SHA256SUMS` 与 SBOM，创建 GitHub Release（先 draft，人工核对后发布）。
  - `nightly.yml`（可选）：soak/性能测试。
- 发布 `v0.1.0` 资产命名：
  - `uvnvr-0.1.0-windows-x64-setup.exe` / `-portable.zip`
  - `uvnvr-0.1.0-windows-arm64-setup.exe` / `-portable.zip`
  - `uvnvr-0.1.0-linux-x64.deb` / `.rpm` / `.tar.gz`
  - `uvnvr-0.1.0-linux-arm64.deb` / `.rpm` / `.tar.gz`
  - `uvnvr-0.1.0-macos-x64.pkg` / `.dmg` / `.zip`
  - `uvnvr-0.1.0-macos-arm64.pkg` / `.dmg` / `.zip`
  - `SHA256SUMS`、`uvnvr-0.1.0-sbom.spdx.json`

### 7.6 与 uvp / libuvcpp 生态协作

- 用 `uvp-agents@0.4.0` 脚手架（`rest-api` + `vue3`，启用 TLS/HTTP2/WebSocket/SOAP/DB），再用 `uvp build/run/verify` 做构建与接口验证。
- 把可复用模块整理为 uvp 模板并向上游 `Antruly/uvp-templates` 提 PR：
  - 后台：认证/RBAC、审计日志、系统设置、通道 CRUD 骨架。
  - 前端：可收起侧栏 + 多 Tab 缓存布局、通道树、视频网格布局、权限矩阵。
- 开发中发现的 libuvcpp 缺口（流式响应背压、WebSocket 二进制广播、HTTP Range、异步文件接口等）先在本项目做适配层，再向上游提 issue/PR。
- 保持生成代码是“普通 libuvcpp 工程”，不引入 uvp 运行时依赖。

### 7.7 FFmpeg 动态加载与许可证门禁

- **项目代码 MIT**；FFmpeg 使用 **LGPL-2.1-or-later** 构建，并通过 `.dll/.so/.dylib` 动态加载，不静态链接。
- FFmpeg 构建禁止 `--enable-gpl` 与 `--enable-nonfree`；不打包 x264/x265；硬件编码优先（NVENC/QSV/VAAPI/VideoToolbox/AMF），纯 CPU H.264 转码不作为 0.1.0 默认能力。
- 发布包必须附带：FFmpeg 对应源码、`configure` 参数、修改 diff（如有）、LGPL 许可证全文、`THIRD_PARTY_NOTICES.md`，并在下载页与关于页写明 FFmpeg 归属和源码地址。
- 动态库放在安装目录且允许用户替换；macOS 若启用 hardened runtime，需对随包 dylib 一并签名或关闭 library validation。
- CI 在 M2 接入 FFmpeg 后增加许可证门禁：检查 `ffmpeg -buildconf` 不含 `--enable-gpl`/`--enable-nonfree`，缺源码或 SBOM 时发布失败。
- **专利声明**：本项目不授予 H.264/H.265/HEVC/AAC 或任何其他编解码格式的专利许可。若自行构建/打包 x264、x265 或其他 GPL/涉及专利的组件，GPL 义务与专利许可责任由使用者承担；README、关于页与 `THIRD_PARTY_NOTICES.md` 均需明确声明。

---

## 8. 里程碑与任务安排

### 实施进度

| 里程碑 | 状态 | 完成内容 |
|---|---|---|
| M0 | ✅ 2026-10-20 | 仓库/规则、uvp 脚手架、libuvcpp 1.8.1、图标、CI 冒烟测试 |
| M1 | ✅ 2026-10-20 | SQLite 迁移、首次安装、会话/CSRF、用户/角色/权限、API Token、Web/API 锁定、审计、仪表盘、中英文 UI、M1 集成测试 |
| M2 | ✅ 2026-10-21 | 通道、ONVIF、RTSP 拉流、实时预览、抓拍、布局 |
| M3 | ✅ 2026-10-21 | 计划录像、TS 切片、SQLite 索引、存储卷、回放、预读、下载 |
| M4 | ✅ 2026-10-21 | RTSP Server、预警/邮件、GPU/性能、时间 API、帮助手册 |
| M5 | ✅ 2026-10-21 | Linux x64/arm64、Windows x64/arm64、macOS x64/arm64 安装包与 CI 矩阵 |
| M6 | ✅ 2026-10-21 | 发布加固、中英文手册、校验和/SBOM、`v0.1.0` 发布 |
| 0.2.0 | ✅ 2026-10-21 | 启动 JSON 配置文件与端口优先级、安装器端口页、回放倍速/快进快退、配置 API/UI、六平台发布包 |
| 0.3.0 | ✅ 2026-10-21 | HTTP Range 断点下载、MP4 导出、共享 LRU 回放缓存与预读、缓存统计 |
| 0.4.0 | ✅ 2026-10-21 | 通道分组/通道树、按用户保存的服务端预览布局、分组与布局集成测试 |
| 0.5.0 | ✅ 2026-10-21 | ONVIF PullPoint 事件订阅、事件到预警映射、通道开关、mock 摄像头集成测试 |


> 估算为 1 名主程 + AI 辅助的“人日”参考，用于排序与范围控制，不作为固定交付日期。每个里程碑结束都要求构建通过、核心路径手工验收、CI 绿。

### M0 — 工程底座（1–2 人日）

- 创建 GitHub 仓库、分支保护、README/规则文件、Issue/PR 模板。
- `uvp` 脚手架 + CMake 重构为 monorepo（backend/frontend/packaging/docs）。
- libuvcpp 1.8.1 本机安装与 CI 依赖脚本；FFmpeg/SQLite/OpenSSL/liburing 开发环境。
- 图标源与生成；CI 空壳跑通。
- **验收**：`cmake --build` 成功；`uvp verify` 返回 HTTP 200；CI 绿；仓库规则生效。

### M1 — 基础平台与 UI 骨架（4–6 人日）

- 配置系统、SQLite 迁移、系统元数据、序列号、版本信息。
- 首次安装向导、admin 密码、登录/登出、会话、CSRF。
- 用户/角色/权限模型、API Token、Web/API 锁定。
- 审计日志、分类查询、保留清理、路径配置。
- 仪表盘 API（CPU/内存/磁盘/通道统计）。
- 前端：登录/首次安装、左菜单、顶栏时钟、语言切换、多 Tab 缓存、仪表盘骨架、系统设置基础页。
- **验收**：首次安装 → 登录 → 建用户/角色 → 权限生效 → 审计可查 → 刷新/重启状态保持。

### M2 — 通道接入与实时预览（6–8 人日）

- 通道 CRUD、分组树、品牌模板、凭据加密、配置同步。
- ONVIF WS-Discovery、设备信息、Profiles、StreamUri；一键添加；手动 RTSP。
- 拉流线程、重连/看门狗、状态事件、码率/分辨率统计。
- PacketHub、时间戳归一化、通道级 TS Muxer、LiveHub、GOP 缓存。
- WebSocket 直播、stream ticket、多客户端背压；抓拍。
- 前端：通道树、预览网格 1/4/8/16/32、自定义网格、布局保存/切换、全屏、离线覆盖、水印拖动、通道管理页。
- **验收**：真实/模拟 ONVIF 摄像头一键添加；16 画面同时预览；断网 30 秒恢复自动重连；同一通道 5 客户端互不影响；抓拍成功。

### M3 — 录像、存储、回放（8–11 人日）

- 录像计划模型与 7×48 计划 UI、同步；计划调度器。
- RecordingSpool、环形缓存、批量写、io_uring/uv_fs、切片（时间/大小）、索引、崩溃恢复。
- 存储卷管理、路径设置、循环/预警、保留清理、空间告警；格式化（运维，先 Linux，后 Win/macOS）。
- 回放时间轴、回放 WebSocket、预读模块、LRU 缓存、拖动定位、跨片无缝。
- 下载/导出（TS/MP4、Range）；抓拍；回放 UI。
- **验收**：连续录像 ≥2 小时；切片索引完整；按时间回放无跳帧；拖动 ≤1 秒出画面；跨片播放无黑屏/卡顿；循环清理正确；kill -9 后重启索引可恢复。

### M4 — 对外能力与系统能力（5–7 人日）

- RTSP Server（TCP interleaved + UDP）、SDP/RTP/RTCP、Basic/Digest、主/子码流。
- REST API 完整化、OpenAPI/Swagger、权限映射、审计。
- 告警引擎、移动侦测、预警录像、邮件（QQ 预设）、预警中心。
- 性能档位、GPU 探测/开关/回退、NTP、浏览器时间同步、系统设置页。
- 用户/角色 UI、审计 UI、关于/帮助页（手册 + FAQ + 搜索）。
- **验收**：VLC/ffplay/GStreamer 播放 RTSP；API 抓拍/预览/下载/预警全通；锁定策略生效；GPU 开关切换正确；中英文切换完整。

### M5 — 跨平台打包与安装（4–6 人日）

- Linux deb/rpm/tar.gz、systemd、权限/端口、干净容器安装测试。
- Windows MinGW x64/arm64、NSIS 服务安装、防火墙、便携 zip、干净 VM 测试。
- macOS x64/arm64 源码构建 libuvcpp、pkg/dmg/zip、launchd、ad-hoc 签名。
- 六目标 CI 全绿；图标嵌入；启动参数/服务模式。
- **验收**：六个平台各在干净环境安装 → 浏览器打开 → 设置向导 → 添加模拟摄像头 → 预览/录像/回放；卸载/升级行为正确。

### M6 — 加固、文档与 0.1.0 发布（4–6 人日）

- 24 小时 soak（8/16 通道、断线重连、循环覆盖、并发客户端）；内存/fd/线程泄漏检查。
- 安全自查：CSRF、路径穿越、SQL 注入、凭据脱敏、RTSP 鉴权、锁定绕过、格式化护栏。
- 文档：README、架构、API、部署、用户手册、FAQ、已知限制、升级/备份。
- Release draft → 资产核对 → 发布 `v0.1.0`；校验和/SBOM。
- **验收**：Release 资产齐全；安装包可下载安装；CI 全绿；验收场景清单全部通过。

**总量参考：32–46 人日。** 先做“单通道端到端垂直切片”（M2 结束）再铺功能，避免 UI 先行、底层返工。

### 8.1 依赖顺序

```
M0 → M1 → M2（单通道预览垂直切片）
              ├→ M3（录像/回放，核心价值）
              └→ M4（RTSP/API/告警）
M3 + M4 → M5（打包） → M6（发布）
```

前端与后端按里程碑并行，不单独排一个“前端阶段”。

---

## 9. 测试与验收

### 9.1 测试层次

- **单元**：口令/锁定、权限判定、计划调度、时间戳归一化、环形缓存、切片边界、索引查询、预读、RTSP 解析/SDP/RTP、路径护栏、配置加密。
- **集成**：SQLite 迁移/回滚；假 RTSP 摄像头（MediaMTX 测试夹具）；ONVIF SOAP mock；磁盘满/只读；kill -9 恢复；邮件 mock SMTP。
- **E2E（Playwright）**：首次安装、登录、添加通道、预览网格、布局保存、计划拖动、回放拖动、下载、用户权限、语言切换。
- **Soak**：24 小时；16 通道；每 30 分钟随机断线重连；循环覆盖；10 客户端并发；监控 RSS/句柄/线程/丢帧。
- **安全**：认证绕过、CSRF、会话固定、路径穿越、命令注入、SQL 注入、暴力破解锁定、RTSP 未授权、下载越权、格式化误操作。
- **兼容性**：Chrome/Edge/Firefox/Safari 最新两个大版本；VLC/ffplay/GStreamer；ONVIF 海康/大华/宇视/通用。

### 9.2 性能目标（参考硬件，最终以实测为准）

| 指标 | 目标 |
|---|---|
| 首帧（局域网，子码流） | ≤ 2 秒 |
| 直播延迟（MSE，子码流） | ≤ 1.5 秒；主码流 ≤ 2.5 秒 |
| 回放拖动出画面 | ≤ 1 秒 |
| 时间轴查询 | ≤ 100 ms（百万级索引） |
| 多客户端 | 单通道 10 路；中档整机 50 路 |
| 录像写放大 | 接近 1（stream copy + 批量顺序写） |
| 16 画面预览 | 8 核 / 16 GB，子码流 640×360 / 512 kbps 稳定 |
| 32 画面预览 | 16 核 / 32 GB，子码流；或降低分辨率/帧率 |

### 9.3 关键验收场景（对应原始需求）

1. 首次安装设置 admin 密码并登录。
2. ONVIF 扫描 + 一键添加；手动 RTSP/海康/大华添加。
3. 10/16/32 画面与自定义布局保存/切换；掉线覆盖层与双击行为。
4. 一周计划拖动、同步到其他通道。
5. 时间/大小切片、索引、回放拖动、跨片无缝、预读命中。
6. 多客户端并发、慢客户端不影响他人。
7. 存储满 Web/邮件告警；按卷循环/录满模式。
8. 非运维模式不可删视频/日志/改序列号；运维模式可操作且审计。
9. RTSP 主/子码流输出；子码流关闭时明确报错。
10. API 抓拍/预览/下载/预警；API 权限与锁定。
11. 性能档位、GPU 探测/关闭回退、NTP/浏览器时间校准。
12. 审计分类查询、180 天清理、路径可配。
13. 中英文切换、关于页开源地址、帮助页手册/FAQ/搜索。
14. 六平台安装包与 zip 可用。

---

## 10. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 浏览器 H.265 支持不一致 | H.265 摄像头无法预览 | 默认用 H.264 子码流；检测 `MediaSource.isTypeSupported`；有 GPU 时转码；无则明确提示并给 RTSP 地址 |
| FFmpeg RTSP 阻塞 | 事件循环卡顿 | 拉流隔离线程 + 超时中断 + 看门狗；事件循环零阻塞 |
| io_uring 内核/权限差异 | 写入退化 | 编译开关 + 运行时探测 + 自动回退 uv_fs；UI 显示当前后端 |
| 多画面解码压力 | 32 画面卡顿 | 子码流、可视区域加载、最大解码数、性能档位限制 |
| ONVIF 厂商差异 | 添加失败 | 品牌模板 + 手动 RTSP 兜底 + 兼容性测试矩阵 |
| 自研 RTSP Server 复杂 | 兼容性问题 | 先 TCP interleaved，再 UDP；VLC/ffplay/GStreamer 回归；协议单测 |
| 交叉编译/安装包矩阵 | CI 不稳定 | 固定依赖 SHA、缓存、分平台脚本、先 Linux/Windows 后 macOS |
| macOS 无签名证书 | Gatekeeper 拦截 | ad-hoc 签名 + 文档；预留 Developer ID secrets |
| 整盘格式化误操作 | 数据丢失 | 仅运维模式 + 管理员 + 输入设备名 + 拒绝系统盘 + 审计 |
| 全局锁定被恶意触发 | 拒绝服务 | 默认按需求全局锁定；同时提供按 IP+账号锁定选项；审计与告警 |
| 摄像头密码泄露 | 安全 | AES-GCM 加密存储；日志/API 脱敏；密钥文件 0600 |
| 许可证污染 | 发布合规 | 默认 MIT + LGPL FFmpeg；GPL 编码器仅作为显式构建选项 |
| 录像盘性能不足 | 丢帧/索引损坏 | 顺序批量写、预留空间、写入队列告警、拒绝网络盘作主存储 |

---

## 11. 需要你确认的事项

### 必须确认

1. **开源协议**：建议 **MIT**（不打包 GPL 编码器，H.264 纯 CPU 转码不作为 0.1.0 必选）。如果你要求安装包内置 x264/x265 的纯 CPU 转码，则改为 **GPL-3.0**。
2. **0.1.0 实时协议**：建议 **WebSocket + MSE（mpegts.js）**，RTSP 对外输出；**WebRTC 放 0.2**。若 0.1.0 必须 WebRTC 超低延迟，需要增加约 1–2 周。
3. **0.1.0 范围**：建议按第 1 节“核心全交付 + 实验特性带开关”。人脸/车牌、本地人形 AI 不阻塞 0.1.0。
4. **仓库**：确认创建 `Antruly/uvnvr`，public，`main` 分支保护。
5. **macOS 签名**：默认 ad-hoc 签名（无 Apple 证书）。若有 Developer ID + 公证凭据，我接入 CI。

### 我将采用的默认值（如无异议）

- 默认首页：仪表盘；预览第二页。
- 录像切片：默认 5 分钟；可切大小模式（默认 256 MB）。
- 录像保留：默认 30 天；每卷循环/录满独立配置。
- 日志保留：180 天；审计库默认在数据目录。
- 锁定：Web 10 次 / 10 分钟；API 100 次 / 10 分钟；均可配。
- 水印：默认预览/抓拍叠加；烧录进录像仅在有转码时可选。
- 端口：HTTP 8080、HTTPS 8443、RTSP 554（回退 8554）。
- 默认关闭 HTTP/3；需要时在系统设置开启。

---

## 12. 批准后的第一批动作

1. 创建 GitHub 仓库与保护规则，提交需求/计划/README/许可证/模板。
2. 安装并固定 libuvcpp 1.8.1、FFmpeg、SQLite、OpenSSL、liburing 开发环境；记录版本。
3. 用 `uvp-agents@0.4.0` 生成 `rest-api + vue3` 工作区，重构为 monorepo。
4. 打通 M0 验收：构建、运行、`/healthz`、CI 绿。
5. 进入 M1/M2 垂直切片：首次安装 → 登录 → 添加一路 RTSP 摄像头 → 预览 → 抓拍。
6. 每个里程碑结束提交可运行版本与验收记录，再进入下一阶段。

---

**请确认第 11 节的 5 个事项（或直接回复“按建议执行”），我再开始创建仓库和开发。**
