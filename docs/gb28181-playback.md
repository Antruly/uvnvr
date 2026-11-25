# GB/T 28181 录像查询与回放（P6.2d）

本模块把本地录像索引和回放能力接到 GB/T 28181 设备侧信令上，作为下级设备向平台提供
`RecordInfo` 查询与回放流。设计基线见
[`compliance-gb28181-gb35114.md`](compliance-gb28181-gb35114.md)。

编译开关为 `UVNVR_ENABLE_GB28181`，默认 `OFF`。默认构建不编译、不链接该模块，
预览/录像/回放/ONVIF/私有协议行为不变；打开开关后也只有 `gb28181.enabled=1` 时
App 才启动服务。

## 1. 组件

| 文件 | 职责 |
| --- | --- |
| `src/gb28181/sip.cpp` | 自研 SIP 栈：REGISTER/Digest、Keepalive、MESSAGE，以及 INVITE/ACK/BYE/INFO 回调与响应发送 |
| `src/gb28181/manscdp.cpp` | MANSCDP XML：`RecordInfo` 查询/响应、`Control/Playback` 倍速与拖拽 |
| `src/gb28181/ts_ps.cpp` | MPEG-TS 解复用 → H.264/H.265 访问单元 → PS 封装 → RTP 发包；实时与回放共用 |
| `src/gb28181/playback.cpp` | 回放读取线程：切片定位、关键帧对齐、PTS 节奏、倍速/暂停/拖拽 |
| `src/gb28181/media_session.cpp` | INVITE/ACK/BYE 与 RTP/RTCP 会话、SSRC、端口分配 |
| `src/gb28181/service.cpp` | 设备侧总装：通道映射、RecordInfo 查询、回放/实时分发、MANSCDP 响应 |

## 2. 配置

设置在 `settings` 表中，App 启动时读取：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `gb28181.enabled` | `0` | 打开设备侧服务 |
| `gb28181.device_id` | 由 center code 生成 | NVR 的 20 位国标 ID（类型码 118） |
| `gb28181.center_code` | `34020000` | 生成通道 ID 的 8 位中心编码 |
| `gb28181.channel.<本地通道ID>.device_id` | 空 | 可选，覆盖某个通道的 20 位 ID |
| `gb28181.server_host` / `server_port` | 空 / `5060` | 平台/注册服务器 |
| `gb28181.transport` | `udp` | `udp` 或 `tcp` |
| `gb28181.local_ip` / `bind_ip` / `local_port` | 空 / 空 / `0` | 本机 SIP 地址；固定端口便于平台回呼 |
| `gb28181.username` / `password` | 设备 ID / 空 | Digest 凭据 |
| `gb28181.stream` / `fallback_stream` | `main` / `sub` | 回放优先码流；主码流无索引时回退 |
| `gb28181.rtp_port_min` / `max` | `30000` / `30998` | 媒体端口范围 |
| `gb28181.max_sessions` | `8` | 同时媒体会话上限 |
| `gb28181.playback_burst_ms` | `500` | 回放启动时预发的媒体时长 |

通道的 20 位 ID 默认由 `中心编码(8) + 00 + 132 + 序号(7)` 生成，序号使用本地
`channels.id`。生产部署建议显式配置 `gb28181.device_id` 与各通道 ID。

## 3. RecordInfo

平台在 SIP MESSAGE 中发送：

```xml
<Query>
  <CmdType>RecordInfo</CmdType>
  <SN>100</SN>
  <DeviceID>34020000001320000001</DeviceID>
  <StartTime>2026-11-25T00:00:00</StartTime>
  <EndTime>2026-11-25T23:59:59</EndTime>
  <Type>all</Type>
</Query>
```

- `DeviceID` 可以是 NVR ID（查询全部通道）或通道 ID（只查询该通道）。
- 时间按设备本地时区解释；`Type` 支持 `all`、`time`、`alarm`、`manual`。
- 本地 `recordings.record_type` 映射：`timed → time`、`event → alarm`、`manual → manual`。
- 正在写入、尚未入索引的切片也会出现在结果中（`FilePath` 指向 `.part`），便于平台
  查询当前录像。
- `FilePath` 输出为相对数据目录的路径，响应最多返回 `10000` 条，避免单条 XML 过大。

响应示例：

```xml
<Response>
  <CmdType>RecordInfo</CmdType>
  <SN>100</SN>
  <DeviceID>34020000001320000001</DeviceID>
  <SumNum>1</SumNum>
  <RecordList Num="1">
    <Item>
      <DeviceID>34020000001320000001</DeviceID>
      <Name>gate</Name>
      <FilePath>recordings/2026/11/25/gate.ts</FilePath>
      <StartTime>2026-11-25T12:00:00</StartTime>
      <EndTime>2026-11-25T12:00:30</EndTime>
      <Secrecy>0</Secrecy>
      <Type>time</Type>
      <FileSize>861980</FileSize>
    </Item>
  </RecordList>
</Response>
```

