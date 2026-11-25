#include "gb35114/tls_gm.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// wincrypt.h (pulled in by the Windows headers) defines X509_NAME as a macro
// for a certificate-name string type, which collides with OpenSSL's typedef.
#undef X509_NAME
#else
#include <cerrno>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace uvnvr::gb35114 {
namespace {

std::string DrainOpenSslErrors(std::string message) {
  unsigned long code = 0;
  char buffer[256];
  while ((code = ERR_get_error()) != 0) {
    ERR_error_string_n(code, buffer, sizeof(buffer));
    if (!message.empty())
      message += "; ";
    message += buffer;
  }
  if (message.empty())
    message = "unknown TLS error";
  return message;
}

void SetError(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
}

GmTlsStatus MapSslResult(SSL* ssl, int result, const char* operation, std::string* error) {
  if (result > 0)
    return GmTlsStatus::Ok;

  const int reason = SSL_get_error(ssl, result);
  switch (reason) {
  case SSL_ERROR_WANT_READ:
    return GmTlsStatus::WantRead;
  case SSL_ERROR_WANT_WRITE:
    return GmTlsStatus::WantWrite;
  case SSL_ERROR_ZERO_RETURN:
    return GmTlsStatus::Closed;
  case SSL_ERROR_SYSCALL:
    // A zero return from SSL_read/SSL_write with SYSCALL means an unexpected
    // EOF, i.e. the peer closed the TCP connection without close_notify.
    if (result == 0)
      return GmTlsStatus::Closed;
    [[fallthrough]];
  case SSL_ERROR_SSL:
  default: {
    std::string message = std::string(operation) + " failed";
    if (reason == SSL_ERROR_SYSCALL) {
#ifdef _WIN32
      message += ": socket error " + std::to_string(WSAGetLastError());
#else
      if (errno != 0)
        message += ": " + std::string(std::strerror(errno));
#endif
    }
    SetError(error, DrainOpenSslErrors(std::move(message)));
    return GmTlsStatus::Error;
  }
  }
}

BIO* MemoryBio(std::string_view data) {
  static const char kEmpty = '\0';
  const char* pointer = data.empty() ? &kEmpty : data.data();
  return BIO_new_mem_buf(pointer, static_cast<int>(data.size()));
}

X509* ReadCertificate(std::string_view pem) {
  BIO* bio = MemoryBio(pem);
  if (!bio)
    return nullptr;
  X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return certificate;
}

EVP_PKEY* ReadPrivateKey(std::string_view pem) {
  BIO* bio = MemoryBio(pem);
  if (!bio)
    return nullptr;
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return key;
}

// Parses all certificates in a PEM blob and appends them to `chain` (created on
// demand). Returns false when a non-empty blob contains no certificate.
bool ReadChain(std::string_view pem, STACK_OF(X509)** chain, std::string* error) {
  if (pem.empty())
    return true;
  BIO* bio = MemoryBio(pem);
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the certificate chain");
    return false;
  }
  if (!*chain)
    *chain = sk_X509_new_null();
  if (!*chain) {
    BIO_free(bio);
    SetError(error, "cannot allocate a certificate chain");
    return false;
  }
  bool added = false;
  while (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
    if (!sk_X509_push(*chain, certificate)) {
      X509_free(certificate);
      BIO_free(bio);
      SetError(error, "cannot append to the certificate chain");
      return false;
    }
    added = true;
  }
  ERR_clear_error();
  BIO_free(bio);
  if (!added) {
    SetError(error, "certificate chain PEM did not contain a certificate");
    return false;
  }
  return true;
}

