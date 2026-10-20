import { createI18n } from 'vue-i18n'

export type Locale = 'en-US' | 'zh-CN'

const messages = {
  'en-US': {
    app: {
      name: 'UvNVR',
      tagline: 'Open-source software NVR',
      subtitle: 'ONVIF · RTSP · recording · playback · RTSP out',
      status: 'M0 scaffolding',
      online: 'Service online',
      offline: 'Service unreachable',
    },
    nav: {
      dashboard: 'Dashboard',
      preview: 'Live preview',
      channels: 'Channels',
      playback: 'Playback',
      storage: 'Storage',
      alerts: 'Alerts',
      users: 'Users & roles',
      logs: 'Audit log',
      settings: 'System',
      about: 'About & help',
      soon: 'Soon',
    },
    common: {
      language: 'Language',
      version: 'Version',
      commit: 'Commit',
      source: 'Source code',
      docs: 'Documentation',
      back: 'Back to dashboard',
    },
    dashboard: {
      title: 'Dashboard',
      intro:
        'UvNVR is being built in milestones. This first milestone establishes the repository, the libuvcpp service, the Vue 3 shell and the release discipline.',
      runtime: 'Runtime',
      runtimeBody: 'libuvcpp 1.8.1 HTTP/2 service with a health endpoint and version metadata.',
      scope: 'Planned scope',
      scopeBody:
        'ONVIF discovery, RTSP ingest, multi-client live view, scheduled recording, seamless playback and RTSP output.',
      storage: 'Recording design',
      storageBody:
        'MPEG-TS segments, SQLite index, ring-buffer writes, io_uring on Linux, LRU cache and read-ahead.',
      license: 'License',
      licenseBody:
        'MIT for UvNVR code. Official builds load LGPL FFmpeg dynamically and bundle no GPL codecs.',
      m0: 'Milestone 0',
      m0Body: 'Repository, rules, service scaffold, frontend shell, brand icons and CI smoke test.',
      next: 'What comes next',
      nextBody:
        'M1 adds first-run setup, authentication, RBAC, SQLite migrations and the audit log.',
      openGithub: 'Open GitHub repository',
      readPlan: 'Read the development plan',
    },
    preview: {
      title: 'Live preview',
      subtitle: 'The preview grid is scheduled for milestone M2.',
      placeholder: 'Channel grid',
      placeholderBody: '1 / 4 / 8 / 16 / 32 and custom layouts will live here.',
      features: 'Planned in this page',
      f1: 'Channel tree with drag-to-cell and online/offline state',
      f2: 'Collapsible sidebar, cached tabs and saved layouts',
      f3: 'Shared channel fan-out with per-client back-pressure',
      f4: 'Main/sub stream selection, fullscreen and snapshots',
    },
    about: {
      title: 'About & help',
      subtitle: 'Project information, licenses and safety notices.',
      version: 'Version',
      source: 'Open-source repository',
      licenseTitle: 'License',
      licenseBody:
        'UvNVR source code is released under the MIT License. Official builds use FFmpeg shared libraries under LGPL-2.1-or-later, loaded dynamically.',
      ffmpegTitle: 'FFmpeg and codec components',
      ffmpegBody:
        'Official release builds must use an LGPL FFmpeg build configured without --enable-gpl and without --enable-nonfree. FFmpeg source, build configuration and license notices are distributed with every release. Do not ship a GPL FFmpeg build as part of an MIT-labeled binary package.',
      patentTitle: 'Patent notice',
      patentBody:
        'This project grants no patent license for H.264, H.265/HEVC, AAC or any other codec. If you build or bundle x264, x265 or another GPL/patent-bearing component, you are responsible for the resulting GPL obligations and for any patent licensing. The MIT license covers copyright only and does not grant patent rights.',
      m0Title: 'Milestone 0',
      m0Body:
        'The backend answers /healthz and /api/v1/system/info; the frontend is a Vue 3 + TypeScript shell. Recording and camera features are not implemented yet.',
    },
  },
  'zh-CN': {
    app: {
      name: 'UvNVR',
      tagline: '开源软件录像机',
      subtitle: 'ONVIF · RTSP · 录像 · 回放 · RTSP 输出',
      status: 'M0 工程底座',
      online: '服务在线',
      offline: '服务不可达',
    },
    nav: {
      dashboard: '仪表盘',
      preview: '实时预览',
      channels: '通道管理',
      playback: '录像回放',
      storage: '存储管理',
      alerts: '预警中心',
      users: '用户与角色',
      logs: '审计日志',
      settings: '系统设置',
      about: '关于与帮助',
      soon: '待开发',
    },
    common: {
      language: '语言',
      version: '版本',
      commit: '提交',
      source: '开源仓库',
      docs: '文档',
      back: '返回仪表盘',
    },
    dashboard: {
      title: '仪表盘',
      intro:
        'UvNVR 按里程碑推进。当前阶段完成仓库、libuvcpp 服务、Vue 3 壳层与发布规范，后续逐步接入摄像头、录像与回放。',
      runtime: '运行环境',
      runtimeBody: 'libuvcpp 1.8.1 HTTP/2 服务，提供健康检查与版本信息。',
      scope: '规划范围',
      scopeBody: 'ONVIF 发现、RTSP 接入、多客户端预览、计划录像、无缝回放与 RTSP 对外输出。',
      storage: '录像设计',
      storageBody: 'MPEG-TS 切片、SQLite 索引、环形缓存写入、Linux io_uring、LRU 缓存与预读。',
      license: '许可证',
      licenseBody: 'UvNVR 代码使用 MIT；官方构建动态加载 LGPL FFmpeg，不打包 GPL 编解码器。',
      m0: '里程碑 0',
      m0Body: '仓库与规则、服务脚手架、前端壳层、品牌图标与 CI 冒烟测试。',
      next: '下一步',
      nextBody: 'M1 将加入首次安装、登录鉴权、RBAC、SQLite 迁移与审计日志。',
      openGithub: '打开 GitHub 仓库',
      readPlan: '查看开发计划',
    },
    preview: {
      title: '实时预览',
      subtitle: '预览网格计划在 M2 交付。',
      placeholder: '通道网格',
      placeholderBody: '1 / 4 / 8 / 16 / 32 与自定义布局将在这里实现。',
      features: '本页计划能力',
      f1: '通道树拖入画面，显示在线/离线状态',
      f2: '可收起侧栏、多标签缓存与布局保存',
      f3: '通道级共享分发，每客户端独立背压',
      f4: '主/子码流切换、全屏与抓拍',
    },
    about: {
      title: '关于与帮助',
      subtitle: '项目信息、许可证与安全声明。',
      version: '版本',
      source: '开源仓库',
      licenseTitle: '许可证',
      licenseBody:
        'UvNVR 源代码采用 MIT 许可证。官方构建动态加载 LGPL-2.1-or-later 的 FFmpeg 共享库。',
      ffmpegTitle: 'FFmpeg 与编解码组件',
      ffmpegBody:
        '官方发布包必须使用 LGPL FFmpeg（不带 --enable-gpl、不带 --enable-nonfree）。每次发布都会附带 FFmpeg 对应源码、编译参数与许可证声明。不要把 GPL 构建的 FFmpeg 作为 MIT 二进制包的一部分分发。',
      patentTitle: '专利声明',
      patentBody:
        '本项目不授予 H.264、H.265/HEVC、AAC 或其他编解码格式的任何专利许可。如果你自行构建或打包 x264、x265 或其他 GPL/涉及专利的组件，由此产生的 GPL 义务与专利许可责任由你自行承担。MIT 许可证仅覆盖版权，不授予专利权。',
      m0Title: '里程碑 0',
      m0Body:
        '后端提供 /healthz 与 /api/v1/system/info；前端是 Vue 3 + TypeScript 壳层。摄像头与录像功能尚未实现。',
    },
  },
} as const

function detectLocale(): Locale {
  const saved = localStorage.getItem('uvnvr.locale')
  if (saved === 'en-US' || saved === 'zh-CN') return saved
  const languages = navigator.languages?.length ? navigator.languages : [navigator.language]
  return languages.some((lang) => lang.toLowerCase().startsWith('zh')) ? 'zh-CN' : 'en-US'
}

export const i18n = createI18n({
  legacy: false,
  locale: detectLocale(),
  fallbackLocale: 'en-US',
  messages,
})
