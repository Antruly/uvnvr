# GA/T 2155.2-2024 产品检测合规差距矩阵（P6.0）

- 文档版本：1.0，2026-11-24
- 适用代码：`night/p6-0-gap`（基线提交 `443e78b`）
- 设计基线：[`compliance-gb28181-gb35114.md`](compliance-gb28181-gb35114.md) §1.3
- 适用范围：UvNVR 单机 NVR 软件，包括 Web/API、RTSP、私有协议、录像/回放、ONVIF/品牌接入与 AI 识别。

> **标准文本说明**：本矩阵是 P6.0 阶段的工程拆解，不是对 GA/T 2155.2-2024 条款的逐字引用。
> 截至编写时尚未取得标准正式文本，因此条目使用内部编号（`IDN`/`ACC`/`AUD`/`INT`/`CON`/`COM`/`FLT`/`UPG`），
> 按设计基线 §1.3 的八大产品检测类别组织。取得标准全文后，应在每一行补充“标准条款号”并逐条校对措辞；
> 本文件不声称任何条目已经通过检测机构认定。

> **许可证边界**：本仓库以 MIT 发布。本差距矩阵中的整改任务不得引入 GPL/LGPL/AGPL 新依赖；
> SIP 栈保持自研；国密 TLS 只使用 Tongsuo/GmSSL（Apache-2.0）作为可选构建；国密算法使用 OpenSSL 3。
> 现有 FFmpeg 仅作为独立 CLI 子进程分发，不链接进 UvNVR 二进制，见 `THIRD_PARTY_NOTICES.md`。

## 1. 状态定义与证据口径

| 状态 | 含义 |
| --- | --- |
| 满足 | 功能已实现，且有明确代码位置、接口或配置项，可按用例自测。 |
| 部分 | 已有基础实现，但覆盖范围、密码强度、默认配置或可审计性不足以满足检测要求。 |
| 缺失 | 未实现，或只有展示性/不可验证的实现。 |
| 不适用 | 产品形态不涉及。本期矩阵无此类条目。 |

证据位置使用 `文件:起始行-结束行`，并尽量给出函数名、路由、配置项或表名。
`src/app.cpp` 中的路由以 `web_.get/post/put/del` 注册；所有 Web 路由默认经过
`App::Protected`（`src/app.cpp:2173-2210`）做认证、CSRF 与权限检查，公开路由单独列出。

## 2. 汇总

| 类别 | 条目数 | 满足 | 部分 | 缺失 | 不适用 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 身份鉴别 | 8 | 4 | 3 | 1 | 0 |
| 访问控制 | 4 | 2 | 1 | 1 | 0 |
| 安全审计 | 7 | 2 | 3 | 2 | 0 |
| 数据完整性 | 5 | 0 | 2 | 3 | 0 |
| 数据保密性 | 5 | 1 | 3 | 1 | 0 |
| 通信安全 | 6 | 0 | 5 | 1 | 0 |
| 软件容错 | 6 | 2 | 4 | 0 | 0 |
| 升级安全 | 5 | 0 | 1 | 4 | 0 |
| **合计** | **46** | **11** | **22** | **13** | **0** |

结论：现有产品在“账户与口令基础能力、RBAC、审计留痕、资源上限”上已有可用实现；
主要差距集中在**双因子/再认证、对象级授权、审计防篡改、数据静态加密、国密通信、升级签名与回滚**。
其中 22 个“部分”与 13 个“缺失”条目全部在
[`compliance-testcases.md`](compliance-testcases.md) 中有对应用例（35 条）。

