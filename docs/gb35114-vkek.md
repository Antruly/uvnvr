# GB 35114：签名协商与 VKEK 分发（P6.3）

本文描述 `src/gb35114/vkek.{h,cpp}` 的协议、状态机、安全属性与验证方法。
设计基线见 [`compliance-gb28181-gb35114.md`](compliance-gb28181-gb35114.md) 的
§1.2 与 §2。它与 P6.3a 的 I 帧签名模块（该模块作为独立交付提供）配套：后者负责
媒体侧 SM3→SM2 签名，本模块负责：

1. **签名协商**：设备与平台用 SM2/SM3 互相认证，选定 A 级签名套件、签名粒度
   （仅 I 帧 / 全帧）和签名记录版本；
2. **VKEK 分发**：平台用设备的 SM2 公钥加密 16 字节 VKEK，设备用 SM2 私钥解出；
3. **密钥生命周期与轮换**：key id / version 单调递增，轮换时旧密钥立即清零；
4. **失败与重放防护**：设备 nonce、会话 transcript、时间窗、严格递增的
   sequence、签名校验与失败锁定。

本模块**不加密视频**，也不修改媒体字节；VKEK 的使用（派生工作密钥、SM4 加密
ES）属于 B 级工作项 #25。模块用编译开关隔离，**不链接进 `uvnvr` 服务**，默认
部署的预览/录像/回放/ONVIF/私有协议行为不变。

## 1. 构建与产物

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_BUILD_TESTS=ON \
  -DUVNVR_ENABLE_GB35114=ON
cmake --build build -j4
```

`UVNVR_ENABLE_GB35114` 默认 **OFF**，依赖 `UVNVR_ENABLE_GM_CRYPTO`（OpenSSL 3
提供的 SM2/SM3/SM4）。GM crypto 不可用时本模块自动关闭并在配置阶段打印原因。

| 目标 | 说明 |
| --- | --- |
| `libuvnvr_gb35114.a` | 协商/分发/密钥管理 |
| `build/tests/gb35114_vkek_test` | 单元测试，含平台侧模拟器 |

SM2 加密使用 OpenSSL 3 的 ASN.1 DER 密文格式，可用
`openssl pkeyutl -encrypt/-decrypt` 直接交叉验证（见 §8）。

## 2. 消息流

```
设备                                                        平台
 |                                                            |
 |-- ClientHello  (device_id, nonce_d, 套件清单, SM2 签名) --->|
 |                                                            |
 |<-- ServerHello (session_id, nonce_p, 选定套件, transcript) -|
 |                        SM2 签名                            |
 |                                                            |
 |<-- VkekDelivery (key_id/version/sequence, 有效期,          |
 |                  SM2(公钥) 加密的 VKEK, SM2 签名) ----------|
 |                                                            |
 |--- VkekAck (session, key_id, status, 设备 SM2 签名) ------->|
 |                                                            |
