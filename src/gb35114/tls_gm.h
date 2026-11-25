#pragma once

// GM/T 0024 (TLCP, GB/T 38636-2020) and RFC 8998 GM TLS 1.3 adaptation.
//
// The adapter is built only when UVNVR_ENABLE_GM_TLS is ON and a Tongsuo
// (Apache-2.0) installation is available. Tongsuo keeps the OpenSSL 3 API, so
// the GB/T 28181 SIP layer can drive the handshake through the same
// non-blocking SSL_read/SSL_write state machine it would use for ordinary TLS.
//
// This module is deliberately standalone: it is not linked into the uvnvr
// server and does not change any existing RTSP, ONVIF or private-protocol
// behaviour. The listening socket, framing and SIP dialogue stay in the
// caller.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb35114 {

// Civil cipher suites from GM/T 0024-2014 / GB/T 38636-2020. Tongsuo accepts
// both the GM/T spellings (ECDHE_SM4_CBC_SM3) and the GB/T spellings
// (ECDHE-SM2-SM4-CBC-SM3); the latter are used by default because they also
// appear in captures and in platform configuration screens.
inline constexpr std::string_view kGmTls12CipherSuites =
    "ECDHE-SM2-SM4-CBC-SM3:ECC-SM2-SM4-CBC-SM3:"
    "ECDHE-SM2-SM4-GCM-SM3:ECC-SM2-SM4-GCM-SM3";
inline constexpr std::string_view kGmTls13CipherSuites =
    "TLS_SM4_GCM_SM3:TLS_SM4_CCM_SM3";

enum class GmTlsProtocol {
  // GM/T 0024-2014 / GB/T 38636-2020 TLCP, called NTLS in Tongsuo. The
  // handshake negotiates ECDHE_SM4_CBC_SM3 or ECC_SM4_CBC_SM3 and uses the
  // separate signing and encryption certificate pair.
  Tlcp,
  // RFC 8998 cipher suites over TLS 1.3: TLS_SM4_GCM_SM3 / TLS_SM4_CCM_SM3.
  GmTls13,
};

struct GmTlsCapabilities {
  bool available = false;
  std::string backend;      // "Tongsuo"
  std::string version;      // Tongsuo/OpenSSL version string
  bool tlcp = false;
  bool gm_tls13 = false;
  std::vector<std::string> tlcp_cipher_suites;
  std::vector<std::string> tls13_cipher_suites;
};

// Reports what the linked backend can negotiate. Safe to call before any
// context exists; it does not touch the network or the certificate store.
GmTlsCapabilities QueryGmTlsCapabilities();

// Credentials are supplied as PEM text so the caller can keep device
// identities in the same certificate store used by src/crypto. TLCP requires a
// signing certificate/key and, for a server, an encryption certificate/key
// (the "double certificate" model). A plain GM TLS 1.3 endpoint only needs the
// signing pair.
struct GmTlsCredentials {
  std::string sign_certificate_pem;
  std::string sign_private_key_pem;

  std::string enc_certificate_pem;
  std::string enc_private_key_pem;

  // Intermediate certificates for the signing and encryption chains. They are
  // ordered leaf-first and may be empty when the peer already trusts the leaf.
  std::string sign_chain_pem;
  std::string enc_chain_pem;

  // Trust anchors used for peer verification. An empty value disables chain
  // verification, which is only appropriate for a lab loopback test.
  std::string ca_pem;

  // Verify the peer certificate chain. When false the handshake proceeds even
  // without a trust anchor; this exists for interoperability bring-up only.
  bool verify_peer = true;
};

struct GmTlsOptions {
  GmTlsProtocol protocol = GmTlsProtocol::Tlcp;

  // Colon-separated cipher list. Empty selects the GM/T 0024 defaults above.
  // For TLCP the list is passed to SSL_CTX_set_cipher_list(); for GM TLS 1.3
  // it is passed to SSL_CTX_set_ciphersuites().
  std::string cipher_suites;

  // SNI and hostname verification name for client sessions.
  std::string server_name;

  // Require a client certificate on a server session (mutual authentication).
  bool require_client_cert = false;
};