## 3. 身份鉴别

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| IDN-01 | 每个账户具有唯一标识；账户可创建、禁用、删除；操作可追溯。 | 满足 | `users` 表 `id INTEGER PRIMARY KEY`、`username TEXT NOT NULL UNIQUE COLLATE NOCASE`（`src/app.cpp:950-956`）；用户 CRUD `GET/POST /api/v1/users`、`PUT/DELETE /api/v1/users/:id`（`src/app.cpp:3574-3907`）；`enabled` 字段与删除逻辑（`src/app.cpp:3782-3825`、`3884-3890`）。 | 保持；P6.1 增加账户有效期与实名/工号字段（可选项），并在用户列表显示最后登录时间。 |
| IDN-02 | 设备具有唯一、持久化的标识，可用于台账、审计与报送。 | 满足 | `system_meta.serial`；`App::EnsureSerial` 首次生成 `UVNVR-<12 hex>` 并持久化（`src/app.cpp:1299-1316`）；总览接口返回 `serial`（`src/app.cpp:2030-2031`）；首次设置状态返回序列号（`src/app.cpp:2448-2455`）。 | 保持；送检前确认序列号由生产工装写入且不可通过网页随意改写（当前 `PUT /api/v1/system/serial` 需维护模式 + 管理员，`src/app.cpp:4449-4484`）。 |
| IDN-03 | 口令有最小长度、复杂度要求；首次启用强制设置口令，不存在出厂默认口令。 | 满足 | `App::PasswordValid`：长度 10–256，至少三类字符（`src/app.cpp:1420-1447`）；`App::Setup` 在没有用户时强制走设置流程（`src/app.cpp:1720-1842`）；`setup_required_ = CountUsers() == 0`（`src/app.cpp:1367`）；创建/修改用户同样校验（`src/app.cpp:3634-3637`、`3729-3733`）。 | 保持；P6.1 可增加口令历史与定期更换策略（检测项若要求）。 |
| IDN-04 | 口令不得明文或可逆存储；应使用加盐慢哈希。 | 满足 | scrypt 参数 `N=2^15, r=8, p=1`（`src/app.cpp:58-61`）；`HashPassword` 生成 16 字节随机盐与 32 字节摘要，格式 `scrypt$N$r$p$salt$hash`（`src/app.cpp:301-314`）；`VerifyPassword` 使用 `CRYPTO_memcmp` 常量时间比较（`src/app.cpp:358-395`）。 | 保持；P6.1 评估提高 N 或迁移 Argon2id 的成本，并保留旧哈希的登录时升级路径。 |
| IDN-05 | 连续登录失败应触发锁定/延迟；锁定应按账户与来源区分，可自动解除；不得形成全局拒绝服务。 | 部分 | Web 与 API 两套全局锁：`IsWebLocked`（`src/app.cpp:1507-1521`）、`RecordLoginFailure`（`src/app.cpp:1581-1613`）、`IsApiLocked`/`RecordApiFailure`（`src/app.cpp:1528-1573`）；配置 `security.web_max_failures=10`、`security.web_lock_minutes=10`、`security.api_max_failures=100`（`src/app.cpp:1218-1222`）；失败记录含 `username`/`ip`（`login_attempts`，`src/app.cpp:975-982`），但计数查询只按 `kind` 过滤（`src/app.cpp:1594-1608`），成功登录清空全部 Web 失败（`src/app.cpp:1522-1526`）。 | 改为按“账户 + 来源 IP”滑动窗口计数；保留全局阈值作为兜底但提高上限；对管理员账户单独策略；锁定/解锁写入审计并触发告警；成功登录只清除本账户本 IP 的失败记录。 |
| IDN-06 | 会话应有绝对超时与空闲超时；注销后立即失效；Cookie 属性安全。 | 部分 | 绝对超时 24 小时 `kSessionTtlSeconds`（`src/app.cpp:58`）；`sessions.expires_at` 校验（`src/app.cpp:1905-1927`）；注销删除会话（`src/app.cpp:2680-2692`）；Cookie 为 HttpOnly、TLS 下 Secure、SameSite=Strict（`src/app.cpp:2155-2159`）；无空闲超时、无会话续期上限，`ip`/`user_agent` 仅存储未校验。 | 增加 `security.session_idle_minutes`（建议 30）与绝对上限；每次请求滑动续期但不超过绝对上限；可选绑定 IP/UA；提供“我的会话”列表与踢出；登出/改密轮换会话标识。 |
| IDN-07 | 高风险操作应二次认证；管理员应支持双因子（标准允许可选时也建议明确支持范围）。 | 缺失 | 当前仅要求管理员角色 + 维护模式：`App::RequireMaintenance`（`src/app.cpp:3159-3170`）；无口令再输入、无 TOTP/WebAuthn/国密证书登录；全仓库无 `totp`/`mfa`/`webauthn` 实现。 | P6.1 增加 TOTP（RFC 6238）与恢复码，管理员默认开启；对审计清除、磁盘格式化、密钥/证书导入导出要求再次输入口令；预留 GB 35114 证书双因子接口。 |
| IDN-08 | 口令变更、角色变更、账户禁用后，旧会话与令牌应立即失效。 | 部分 | 禁用、口令变更与角色变更都会删除该用户全部会话与 API Token（`src/auth/session_revocation.cpp`，由 `src/app.cpp` 的用户更新处理调用）；删除用户同样删除会话与令牌；仍无“用户自助改密”接口。 | 提供 `PUT /api/v1/users/me/password`（需旧口令）并审计；如需按令牌粒度失效可引入 `token_version`。 |

## 4. 访问控制

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| ACC-01 | 采用角色/权限模型；默认拒绝；权限点覆盖主要功能；角色可分离。 | 满足 | 权限全集 `AllPermissions`（`src/app.cpp:738-748`）；权限蕴含关系 `PermissionImplies`（`src/app.cpp:760-783`）；内置角色 admin/operator/viewer（`src/app.cpp:1188-1213`、`833-836`）；路由统一经 `App::Protected`，无权限返回 403（`src/app.cpp:2173-2210`）；未认证返回 401（`src/app.cpp:1844-1941`）。 | 保持；P6.1 增加权限矩阵导出（角色 × 权限 × 路由）作为送检材料。 |
| ACC-02 | 防止水平越权：用户只能访问被授权的通道、录像、抓图、回放与下载对象。 | 部分 | 仅做功能级权限，没有通道/分组级 ACL：`GET /api/v1/channels` 对任何 `channel.view` 返回全部通道（`src/app.cpp:5484-5544`）；预览/回放/导出只检查 `preview.view`/`playback.download`（`src/app.cpp:5585-5606`、`8867-9004`）；抓图只检查 `api.snapshot`（`src/app.cpp:6321-6404`）；`channels` 表无 `owner`/`group_acl` 字段（`src/app.cpp:1002-1022`）。 | 以通道分组为授权范围，增加 `user_channel_grants`/`role_channel_grants`；在列表过滤与所有按 `channelId` 的读写接口（快照、直播票据、回放、导出、私有协议）统一校验；补“越权访问返回 404/403 且写审计”的用例。 |
| ACC-03 | API Token 的权限不得超过所属用户角色；有有效期、可撤销、静态不可还原。 | 满足 | Token 范围与角色权限取交集 `EffectiveTokenPermissions`（`src/app.cpp:808-831`）；仅存 `SHA256(token)`（`src/app.cpp:4647`）；有效期 0–3650 天（`src/app.cpp:4601-4609`）；删除接口 `DELETE /api/v1/tokens/:id`（`src/app.cpp:4682-4725`）；每次使用更新 `last_used_at`（`src/app.cpp:1888-1893`）。 | 保持；P6.1 增加 Token 绑定来源网段/首次使用 IP 的可选项，并在审计中记录 Token ID。 |
| ACC-04 | 管理面应限制来源（管理网段/跳板机/VPN），非管理来源不得访问管理接口。 | 缺失 | 默认监听 `0.0.0.0:8080`（`src/config.h:13-14`、`src/app.cpp:2230-2232`），仅能通过 `http.host` 改绑定地址；无 CIDR 白名单、无管理面/业务面分离、无登录来源限制；`Protected` 只检查会话与权限（`src/app.cpp:2173-2210`）。 | 增加 `security.admin_allow_cidrs`，对 `/api/v1/users`、`/roles`、`/settings`、`/audit/purge`、`/storage/format`、`/system/*` 强制校验来源；支持独立管理端口；拒绝时写审计并告警。 |