```

- `ClientHello` 由设备生成并签名，平台用它认证设备并取得 `nonce_d`。
- `ServerHello` 的签名同时认证平台；它回显 `nonce_d`，选定算法。
- `transcript = SM3("GB35114-VKEK-TR-1" || len(CH) || CH || len(SH) || SH)`，
  其中 CH/SH 是**完整 wire 记录**（含各自签名）。`VkekDelivery` 携带该值，
  把 VKEK 绑定到本次协商。
- `VkekAck` 是设备对已安装密钥的签名确认；`status=0` 表示接受，非 0 为
  `VkekStatus` 数值。

## 3. Wire 格式

每条消息是自定界的二进制记录，大端序。记录头 16 字节：

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 8 | 魔数 `GB35114V` |
| 8 | 2 | 协议版本（1） |
| 10 | 1 | 类型：1=ClientHello，2=ServerHello，3=VkekDelivery，4=VkekAck |
| 11 | 1 | 保留，必须为 0 |
| 12 | 4 | body 长度 |
| 16 | … | body |

变长字段统一为 `u16 长度 + 字节`；长度上限 64 KiB。解析器要求**规范编码**：
重新序列化解析结果必须与输入逐字节相同，避免签名对象存在歧义重编码。

### 3.1 ClientHello body

| 字段 | 编码 |
| --- | --- |
| device_id | var（1–255 字节，UTF-8） |
| device_nonce | 32 字节 |
| timestamp_ms | u64 |
| signature_suites | u16 count + u16[]，设备提供 `1`（SM2-SM3） |
| cipher_suites | u16 count + u16[]，设备提供 `1`（SM4-CBC）、`2`（SM4-CTR） |
| sign_modes | u16 count + u16[]，设备提供 `1`（I 帧）、`2`（全帧） |
| certificate_fingerprint | 32 字节，SM3(DER 设备证书)；未配置证书时为全 0 |
| signature | var，SM2 DER 签名 |

签名对象：`"GB35114-VKEK-CH-1" || body（不含 signature 字段）`。

### 3.2 ServerHello body

| 字段 | 编码 |
| --- | --- |
| session_id | 16 字节，不能全 0 |
| platform_nonce | 32 字节 |
| device_nonce | 32 字节，回显 ClientHello |
| signature_suite | u16 |
| cipher_suite | u16 |
| sign_mode | u16 |
| record_version | u16，当前为 `1` |
| timestamp_ms | u64 |
| signature | var，SM2 DER 签名 |

签名对象：`"GB35114-VKEK-SH-1" || body（不含 signature）`。

### 3.3 VkekDelivery body

| 字段 | 编码 |
| --- | --- |
| session_id | 16 字节 |
| device_nonce | 32 字节 |
| transcript_hash | 32 字节 |
| key_id | u64，跨轮换严格递增 |
| key_version | u32，跨轮换严格递增 |
| sequence | u64，会话内严格递增 |
| not_before_ms | u64，Unix 毫秒 |
| not_after_ms | u64，Unix 毫秒 |
| timestamp_ms | u64，消息生成时间 |
| encrypted_vkek | var，SM2 密文（16 字节明文通常为 123/124 字节 DER） |
| signature | var，SM2 DER 签名 |

签名对象：`"GB35114-VKEK-DL-1" || body（不含 signature）`。签名覆盖密文，
因此篡改密文会先表现为 `bad-signature`；重新签名后的乱码密文则在解密阶段
返回 `decrypt-failed`。

### 3.4 VkekAck body

| 字段 | 编码 |
| --- | --- |
| session_id | 16 字节 |
| key_id | u64 |
| key_version | u32 |
| sequence | u64 |
| status | u16，0 = 接受 |
| timestamp_ms | u64 |
| signature | var，设备 SM2 签名 |

签名对象：`"GB35114-VKEK-ACK-1" || body（不含 signature）`。

## 4. 密钥生命周期

| 阶段 | 行为 |
| --- | --- |
| 首次安装 | 无活动密钥时，`key_id ≥ 1`、`key_version ≥ 1`，签名/时间窗/有效期均通过后安装 |
| 轮换 | 新投递必须同时满足 `key_id > 当前 key_id` 且 `key_version > 当前 key_version`，否则 `key-rotation-violation` |
| 旧密钥销毁 | 安装新密钥前用 `OPENSSL_cleanse` 清零旧密钥，`stats.keys_destroyed/rotations` 递增；`GetKeyForStream(old_id)` 拒绝解析 |
| 有效期 | `CopyActiveKey()` 检查 `not_before ≤ now ≤ not_after`；未生效或过期都拒绝，媒体路径不会拿到过期密钥 |
| 过期后更新 | 新投递仍是普通的单调轮换；过期本身不会自动删除密钥，是否继续使用由 `CopyActiveKey()` 拒绝 |
| 重协商 | `CreateClientHello()` 清除旧会话与 pending offer，但保留当前活动密钥，直到新 VKEK 到达；旧会话的投递因 session 不匹配被拒绝 |

“轮换后旧流不可解”由两点保证：旧密钥字节已经从进程内清除，且按旧
`key_id` 查询会被拒绝。测试用旧密钥实际加密了一段数据，轮换后无论用旧 id
取密钥还是用新密钥解密都失败。

## 5. 重放与失败防护

| 检查 | 失败状态 |
| --- | --- |
| 魔数/版本/长度/规范编码 | `malformed` |
| 回显的设备 nonce 与当前 pending nonce 不一致 | `bad-nonce` |
| 会话 id 不匹配 | `unknown-session` |
| transcript 不匹配 | `bad-transcript` |
| 消息时间戳超出 `allowed_clock_skew_ms`（默认 ±60 s） | `stale-timestamp` |
| SM2 签名无效或签名者不是平台身份 | `bad-signature` |
| 平台选择的套件/签名粒度/记录版本未提供 | `unsupported-algorithm` |
| `sequence` 未严格递增（同一 wire 重放、旧投递重放） | `replay` |
| VKEK 不在有效期内 | `invalid-key` / `key-expired` |
| key id 或 version 未严格递增 | `key-rotation-violation` |
| SM2 解密失败、密钥长度非 16、全 0 密钥 | `decrypt-failed` / `invalid-key` |
| 未协商、无 pending offer | `not-negotiated` / `no-pending-offer` |

每次拒绝都会增加 `stats.failures`。只有**已绑定到当前随机 nonce 与会话**
（即已通过 nonce/transcript 校验）的失败才增加 `consecutive_failures`；达到
`max_consecutive_failures`（默认 5）后 `IsLocked()==true`，后续协商与投递
返回 `locked`，直到运维调用 `ResetFailureLock()`。这样信令网络上的任意垃圾
报文只会增加统计，不能单独把设备锁死。成功处理一条消息会把连续失败计数
清零；已锁定时的调用不会继续累加计数。

失败计数器是进程内状态，不替代审计日志；需要留痕时应与 P6.1b 的审计链接口。

## 6. 在 SIP MESSAGE 上承载

协议记录与传输无关，推荐作为 SIP MESSAGE body 以标准 base64 承载：

```cpp
uvnvr::gb35114::VkekManager manager;
// ... Configure(...)
gm::Bytes offer;
manager.CreateClientHello(&offer);
const std::string body = uvnvr::gb35114::EncodeMessageBase64(offer);
// Content-Type: application/vnd.gb35114.vkek
```

接收侧先 `DecodeMessageBase64()`，再交给 `ProcessServerHello()` /
`ProcessVkekDelivery()`。解码忽略 CR/LF/空格，要求 4 的倍数长度和合法 base64
字符。实际平台的 GB35114 消息体字段命名由平台厂商与标准文本决定；本模块定义的
是规范化内部记录，联调时的字段映射由 #27 完成。

## 7. 设备侧接入示例

```cpp
#include "gb35114/vkek.h"

