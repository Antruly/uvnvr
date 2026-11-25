# 国密 TLS（GM/T 0024）适配

本文件记录 GH-26 的实现、选型依据与实测证据。设计基线见
[`compliance-gb28181-gb35114.md`](compliance-gb28181-gb35114.md) §1.2 与 §4。

## 1. 范围与边界

- 为 GB 35114 信令提供 **TLCP**（GM/T 0024-2014 / GB/T 38636-2020）握手，
  同套代码还支持 **RFC 8998 GM TLS 1.3**（`TLS_SM4_GCM_SM3`）。
- 新增模块 `src/gb35114/tls_gm.{h,cpp}` 是独立静态库 `uvnvr_gm_tls`，
  **不链接进 `uvnvr` 服务端**。现有 RTSP、ONVIF、私有协议、HTTPS 路径仍使用系统
  OpenSSL 3，行为不变。
- 编译开关 `UVNVR_ENABLE_GM_TLS` 默认 **OFF**；打开时必须指定 Tongsuo 安装前缀。
- 本阶段交付「可握手、可抓包、可单测」的适配层与探针工具。SIP 事务层
  （`src/gb28181/sip.*`）在本分支尚未落地，因此本模块只提供会话接口，不修改
  任何现有信令路径。

## 2. 依赖选型：Tongsuo vs GmSSL

| 维度 | Tongsuo 8.4.0 | GmSSL 3.1.1 |
| --- | --- | --- |
| 许可证 | Apache-2.0 | Apache-2.0 |
| 协议 | TLCP（代码中称 NTLS）、GM TLS 1.3、TLS 1.2/1.3 | TLCP、TLS 1.2/1.3（国密套件） |
| API 形态 | 兼容 OpenSSL 3：`SSL_*`、`NTLS_*_method`、`SSL_CTX_enable_ntls`、双证书 `SSL_CTX_use_sign_*` / `use_enc_*` | 自有 C API：`TLS_CTX` / `TLS_CONNECT`，`tls_socket_recv/send` 为阻塞式，证书按文件加载 |
| 非阻塞 / 事件循环 | 支持 `SSL_ERROR_WANT_READ/WRITE`，可直接接入 libuvcpp | socket 收发阻塞；每连接需独立线程或改协议栈 |
| 构建 | Perl + make；`Configure linux-x86_64` / `mingw64`；**默认 `no-ntls`，必须显式 `enable-ntls`** | CMake，纯 C，交叉编译简单 |
| 互通性 | 本仓库实测：Tongsuo ↔ Tongsuo TLCP 成功（见 §5） | 实测其 `tlcp_server` 拒绝任何 ClientHello 扩展；其客户端与 Tongsuo 服务端在 ServerKeyExchange 验签处失败。3.1.1 的 TLCP 实现较严格/有限 |
| 结论 | **采用**：与现有 OpenSSL 使用方式一致，非阻塞、双证书、TLCP + GM TLS 1.3 齐全 | 评估后不引入：许可证合规，但阻塞 API 与扩展限制不适合 SIP/libuv 架构 |

许可证结论：两者均为 Apache-2.0，不引入 GPL/LGPL/AGPL；本仓库只把
Tongsuo 作为**可选构建依赖**，不随 MIT 源码分发。发布包若包含 Tongsuo
二进制，需按 Apache-2.0 附带许可证与 NOTICE。

## 3. 构建

### 3.1 Tongsuo

```sh
curl -L -o Tongsuo-8.4.0.tar.gz \
  https://codeload.github.com/Tongsuo-Project/Tongsuo/tar.gz/refs/tags/8.4.0
tar -xzf Tongsuo-8.4.0.tar.gz
cd Tongsuo-8.4.0

# 关键：8.4.0 默认关闭 NTLS，不加 enable-ntls 就没有 GM/T 0024 套件
./Configure linux-x86_64 \
  --prefix="$HOME/.local/tongsuo-8.4.0" \
  --openssldir="$HOME/.local/tongsuo-8.4.0/ssl" \
  no-shared no-tests enable-ntls
make -j"$(nproc)"
make install_sw
```

