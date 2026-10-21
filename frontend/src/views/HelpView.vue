<script setup lang="ts">
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import AppIcon from '@/components/AppIcon.vue'
import { useAuthStore } from '@/stores/auth'

defineOptions({ name: 'HelpView' })

const { t, locale } = useI18n()
const auth = useAuthStore()
const query = ref('')
const canUseApi = () => auth.hasPermission('api.access')

interface Section {
  id: string
  title: string
  body: string[]
}

const sections: Record<'en-US' | 'zh-CN', Section[]> = {
  'en-US': [
    {
      id: 'start',
      title: 'Quick start',
      body: [
        'Open the web UI, complete the first-run setup and sign in as admin.',
        'Add a camera from Channels: use ONVIF Scan, a Hikvision/Dahua preset, or a direct RTSP URL.',
        'Assign cameras to the Live preview grid. Drag a channel onto a cell or click a channel and then an empty cell.',
        'Create a Recording plan, choose the recording stream and paint the weekly schedule. Save it.',
        'Open Playback, pick the channel and date, then click or double-click a timeline segment.',
      ],
    },
    {
      id: 'config',
      title: 'Startup configuration',
      body: [
        'Ports, bind address, the data directory and TLS are read from uvnvr.json at startup. Command-line flags override environment variables, which override the file.',
        'Windows installer: choose the HTTP and RTSP ports on the Service ports page; the file is %ProgramData%\\UvNVR\\uvnvr.json. Linux: /etc/uvnvr/uvnvr.json. macOS: /Library/Application Support/UvNVR/uvnvr.json.',
        'Run uvnvr --print-config to see the merged settings, then restart the service after editing the file.',
      ],
    },
    {
      id: 'service',
      title: 'Running as a system service',
      body: [
        'The Windows installer registers UvNVR as a service by default on the Startup page. Clear the option to run a console instance instead.',
        'Settings → System service shows the registration state and can register or remove it. Registration needs administrator (Windows) or root (Linux) rights, restarts UvNVR and the page reconnects automatically.',
        'With the service, the single desktop shortcut opens the web UI. Without it, the shortcut starts UvNVR and opens the browser; pass --open from a terminal for the same effect.',
        'Windows service logs are in %ProgramData%\\UvNVR\\logs\\service.log. On Linux use journalctl -u uvnvr.',
      ],
    },
    {
      id: 'camera',
      title: 'Camera protocols',
      body: [
        'ONVIF: the camera must expose WS-Discovery and the ONVIF media service. Enter credentials before Probe.',
        'RTSP: use the full URL, e.g. rtsp://user:pass@192.168.1.64:554/Streaming/Channels/101.',
        'Hikvision/Dahua: leave the URL empty and fill host, port and credentials; the path template is generated.',
        'Main and sub streams are independent. Sub stream is recommended for multi-cell preview.',
        'Use Groups to build a channel tree; the Live preview rail shows the same groups and can collapse them.',
        'Enable Use camera ONVIF events to receive motion, person, tamper and line-crossing notifications from the camera in Alerts.',
        'Use Auto detect in the channel form or Add now in the ONVIF scan results. It tries ONVIF, then Hikvision ISAPI/Dahua CGI, and uses generated main/sub paths; set HTTP / ONVIF port when the camera HTTP service is not on 80.',
      ],
    },
    {
      id: 'recording',
      title: 'Recording and storage',
      body: [
        'Recording is plan-driven. Timed cells record continuously; event and timed+event cells also record alert-triggered clips with pre-roll and post-roll, configured under System → Event recording.',
        'Segments are MPEG-TS files, aligned to keyframes and indexed in SQLite.',
        'Storage volumes support loop recording (delete oldest) or stop-and-alert. Retention is enforced by the recorder.',
        'Change the slice duration or size in the settings keys recording.segment_minutes / recording.segment_size_mb.',
      ],
    },
    {
      id: 'playback',
      title: 'Playback and preload',
      body: [
        'The playback player streams recorded segments over a WebSocket; it can cross file boundaries without a gap.',
        'Seeking starts a new ticket at the target time. Click the timeline or use the −30s / −10s / +10s / +30s buttons.',
        'Use the speed selector for 0.25×–32× playback. Zoom the timeline from 24h to 5m; the skip step scales with the visible window. Reverse steps backward at the selected rate.',
        'Use From/To and Download by time for an arbitrary MP4 range. A codec switch starts a new segment; seek to the next segment to change codec.',
        'Use Download segment to fetch a single MPEG-TS file (HTTP Range/resume is supported), or Export MP4 to remux it with the bundled FFmpeg.',
        'Playback and downloads share an LRU block cache and preload the next segment. Tune storage.cacheMb and storage.readaheadMb in the startup configuration.',
      ],
    },
    {
      id: 'api',
      title: 'API and RTSP output',
      body: [
        'REST endpoints live under /api/v1. Create an API token in System → API Token and use Authorization: Bearer.',
        'OpenAPI 3.1 is served at /api/v1/openapi.json. The self-hosted Swagger UI at /api/docs loads the spec and lets you authorize with a bearer token; both require api.access.',
        'Snapshots: GET /api/v1/channels/{id}/snapshot (optional width/height/quality). Recordings: GET /api/v1/recordings, including the active .ts.part segment. Alerts: GET /api/v1/alerts. Encoder: GET/PUT /api/v1/channels/{id}/encoder.',
        'Real-time events: POST /api/v1/events/ticket then connect to /ws/events for alert and channel-state messages.',
        'RTSP output: rtsp://user:pass@host:554/live/{channel}/{main|sub}. The server falls back to 8554/8555 when 554 is unavailable.',
        'RTSP accepts Basic and Digest authentication and sends H.264/H.265 video as RTP payload type 96 plus AAC/G.711 audio when present. DESCRIBE returns 503 while the channel is offline.',
      ],
    },
    {
      id: 'security',
      title: 'Users and security',
      body: [
        'The first account is admin. Create roles with only the Web/API permissions they need.',
        'API tokens can be limited with grouped scopes (preview, channels, download, alerts, time and more); select all or leave empty to inherit the role.',
        'Web login failures and API failures lock the recorder independently; thresholds are in System settings.',
        'Camera passwords are encrypted with a key file in the data directory. Back up data/secret.key with the database.',
        'Video files cannot be deleted manually. Only maintenance mode exposes deletion/format/serial operations.',
      ],
    },
    {
      id: 'alerts',
      title: 'Alerts and email',
      body: [
        'Motion detection samples a small grayscale frame and compares it with the previous sample; it is experimental.',
        'Offline and storage-full alerts are generated by the monitor.',
        'Configure SMTP in System → Email alerts. QQ Mail uses smtp.qq.com:465 and an authorization code.',
      ],
    },
    {
      id: 'faq',
      title: 'Troubleshooting',
      body: [
        'Camera offline: verify the RTSP URL from VLC/ffplay, then check the channel last error.',
        'No preview: check that the browser supports MSE; H.265 depends on hardware/browser support. Prefer an H.264 sub stream.',
        'No recording: check the plan covers the current weekday/time, the volume is enabled, and the channel is online.',
        'RTSP 554 fails: run the service with CAP_NET_BIND_SERVICE or use the fallback port shown in startup logs.',
        'Email test fails: QQ requires an authorization code, not the account password; TLS certificate verification is required.',
      ],
    },
  ],
  'zh-CN': [
    {
      id: 'start',
      title: '快速开始',
      body: [
        '打开 Web 页面，完成首次初始化并以 admin 登录。',
        '在“通道管理”中添加摄像头：可使用 ONVIF 扫描、海康/大华模板，或直接填写 RTSP URL。',
        '把通道拖到实时预览网格中，或先点击通道再点击空白画面。',
        '在“录像计划”中选择通道与录像码流，拖动绘制一周计划并保存。',
        '打开“录像回放”，选择通道和日期，点击时间轴切片即可播放。',
      ],
    },
    {
      id: 'config',
      title: '启动配置',
      body: [
        '端口、绑定地址、数据目录与 TLS 在启动时从 uvnvr.json 读取。命令行参数优先于环境变量，环境变量优先于配置文件。',
        'Windows 安装包在“服务端口”页设置 HTTP/RTSP 端口，写入 %ProgramData%\\UvNVR\\uvnvr.json；Linux 为 /etc/uvnvr/uvnvr.json；macOS 为 /Library/Application Support/UvNVR/uvnvr.json。',
        '使用 uvnvr --print-config 查看最终配置；修改文件后重启服务生效。',
      ],
    },
    {
      id: 'service',
      title: '以系统服务运行',
      body: [
        'Windows 安装包默认在“启动”页注册系统服务；取消勾选则以控制台方式运行。',
        '“设置 → 系统服务”显示注册状态，并可随时注册或卸载。注册需要管理员（Windows）或 root（Linux）权限，会重启 UvNVR，页面会自动重连。',
        '已注册服务时，桌面唯一的快捷方式直接打开 Web 界面；未注册时，快捷方式启动 UvNVR 并打开浏览器。命令行加 --open 效果相同。',
        'Windows 服务日志位于 %ProgramData%\\UvNVR\\logs\\service.log；Linux 使用 journalctl -u uvnvr。',
      ],
    },
    {
      id: 'camera',
      title: '摄像头协议',
      body: [
        'ONVIF：摄像头需支持 WS-Discovery 与 ONVIF 媒体服务；探测前填写账号密码。',
        'RTSP：填写完整 URL，例如 rtsp://user:pass@192.168.1.64:554/Streaming/Channels/101。',
        '海康/大华：URL 可留空，填写 IP、端口和凭据后由系统生成路径模板。',
        '主码流与子码流独立配置；多画面预览建议使用子码流。',
        '使用“通道分组”构建通道树；实时预览左侧会按分组展示并支持折叠。',
        '在通道编辑中点击“自动识别”，或在 ONVIF 扫描结果中点击“立即添加”。会依次尝试 ONVIF、海康 ISAPI、大华 CGI，并使用主/子码流路径；摄像头 HTTP 不在 80 时填写 HTTP / ONVIF 端口。',
        '启用“使用摄像头 ONVIF 事件”后，摄像头推送的移动、人形、遮挡与越线事件会进入预警中心。',
      ],
    },
    {
      id: 'recording',
      title: '录像与存储',
      body: [
        '录像是计划驱动的：定时格持续录像；预警格与定时+预警格还会在预警时录制带预录/后录的片段，可在 系统设置 → 事件录像 中配置。',
        '切片为 MPEG-TS 文件，在关键帧处切分，并写入 SQLite 索引。',
        '存储卷支持循环录制（删除最旧）或录满停止并预警；保留策略由录像服务执行。',
        '切片时长与大小可在设置 recording.segment_minutes / recording.segment_size_mb 中调整。',
      ],
    },
    {
      id: 'playback',
      title: '回放与预读',
      body: [
        '回放通过 WebSocket 顺序发送录像切片，可以跨文件无缝播放。',
        '跳转会从目标时间创建新的播放票据；可点击时间轴或使用 −30s / −10s / +10s / +30s 按钮。',
        '倍速支持 0.25×–32×；时间轴可从 24 小时放大到 5 分钟，跳转步长随窗口等比变化；倒放按所选倍速向后步进。',
        '可用“开始/结束 + 按时间下载”导出任意时间范围的 MP4。码流切换会开始新切片，跳到下一段即可切换编码。',
        '“下载切片”返回单个 MPEG-TS 文件，支持 HTTP Range 断点续传；“导出 MP4”使用随包 FFmpeg 转封装。',
        '回放与下载共享 LRU 块缓存并预读下一片，可通过启动配置的 storage.cacheMb / storage.readaheadMb 调整。',
      ],
    },
    {
      id: 'api',
      title: 'API 与 RTSP 输出',
      body: [
        'REST 接口位于 /api/v1。在“系统设置 → API Token”创建 Token，并使用 Authorization: Bearer。',
        'OpenAPI 3.1 文档位于 /api/v1/openapi.json；自托管 Swagger UI 位于 /api/docs，可填入 Bearer Token 调试；两者都需要 api.access。',
        '抓拍：GET /api/v1/channels/{id}/snapshot（可选宽高/质量）；录像查询：GET /api/v1/recordings（包含正在写入的 .ts.part 切片）；预警：GET /api/v1/alerts；编码参数：GET/PUT /api/v1/channels/{id}/encoder。',
        '实时事件：先 POST /api/v1/events/ticket，再连接 /ws/events 接收预警与通道状态。',
        'RTSP 输出：rtsp://user:pass@host:554/live/{channel}/{main|sub}。554 不可用时回退到 8554/8555。',
        'RTSP 支持 Basic 与 Digest 鉴权，视频以 RTP payload type 96 发送 H.264/H.265，源流带音频时附加 AAC/G.711；通道离线时 DESCRIBE 返回 503。',
      ],
    },
    {
      id: 'security',
      title: '用户与安全',
      body: [
        '首个账户为 admin。创建角色时只勾选实际需要的 Web/API 权限。',
        'API Token 可按分组勾选权限（预览、通道、下载、预警、时间等），支持全选；不勾选则继承账户角色。',
        'Web 登录失败与 API 失败分别锁定；阈值在系统设置中配置。',
        '摄像头密码使用数据目录中的密钥文件加密；请将 data/secret.key 与数据库一起备份。',
        '视频文件不能手动删除；只有运维模式开放删除、格式化、序列号修改等操作。',
      ],
    },
    {
      id: 'alerts',
      title: '预警与邮件',
      body: [
        '移动侦测通过低分辨率灰度帧与上一帧比较实现，属于实验功能。',
        '离线预警与存储将满预警由监控线程生成。',
        '在“系统设置 → 邮件预警”配置 SMTP。QQ 邮箱使用 smtp.qq.com:465 与授权码。',
      ],
    },
    {
      id: 'faq',
      title: '常见问题',
      body: [
        '摄像头离线：先用 VLC/ffplay 验证 RTSP URL，再查看通道最后错误。',
        '没有画面：确认浏览器支持 MSE；H.265 依赖浏览器/硬件支持，建议使用 H.264 子码流。',
        '没有录像：确认计划覆盖当前星期与时间、存储卷已启用、通道在线。',
        'RTSP 554 失败：为服务授予 CAP_NET_BIND_SERVICE，或使用启动日志中的回退端口。',
        '邮件测试失败：QQ 邮箱需要使用授权码而不是登录密码；TLS 证书必须校验通过。',
      ],
    },
  ],
}