using namespace uvnvr::gb35114;

VkekConfig config;
config.device_id = "34020000001320000001";
config.device_private_key_pem = load_pem("device_key.pem");
config.device_certificate_pem = load_pem("device_cert.pem");   // 可选
config.platform_certificate_pem = load_pem("platform_cert.pem");
config.trusted_roots_pem = load_pem("ca.pem");                 // 可选，启用链校验

VkekManager manager;
std::string error;
if (!manager.Configure(std::move(config), &error)) { /* 配置/证书不匹配 */ }

gm::Bytes offer;
manager.CreateClientHello(&offer, &error);
// 通过 SIP MESSAGE 发送 offer；平台回复 -> 解码为 server_hello / delivery

if (manager.ProcessServerHello(server_hello, &error) != VkekStatus::kOk) { /* 记录并告警 */ }

gm::Bytes ack;
if (manager.ProcessVkekDelivery(delivery, &ack, &error) == VkekStatus::kOk) {
  // 发送 ack；协商参数已可供媒体签名模块使用
  SessionInfo session;
  manager.GetSessionInfo(&session, &error);
  // session.signature.suite / sign_mode / record_version
}
```

媒体路径每次加密前调用 `GetKeyForStream(key_id, &key, &error)`；轮换后旧
`key_id` 会返回错误，这正是要求的失效行为。

线程模型：与 SIP 栈一致，单个事件循环线程持有 `VkekManager`，不支持并发调用。

## 8. 验证与证据

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_BUILD_TESTS=ON -DUVNVR_ENABLE_GB35114=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

本实现的实测输出（2026-11-25，OpenSSL 3.0.13）：

```
[ok] handshake: device signature verified, platform signature verified
[ok] signature negotiation: SM2-SM3, I-frames, record v1, cipher SM4-CTR
[ok] VKEK key_id=1 version=1 installed (16-byte SM4 key)
[ok] device acknowledgement signed with the device SM2 key
[ok] replay of key_id=1 sequence=5 rejected: replay
[ok] stale timestamp rejected: stale-timestamp
[ok] nonce/session/transcript binding rejects mismatched VKEK deliveries
[ok] VKEK signed by a non-platform key rejected: bad-signature
[ok] rotation: key_id=2 version=2 installed, key_id=1 zeroised
[ok] GetKeyForStream(1) refused: key id 1 has been retired
[ok] ciphertext produced under key_id=1 no longer decrypts after rotation
[ok] non-increasing key id/version rejected: key-rotation-violation
[ok] unbound junk is counted but cannot trip the lockout threshold
[ok] failure lockout triggers at the configured threshold and resets explicitly
[ok] active VKEK is refused once its validity window ends
gb35114_vkek_test: all checks passed
```

SM2 密文与 OpenSSL 命令行互通（同一份 16 字节 VKEK）：

```
$ ./build/tests/gm_crypto_test --encrypt artifacts/sm2_pub.pem \
    artifacts/sm2_vkek_plain.bin /tmp/our_enc.der