## 5. 安全审计

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| AUD-01 | 登录、注销、用户/角色/权限、配置、通道、存储、回放导出、维护操作均应留痕；媒体访问也应可追溯。 | 部分 | 已覆盖 `auth`（`src/app.cpp:1639-1708`、`2686`）、`user`（`3684-3897`）、`permission`（`4009-4181`）、`system/settings`（`3028`）、`channel`（`5864-6774`）、`record`（`7958-8027`、`8968`）、`storage`（`8242-8568`）、`api token`（`4668-4719`）、`maintenance`（`4437-4541`、`8467-8568`）、`alert`（`9390`）、时间同步（`10038-10079`）。但 RTSP 直播/回放、私有协议直播/下载的会话开始/结束未写审计；鉴权失败（除登录外）与权限拒绝未写审计。 | 为 RTSP/私有协议的 OPEN/CLOSE、下载开始/完成、权限拒绝增加审计事件（含用户、通道、流、来源 IP、会话 ID、字节数、结果）；为 `Protected` 的 403 增加统一审计（可限流）。 |
| AUD-02 | 审计记录应包含时间、主体、来源、对象、动作、结果、错误码、请求标识等要素。 | 部分 | `audit_log` 字段齐全：`ts/category/level/actor/ip/user_agent/object_type/object_id/action/result/error_code/detail/request_id`（`src/app.cpp:986-1001`）；但 `App::Audit` 把 `request_id` 固定写成空串（`src/app.cpp:1404-1406`），查询接口也不返回该字段（`src/app.cpp:4267-4288`）；无会话 ID、无来源端口。 | 从请求中间件获取 `X-Request-Id`（`uvcpp::web_middleware_request_id` 已启用，`src/app.cpp:2089`）写入审计；增加 `session_id`、`protocol`、`result_detail`；查询/导出返回 `request_id`；对系统事件记录触发源。 |
| AUD-03 | 审计日志应防篡改：修改、删除、截断可被检出；清除需双人/审批。 | 缺失 | 审计为普通 SQLite 表，无哈希链、HMAC、签名或只追加约束（`src/app.cpp:986-1001`）；`App::Audit` 直接 `INSERT`（`src/app.cpp:1389-1417`）；保留期清理直接 `DELETE`（`src/app.cpp:1377-1387`）；提供 `POST /api/v1/audit/purge` 批量删除（`src/app.cpp:4390-4446`）。 | P6.1 按 `id` 串联 SM3 哈希链（`prev_hash` + 记录规范化字段），定期把链头写入外部 syslog/对象存储/WORM；查询与导出时校验链并报告断点；purge 改为标记删除 + 双人审批 + 保留链锚。 |
| AUD-04 | 审计日志应有明确、可配置的留存期，并到期自动清理。 | 满足 | 默认 `audit.retention_days=180`（`src/app.cpp:1223`），可配置 1–3650（`src/app.cpp:2784`）；每日清理 `CleanupAuditLocked`（`src/app.cpp:1377-1387`）；清理由 purge 触发的行为写入 `maintenance/purge` 审计（`src/app.cpp:4437-4440`）。 | 保持；P6.1 增加留存期变更审计与容量上限告警。 |
| AUD-05 | 审计日志仅授权人员可读、可导出；导出不得泄露口令等秘密。 | 满足 | 查询/导出要求 `log.view`（`src/app.cpp:4206-4290`、`4301-4378`）；purge 额外要求维护模式 + 管理员（`src/app.cpp:4390-4395` + `3159-3170`）；设置变更写审计前删除 `password` 字段（`src/app.cpp:3022-3029`）；导出 CSV 最多 10 万行（`src/app.cpp:4340-4345`）。 | 保持；P6.1 导出增加完整性摘要（文件 SM3）与下载审计。 |
| AUD-06 | 审计写入失败不得静默丢失；高风险操作在审计不可用时应有明确策略。 | 缺失 | `App::Audit` 写库失败仅打印 `stderr` 后返回（`src/app.cpp:1407-1413`），调用方继续执行；无持久化队列、无重试、无告警；数据库繁忙或磁盘满时可能丢失审计。 | P6.1 增加审计写失败告警与本地追加日志兜底；对维护/删除/格式化/用户权限变更等高风险操作，审计失败时拒绝执行（fail-closed）；提供审计积压监控。 |
| AUD-07 | 审计时间应可信；日志应支持外送，防止本机被攻陷后销毁。 | 部分 | 审计使用本机 `NowSeconds()`（`src/app.cpp:1389-1401`）；NTP 默认关闭（`system.ntp_enabled=0`，`src/app.cpp:1227-1228`），同步接口存在但无认证/漂移告警（`src/app.cpp:10055-10090`）；无 syslog/远程日志外送，仅进程输出到 journald（`packaging/linux/uvnvr.service:18-19`）。 | 增加 NTP/NTS 或 GPS 时间源、时钟漂移阈值告警；P6.1 支持 TLS syslog/HTTP 批量外送（至少审计与安全事件），外送失败不阻塞本地审计。 |