## 4. 回放 INVITE

平台发起 INVITE，SDP 使用 `s=Playback` 和 `u=<通道ID>:<开始>:<结束>`（本地时间，
`YYYYMMDDHHMMSS`），媒体行为 `PS/90000`：

```
v=0
o=34020000001320000001 0 0 IN IP4 192.0.2.10
s=Playback
u=34020000001320000001:20261125120000:20261125120100
c=IN IP4 192.0.2.10
t=0 0
m=video 40000 RTP/AVP 96
a=recvonly
a=rtpmap:96 PS/90000
y=01020304
```

设备回 200 OK，携带 `MediaSessionManager` 分配的本地 RTP 端口、SSRC 和 `a=sendonly`。
平台 ACK 后开始推流；BYE 关闭会话并发送 RTCP BYE。

- 只接受 `s=Playback` / 带 `u=` 范围的请求；`s=Play` 走实时流路径。
- 时间范围必须有效；结束时间缺省时按开始时间 + 1 小时处理。
- 主码流在该范围内没有索引时自动回退到 `gb28181.fallback_stream`。

## 5. 倍速、暂停、拖拽

平台在已建立的对话中发 SIP INFO，正文为 MANSCDP `Control/Playback`：

```xml
<Control>
  <CmdType>Playback</CmdType>
  <SN>31</SN>
  <DeviceID>34020000001320000001</DeviceID>
  <Scale>2.00</Scale>
  <Range>2026-11-25T12:00:20/2026-11-25T12:00:35</Range>
</Control>
```

- `Scale > 0`：前向倍速，范围 `0.25–16`，`1.00` 为实时。
- `Scale = 0`：暂停；读取线程停止发包，恢复后重新建立节奏锚点，不补发积压。
- `Scale < 0`：反向播放未实现，设备回 `488` 并保持当前方向，不会静默按正放处理。
- `Range`：拖拽到新位置，从最近的关键帧重新开始；暂停状态下拖拽后仍保持暂停。
- 回放节奏以 PES PTS 为基准（90 kHz），按 `Scale` 折算墙钟时间；切片切换时重新锚定。

## 6. 与实时流共用管线

实时预览和回放都使用同一条管线：

```
MediaService 原始 TS ─┐
                      ├─ TsDemuxer → SplitAnnexBAccessUnits → PsMuxer → RTP
录像切片文件 TS ──────┘
```

- 实时订阅 `MediaService::AddRawSubscriber`，回放读取 `recordings` 索引中的切片。
- 两边都调用 `PsRtpPipeline`，因此 PS 头、PSM/CRC、PTS 和关键帧标记只有一份实现。
- 回放读取线程不直接碰 SIP/RTP 句柄：帧进入发送队列，由 SIP 事件循环线程调用
  `MediaSessionManager::SendFrame`，避免与 RTCP/Tick 并发。
- 打开 `UVNVR_ENABLE_SECURE` 且启用了录像静态加密时，App 向回放注入 SM4 范围解密
  读取回调，`.ts.enc` 切片仍可回放。

## 7. 验证

单元测试：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUVNVR_ENABLE_GB28181=ON \
  -DUVNVR_BUILD_TESTS=ON \
  -DUVNVR_LIBUVCPP_ROOT=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DCMAKE_PREFIX_PATH=/home/zwy/.local/uvcpp-1.9.1/usr
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

平台侧联调脚本（真实 uvnvr + 临时数据库 + ffmpeg 生成的 H.264 TS）：

```sh
python3 tests/integration/gb28181_record_playback.py --binary build/uvnvr
```

脚本会打印 RecordInfo 响应、INVITE 200 OK SDP、RTP/PS 解码结果，以及
`Scale=2` 的实测倍率、`Scale=0` 的 1 秒发包数、拖拽后的 RTP 时间戳重置。

## 8. 已知限制

- 不支持反向播放（`Scale < 0`）。
- 回放流播完或读取失败时关闭媒体会话并发送 RTCP BYE，但设备侧尚未主动发送 SIP
  BYE；平台通常会在媒体结束后自行发 BYE。
- 回放索引按切片粒度返回，切片内部的精确到帧定位依赖关键帧对齐；拖拽会退到
  请求位置之前最近的关键帧。
- RecordInfo 单次响应最多 10000 条，暂未实现分页。
