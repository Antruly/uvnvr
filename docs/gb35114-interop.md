# GB 35114 平台联调：A 级 → B 级

本文档记录 GH-27 的联调方案与实测证据。目标是在 **GB/T 28181 的 SIP 信令与 RTP
媒体链路**上，把 GH-23/24/25/26 交付的模块串起来，逐项验证：

| 验收项 | 实现 | 证据 |
| --- | --- | --- |
| 身份认证 | 设备/平台双向 SM2 签名（ClientHello / ServerHello / VkekDelivery / VkekAck），VkekAck 必须绑定到本次会话/密钥/序号 | `identity=ok`；伪造设备密钥与伪造平台密钥两个负向探针均 `rejected`（§4.2） |
| 签名可验 | A 级：SM3(access unit) + SM2，按帧索引/通道/时间戳绑定 | `signature_verified=2`（2 个关键帧全部通过） |
| 加密可解 | B 级：会话协商出的 VKEK 经 HKDF-SM3 派生，SM4-CTR + HMAC-SM3 封帧 | `cipher_frames=20`、`plaintext_match=yes`、SHA-256 与设备侧一致 |
| 篡改被拒 | 修改签名摘要、修改密文正文（第 56 字节起）各一个探针 | `signature_tamper=rejected`、`cipher_tamper=rejected` |
| 联调记录与抓包 | 本文 §4 + 端点 pcap | `docs/evidence/gb35114-interop-record.txt`、`docs/evidence/gb35114-interop.pcap` |

## 1. 联调拓扑

```
+---------------------+        UDP/127.0.0.1         +-------------------------+
|  uvnvr 设备侧       |                               |  参考 35114 平台        |
|  gb35114_interop    |  SIP MESSAGE (ClientHello) -> |  gb35114_interop        |
|    device           | <- ServerHello                |    platform             |
|                     | <- VkekDelivery (SM2 包装)    |                         |
|  VkekManager        |  VkekAck ->                   |  SM2 验签 + 解密 VKEK   |
|  sign.cpp           |                               |                         |
|  es_crypto.cpp      |  RTP PT=96 (SM4 封帧) ->      |  重组 RTP -> OpenEsFrame|
|  RtpSession         |  签名旁路 (GSIG) ->           |  SM3/SM2 验签           |
+---------------------+                               +-------------------------+
                                                                  |
                                                        endpoint pcap 记录
```

平台侧是仓库自带的**参考实现**，不是商用 35114 平台。实验室没有可用的第三方
35114 平台和具备资质的设备证书，因此本次联调验证的是协议实现、线格式、
密码学行为和传输过程；与厂商平台的最终互通仍需要一次现场联调（见 §6）。

## 2. 线格式

### 2.1 信令

每条协商记录按 GH-24 的格式序列化后，放在标准 SIP MESSAGE 的 body 中：

```
MESSAGE sip:platform@127.0.0.1 SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:<port>;branch=z9hG4bK...
From: <sip:device@127.0.0.1>;tag=...
To: <sip:platform@127.0.0.1>
Call-ID: ...@127.0.0.1
CSeq: 1 MESSAGE
Content-Type: application/vnd.uvnvr.gb35114
X-GB35114-Message: ClientHello | ServerHello | VkekDelivery | VkekAck | Report
Content-Length: <n>

<base64(record)>
```

记录本身（`src/gb35114/vkek.h`）是自定界二进制：16 字节头（magic/version/type/
length）+ 规范化的 body。签名的输入是每条消息的 canonical unsigned body，
所以 base64 外壳、SIP 头顺序或 UDP 分片都不会改变验签结果。

### 2.2 媒体

- **A 级签名**：`gb35114_verify`/`sign.cpp` 的 88 字节记录，按 access unit 索引
  绑定 `channel_id`、`frame_index`、`pts`、`payload_size`、SM3 摘要。
- **B 级密文**：`SealEsFrame` 的 `GBS1` 自定界帧（SM4-CTR + 16 字节 HMAC-SM3）。
- **传输**：密文帧原样交给 `gb28181::RtpSession`，RTP payload type 96、
  90 kHz 时间戳、marker 表示帧尾；平台用 `ParseRtpPacket` 重组。
  密文帧自定界，因此 PS 封装/RTP 分片不感知加密层（PS 往返由
  `tests/integration/gb35114_sm4.py` 单独覆盖）。

### 2.3 A 级到 B 级的密钥绑定

协商得到 16 字节 VKEK 和 16 字节 session id 后，双方执行：

```
salt = session_id                    # 16 字节，绑定本次双向认证
PRK  = HMAC-SM3(salt, VKEK)
video_key/iv_seed/mac_key = HKDF-SM3(PRK, "GB35114/v1/<label>" | key_id | stream_id)
```

这是 UvNVR 在标准文本之外的 profile 选择：把 B 级工作密钥绑定到 A 级会话，
避免不同会话复用同一 VKEK 派生出相同媒体密钥。待拿到 GB 35114 线格式正式文本后
应按标准字段替换（见 §6）。