## 6. 数据完整性

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| INT-01 | 启动配置、安全策略应可校验完整性，未授权修改应被拒绝或告警。 | 缺失 | JSON 配置由 `ApplyConfigJson`/`LoadConfigFile` 直接解析，未知键被忽略（`src/config.cpp:52-130`）；`ValidateConfig` 只校验端口、TLS 配对、目录与日志级别（`src/config.cpp:210-247`）；无签名、无哈希清单、无配置变更审计（配置文件修改不经过应用）。 | P6.1 为配置文件增加 SM2 签名或“配置哈希锚定”（启动时与 DB/TPM 中记录比对）；安全相关键变更必须走 API 并审计；未知键告警；提供 `--verify-config`。 |
| INT-02 | 录像文件与索引应可验证完整性；损坏/缺失/篡改应被检出。 | 部分 | 录像分段落盘为 `.ts.part`，`fsync` 后重命名为 `.ts` 并写 `recordings` 行（`src/recording.cpp:271`、`403-465`）；元数据含 `size_bytes/duration_ms`（`src/app.cpp:1065-1077`），但无每段摘要/清单；回放与导出直接按路径读取（`src/app.cpp:8867-9004`），不校验哈希；启动不扫描孤立 `.part`。 | P6.1 为每段生成 SM3 摘要并写入 `recording_segments` 清单；播放/导出前校验，失败则告警并隔离；启动扫描 `.part` 与孤立文件，修复或标记；清单链式锚定到审计。 |
| INT-03 | 数据库应定期做完整性自检，损坏可发现、可恢复。 | 缺失 | SQLite 使用 WAL 与 `synchronous=NORMAL`（`src/app.cpp:939-940`）；打开后直接迁移，无 `PRAGMA integrity_check`/`quick_check`（`src/app.cpp:1340-1345`）；无内置备份/恢复接口；文档只建议手工备份数据目录（`docs/deployment.md:101-103`）。 | P6.1 启动执行 `PRAGMA quick_check`，失败则拒绝写入并告警；每日 `integrity_check` + 校验备份；提供带 SM3 清单的在线备份/恢复 API（维护模式 + 管理员）。 |
| INT-04 | 传输中的控制与媒体数据应能检出篡改，而不仅是随机误码。 | 部分 | 私有协议帧使用未加密 CRC32（IEEE 802.3）覆盖头+负载（`src/proto/proto_protocol.h:56-75`，`docs/private-protocol.md` §2.2）；CRC 可被任意重算，不提供抗恶意篡改；TCP 私有协议为明文；RTSP RTP 无应用层完整性；QUIC 由 TLS 1.3 提供完整性。 | P6.1 为 TCP 控制帧增加 HMAC-SM3（会话密钥派生）或直接要求 QUIC；为 RTP 增加 SRTP/国密媒体完整性（与 GB 35114 对齐）；保留 CRC 作为分帧校验。 |
| INT-05 | 录像/导出应带水印或来源证明，能证明由本设备产生且未被剪辑。 | 缺失 | `watermark` 只是通道配置 JSON（`src/app.cpp:4930-4955`），前端以 CSS 文本叠加显示（`frontend/src/components/LiveTile.vue:183`），不烧录进录像/导出文件；导出 MP4 无清单、无摘要、无签名（`src/app.cpp:8867-9004`）；`recording.cpp` 无 `drawtext`/水印处理。 | P6.1/P6.3 提供可选烧录水印（设备序列号、通道、时间）；导出时生成 SM3 清单并用设备 SM2 证书签名，随文件提供 `.manifest.json`；播放器/平台可验签。 |