const filtered = computed(() => {
  const language = locale.value === 'zh-CN' ? 'zh-CN' : 'en-US'
  const source = sections[language]
  const needle = query.value.trim().toLowerCase()
  if (!needle) return source
  return source.filter(
    (section) =>
      section.title.toLowerCase().includes(needle) ||
      section.body.some((line) => line.toLowerCase().includes(needle)),
  )
})
</script>

<template>
  <section class="panel page-head">
    <div>
      <span class="pill accent">{{ t('help.title') }}</span>
      <h2>{{ t('help.title') }}</h2>
      <p>{{ t('help.subtitle') }}</p>
    </div>
    <div class="head-actions">
      <template v-if="canUseApi()">
        <a class="button" href="/api/docs" target="_blank" rel="noreferrer">
          <AppIcon name="key" :size="16" />
          {{ t('help.openSwagger') }}
        </a>
        <a class="button" href="/api/v1/openapi.json" target="_blank" rel="noreferrer">
          <AppIcon name="logs" :size="16" />
          {{ t('help.openSpec') }}
        </a>
      </template>
      <RouterLink class="button" to="/about">
        <AppIcon name="arrow" :size="16" />
        {{ t('nav.about') }}
      </RouterLink>
    </div>
  </section>

  <section class="panel help-search">
    <label class="field grow">
      <span>{{ t('help.search') }}</span>
      <input v-model="query" class="input" type="search" :placeholder="t('help.searchHint')" />
    </label>
  </section>

  <section class="help-layout">
    <aside class="panel help-nav">
      <a v-for="section in filtered" :key="section.id" :href="`#${section.id}`">{{ section.title }}</a>
      <p v-if="filtered.length === 0" class="small-note">{{ t('help.noResults') }}</p>
    </aside>

    <div class="help-sections">
      <article v-for="section in filtered" :id="section.id" :key="section.id" class="panel help-section">
        <h3>{{ section.title }}</h3>
        <p v-for="line in section.body" :key="line">{{ line }}</p>
      </article>
    </div>
  </section>
</template>