MinGW 交叉编译见 §6。

### 3.2 UvNVR

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT=/path/to/uvcpp-1.9.1/usr \
  -DCMAKE_PREFIX_PATH=/path/to/uvcpp-1.9.1/usr \
  -DUVNVR_ENABLE_GM_TLS=ON \
  -DUVNVR_GM_TLS_ROOT="$HOME/.local/tongsuo-8.4.0"
cmake --build build -j4
```

CMake 会校验头文件确为 Tongsuo（存在 `TONGSUO_VERSION_MAJOR`）且未定义
`OPENSSL_NO_NTLS`；否则把 `UVNVR_ENABLE_GM_TLS` 强制为 OFF 并打印原因，不会
悄悄退回系统 OpenSSL。环境变量 `TONGSUO_ROOT` / `TONGSUO_INSTALL_ROOT`
也可提供安装前缀。

产物：

- `libuvnvr_gm_tls.a`：适配层；
- `gm_tls_cli`：握手探针（客户端/服务端/能力查询）；
- `tests/gm_tls_test`：进程内双向握手单测（`-DUVNVR_BUILD_TESTS=ON`）。

## 4. 接口

```cpp
#include "gb35114/tls_gm.h"
using namespace uvnvr::gb35114;

GmTlsCredentials credentials;          // 签名证书/私钥、TLCP 加密证书/私钥、CA、链
GmTlsOptions options;                  // Tlcp 或 GmTls13、套件、SNI、是否要求客户端证书
auto context = GmTlsContext::CreateServer(credentials, options, &error);

GmTlsSession session = GmTlsSession::Accept(context, fd, &error);
while (session.Handshake(&error) == GmTlsStatus::WantRead) {
  // 事件循环在 fd 可读后重试；WantWrite 同理
}
// session.NegotiatedCipher() -> "ECDHE-SM2-SM4-CBC-SM3"
// session.PeerSubject() / PeerVerified()
```

要点：

- `Handshake/Read/Write/Shutdown` 语义与 OpenSSL 的 `WANT_READ/WANT_WRITE`
  一致，可直接由 libuv 回调驱动；`HandshakeBlocking()` 仅用于工具与测试。
- TLCP 使用**双证书**：签名证书（digitalSignature）与加密证书
  （keyEncipherment）。服务端两者都必须提供；客户端在 ECDHE-SM2 套件下也必须
  提供，否则 NTLS 服务端会以 `no certificates returned` 拒绝（见 Tongsuo
  `ntls_statem_srvr.c` 对 `SSL_kSM2DHE` 的强制校验）。
- 对端校验：客户端默认校验证书链与主机名；服务端可要求客户端证书
  （`require_client_cert`）。`--insecure` / `verify_peer=false` 只用于实验室互通。
- 默认套件按 GM/T 0024 / GB/T 38636 名称给出：
  `ECDHE-SM2-SM4-CBC-SM3:ECC-SM2-SM4-CBC-SM3`（及 GCM 变体）。
- 加密证书链：Tongsuo 的 `SSL_CTX_set1_chain` 作用于默认/签名链；当前结构体保留
  `enc_chain_pem` 字段，但适配层只安装签名链。生产证书建议把加密证书中间链
  直接随加密证书一起预置到对端信任库，或在后续 SIP 层按平台要求补齐。

## 5. 实测证据

环境：Linux x86_64，Tongsuo 8.4.0（`enable-ntls`），GCC 13.3.0。

### 5.1 能力查询

```text
$ ./build/gm_tls_cli --print-capabilities
backend=Tongsuo version=Tongsuo 8.4.0 tlcp=yes gm_tls13=yes
tlcp_ciphers=ECDHE-SM2-SM4-CBC-SM3,ECC-SM2-SM4-CBC-SM3,ECDHE-SM2-SM4-GCM-SM3,ECC-SM2-SM4-GCM-SM3
tls13_ciphers=TLS_SM4_GCM_SM3,TLS_SM4_CCM_SM3
```

### 5.2 进程内握手（CTest）

```text
$ ctest --test-dir build -R gm_tls --output-on-failure
backend=Tongsuo version=Tongsuo 8.4.0 tlcp=yes gm_tls13=yes
tlcp: protocol=NTLSv1.1 cipher=ECDHE-SM2-SM4-CBC-SM3 peer=CN=localhost,O=UvNVR Lab,C=CN
tlcp: server protocol=NTLSv1.1 cipher=ECDHE-SM2-SM4-CBC-SM3
tls13: protocol=TLSv1.3 cipher=TLS_SM4_GCM_SM3 peer=CN=localhost,O=UvNVR Lab,C=CN
tls13: server protocol=TLSv1.3 cipher=TLS_SM4_GCM_SM3
gm_tls: all checks passed
```

单测生成 SM2 CA、签名叶证书和加密叶证书，双方完成双向认证，再交换一行应用数据；
TLCP 断言 `NTLSv1.1` + `ECDHE-SM2-SM4-CBC-SM3`，GM TLS 1.3 断言
`TLS_SM4_GCM_SM3`。

### 5.3 真实 TCP 进程 + 抓包

两个独立进程（`gm_tls_cli --mode server` / `--mode client`，对端是 Tongsuo NTLS
栈）经一个用户态 TCP 代理互通，代理把两个方向写成 pcap（无需 root；生产环境
仍建议 `tcpdump`/交换机镜像）。GmSSL 3.1.1 的互通限制见 §2 与 §7。

```text
$ gm_tls_cli --mode client --protocol tlcp --host 127.0.0.1 --port 14471 \
    --cert sign.pem --key signkey.pem --enc-cert enc.pem --enc-key enckey.pem \
    --ca ca.pem --sni localhost --message hello-captured