## 7. 数据保密性

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| CON-01 | 除登录口令外，其他认证凭据也应抗离线破解、不可逆、可轮换。 | 部分 | RTSP Digest 使用 `rtsp_ha1 = MD5(username:UvNVR:password)`，无盐、可离线爆破（`src/app.cpp:289-291`、`1684-1689`）；会话令牌为 256 位随机值的 SHA-256（`src/app.cpp:274-280`、`1470-1497`）；API Token 为 `uvnvr_` + 256 位随机值的 SHA-256（`src/app.cpp:4636-4647`）；相机/SMTP 口令为 AES-256-GCM（见 CON-02）。 | 用 scrypt/Argon2 派生 RTSP 校验值或改用 SRP/HTTP Digest-SHA-256（无明文等价 HA1）；登录时迁移旧 HA1；禁用 Basic 或仅允许 TLS 下使用；支持凭据轮换与失效。 |
| CON-02 | 相机口令、SMTP 口令等敏感配置应加密存储；接口不得回显秘密。 | 满足 | AES-256-GCM，随机 12 字节 IV、16 字节 tag，密钥 `secret.key` 32 字节（`src/app.cpp:4798-4891`）；`secret.key` 创建时设为 `0600`（`src/app.cpp:4765-4768`）；通道接口仅返回 `hasPassword`（`src/app.cpp:4947-4955`）；设置审计前删除 `password`（`src/app.cpp:3022-3029`）。 | 保持；P6.1 将 AES-GCM 替换/封装为 SM4-GCM（国密要求时），并保持密钥层级不变。 |
| CON-03 | 密钥应安全生成、存储、轮换；密钥泄露影响范围可控。 | 部分 | `secret.key` 与数据库同目录、32 字节明文文件（`src/app.cpp:4731-4768`）；仅在首次创建时设置权限，已存在的文件不校正权限；无版本号、无轮换、无 TPM/OS keystore、无密钥备份/恢复流程；数据目录被复制即可解密相机口令。 | P6.1 引入密钥版本与 `key_id`，支持轮换与重加密；优先使用 OS keystore/TPM（Windows DPAPI、Linux TPM2）；加载时强制 `0600` 并校验属主；密钥与数据分离存放；密钥使用写审计。 |
| CON-04 | 录像、抓图、数据库、备份与导出等静态数据应加密或脱敏。 | 缺失 | 录像以 `0644` 创建为明文 TS（`src/recording.cpp:234-242`）；告警抓图明文 JPEG（`src/app.cpp:9401-9450`）；AI 全图/裁剪图明文落盘（`src/ai/ai_service.cpp:904-964`、`1096-1148`）；SQLite 数据库明文；备份为手工复制数据目录（`docs/deployment.md:101-103`）；导出 MP4 明文（`src/app.cpp:8867-9004`）。 | P6.1 设计信封加密：数据密钥（SM4-GCM）加密录像/抓图/导出，密钥由设备主密钥封装；数据库敏感列（口令、特征、PII）列级加密；备份默认加密并带清单；提供密钥托管/恢复。 |
| CON-05 | 人脸/车牌等个人信息应最小化采集、受控访问、限期保存、可删除。 | 部分 | 人脸特征以原始 `BLOB` 存 `ai_face_events.feature_blob`（`src/app.cpp:1121-1137`），全图/裁剪图明文（`src/ai/ai_service.cpp:1096-1148`）；保留期 `ai.snapshotRetentionDays=30`（`src/app.cpp:1270`、`src/ai/ai_service.cpp:496`）；`alert.view` 即可读取 AI 事件与抓图（`src/app.cpp:9633-9770`），无单独 PII 权限、无脱敏、无导出审批。 | 增加 `ai.pii.view`/`ai.pii.export` 权限；特征模板加密存储、可删除并审计；抓图访问短期签名 URL；导出脱敏/审批；按目的限定保留期与自动清理证明。 |

## 8. 通信安全

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| COM-01 | 管理面应默认或强制使用 TLS，禁用弱协议，设置 HSTS。 | 部分 | HTTPS 为可选：仅在配置 `tls.cert`+`tls.key` 或 `--tls-self-signed` 时启用（`src/app.cpp:2244-2260`，`src/config.h:20-22`）；默认 `http://0.0.0.0:8080`；无最低 TLS 版本/套件配置；安全响应头无 HSTS（`src/app.cpp:2074-2087`）；自签证书仅用于 localhost（`src/app.cpp:2253`）。 | P6.1 生产模式默认要求 TLS，HTTP 仅监听回环或重定向；增加 `tls.minVersion`/`tls.ciphers`；启用 HSTS（含子域）；证书过期告警；提供国密 TLS 开关（见 COM-02）。 |
| COM-02 | 按项目要求支持国密 TLS（GM/T 0024），用于 GB 35114 信令与平台接入。 | 部分 | 新增 `UVNVR_ENABLE_GM_TLS`（默认 OFF）与 Tongsuo 适配层 `src/gb35114/tls_gm.{h,cpp}`：TLCP（GB/T 38636-2020）双证书、GM TLS 1.3、非阻塞握手、对端校验；`gm_tls_cli` 与 `tests/gm_tls_test` 实测 `NTLSv1.1` + `ECDHE-SM2-SM4-CBC-SM3` 成功，MinGW 交叉编译并 Wine 运行成功（`docs/gm-tls.md`）。模块未链接进 `uvnvr`，SIP 事务层尚未落地，现有 QUIC 仍固定 TLS 1.3（`src/proto/proto_server.cpp:468-469`）。 | P6.2/P6.3 在 SIP 栈落地时把 `GmTlsSession` 接入 REGISTER/INVITE 的 libuv 状态机；补充 CRL/OCSP、会话复用与双证书到期告警；与支持 35114 的平台联调。 |
| COM-03 | 视频、控制信令与私有协议应加密传输；不得默认明文。 | 部分 | RTSP 为明文 RTP/RTCP，Basic/Digest 明文传输（`src/rtsp.cpp:1191-1267`）；私有协议 TCP 为明文，QUIC 为 TLS 1.3（`docs/private-protocol.md` §1，`src/proto/proto_server.cpp:733`）；Web 默认 HTTP（COM-01）；仅 QUIC/HTTPS 提供传输加密。 | P6.1 默认启用 QUIC 并逐步淘汰明文 TCP；RTSP 增加 RTSPS/SRTP（可选强制）；GB 28181/35114 信令走国密 TLS；文档明确“仅可信网络可关闭加密”。 |
| COM-04 | 设备/平台接入应使用强认证，凭据不得以可重放/弱摘要方式在明文通道传输。 | 部分 | RTSP 同时接受 Basic 与 Digest-MD5，且允许 URL 内嵌凭据（`src/rtsp.cpp:1191-1260`、`1263-1267`）；ONVIF 与品牌 HTTP 只支持 `http://`，使用 Digest-MD5 或 Basic（`src/onvif.cpp:217-219`、`415-470`；`src/brand_http.cpp:367-450`）；相机连接无 TLS。 | 默认禁用 Basic 与 URL 凭据；RTSP 改用 Digest-SHA-256 或 RTSPS；相机接入优先 HTTPS/ONVIF over TLS，至少对凭据做通道加密；失败计数与来源限制。 |
| COM-05 | 控制信令应抗重放：随机挑战、一次性 nonce、时间窗/序号。 | 部分 | RTSP nonce 在进程启动时生成一次，所有连接共享，不校验 `nc`/时间窗，未认证先解析 URL（`src/rtsp.cpp:1004-1008`、`1210-1238`）；私有协议会话 ID 为 64 位随机数（`src/proto/proto_server.cpp:37-47`、`759-768`），但请求帧无 nonce/时间戳/序号绑定，会话有效期内可重放；QUIC 传输层防重放，应用层无。 | RTSP nonce 一次性或按时间窗轮换，校验 `nc` 单调性；私有协议请求增加单调序号 + 时间戳 + HMAC（会话密钥），服务端滑动窗口拒绝重放；为拒绝事件写审计。 |
| COM-06 | 应校验对端证书/身份；不得无条件信任自签证书。 | 部分 | SMTP 使用 TLS 并校验证书（`src/app.cpp:472-495`）；HTTPS 服务端可配置证书，自签模式供本地使用（`src/app.cpp:2244-2260`）；私有 QUIC 默认生成自签证书，客户端无证书固定/双向认证（`src/proto/proto_server.cpp:471-477`、`docs/private-protocol.md` §2.4）；相机 HTTP 无 TLS，ONVIF 不支持 https（`src/onvif.cpp:217-219`）。 | 私有协议支持 CA 签发证书、证书固定与可选双向 TLS；QUIC 客户端校验证书；GB 35114 使用设备证书双向认证；相机接入支持 HTTPS 并校验证书（可配置信任库）。 |