## 3. 运行方法

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DCMAKE_PREFIX_PATH=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DUVNVR_ENABLE_GB28181=ON -DUVNVR_ENABLE_GB35114=ON
cmake --build build -j4

python3 tests/integration/gb35114_interop.py ./build/gb35114_interop \
  --evidence-dir /tmp/gb35114-evidence
```

脚本会生成 SM2 身份、用 ffmpeg 生成 20 帧 H.264 素材、启动平台/设备两个进程，
解析平台写出的 pcap，并断言身份、签名、解密、两个篡改探针和两个伪造身份负向探针。
默认输出：`gb35114_interop.pcap`、`platform.log`、`device.log`、
`interop-summary.txt`、`summary.json`；负向探针的日志在 `negative-*/` 下。
`--skip-negative-controls` 可只跑主流程。

## 4. 实测记录

环境：Linux 6.14，GCC 13.3，OpenSSL 3.0.13，libuvcpp 1.9.1，ffmpeg 9.0.1（H.264/`libx264`）。
命令与输出均来自 2026-11-26 的实际运行：

```text
$ python3 tests/integration/gb35114_interop.py ./build/gb35114_interop --evidence-dir /tmp/gh27-final
[1/7] generate the SM2 identities and an H.264 fixture
      fixture: 49495 bytes sha256=98cf25d8dd7cdc4aaff34a0d68ac5a9e7b3db396067ac46bf52e3fd68f5ac979
[2/7] start the reference platform
      platform ready negotiate=49049 media=34473 signature=42598 pcap=/tmp/gh27-final/gb35114_interop.pcap
[3/7] run the device handshake, media and tamper probes
[4/7] collect the platform verdict
[5/7] parse the capture
[6/7] negative identity controls (forged device / forged platform)
      forged-device: rejected (device exit 1, platform exit 1)
      forged-platform: rejected (device exit 1, platform exit 1)
[7/7] write the record

GB 35114 A-level -> B-level interop record
stream id: 34020000001320000001
capture: gb35114_interop.pcap (64201 bytes, sha256=207dc75a163e3017f3f44f150987ec67da426b85b12425d451d6a7a979b9c407)
packets: 56, RTP: 46 (50615 payload bytes), other: 0
SIP MESSAGE: ClientHello=1, Report=1, ServerHello=1, VkekAck=1, VkekDelivery=1
side channel: type0(end)=1 type1(signature)=2 type2(tampered signature)=1 type3(tampered ciphertext)=1
negative identity (forged device key) = rejected
negative identity (forged platform key) = rejected

device output:
device: ClientHello sent (199 bytes)
device: ServerHello ok
device: VkekDelivery ok
device: B-level keys derived key_id=1 session=2d6db005825ecc16
device: sent 20 sealed frames, 2 signatures, plaintext_sha256=98cf25d8dd7cdc4aaff34a0d68ac5a9e7b3db396067ac46bf52e3fd68f5ac979
device: platform report
identity=ok
signature_verified=2
signature_total=2
signature_tamper=rejected
cipher_frames=20
plaintext_sha256=98cf25d8dd7cdc4aaff34a0d68ac5a9e7b3db396067ac46bf52e3fd68f5ac979
plaintext_match=yes
cipher_tamper=rejected
overall=ok

platform output:
platform ready negotiate=49049 media=34473 signature=42598 pcap=/tmp/gh27-final/gb35114_interop.pcap
platform: ClientHello from device=34020000001320000001 nonce=4b98828f580260cb identity=authenticated
platform: VkekAck key_id=1 status=accepted binding=ok signature=verified
platform: identity=ok signatures=2/2 tampered_signature=rejected
platform: cipher_frames=20 plaintext_sha256=98cf25d8dd7cdc4aaff34a0d68ac5a9e7b3db396067ac46bf52e3fd68f5ac979 plaintext_match=yes tampered_ciphertext=rejected
platform: overall=ok
```

### 4.1 抓包

平台侧对每个 `sendto`/`recvfrom` 记录一份 libpcap（Ethernet/IPv4/UDP 头按内核
返回的 socket 地址重建；实验室账号没有 `CAP_NET_RAW`，`tcpdump` 无法抓 loopback，
因此这是**端点抓包**，不是网卡混杂模式抓包）。抓包可用 `tcpdump` 直接读取：

```
$ tcpdump -nn -r docs/evidence/gb35114-interop.pcap | head -8
reading from file docs/evidence/gb35114-interop.pcap, link-type EN10MB (Ethernet), snapshot length 65535
02:04:09.647029 IP 127.0.0.1.39003 > 127.0.0.1.49049: UDP, length 618
02:04:09.650102 IP 127.0.0.1.49049 > 127.0.0.1.39003: UDP, length 598
02:04:09.651925 IP 127.0.0.1.49049 > 127.0.0.1.39003: UDP, length 787
02:04:09.654463 IP 127.0.0.1.39003 > 127.0.0.1.49049: UDP, length 526
02:04:09.655629 IP 127.0.0.1.37293 > 127.0.0.1.34473: UDP, length 1412
02:04:09.655665 IP 127.0.0.1.37696 > 127.0.0.1.42598: UDP, length 171
02:04:09.655673 IP 127.0.0.1.37293 > 127.0.0.1.34473: UDP, length 1412
02:04:09.655680 IP 127.0.0.1.37293 > 127.0.0.1.34473: UDP, length 1412
```

端口对照：49049 协商（SIP MESSAGE），34473 RTP，42598 签名旁路。
抓包文件与记录已随仓库提交：

- [`evidence/gb35114-interop.pcap`](evidence/gb35114-interop.pcap)
  （64201 字节，SHA-256 `207dc75a163e3017f3f44f150987ec67da426b85b12425d451d6a7a979b9c407`）
- [`evidence/gb35114-interop-record.txt`](evidence/gb35114-interop-record.txt)
- [`evidence/gb35114-interop.pcap.sha256`](evidence/gb35114-interop.pcap.sha256)
- [`evidence/gb35114-interop-negative.txt`](evidence/gb35114-interop-negative.txt)

### 4.2 身份认证的负向控制

主流程证明"真设备 + 真平台"能互认；两个负向探针证明认错身份时会失败关闭。
脚本把其中一方的固定公钥替换成另一方的公钥后再跑一次完整握手：

```text
=== forged device key: platform pins the wrong device identity ===
--- platform output ---
platform: ClientHello from device=34020000001320000001 nonce=51af8835be15797e identity=rejected
error: device ClientHello signature did not verify