encrypted 16 plaintext bytes, 124 ciphertext bytes
$ openssl pkeyutl -decrypt -inkey artifacts/sm2_key.pem \
    -in /tmp/our_enc.der -out /tmp/openssl_dec.bin
$ cmp artifacts/sm2_vkek_plain.bin /tmp/openssl_dec.bin && echo OK
OK
$ openssl pkeyutl -encrypt -pubin -inkey artifacts/sm2_pub.pem \
    -in artifacts/sm2_vkek_plain.bin -out /tmp/openssl_enc.der
$ ./build/tests/gm_crypto_test --decrypt artifacts/sm2_key.pem \
    /tmp/openssl_enc.der /tmp/our_dec.bin
decrypted 123 ciphertext bytes, 16 plaintext bytes
$ cmp artifacts/sm2_vkek_plain.bin /tmp/our_dec.bin && echo OK
OK
```

## 9. 边界与限制

- **平台消息原文**：本模块定义规范化 wire 记录；真实平台若使用特定 XML/JSON
  封装，需要一层字段映射，联调归 #27。签名对象与 transcript 的构造规则必须
  双方一致，否则会稳定地返回 `bad-signature`，不会降级放行。
- **证书校验**：配置 `platform_certificate_pem` 时按固定叶子证书验签；同时
  配置 `trusted_roots_pem` 才做链校验。CRL/OCSP 未实现。
- **SM2 用户 ID**：签名与验签使用 GB/T 32918 默认 ID `1234567812345678`，
  平台必须使用同一 ID。
- **时钟**：重放时间窗依赖设备时钟；现场应启用 NTP。单调 sequence 与
  nonce/transcript 仍是主防线。
- **密钥保护**：VKEK 与设备私钥只存在进程内存，轮换/析构时清零；没有
  TPM/密码机支持，root 权限的内存转储不在防护范围内。
- **媒体加密**：本模块只负责分发与生命周期，SM4 视频加密与平台侧解密播放
  属于 #25。
- **许可证**：仅依赖 OpenSSL 3（Apache-2.0）与仓库自有代码，未引入
  GPL/LGPL/AGPL 组件。