## 9. 软件容错

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| FLT-01 | 对异常输入、超大请求、非法类型、路径穿越应拒绝且不崩溃。 | 部分 | 绝大多数 JSON 路由使用 `json::parse(..., nullptr, false)` 并检查 `is_discarded()`，对 ID/范围/长度做校验（示例：`src/app.cpp:2735-2785`、`5930-5960`、`6839-6855`、`8180-8185`）；HTTP 框架请求体默认上限 16 MiB、头 16 KiB（libuvcpp `uvcpp_web_app.h`）；但无统一 schema 校验，部分字符串字段只限长度；Windows 下 ffmpeg 命令由拼接字符串构造（`src/media.cpp:1601-1617`），URL 中若含引号需依赖输入校验。 | P6.1 引入统一请求 schema 与最大字段长度；URL/命令参数使用参数数组（POSIX 已使用 `execvp`，Windows 改为 `CreateProcess` 参数数组或严格转义）；为扫描/探测接口增加速率与目标网段限制。 |
| FLT-02 | 通道数、内存、单客户端积压、导出大小等资源应有上限。 | 满足 | 默认通道上限 128（`src/app.cpp:5778-5790`、`3257`）；流客户端 backlog 上限 2 MiB / 1 秒（`src/media.cpp:491-541`）；快照缓存 4 MiB（`src/media.cpp:1161-1163`）；导出限制 6 小时且 4 GiB（`src/app.cpp:8888-8891`、`8935-8940`）；AI 队列深度可配（`src/app.cpp:1265`）；数据库连接池与超时（`src/app.cpp:1340-1348`）。 | 保持；补做资源耗尽专项测试（大量并发连接、超大 JSON、满队列）并形成报告。 |
| FLT-03 | 磁盘满/存储故障时应停止写入、保留可用空间并告警，不得破坏已有录像。 | 满足 | 存储卷支持 `loop` 模式、`max_usage_pct`、`reserve_gb`、`retention_days`（`src/recording.cpp:800-880`）；达到阈值按最早录像删除（`src/recording.cpp:860-880`）；磁盘错误与 90% 使用率告警（`src/app.cpp:9210-9242`）；写失败记录错误并继续/重连（`src/recording.cpp:520-534`）。 | 保持；补做真实磁盘满测试，确认告警、停止写入与恢复后自动续录。 |
| FLT-04 | 媒体进程与主进程异常应自动恢复；服务管理器应能拉起。 | 部分 | ffmpeg 子进程退出后按 1/2/5/10/30 秒退避重连（`src/media.cpp:1451`、`1574-1577`），并设置 `PR_SET_PDEATHSIG`（`src/media.cpp:1651`）；systemd 单元 `Restart=on-failure`、`RestartSec=5`（`packaging/linux/uvnvr.service:15-16`）；Windows 服务由 SCM 管理；但应用内无主进程 watchdog，RTSP/HTTP 监听失败只告警不退出（`src/app.cpp:2320-2331`），非 systemd 运行方式无自动拉起。 | P6.1 增加健康检查/自守护（可选 supervisor 模式），关键监听失败按配置退出以便服务管理器重启；记录进程重启次数与原因；Windows 服务恢复策略显式配置。 |
| FLT-05 | 崩溃/断电后索引与文件应保持一致，可自动修复。 | 部分 | SQLite 使用 WAL 与 `synchronous=NORMAL`（`src/app.cpp:939-940`）；录像先写 `.part`，`fsync` 后重命名并写索引（`src/recording.cpp:271`、`403-465`）；但启动不扫描孤立 `.part`/索引不一致，未做数据库完整性自检；`synchronous=NORMAL` 在断电时可能丢失最近事务。 | P6.1 启动恢复流程：扫描 `.part`，按可解析 TS 尾部恢复或隔离；核对文件大小与索引，修正/标记；关键元数据使用 `synchronous=FULL` 或显式 checkpoint；提供恢复报告。 |
| FLT-06 | 登录锁定、权限拒绝、审计异常、存储异常等安全事件应告警并可查询。 | 部分 | 有通道离线、磁盘错误/满、服务错误告警（`src/app.cpp:9017-9064`、`9108-9115`、`9210-9242`）；告警可邮件外发（`src/app.cpp:9035-9060`）；但登录锁定、权限拒绝、审计写失败、证书过期、时钟漂移没有告警；无告警去重/升级策略。 | P6.1 增加安全事件告警类型（`auth_lockout`、`permission_denied`、`audit_write_failure`、`cert_expiry`、`clock_drift`），统一进入 `alerts` 并支持邮件/Webhook；对高频事件限流聚合。 |