=== forged platform key: device pins the wrong platform identity ===
--- device output ---
device: ServerHello bad-signature
error: platform authentication failed: the platform server hello signature is invalid: SM2 signature verification failed (bad-signature)
```

两次运行中双方进程都以非零状态退出，脚本据此断言 `negative identity ... = rejected`。
完整日志见 [`evidence/gb35114-interop-negative.txt`](evidence/gb35114-interop-negative.txt)。

## 5. 逐条对应 issue #27

| issue 条目 | 实现位置 | 本次证据 |
| --- | --- | --- |
| 身份认证 | `VkekManager`（`src/gb35114/vkek.cpp`）、`gb35114_interop` | `identity=ok`；`binding=ok`；ClientHello/ServerHello/VkekDelivery/VkekAck 各 1 条；两个伪造身份负向探针均 `rejected` |
| 签名可验 | `src/gb35114/sign.cpp`、平台侧 `VerifyFrameSignature` | `signature_verified=2`，`signature_total=2` |
| 加密可解 | `src/gb35114/stream_keys.cpp` + `es_crypto.cpp` | 20 帧全部解密，明文 SHA-256 与设备侧一致 |
| 篡改被拒 | 平台侧负向探针 | 改 1 bit 签名摘要 / 改 1 bit 密文正文：`rejected` |
| 联调记录 | 本文 §4、§4.2 | 实际命令与输出；`docs/evidence/gb35114-interop-record.txt` |
| 抓包 | `docs/evidence/gb35114-interop.pcap` | 56 个 UDP 包，`tcpdump -r` 可读，SHA-256 见 §4.1 |

## 6. 遗留问题与限制

1. **对方平台是参考实现**：本次没有商用 35114 平台和资质 CA 签发的设备证书。
   `gb35114_interop` 覆盖了线格式、双向签名、VKEK 解包、媒体验签/解密和篡改拒绝，
   但不等于厂商互通测试。现场联调时需要平台侧确认：
   `X-GB35114-Message` 之外的信令承载（部分平台要求 35114 专用 SIP 头）、
   VKEK 分发消息的具体 ABNF，以及 B 级加密是 ES 层还是 PS/RTP 层。
2. **B 级密钥派生是 UvNVR profile**：`salt = session_id`、标签
   `GB35114/v1/*` 尚未与正式标准文本逐字节对齐；替换时只需改
   `stream_keys.cpp`，媒体/传输层不受影响。
3. **证书链未经联调**：本次身份认证使用 SM2 公钥，证书导入/CSR/CA 链校验由
   GH-22 的单测覆盖（`tests/gm_crypto_test.cpp`）。现场应使用资质 CA 的设备证书
   和平台证书。
4. **国密 TLS 未参与本次链路**：本机没有 Tongsuo，`UVNVR_ENABLE_GM_TLS=OFF`。
   GH-26 的适配器与握手探针单独验证；35114 现场若要求 TLCP，需要先部署 Tongsuo。
5. **抓包是端点抓包**：见 §4.1。若现场允许抓包，应在交换机/网卡侧复抓一次并
   与本记录比对。
6. **只覆盖 UDP**：GB/T 28181 的 TCP 主动/被动媒体模式由
   `tests/integration/gb28181_media_session.py` 单独覆盖；35114 加密层与传输
   无关，因此没有重复测 TCP。
