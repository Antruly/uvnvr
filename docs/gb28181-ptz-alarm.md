# GB/T 28181 云台控制、报警上报与订阅（P6.2e）

本文件说明 GH-20 增加的三部分能力：

1. `PTZCmd`（`DeviceControl`）的解析与下发；
2. `Alarm` 通知的上报，以及 AI 车牌/人脸事件的接入；
3. SIP `SUBSCRIBE` / `NOTIFY` 订阅与取消订阅。

模块仍由 `-DUVNVR_ENABLE_GB28181=ON` 编译开关隔离，并只在
`gb28181.enabled=1` 时启动；默认构建和现有预览/录像/回放/ONVIF/私有协议
路径不受影响。

## 1. PTZCmd

### 1.1 命令格式

平台在 SIP `MESSAGE` 的 MANSCDP `DeviceControl` 文档中下发 8 字节命令：

```xml
<Control>
  <CmdType>DeviceControl</CmdType>
  <SN>1</SN>
  <DeviceID>34020000001320000001</DeviceID>
  <PTZCmd>A501FF00000000A5</PTZCmd>
  <Info><ControlPriority>1</ControlPriority></Info>
</Control>
```

`PTZCmd` 为 16 个十六进制字符：

| 字节 | 含义 |
|---|---|
| 0 | 固定 `0xA5` |
| 1 | 动作码（见下表） |
| 2 | 水平速度 `0x00..0xFF` |
| 3 | 垂直速度 `0x00..0xFF` |
| 4 | 变倍速度 `0x00..0xFF` |
| 5 | 预置位编号高字节 |
| 6 | 预置位编号低字节 |
| 7 | 校验码：前 7 字节之和模 256 |

动作码：`0x00` 停止，`0x01..0x08` 八个方向，`0x09/0x0A` 变倍，
`0x0B..0x0E` 聚焦/光圈，`0x0F/0x10/0x11` 设置/调用/删除预置位。
解码器同时接受部分设备使用的 `0x81/0x82/0x83` 预置位写法，编码器始终
输出标准动作码。首字节、校验码和预置位编号都会校验；非法命令不会下发。

`PtzVelocityFromCmd()` 把命令归一化为 `pan/tilt/zoom ∈ [-1,1]`，与具体
摄像机协议解耦；该函数是单元测试的检查点。

### 1.2 下发路径

```
SIP MESSAGE ──> ManscdpResponder::Handle
                    └─ ParseDeviceControl
                        └─ Gb28181Service ptz handler
                            └─ App::QueuePtz (worker thread)
                                └─ OnvifPtzControl
```

- PTZ 处理函数运行在 GB28181 事件循环线程上，因此只做校验和入队，不做
  网络 I/O。`App::QueuePtz()` 在上传工作线程中查询通道、解密密码并调用
  ONVIF。
- ONVIF 侧使用 ver20 PTZ 服务：`GetCapabilities(Category=PTZ)` 解析
  `XAddr`，必要时回退到 `/onvif/ptz_service`；`GetProfiles` 取第一个
  profile token；随后发送 `ContinuousMove`、`Stop`、`SetPreset`、
  `GotoPreset` 或 `RemovePreset`。
- 私有协议新增 `PTZ_CONTROL`（opcode `0x0034`），字段为
  `channelId`、`action`、`speed`、`preset`，与 GB 路径共用同一个
  `App::QueuePtz()` 和 ONVIF 后端。这满足“下发到 ONVIF/私有协议”的双入口
  要求，同时只有一份摄像机控制实现。
- 只支持 ONVIF 通道。通道没有可用 ONVIF 服务时，失败写入日志；SIP
  层已经对原 MESSAGE 回了 200 OK，符合设备侧异步执行的语义。

## 2. Alarm 上报

### 2.1 文档

`Gb28181Service::ReportAlarm()` 线程安全，任何线程都可以调用。事件循环
在下一个 tick 中把它编码为：

```xml
<Notify>
  <CmdType>Alarm</CmdType>
  <SN>1</SN>
  <DeviceID>34020000001320000001</DeviceID>
  <AlarmPriority>2</AlarmPriority>
  <AlarmMethod>5</AlarmMethod>
  <AlarmTime>2026-11-25T20:21:36</AlarmTime>
  <AlarmDescription>AI 车牌预警: 京A12345</AlarmDescription>
  <AlarmInfo>
    <AlarmType>1</AlarmType>
    <AlarmTypeParam>AI-PLATE</AlarmTypeParam>
  </AlarmInfo>
</Notify>
```

- 先通过 SIP `MESSAGE` 发给平台；
- 再向所有未过期的 `Event: Alarm` 订阅者发送 `NOTIFY`；
- `DeviceID` 优先使用本地通道映射出的 20 位通道 ID，通道为 0 时使用
  NVR 自身 ID；