## 10. 升级安全

| 条目 | 要求 | 现状 | 证据位置 | 整改任务 |
| --- | --- | --- | --- | --- |
| UPG-01 | 升级包应经过数字签名验证，来源可信，拒绝伪造包。 | 缺失 | 产品没有固件/软件包上传或升级接口；`AllPermissions` 无升级权限（`src/app.cpp:738-748`）；无签名清单、无公钥、无验签代码；发行包脚本直接替换文件（`packaging/linux/build-package.sh`、`postinst`）；Windows/macOS 安装包未签名（`docs/deployment.md` 明确说明）。 | P6.1 设计升级包格式（版本、目标型号、哈希、SM2 签名、签名证书链）；设备内置升级公钥；校验签名、型号、版本与哈希后才允许安装；提供离线验签工具。 |
| UPG-02 | 升级过程应防止降级/回滚到已知漏洞版本。 | 缺失 | 无升级流程；唯一的版本写接口 `PUT /api/v1/system/version` 只改显示用 `version_override`，可任意降低或清空，不校验单调性（`src/app.cpp:4494-4551`）；无最低安全版本、无防回滚计数器。 | P6.1 在安全存储中保存单调递增的 `min_security_version`/回滚计数器，升级包版本必须不低于该值；拒绝降级并审计；提供维护模式下的受控降级流程（双人审批）。 |
| UPG-03 | 升级应原子化；失败可回滚到上一可用版本。 | 缺失 | 无 A/B 分区、无事务安装、无回滚点；Linux 包通过 `prerm`/`postinst` 停止服务并替换文件（`packaging/linux/prerm`、`postinst`）；Windows 安装器在升级时停止旧实例并替换文件（`docs/deployment.md`）；失败后需人工重装。 | P6.1 采用 A/B 目录或快照安装：新版本写入备用槽，健康检查通过后切换启动指针；保留上一版本与配置/数据库备份；失败自动回滚并报告。 |
| UPG-04 | 升级操作应仅授权管理员可执行，并完整审计（版本、包哈希、结果、回滚）。 | 缺失 | 无升级接口，因此无升级权限与审计；维护模式只覆盖序列号/版本显示/审计清除/格式化/删除（`src/app.cpp:3159-3170`）；版本显示变更虽写审计（`src/app.cpp:4541-4543`），但与真实升级无关。 | P6.1 增加 `system.upgrade` 权限（默认仅 admin），升级前再次认证；审计记录包名、SM2 签名主体、哈希、源版本/目标版本、开始/结束、结果与回滚原因；失败告警。 |
| UPG-05 | 应提供可核验的版本与第三方组件清单（SBOM），发行物可追溯。 | 部分 | `VERSION`、`CHANGELOG.md`、`THIRD_PARTY_NOTICES.md` 存在；`THIRD_PARTY_NOTICES.md:30` 声称“权威列表为随版本附带的 SBOM”，但仓库与构建产物中没有 SBOM 文件、无依赖哈希/签名；版本接口返回 `UVNVR_VERSION` 与 `UVNVR_GIT_SHA`（`src/app.cpp:2395-2403`）。 | P6.1 在 CI 生成 CycloneDX/SPDX SBOM，随发行包附带并签名；构建可复现；`/api/v1/system/info` 可选返回 SBOM 摘要；送检材料包含组件许可证与哈希清单。 |

## 11. 整改优先级与阶段映射