handshake=ok protocol=NTLSv1.1 cipher=ECDHE-SM2-SM4-CBC-SM3 peer_verified=yes
peer_subject=CN=localhost,O=UvNVR Lab,C=CN
peer_issuer=CN=UvNVR Test CA,O=UvNVR Lab,C=CN
reply=gm-tls-server: hello-captured
```

`tcpdump -nn -r gm-tls-tlcp.pcap -X` 的握手首包：

```text
IP 127.0.0.1.56388 > 127.0.0.1.14471: Flags [P.], length 82
0x0020: 5018 ffff f2ed 0000 1601 0100 4d01 0000  P...........M...
0x0030: 4901 016b 4e56 7260 9af3 6093 b579 ef89  I..kNVr`..`..y..
...
0x0050: 8ce3 c100 000a e011 e013 e051 e053 00ff  ...........Q.S..
0x0060: 0100 0016 0000 000e 000c 0000 096c 6f63  .............loc
0x0070: 616c 686f 7374 0023 0000                 alhost.#..
```

解读：

- `16 0101`：Handshake，记录版本 `0x0101`（NTLS/TLCP 1.1）；
- `01 0000 49`：ClientHello，长度 0x49；
- `000a e011 e013 e051 e053`：4 个国密套件，
  `0x0300E011` = `ECDHE-SM2-SM4-CBC-SM3`，`0x0300E013` =
  `ECC-SM2-SM4-CBC-SM3`；
- 服务端 ServerHello 选择 `e011`，随后 Certificate 消息中可见 SM2 曲线
  OID `1.2.156.10197.1.301`（`2a 811c cf55 01 8375`）与 SM2-with-SM3 签名。

## 6. MinGW 交叉编译验证

Tongsuo 交叉构建（静态库）：

```sh
cd Tongsuo-8.4.0
CROSS_COMPILE=x86_64-w64-mingw32- ./Configure mingw64 \
  --cross-compile-prefix=x86_64-w64-mingw32- \
  --prefix="$HOME/.local/tongsuo-8.4.0-mingw" \
  no-shared no-tests no-asm enable-ntls