- `SN` 单调递增，`AlarmTime` 为设备本地时间。

### 2.2 告警映射

`App::RaiseAlert()` 是所有告警的统一入口，AI 车牌/人脸检测在工作线程中
调用它。GB 桥接在同一个位置完成，因此 AI 事件不需要单独的协议代码：

| 本地告警类型 | AlarmMethod | AlarmType | AlarmTypeParam |
|---|---|---|---|
| `ai_plate` | 5（视频报警） | 1 | `AI-PLATE` |
| `ai_face` | 5 | 1 | `AI-FACE` |
| `motion` / `person` / `intrusion` / `tamper` | 5 | 1 | 对应大写名称 |
| `channel_offline` | 6（设备故障） | 2 | `OFFLINE` |
| `channel_online` | 6 | 2 | `ONLINE` |
| `disk_error` / `storage_full` | 6 | 2 | `DISK-ERROR` / `STORAGE-FULL` |
| 其他 | 5 | 1 | `NVR-ALERT` |

优先级：`error → 1`、`warning → 2`、`info → 3`；`channel_online` 固定为
4。验收要求中的“AI 事件能作为报警推送到平台”就是 `ai_plate`/`ai_face`
两行，`tests/integration/gb28181_ptz_alarm.py` 用 stub 车牌引擎实际跑通。

## 3. 订阅与取消订阅

设备侧支持事件包 `Catalog` 和 `Alarm`：

- `SUBSCRIBE` 由 `SipUserAgent` 交给服务处理（不自动回 200），服务回
  `200 OK`（带 `Expires`、`Event`、`Contact` 和稳定的 To tag），随后立即
  发一个 `NOTIFY`：
  - `Catalog`：`Subscription-State: active`，正文为当前目录；
  - `Alarm`：`Subscription-State: active`，正文为空，后续报警事件填充。
- 刷新：同一 `Call-ID` 的新 `SUBSCRIBE` 更新过期时间并再次发送初始
  `NOTIFY`。
- 取消：`Expires: 0` 回 200，发送
  `Subscription-State: terminated` 的 `NOTIFY` 并删除订阅。
- 自然过期：tick 中清理并发送 `terminated`。
- 出站 `NOTIFY` 的 CSeq 按 `Call-ID` 独立递增，`To` 使用订阅建立时的
  tag；目标地址取 `SUBSCRIBE` 的 `Contact`，为空时回退到注册服务器。
- 入站 `NOTIFY`（平台侧通知）先回 200 OK，再交给 `on_notify` 回调记录。

## 4. 配置

无需新增启动参数；沿用现有 `gb28181.*` settings：

| key | 说明 |
|---|---|
| `gb28181.channel.<id>.device_id` | 通道 20 位 ID；PTZ/报警都使用该映射 |
| `gb28181.alarm.*` | 无（默认全部转发） |

ALARM 转发没有单独开关；只要 `gb28181.enabled=1` 且服务启动成功，所有
`RaiseAlert` 都会进入 GB 上报队列。

## 5. 验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_ENABLE_GB28181=ON -DUVNVR_BUILD_TESTS=ON \
  -DUVNVR_LIBUVCPP_ROOT=$HOME/.local/uvcpp-1.9.1/usr \
  -DCMAKE_PREFIX_PATH=$HOME/.local/uvcpp-1.9.1/usr
cmake --build build -j4

# 编解码、校验、速度换算、告警文档、PTZ 分发
./build/tests/gb28181_manscdp_test

# ONVIF ver20 的 ContinuousMove/Stop/预置位（进程内假摄像机）
./build/tests/onvif_ptz_test

# 真实 uvnvr + 假 SIP 平台 + 假 ONVIF 摄像机 + 合成 RTSP + AI stub
python3 tests/integration/gb28181_ptz_alarm.py --binary build/uvnvr
```

集成脚本打印平台收到的每条 PTZ/报警证据，包括 AI 车牌
`京A12345` 对应的完整 Alarm XML 和订阅 NOTIFY。

## 6. 边界

- 云台绝对定位、巡航、雨刷等 GB/T 28181 扩展命令未在本次范围内；未知
  动作码会被拒绝并留在日志里。
- 聚焦/光圈（`0x0B..0x0E`）没有映射到 ONVIF `ContinuousMove`（该接口
  只覆盖 pan/tilt/zoom），会返回不支持。
- 私有协议 `PTZ_CONTROL` 是本仓库的增量 opcode；现有 SDK/客户端不调用
  它时行为不变。
- 真机联调仍需在支持 ONVIF PTZ 的摄像机上确认速度方向和预置位编号。