| 优先级 | 条目 | 阶段 | 说明 |
| --- | --- | --- | --- |
| P0（送检阻断） | AUD-03、INT-01、INT-03、CON-04、COM-02、UPG-01、UPG-02、UPG-03、UPG-04 | P6.1 / P6.3 | 直接影响“防篡改、保密、升级安全”结论，必须在送检前完成。 |
| P0（身份与授权） | IDN-05、IDN-07、IDN-08、ACC-02、ACC-04 | P6.1 | 锁定策略、双因子、会话失效与对象级授权是常见否决项。 |
| P1 | AUD-01、AUD-02、AUD-06、AUD-07、INT-02、INT-05、CON-01、CON-03、CON-05 | P6.1 | 补齐证据链与数据保护，支撑 P6.4 自测报告。 |
| P1（通信） | COM-01、COM-03、COM-04、COM-05、COM-06 | P6.2 / P6.3 | 与 GB 28181 互通、GB 35114 A/B 级同步实现。 |
| P2 | FLT-01、FLT-04、FLT-05、FLT-06、UPG-05 | P6.1 / P6.4 | 专项测试与运维加固，随自测报告完善。 |
| 保持 | IDN-01、IDN-02、IDN-03、IDN-04、ACC-01、ACC-03、AUD-04、AUD-05、CON-02、FLT-02、FLT-03 | — | 已有实现，纳入回归测试，防止整改引入退化。 |

## 12. 自测用例索引

“部分/缺失”条目与用例一一对应，详细步骤见
[`compliance-testcases.md`](compliance-testcases.md)。

| 用例 | 条目 | 用例 | 条目 |
| --- | --- | --- | --- |
| TC-IDN-05 | IDN-05 | TC-INT-01 | INT-01 |
| TC-IDN-06 | IDN-06 | TC-INT-02 | INT-02 |
| TC-IDN-07 | IDN-07 | TC-INT-03 | INT-03 |
| TC-IDN-08 | IDN-08 | TC-INT-04 | INT-04 |
| TC-ACC-02 | ACC-02 | TC-INT-05 | INT-05 |
| TC-ACC-04 | ACC-04 | TC-CON-01 | CON-01 |
| TC-AUD-01 | AUD-01 | TC-CON-03 | CON-03 |
| TC-AUD-02 | AUD-02 | TC-CON-04 | CON-04 |
| TC-AUD-03 | AUD-03 | TC-CON-05 | CON-05 |
| TC-AUD-06 | AUD-06 | TC-COM-01 | COM-01 |
| TC-AUD-07 | AUD-07 | TC-COM-02 | COM-02 |
| TC-FLT-01 | FLT-01 | TC-COM-03 | COM-03 |
| TC-FLT-04 | FLT-04 | TC-COM-04 | COM-04 |
| TC-FLT-05 | FLT-05 | TC-COM-05 | COM-05 |
| TC-FLT-06 | FLT-06 | TC-COM-06 | COM-06 |
| TC-UPG-01 | UPG-01 | TC-UPG-02 | UPG-02 |
| TC-UPG-03 | UPG-03 | TC-UPG-04 | UPG-04 |
| TC-UPG-05 | UPG-05 | | |

## 13. 统计口径与校验命令

以下命令在本文件上运行，用于核对“条目数、满足数、部分/缺失数”。表格行以 `| IDN-` 等开头，
状态列为独立的 `| 满足 |`、`| 部分 |`、`| 缺失 |`、`| 不适用 |`。

```bash
# 条目总数（每个条目行以 8 个类别前缀之一开头）
grep -cE '^\| (IDN|ACC|AUD|INT|CON|COM|FLT|UPG)-[0-9]{2} \|' docs/compliance-gap.md
# 各状态数量
grep -cE '^\| (IDN|ACC|AUD|INT|CON|COM|FLT|UPG)-[0-9]{2} \|.*\| 满足 \|' docs/compliance-gap.md
grep -cE '^\| (IDN|ACC|AUD|INT|CON|COM|FLT|UPG)-[0-9]{2} \|.*\| 部分 \|' docs/compliance-gap.md
grep -cE '^\| (IDN|ACC|AUD|INT|CON|COM|FLT|UPG)-[0-9]{2} \|.*\| 缺失 \|' docs/compliance-gap.md
grep -cE '^\| (IDN|ACC|AUD|INT|CON|COM|FLT|UPG)-[0-9]{2} \|.*\| 不适用 \|' docs/compliance-gap.md
# 大类数量（第 3–10 节各一个 ## 标题）
grep -cE '^## [3-9]\.|^## 10\.' docs/compliance-gap.md
# 自测用例数量
grep -cE '^### TC-[A-Z]{3}-[0-9]{2} ' docs/compliance-testcases.md
```

当前基线（2026-11-24，提交 `443e78b`）的统计结果：

```text
条目数=46  满足=11  部分=22  缺失=13  不适用=0
大类数=8   用例数=35
```

> 统计只匹配第 3–10 节的条目行；第 12 节的索引表以 `TC-` 开头，不会被条目正则计入。
> 整改后应重跑同一组命令，并把新的数字写入 P6.4 自测报告。

## 14. 与设计基线的对应关系

- 身份鉴别、访问控制、审计 → 对应 GB 35114 的设备/平台身份认证、审计与证书状态要求。
- 数据完整性、数据保密性 → 对应 GB 35114 A 级签名、B 级 SM4 加密与 VKEK。
- 通信安全 → 对应 GB 35114 国密 TLS 与 GB 28181 SIP/媒体传输。
- 软件容错、升级安全 → 对应 GA/T 2155.2 产品自身安全，也是送检材料（P6.4）的必要组成。

本矩阵只覆盖“产品检测”视角；GB/T 28181 的协议互通与 GB 35114 的密码协议一致性另有专项用例，
不在本文件的 46 条之内。