make -j"$(nproc)" build_libs
make install_dev
```

适配层与探针（静态链接，便于在无 MinGW 运行库的机器上直接运行）：

```sh
x86_64-w64-mingw32-g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0601 \
  -static -static-libgcc -static-libstdc++ \
  -I src -I "$HOME/.local/tongsuo-8.4.0-mingw/include" \
  src/gb35114/tls_gm.cpp tools/gm_tls_cli/main.cpp \
  "$HOME/.local/tongsuo-8.4.0-mingw/lib64/libssl.a" \
  "$HOME/.local/tongsuo-8.4.0-mingw/lib64/libcrypto.a" \
  -lws2_32 -lcrypt32 -lbcrypt -o gm_tls_cli.exe
```

实测结果：

```text
$ file gm_tls_cli.exe
gm_tls_cli.exe: PE32+ executable (console) x86-64, for MS Windows, 19 sections

$ wine gm_tls_cli.exe --print-capabilities
backend=Tongsuo version=Tongsuo 8.4.0 tlcp=yes gm_tls13=yes
tlcp_ciphers=ECDHE-SM2-SM4-CBC-SM3,ECC-SM2-SM4-CBC-SM3,ECDHE-SM2-SM4-GCM-SM3,ECC-SM2-SM4-GCM-SM3
tls13_ciphers=TLS_SM4_GCM_SM3,TLS_SM4_CCM_SM3
```

Wine 只用于验证 Windows 二进制的加载与握手路径，不是运行平台要求。Windows
客户端与 Linux 服务端完成了真实 TCP 的 TLCP 双向认证：

```text
$ wine gm_tls_cli.exe --mode client --protocol tlcp --host 127.0.0.1 --port 14495 \
    --cert sign.pem --key signkey.pem --enc-cert enc.pem --enc-key enckey.pem \
    --ca ca.pem --sni localhost --message hello-from-windows
handshake=ok protocol=NTLSv1.1 cipher=ECDHE-SM2-SM4-CBC-SM3 peer_verified=yes
reply=gm-tls-server: hello-from-windows
```

交叉编译时发现并处理了一个 Windows 头文件冲突：`wincrypt.h` 把
`X509_NAME` 定义为宏 `((LPCSTR) 7)`，与 OpenSSL 的类型名冲突。`tls_gm.cpp`
在包含 Winsock 之后 `#undef X509_NAME`，因此同一份源码在 Linux 与 MinGW 下
都能编译。

## 7. 遗留问题

1. SIP 信令层尚未落地（`src/gb28181/` 不存在于本分支），因此还没有把
   `GmTlsSession` 接到 INVITE/REGISTER 的事务状态机；本任务完成的是适配层、
   探针与握手证据。后续在 P6.2 SIP 落地时直接复用该接口。
2. 未做会话复用、CRL/OCSP 吊销检查和双证书到期告警；这些属于 P6.3 证书状态
   工作线。
3. GmSSL 3.1.1 的 TLCP 对 ClientHello 扩展过度严格（任何扩展都回
   `unexpected message`），其客户端与 Tongsuo 服务端在 ServerKeyExchange
   验签不一致。若未来必须与 GmSSL 设备互通，需要在适配层增加“无扩展”
   ClientHello 模式，或推动对端升级。
4. 抓包证据由用户态代理生成（无 root 环境）；生产联调建议用
   `tcpdump -i any -s 0 -w tlcp.pcap 'tcp port 5061'` 采集。
5. Tongsuo 的加密证书链没有独立 API（`SSL_CTX_set1_chain` 只作用于默认/签名
   链），`enc_chain_pem` 暂未安装；见 §4。