bool InstallTrustAnchors(SSL_CTX* ctx, std::string_view ca_pem, std::string* error) {
  if (ca_pem.empty())
    return true;
  BIO* bio = MemoryBio(ca_pem);
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the CA bundle");
    return false;
  }
  X509_STORE* store = X509_STORE_new();
  if (!store) {
    BIO_free(bio);
    SetError(error, "cannot allocate an X509 store");
    return false;
  }
  int count = 0;
  while (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
    if (X509_STORE_add_cert(store, certificate) != 1) {
      X509_free(certificate);
      X509_STORE_free(store);
      BIO_free(bio);
      SetError(error, DrainOpenSslErrors("cannot add a CA certificate"));
      return false;
    }
    X509_free(certificate);
    ++count;
  }
  ERR_clear_error();
  BIO_free(bio);
  if (count == 0) {
    X509_STORE_free(store);
    SetError(error, "CA bundle did not contain a certificate");
    return false;
  }
  // SSL_CTX_set_cert_store takes ownership of the store.
  SSL_CTX_set_cert_store(ctx, store);
  return true;
}

#ifdef OPENSSL_NO_NTLS
constexpr bool kHasNtls = false;
#else
constexpr bool kHasNtls = true;
#endif

bool ConfigureTlcp(SSL_CTX* ctx, const GmTlsOptions& options, std::string* error) {
#ifndef OPENSSL_NO_NTLS
  // Tongsuo keeps SM2 under the "ntls" feature flag separately from the
  // NTLS_*_method() selection; both are required for a TLCP context.
  SSL_CTX_enable_ntls(ctx);
  const std::string defaults(kGmTls12CipherSuites);
  const char* suites =
      options.cipher_suites.empty() ? defaults.c_str() : options.cipher_suites.c_str();
  if (SSL_CTX_set_cipher_list(ctx, suites) != 1) {
    SetError(error, DrainOpenSslErrors("cannot set the GM/T 0024 cipher suites"));
    return false;
  }
  // NTLS_*_method() already pins the protocol to TLCP 1.1.
  return true;
#else
  (void)ctx;
  (void)options;
  SetError(error, "the linked crypto library was built without NTLS (GM/T 0024) support");
  return false;
#endif
}

bool ConfigureGmTls13(SSL_CTX* ctx, const GmTlsOptions& options, std::string* error) {
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1) {
    SetError(error, DrainOpenSslErrors("cannot pin TLS 1.3"));
    return false;
  }
  const std::string defaults(kGmTls13CipherSuites);
  const char* suites =
      options.cipher_suites.empty() ? defaults.c_str() : options.cipher_suites.c_str();
  if (SSL_CTX_set_ciphersuites(ctx, suites) != 1) {
    SetError(error, DrainOpenSslErrors("cannot set the GM TLS 1.3 cipher suites"));
    return false;
  }
  return true;
}

bool LoadSigningIdentity(SSL_CTX* ctx, const GmTlsCredentials& credentials, bool server,
                         GmTlsProtocol protocol, std::string* error) {
  if (credentials.sign_certificate_pem.empty() || credentials.sign_private_key_pem.empty()) {
    // A client may connect anonymously when the peer does not require a client
    // certificate; a server always needs a signing identity.
    if (server) {
      SetError(error, "a server GM TLS identity requires a signing certificate and key");
      return false;
    }
    return true;
  }

  X509* certificate = ReadCertificate(credentials.sign_certificate_pem);
  if (!certificate) {
    SetError(error, DrainOpenSslErrors("cannot parse the signing certificate"));
    return false;
  }
  EVP_PKEY* key = ReadPrivateKey(credentials.sign_private_key_pem);
  if (!key) {
    X509_free(certificate);
    SetError(error, DrainOpenSslErrors("cannot parse the signing private key"));
    return false;
  }

  int ok = 0;
#ifndef OPENSSL_NO_NTLS
  if (protocol == GmTlsProtocol::Tlcp) {
    // Tongsuo's NTLS server also needs the ordinary default certificate slot;
    // the sign/enc slots alone do not populate the Certificate message. Use
    // the signing pair for both, as a TLCP peer may present one identity.
    ok = SSL_CTX_use_sign_certificate(ctx, certificate) == 1 &&
                 SSL_CTX_use_sign_PrivateKey(ctx, key) == 1 &&
                 SSL_CTX_use_certificate(ctx, certificate) == 1 &&
                 SSL_CTX_use_PrivateKey(ctx, key) == 1
             ? 1
             : 0;
  } else
#endif
  {
    ok = SSL_CTX_use_certificate(ctx, certificate) == 1 &&
                 SSL_CTX_use_PrivateKey(ctx, key) == 1
             ? 1
             : 0;
  }
  X509_free(certificate);
  EVP_PKEY_free(key);
  if (!ok) {
    SetError(error, DrainOpenSslErrors("cannot install the signing certificate pair"));
    return false;
  }

  STACK_OF(X509)* chain = nullptr;
  if (!ReadChain(credentials.sign_chain_pem, &chain, error)) {
    sk_X509_pop_free(chain, X509_free);
    return false;
  }
  if (chain) {
    if (SSL_CTX_set1_chain(ctx, chain) != 1) {
      sk_X509_pop_free(chain, X509_free);
      SetError(error, DrainOpenSslErrors("cannot install the signing certificate chain"));
      return false;
    }
    sk_X509_pop_free(chain, X509_free);
  }

  if (protocol == GmTlsProtocol::Tlcp && SSL_CTX_check_private_key(ctx) != 1) {
    SetError(error, DrainOpenSslErrors("the signing private key does not match the certificate"));
    return false;
  }
  return true;
}