enum class GmTlsStatus {
  Ok,
  WantRead,  // socket not readable; retry when the event loop reports readable
  WantWrite, // socket not writable; retry when the event loop reports writable
  Closed,    // peer sent close_notify / TCP EOF
  Error,
};

class GmTlsContext;
class GmTlsSession;

// Immutable per-endpoint configuration: protocol version, cipher suites,
// certificate pair and verification policy. One context may serve many
// sessions.
class GmTlsContext {
 public:
  ~GmTlsContext();
  GmTlsContext(const GmTlsContext&) = delete;
  GmTlsContext& operator=(const GmTlsContext&) = delete;

  static std::shared_ptr<GmTlsContext> CreateServer(const GmTlsCredentials& credentials,
                                                    const GmTlsOptions& options,
                                                    std::string* error = nullptr);
  static std::shared_ptr<GmTlsContext> CreateClient(const GmTlsCredentials& credentials,
                                                    const GmTlsOptions& options,
                                                    std::string* error = nullptr);

 private:
  struct Impl;
  explicit GmTlsContext(std::unique_ptr<Impl> impl);

  static std::shared_ptr<GmTlsContext> Create(const GmTlsCredentials& credentials,
                                              const GmTlsOptions& options, bool server,
                                              std::string* error);

  friend class GmTlsSession;
  friend GmTlsStatus HandshakeBlocking(GmTlsSession& session, std::string* error);
  std::unique_ptr<Impl> impl_;
};

// One handshake. The caller owns the connected socket and keeps it open for
// the lifetime of the session. Handshake/Read/Write never block: they return
// WantRead/WantWrite exactly like OpenSSL, so the SIP event loop can retry
// after poll/epoll reports readiness.
class GmTlsSession {
 public:
  GmTlsSession();
  ~GmTlsSession();
  GmTlsSession(GmTlsSession&& other) noexcept;
  GmTlsSession& operator=(GmTlsSession&& other) noexcept;
  GmTlsSession(const GmTlsSession&) = delete;
  GmTlsSession& operator=(const GmTlsSession&) = delete;

  // Creates the SSL object for an already connected socket and starts the
  // handshake. The socket must stay valid until the session is destroyed.
  static GmTlsSession Connect(std::shared_ptr<GmTlsContext> context, int fd,
                              std::string* error = nullptr);
  static GmTlsSession Accept(std::shared_ptr<GmTlsContext> context, int fd,
                             std::string* error = nullptr);

  // Drives the handshake until it completes or the socket needs readiness.
  GmTlsStatus Handshake(std::string* error = nullptr);

  GmTlsStatus Read(void* data, std::size_t size, std::size_t* received,
                   std::string* error = nullptr);
  GmTlsStatus Write(const void* data, std::size_t size, std::size_t* sent,
                    std::string* error = nullptr);

  // Sends close_notify. Returns Ok when both directions are shut down,
  // Closed when the peer's close_notify has been consumed, and WantRead/
  // WantWrite when the caller should retry.
  GmTlsStatus Shutdown(std::string* error = nullptr);

  bool IsHandshakeComplete() const;
  std::string NegotiatedProtocol() const; // e.g. "NTLSv1.1" or "TLSv1.3"
  std::string NegotiatedCipher() const;   // e.g. "ECDHE-SM2-SM4-CBC-SM3"
  std::string PeerSubject() const;        // RFC 2253, empty when no peer cert
  std::string PeerIssuer() const;
  bool PeerVerified() const;

 private:
  struct Impl;
  explicit GmTlsSession(std::unique_ptr<Impl> impl);
  static GmTlsSession CreateSession(std::shared_ptr<GmTlsContext> context, int fd, bool server,
                                    std::string* error);

  friend GmTlsStatus HandshakeBlocking(GmTlsSession& session, std::string* error);
  std::unique_ptr<Impl> impl_;
};

// Convenience for blocking callers and tools: drives Handshake to completion,
// waiting for socket readiness with poll()/select(). Returns Ok, Closed or
// Error. The session's socket is not put into blocking mode.
GmTlsStatus HandshakeBlocking(GmTlsSession& session, std::string* error = nullptr);

const char* ToString(GmTlsStatus status);

} // namespace uvnvr::gb35114
