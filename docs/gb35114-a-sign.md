# GB 35114 A 级：I 帧签名与验证（P6.3a）

本文描述 `src/gb35114/sign.{h,cpp}` 与 `tools/gb35114_verify` 的实现、数据格式、
接入方式和验证方法。设计基线见
[`compliance-gb28181-gb35114.md`](compliance-gb28181-gb35114.md) §1.2。

A 级只要求完整性和身份认证，不加密视频。本模块因此**不修改媒体字节**：原始
MPEG-TS / Annex B H.264 照常输出，签名记录走独立通道（直播）或旁路文件（录像）。

## 1. 编译开关

```sh
cmake -S . -B build \
  -DUVNVR_ENABLE_GB35114=ON \
  -DUVNVR_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

`UVNVR_ENABLE_GB35114` 默认 **OFF**。它依赖 `UVNVR_ENABLE_GM_CRYPTO`（OpenSSL 3
的 SM2/SM3/SM4）；后者不可用时本模块自动关闭并在配置阶段打印原因。模块以
`uvnvr_gb35114` 静态库形式提供，**不链接进 `uvnvr` 服务**，所以默认部署的
预览/录像/回放/ONVIF/私有协议行为不变。

产物：

| 目标 | 说明 |
| --- | --- |
| `libuvnvr_gb35114.a` | 签名/验签、TS→访问单元、记录读写 |
| `build/gb35114_verify` | 命令行工具 |
| `build/gb35114_sign_test` | 单元测试（`UVNVR_BUILD_TESTS=ON`） |

## 2. 签名对象的粒度

读取器把 H.264 Annex B 流切成 **访问单元（access unit）**：

- 按 3/4 字节起始码扫描 NAL；
- AUD、SPS、PPS、SEI 归入其后的图像；
- VCL NAL 的 `first_mb_in_slice == 0` 作为新图像边界；
- IDR（NAL type 5）或 `slice_type % 5 ∈ {2,4}` 标记为关键帧。

默认**只签关键帧（I 帧）**；`--all-frames` 可逐帧签名。帧序号 `frame_index`
统计的是所有访问单元（包括不签名的 P/B 帧），因此验签时能发现丢帧、插帧和重排。

MPEG-TS 输入经过 PAT → PMT → 视频 PES 解复用，PTS 从 PES 头读取。H.265
（PMT `stream_type=0x24`）暂不支持，读取器会返回明确错误。PAT/PMT 必须位于
单个 TS 包内（现场录像的常见形态）。

## 3. 签名记录格式

所有整数为大端，固定 88 字节头，后跟 DER 编码的 SM2 签名：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 8 | 魔数 `GB35114S` |
| 8 | 2 | 记录版本（1） |
| 10 | 2 | 头长度（88） |
| 12 | 4 | 标志：bit0 关键帧，bit1 带 PTS，bit2 全帧覆盖 |
| 16 | 8 | 通道 ID |
| 24 | 8 | 访问单元序号（从 0 开始） |
| 32 | 8 | PTS（90 kHz，无 PTS 时为 0） |
| 40 | 8 | 被签负载长度 |
| 48 | 32 | 负载的 SM3 摘要 |
| 80 | 4 | SM2 签名长度 |
| 84 | 4 | 保留（0） |
| 88 | … | SM2 签名（DER） |

SM2 实际签名的不是视频负载，而是下面 80 字节的规范消息：

```
"GB35114-A1" || version(2) || flags(4) || channel_id(8) ||
frame_index(8) || pts(8) || payload_size(8) || sm3(payload)(32)
```

SM2 内部再按 GB/T 32918 对消息做一次 SM3。把元数据纳入签名后，一条合法记录
无法被挪到另一帧、另一通道或另一个时间戳下复用。

验签顺序：

1. 记录中的 `payload_size` 必须等于重新解出的访问单元长度；
2. 重新计算 `SM3(payload)`，与记录中的摘要逐字节比较——**单字节篡改在这里被检出**；
3. 用设备证书或公钥验证 SM2 签名；
4. 流式验签还检查关键帧标志与 PTS 是否与重解出的访问单元一致；
5. 检查**覆盖完整性**：默认模式下每个关键帧都必须有记录；`--all-frames`
   生成的记录带全帧覆盖标志，此时每个访问单元都必须有记录。这样删除一条旁路
   记录也会被判定为失败，而不是安静地少验几帧。

## 4. 独立通道与旁路文件

`FrameSigner::SetSink()` 是输出接口。每条记录在签名完成后立即回调，调用方可以：

- 写入录像旁路文件（`gb35114_verify sign --output` 的做法）；
- 通过 WebSocket / 私有协议作为独立消息发送；
- 放入 RTP 头扩展（记录是自定界的，88 字节头已含长度）。

直播接入示例（每个通道/码流一个 signer，只在同一线程调用）：

```cpp
using namespace uvnvr::gb35114;