bool LoadEncryptionIdentity(SSL_CTX* ctx, const GmTlsCredentials& credentials,
                            std::string* error) {
  if (credentials.enc_certificate_pem.empty() && credentials.enc_private_key_pem.empty())
    return true;
  if (credentials.enc_certificate_pem.empty() || credentials.enc_private_key_pem.empty()) {
    SetError(error, "the TLCP encryption certificate and key must be supplied together");
    return false;
  }

#ifndef OPENSSL_NO_NTLS
  X509* certificate = ReadCertificate(credentials.enc_certificate_pem);
  if (!certificate) {
    SetError(error, DrainOpenSslErrors("cannot parse the encryption certificate"));
    return false;
  }
  EVP_PKEY* key = ReadPrivateKey(credentials.enc_private_key_pem);
  if (!key) {
    X509_free(certificate);
    SetError(error, DrainOpenSslErrors("cannot parse the encryption private key"));
    return false;
  }
  const bool ok = SSL_CTX_use_enc_certificate(ctx, certificate) == 1 &&
                  SSL_CTX_use_enc_PrivateKey(ctx, key) == 1;
  X509_free(certificate);
  EVP_PKEY_free(key);
  if (!ok) {
    SetError(error, DrainOpenSslErrors("cannot install the TLCP encryption certificate pair"));
    return false;
  }
  return true;
#else
  (void)ctx;
  (void)credentials;
  SetError(error, "the linked crypto library was built without NTLS (encryption certificate)");
  return false;
#endif
}

std::string NameToRfc2253(X509_NAME* name) {
  if (!name)
    return {};
  BIO* bio = BIO_new(BIO_s_mem());
  if (!bio)
    return {};
  if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253) < 0) {
    BIO_free(bio);
    return {};
  }
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  std::string result;
  if (memory && memory->data && memory->length)
    result.assign(memory->data, memory->length);
  BIO_free(bio);
  return result;
}

std::string SslVersionString(SSL* ssl) {
  const char* version = SSL_get_version(ssl);
  return version ? version : "";
}

std::string CipherString(SSL* ssl) {
  const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
  if (!cipher)
    return {};
  const char* name = SSL_CIPHER_get_name(cipher);
  return name ? name : "";
}

} // namespace

struct GmTlsContext::Impl {
  SSL_CTX* ctx = nullptr;
  GmTlsProtocol protocol = GmTlsProtocol::Tlcp;
  std::string server_name;

  ~Impl() {
    if (ctx)
      SSL_CTX_free(ctx);
  }
};

struct GmTlsSession::Impl {
  std::shared_ptr<GmTlsContext> context;
  SSL* ssl = nullptr;
  int fd = -1;

  ~Impl() {
    if (ssl)
      SSL_free(ssl);
  }
};

GmTlsCapabilities QueryGmTlsCapabilities() {
  GmTlsCapabilities capabilities;
  capabilities.available = true;
#ifdef TONGSUO_VERSION_TEXT
  capabilities.backend = "Tongsuo";
  capabilities.version = TONGSUO_VERSION_TEXT;
#else
  capabilities.backend = "OpenSSL";
  capabilities.version = OpenSSL_version(OPENSSL_VERSION);
#endif
  capabilities.tlcp = kHasNtls;
  capabilities.gm_tls13 = true;
  capabilities.tlcp_cipher_suites = {"ECDHE-SM2-SM4-CBC-SM3", "ECC-SM2-SM4-CBC-SM3",
                                     "ECDHE-SM2-SM4-GCM-SM3", "ECC-SM2-SM4-GCM-SM3"};
  capabilities.tls13_cipher_suites = {"TLS_SM4_GCM_SM3", "TLS_SM4_CCM_SM3"};
  return capabilities;
}

GmTlsContext::GmTlsContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GmTlsContext::~GmTlsContext() = default;

std::shared_ptr<GmTlsContext> GmTlsContext::Create(const GmTlsCredentials& credentials,
                                                   const GmTlsOptions& options, bool server,
                                                   std::string* error) {
  ERR_clear_error();
  auto impl = std::make_unique<GmTlsContext::Impl>();
  impl->protocol = options.protocol;
  impl->server_name = options.server_name;

  const SSL_METHOD* method = nullptr;
  if (options.protocol == GmTlsProtocol::Tlcp) {
#ifndef OPENSSL_NO_NTLS
    method = server ? NTLS_server_method() : NTLS_client_method();
#endif
    if (!method) {
      SetError(error, "the linked crypto library was built without NTLS (GM/T 0024) support");
      return nullptr;
    }
  } else {
    method = server ? TLS_server_method() : TLS_client_method();
  }

  impl->ctx = SSL_CTX_new(method);
  if (!impl->ctx) {
    SetError(error, DrainOpenSslErrors("cannot create the TLS context"));
    return nullptr;
  }

  const bool configured = options.protocol == GmTlsProtocol::Tlcp
                              ? ConfigureTlcp(impl->ctx, options, error)
                              : ConfigureGmTls13(impl->ctx, options, error);
  if (!configured)
    return nullptr;

  if (!LoadSigningIdentity(impl->ctx, credentials, server, options.protocol, error))
    return nullptr;
  if (options.protocol == GmTlsProtocol::Tlcp &&
      !LoadEncryptionIdentity(impl->ctx, credentials, error))
    return nullptr;

  if (!InstallTrustAnchors(impl->ctx, credentials.ca_pem, error))
    return nullptr;

  int verify_mode = SSL_VERIFY_NONE;
  if (credentials.verify_peer) {
    verify_mode = SSL_VERIFY_PEER;
    if (server && options.require_client_cert)
      verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  SSL_CTX_set_verify(impl->ctx, verify_mode, nullptr);

  if (credentials.verify_peer && credentials.ca_pem.empty() && !server) {
    if (SSL_CTX_set_default_verify_paths(impl->ctx) != 1) {
      SetError(error, DrainOpenSslErrors("cannot load the system trust store"));
      return nullptr;
    }
  }

  if (options.protocol == GmTlsProtocol::Tlcp && server &&
      (credentials.enc_certificate_pem.empty() || credentials.enc_private_key_pem.empty())) {
    SetError(error,
             "a TLCP server requires an encryption certificate and key (double certificate)");
    return nullptr;
  }
  if (server && credentials.sign_certificate_pem.empty()) {
    SetError(error, "a server GM TLS identity requires a signing certificate and key");
    return nullptr;
  }

  return std::shared_ptr<GmTlsContext>(new GmTlsContext(std::move(impl)));
}

std::shared_ptr<GmTlsContext> GmTlsContext::CreateServer(const GmTlsCredentials& credentials,
                                                         const GmTlsOptions& options,
                                                         std::string* error) {
  return Create(credentials, options, true, error);
}

std::shared_ptr<GmTlsContext> GmTlsContext::CreateClient(const GmTlsCredentials& credentials,
                                                         const GmTlsOptions& options,
                                                         std::string* error) {
  return Create(credentials, options, false, error);
}

GmTlsSession::GmTlsSession() = default;

GmTlsSession::GmTlsSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GmTlsSession::~GmTlsSession() = default;

GmTlsSession::GmTlsSession(GmTlsSession&& other) noexcept = default;

GmTlsSession& GmTlsSession::operator=(GmTlsSession&& other) noexcept = default;

GmTlsSession GmTlsSession::CreateSession(std::shared_ptr<GmTlsContext> context, int fd,
                                         bool server, std::string* error) {
  if (!context || !context->impl_ || !context->impl_->ctx) {
    SetError(error, "GM TLS context is not initialised");
    return GmTlsSession();
  }
  ERR_clear_error();
  auto impl = std::make_unique<GmTlsSession::Impl>();
  impl->context = std::move(context);
  impl->fd = fd;
  impl->ssl = SSL_new(impl->context->impl_->ctx);
  if (!impl->ssl) {
    SetError(error, DrainOpenSslErrors("cannot create the TLS session"));
    return GmTlsSession();
  }
  if (SSL_set_fd(impl->ssl, fd) != 1) {
    SetError(error, DrainOpenSslErrors("cannot attach the socket to the TLS session"));
    return GmTlsSession();
  }
  SSL_set_connect_state(impl->ssl);
  if (server)
    SSL_set_accept_state(impl->ssl);

  const std::string& server_name = impl->context->impl_->server_name;
  if (!server && !server_name.empty()) {
    if (SSL_set_tlsext_host_name(impl->ssl, server_name.c_str()) != 1) {
      SetError(error, DrainOpenSslErrors("cannot set the TLS server name"));
      return GmTlsSession();
    }
    X509_VERIFY_PARAM* param = SSL_get0_param(impl->ssl);
    if (param && X509_VERIFY_PARAM_set1_host(param, server_name.c_str(), 0) != 1) {
      SetError(error, DrainOpenSslErrors("cannot set the certificate hostname"));
      return GmTlsSession();
    }
  }
  return GmTlsSession(std::move(impl));
}

GmTlsSession GmTlsSession::Connect(std::shared_ptr<GmTlsContext> context, int fd,
                                   std::string* error) {
  return CreateSession(std::move(context), fd, false, error);
}

GmTlsSession GmTlsSession::Accept(std::shared_ptr<GmTlsContext> context, int fd,
                                  std::string* error) {
  return CreateSession(std::move(context), fd, true, error);
}

GmTlsStatus GmTlsSession::Handshake(std::string* error) {
  if (!impl_ || !impl_->ssl) {
    SetError(error, "GM TLS session is not initialised");
    return GmTlsStatus::Error;
  }
  if (SSL_is_init_finished(impl_->ssl))
    return GmTlsStatus::Ok;
  ERR_clear_error();
  const int result = SSL_do_handshake(impl_->ssl);
  return MapSslResult(impl_->ssl, result, "GM TLS handshake", error);
}

GmTlsStatus GmTlsSession::Read(void* data, std::size_t size, std::size_t* received,
                               std::string* error) {
  if (!impl_ || !impl_->ssl) {
    SetError(error, "GM TLS session is not initialised");
    return GmTlsStatus::Error;
  }
  const int chunk = static_cast<int>(std::min<std::size_t>(
      size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  ERR_clear_error();
  const int result = SSL_read(impl_->ssl, data, chunk);
  if (received)
    *received = result > 0 ? static_cast<std::size_t>(result) : 0;
  return MapSslResult(impl_->ssl, result, "GM TLS read", error);
}

GmTlsStatus GmTlsSession::Write(const void* data, std::size_t size, std::size_t* sent,
                                std::string* error) {
  if (!impl_ || !impl_->ssl) {
    SetError(error, "GM TLS session is not initialised");
    return GmTlsStatus::Error;
  }
  const int chunk = static_cast<int>(std::min<std::size_t>(
      size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  ERR_clear_error();
  const int result = SSL_write(impl_->ssl, data, chunk);
  if (sent)
    *sent = result > 0 ? static_cast<std::size_t>(result) : 0;
  return MapSslResult(impl_->ssl, result, "GM TLS write", error);
}

GmTlsStatus GmTlsSession::Shutdown(std::string* error) {
  if (!impl_ || !impl_->ssl) {
    SetError(error, "GM TLS session is not initialised");
    return GmTlsStatus::Error;
  }
  ERR_clear_error();
  const int result = SSL_shutdown(impl_->ssl);
  if (result == 0)
    return GmTlsStatus::WantRead;
  if (result == 1)
    return GmTlsStatus::Ok;
  return MapSslResult(impl_->ssl, result, "GM TLS shutdown", error);
}

bool GmTlsSession::IsHandshakeComplete() const {
  return impl_ && impl_->ssl && SSL_is_init_finished(impl_->ssl);
}

std::string GmTlsSession::NegotiatedProtocol() const {
  return (impl_ && impl_->ssl) ? SslVersionString(impl_->ssl) : std::string();
}

std::string GmTlsSession::NegotiatedCipher() const {
  return (impl_ && impl_->ssl) ? CipherString(impl_->ssl) : std::string();
}

std::string GmTlsSession::PeerSubject() const {
  if (!impl_ || !impl_->ssl)
    return {};
  X509* certificate = SSL_get_peer_certificate(impl_->ssl);
  if (!certificate)
    return {};
  std::string subject = NameToRfc2253(X509_get_subject_name(certificate));
  X509_free(certificate);
  return subject;
}

std::string GmTlsSession::PeerIssuer() const {
  if (!impl_ || !impl_->ssl)
    return {};
  X509* certificate = SSL_get_peer_certificate(impl_->ssl);
  if (!certificate)
    return {};
  std::string issuer = NameToRfc2253(X509_get_issuer_name(certificate));
  X509_free(certificate);
  return issuer;
}

bool GmTlsSession::PeerVerified() const {
  if (!impl_ || !impl_->ssl)
    return false;
  X509* certificate = SSL_get_peer_certificate(impl_->ssl);
  if (!certificate)
    return false;
  X509_free(certificate);
  return SSL_get_verify_result(impl_->ssl) == X509_V_OK;
}

const char* ToString(GmTlsStatus status) {
  switch (status) {
  case GmTlsStatus::Ok:
    return "ok";
  case GmTlsStatus::WantRead:
    return "want-read";
  case GmTlsStatus::WantWrite:
    return "want-write";
  case GmTlsStatus::Closed:
    return "closed";
  case GmTlsStatus::Error:
    return "error";
  }
  return "unknown";
}

GmTlsStatus HandshakeBlocking(GmTlsSession& session, std::string* error) {
  for (;;) {
    const GmTlsStatus status = session.Handshake(error);
    if (status != GmTlsStatus::WantRead && status != GmTlsStatus::WantWrite)
      return status;
    if (!session.impl_ || session.impl_->fd < 0) {
      SetError(error, "GM TLS session has no socket to wait on");
      return GmTlsStatus::Error;
    }
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (status == GmTlsStatus::WantRead)
      FD_SET(session.impl_->fd, &read_set);
    else
      FD_SET(session.impl_->fd, &write_set);
    timeval timeout{};
    timeout.tv_sec = 30;
#ifdef _WIN32
    const int ready = select(0, &read_set, &write_set, nullptr, &timeout);
#else
    const int ready =
        select(session.impl_->fd + 1, &read_set, &write_set, nullptr, &timeout);
#endif
    if (ready == 0) {
      SetError(error, "timed out waiting for the GM TLS handshake");
      return GmTlsStatus::Error;
    }
    if (ready < 0) {
      SetError(error, "socket wait failed during the GM TLS handshake");
      return GmTlsStatus::Error;
    }
  }
}

} // namespace uvnvr::gb35114