SigningOptions options;
options.private_key_pem = device_private_key_pem;  // 设备 SM2 私钥
options.channel_id = channel_id;
FrameSigner signer(options, InputFormat::kMpegTs);
std::string error;
if (!signer.Configure(&error)) { /* 证书/私钥不匹配时在这里失败 */ }

signer.SetSink([](const FrameSignature& record) {
  const gm::Bytes bytes = SerializeFrameSignature(record);
  /* 写到独立通道；不要写进 TS 负载 */
});

uint64_t subscription = media->AddRawSubscriber(
    channel_id, stream,
    [&signer](const uint8_t* data, size_t len) {
      std::string push_error;
      if (!signer.Push(data, len, &push_error)) { /* 记录并停止该订阅 */ }
    },
    &error);
```

`MediaService::AddRawSubscriber` 提供的就是 P6.2c 流管线中的同一份 MPEG-TS
字节；签名器按任意块边界增量解析，不需要缓存整段录像。录像场景在
`RecordingService` 完成一个片段后调用 `SignFile`，把 `.gbs` 旁路文件与 `.ts`
放在同一目录即可。

## 5. 命令行工具

```sh
# 生成设备密钥对（生产环境应由 CA 签发证书，密钥导入见 cert.h）
gb35114_verify keygen --private-key device.key --public-key device.pub

# 对录像的 I 帧签名；记录随签名逐条写入旁路文件
gb35114_verify sign --input rec.ts --private-key device.key \
    --cert device.crt --output rec.ts.gbs --channel 1

# 验签（公钥或设备证书二选一）
gb35114_verify verify --input rec.ts --signatures rec.ts.gbs --cert device.crt

# 查看访问单元/关键帧/PTS，便于确认输入格式
gb35114_verify inspect --input rec.ts
```

默认 `--format auto`：首字节为 `0x47` 按 MPEG-TS 处理，否则按 Annex B ES 处理。
`--all-frames` 对每个访问单元签名；`--json` 输出机器可读结果。

退出码：`0` 成功；`1` 验签失败或输入处理失败；`2` 参数/文件错误。

## 6. 验收与自测

`tests/gb35114_sign_test` 覆盖：

- Annex B 访问单元切分与关键帧识别；
- 自建 PAT/PMT/PES 的 TS 解复用、PTS 归属；
- 记录序列化/解析往返；
- SM2 签名验证；
- 改 1 比特负载 → `SM3 digest mismatch`；
- 改签名 → `SM2 signature verification failed`；
- 改 PTS → 签名绑定失败；
- 97 字节非对齐分块流式签名；
- 旁路文件写读往返。

真实录像自测（12 秒、1280×720、60 fps H.264，721 个访问单元、25 个关键帧）
的结果见提交说明；单字节篡改时验签在 frame 0 报摘要不匹配并返回 1。

## 7. 边界与后续

- **只做完整性，不做保密性。** 媒体仍为明文；B 级 SM4/VKEK 属于下一阶段。
- **旁路记录必须整体保留。** 删除记录等于删除完整性证据；验签工具要求每个关键帧
  （或 `--all-frames` 下的每个访问单元）都有记录，缺失即失败。
- **PTS 是签名元数据，不是 ES 摘要的一部分。** 验签会比对重解出的 PTS；
  若录像被重新封装（PTS 整体偏移），需要重新签名。
- **H.265 尚未支持**，PMT 检测到 `0x24` 会明确报错。
- 证书链、CRL/OCSP 与私钥安全存储由 P6.1a 的 `crypto/cert.h` 负责；本模块只
  使用已导入的设备身份，不自签冒充 CA。
- 本模块无 GPL/LGPL 依赖；密码学全部走 OpenSSL 3 的 Apache-2.0 实现。
