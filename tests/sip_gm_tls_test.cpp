// End-to-end SIP REGISTER over the GM/T 0024 transport.
//
// Starts a minimal SIP registrar on loopback (the server side of
// src/gb35114/tls_gm), points SipUserAgent at it with transport=tls-gm and
// checks that:
//   * registration completes and the registered callback fires;
//   * the negotiated suite is ECDHE-SM2-SM4-CBC-SM3 on both ends;
//   * plain TCP still registers unchanged (regression);
//   * the non-blocking WantRead/WantWrite paths are actually exercised.
//
// The server writes every response in tiny TLS records and deliberately stops
// reading for a moment while the client emits a multi-megabyte MESSAGE, which
// makes the client's SSL_read hit WantRead between records and its SSL_write
// return WantWrite until the peer drains the socket.
//
// The loopback server uses POSIX sockets; on Windows the test reports a skip
// so the GM TLS module is still built and the existing gm_tls_test covers the
// handshake there.

#include "gb28181/sip.h"
#include "gb35114/tls_gm.h"

#include <handle/uvcpp_loop.h>
#include <uv.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace uvnvr::gb28181;
using namespace uvnvr::gb35114;
using Clock = std::chrono::steady_clock;

int failures = 0;

void Check(bool condition, const std::string& name) {
  if (condition)
    return;
  std::fprintf(stderr, "FAIL %s\n", name.c_str());
  ++failures;
}

void CheckContains(const std::string& value, const std::string& needle, const std::string& name) {
  if (value.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: '%s' does not contain '%s'\n", name.c_str(), value.c_str(),
               needle.c_str());
  ++failures;
}

void SleepMs(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// ---------------------------------------------------------------------------
// SM2 test PKI (self-signed CA, signing leaf, encryption leaf)
// ---------------------------------------------------------------------------

struct PkeyDeleter {
  void operator()(EVP_PKEY* value) const {
    if (value)
      EVP_PKEY_free(value);
  }
};
struct X509Deleter {
  void operator()(X509* value) const {
    if (value)
      X509_free(value);
  }
};
struct BioDeleter {
  void operator()(BIO* value) const {
    if (value)
      BIO_free(value);
  }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

std::string BioToString(BIO* bio) {
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  if (!memory || !memory->data || !memory->length)
    return {};
  return std::string(memory->data, memory->length);
}

PkeyPtr GenerateSm2Key() {
  EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_from_name(nullptr, "SM2", nullptr);
  if (!raw_context)
    return {};
  std::unique_ptr<EVP_PKEY_CTX, void (*)(EVP_PKEY_CTX*)> context(raw_context, EVP_PKEY_CTX_free);
  if (EVP_PKEY_keygen_init(context.get()) <= 0)
    return {};
  EVP_PKEY* raw_key = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw_key) <= 0)
    return {};
  return PkeyPtr(raw_key);
}

bool AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
  if (!extension)
    return false;
  const bool ok = X509_add_ext(certificate, extension, -1) == 1;
  X509_EXTENSION_free(extension);
  return ok;
}

bool KeyToPem(EVP_PKEY* key, std::string* pem) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
    return false;
  *pem = BioToString(bio.get());
  return !pem->empty();
}

bool CertificateToPem(X509* certificate, std::string* pem) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1)
    return false;
  *pem = BioToString(bio.get());
  return !pem->empty();
}

struct Identity {
  std::string certificate_pem;
  std::string key_pem;
};

struct Authority {
  Identity identity;
  PkeyPtr key;
  X509Ptr certificate;
};

struct Leaf {
  Identity identity;
  std::string chain_pem;
};

bool CreateAuthority(Authority* authority) {
  authority->key = GenerateSm2Key();
  if (!authority->key)
    return false;

  X509Ptr certificate(X509_new());
  if (!certificate)
    return false;
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60L * 60 * 24 * 3650);
  if (X509_set_pubkey(certificate.get(), authority->key.get()) != 1)
    return false;

  X509_NAME* name = X509_get_subject_name(certificate.get());
  if (X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("CN"), -1, -1, 0) != 1 ||
      X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("UvNVR Lab"), -1, -1, 0) !=
          1 ||
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("UvNVR Test CA"), -1, -1,
                                 0) != 1) {
    return false;
  }
  X509_set_issuer_name(certificate.get(), name);
  if (!AddExtension(certificate.get(), certificate.get(), NID_basic_constraints,
                    "critical,CA:TRUE") ||
      !AddExtension(certificate.get(), certificate.get(), NID_key_usage,
                    "critical,keyCertSign,cRLSign") ||
      !AddExtension(certificate.get(), certificate.get(), NID_subject_key_identifier, "hash")) {
    return false;
  }
  if (X509_sign(certificate.get(), authority->key.get(), EVP_sm3()) <= 0)
    return false;

  authority->certificate = std::move(certificate);
  return CertificateToPem(authority->certificate.get(), &authority->identity.certificate_pem) &&
         KeyToPem(authority->key.get(), &authority->identity.key_pem);
}

bool IssueLeaf(const Authority& authority, long serial, const char* key_usage, Leaf* leaf) {
  PkeyPtr key = GenerateSm2Key();
  if (!key)
    return false;

  X509Ptr certificate(X509_new());
  if (!certificate)
    return false;
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 60L * 60 * 24 * 365);
  if (X509_set_pubkey(certificate.get(), key.get()) != 1)
    return false;

  X509_NAME* subject = X509_get_subject_name(certificate.get());
  if (X509_NAME_add_entry_by_txt(subject, "C", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("CN"), -1, -1, 0) != 1 ||
      X509_NAME_add_entry_by_txt(subject, "O", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("UvNVR Lab"), -1, -1, 0) !=
          1 ||
      X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
                                 0) != 1) {
    return false;
  }
  X509_set_issuer_name(certificate.get(), X509_get_subject_name(authority.certificate.get()));
  if (!AddExtension(certificate.get(), authority.certificate.get(), NID_basic_constraints,
                    "critical,CA:FALSE") ||
      !AddExtension(certificate.get(), authority.certificate.get(), NID_key_usage, key_usage) ||
      !AddExtension(certificate.get(), authority.certificate.get(), NID_subject_alt_name,
                    "DNS:localhost") ||
      !AddExtension(certificate.get(), authority.certificate.get(), NID_authority_key_identifier,
                    "keyid:always")) {
    return false;
  }
  if (X509_sign(certificate.get(), authority.key.get(), EVP_sm3()) <= 0)
    return false;

  if (!CertificateToPem(certificate.get(), &leaf->identity.certificate_pem) ||
      !KeyToPem(key.get(), &leaf->identity.key_pem)) {
    return false;
  }
  leaf->chain_pem = authority.identity.certificate_pem;
  return true;
}

GmTlsCredentials MakeCredentials(const Identity& ca, const Leaf& signing, const Leaf& encryption,
                                 bool verify) {
  GmTlsCredentials credentials;
  credentials.sign_certificate_pem = signing.identity.certificate_pem;
  credentials.sign_private_key_pem = signing.identity.key_pem;
  credentials.enc_certificate_pem = encryption.identity.certificate_pem;
  credentials.enc_private_key_pem = encryption.identity.key_pem;
  credentials.sign_chain_pem = signing.chain_pem;
  credentials.enc_chain_pem = encryption.chain_pem;
  credentials.ca_pem = ca.certificate_pem;
  credentials.verify_peer = verify;
  return credentials;
}

// ---------------------------------------------------------------------------
// Loopback registrar
// ---------------------------------------------------------------------------

int CreateListener(std::uint16_t* port) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0)
    return -1;
  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listener, 1) != 0) {
    ::close(listener);
    return -1;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::close(listener);
    return -1;
  }
  *port = ntohs(address.sin_port);
  return listener;
}

std::string MakeOkResponse(const SipMessage& request) {
  std::string response = "SIP/2.0 200 OK\r\n";
  for (const SipHeader* via : request.FindHeaders("Via"))
    response += "Via: " + via->value + "\r\n";
  response += "From: " + request.GetHeader("From") + "\r\n";
  std::string to = request.GetHeader("To");
  if (to.find(";tag=") == std::string::npos)
    to += ";tag=uvnvr-test-server";
  response += "To: " + to + "\r\n";
  response += "Call-ID: " + request.GetHeader("Call-ID") + "\r\n";
  response += "CSeq: " + request.GetHeader("CSeq") + "\r\n";
  response += "Server: UvNVR-SIP-GM-TLS-Test\r\n";
  response += "Content-Length: 0\r\n\r\n";
  return response;
}

// A blocking transport wrapper used only by the test server thread.
struct PeerIo {
  int fd = -1;
  bool tls = false;
  std::unique_ptr<GmTlsSession> session;

  bool Read(char* buffer, std::size_t size, std::size_t* received) {
    if (!tls) {
      for (;;) {
        const ssize_t got = ::recv(fd, buffer, size, 0);
        if (got > 0) {
          *received = static_cast<std::size_t>(got);
          return true;
        }
        if (got == 0)
          return false;
        if (errno == EINTR)
          continue;
        return false;
      }
    }
    for (;;) {
      std::size_t got = 0;
      std::string error;
      const GmTlsStatus status = session->Read(buffer, size, &got, &error);
      if (status == GmTlsStatus::Ok) {
        *received = got;
        return true;
      }
      if (status == GmTlsStatus::WantRead || status == GmTlsStatus::WantWrite)
        continue;  // blocking socket: wait for the record
      return false;
    }
  }

  bool WriteAll(const void* data, std::size_t size) {
    const char* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
      if (!tls) {
        const ssize_t sent = ::send(fd, bytes + offset, size - offset, 0);
        if (sent > 0) {
          offset += static_cast<std::size_t>(sent);
          continue;
        }
        if (sent < 0 && errno == EINTR)
          continue;
        return false;
      }
      std::size_t sent = 0;
      std::string error;
      const GmTlsStatus status = session->Write(bytes + offset, size - offset, &sent, &error);
      if (status == GmTlsStatus::Ok && sent > 0) {
        offset += sent;
        continue;
      }
      if (status == GmTlsStatus::WantRead || status == GmTlsStatus::WantWrite)
        continue;
      return false;
    }
    return true;
  }

  // Small records make the client read path wake up repeatedly and hit
  // WantRead between them.
  bool WriteFragmented(const std::string& data, std::size_t chunk) {
    for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
      const std::size_t length = std::min(chunk, data.size() - offset);
      if (!WriteAll(data.data() + offset, length))
        return false;
      SleepMs(1);
    }
    return true;
  }
};

struct ServerResult {
  bool ok = false;
  // Written by the server thread; the client polls them while it runs, so
  // they must be atomic. Everything else is read after join().
  std::atomic<bool> registered{false};
  std::atomic<bool> big_message{false};
  std::string error;
  std::string protocol;
  std::string cipher;
  bool peer_verified = false;
};

struct ServerOptions {
  bool tls = true;
  bool pause_before_big_message = true;
  std::size_t big_message_threshold = 1024 * 1024;
  std::shared_ptr<GmTlsContext> context;
};

void RunServer(int listener, const ServerOptions& options, ServerResult* output) {
  ServerResult& result = *output;
  const int fd = ::accept(listener, nullptr, nullptr);
  if (fd < 0) {
    result.error = "accept failed";
    return;
  }
  int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  timeval timeout{};
  timeout.tv_sec = 15;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  PeerIo peer;
  peer.fd = fd;
  peer.tls = options.tls;
  if (options.tls) {
    std::string error;
    peer.session = std::make_unique<GmTlsSession>(
        GmTlsSession::Accept(options.context, fd, &error));
    if (!error.empty()) {
      result.error = error;
      ::close(fd);
      return;
    }
    if (HandshakeBlocking(*peer.session, &error) != GmTlsStatus::Ok) {
      result.error = "handshake: " + error;
      ::close(fd);
      return;
    }
    result.protocol = peer.session->NegotiatedProtocol();
    result.cipher = peer.session->NegotiatedCipher();
    result.peer_verified = peer.session->PeerVerified();
    std::printf("server: protocol=%s cipher=%s peer=%s\n", result.protocol.c_str(),
                result.cipher.c_str(), result.peer_verified ? "verified" : "unverified");
  }

  // Frame incrementally: scanning the whole accumulated stream with
  // SipMessage::Parse() on every 32 KiB read is O(n^2) and makes the server
  // too slow to drain the client's multi-megabyte MESSAGE.
  const auto expected_total = [](const std::string& data) -> std::size_t {
    const std::size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos)
      return 0;
    std::size_t content_length = 0;
    std::size_t position = data.find("Content-Length:");
    if (position == std::string::npos)
      position = data.find("content-length:");
    if (position != std::string::npos && position < header_end) {
      position += std::strlen("Content-Length:");
      while (position < header_end &&
             (data[position] == ' ' || data[position] == '\t'))
        ++position;
      while (position < header_end && data[position] >= '0' && data[position] <= '9') {
        content_length = content_length * 10 +
                         static_cast<std::size_t>(data[position] - '0');
        ++position;
      }
    }
    return header_end + 4 + content_length;
  };

  bool paused = false;
  std::string stream;
  std::size_t wanted = 0;
  char buffer[32768];
  for (;;) {
    std::size_t received = 0;
    if (!peer.Read(buffer, sizeof(buffer), &received)) {
      result.error = "peer closed";
      break;
    }
    if (received == 0) {
      result.error = "read timeout";
      break;
    }
    stream.append(buffer, received);

    bool keep_going = true;
    while (keep_going) {
      if (wanted == 0)
        wanted = expected_total(stream);
      if (wanted == 0 || stream.size() < wanted)
        break;
      SipMessage request;
      std::size_t consumed = 0;
      std::string parse_error;
      const SipParseStatus status = SipMessage::Parse(stream, &request, &consumed, &parse_error);
      if (status != SipParseStatus::kComplete) {
        result.error = "invalid SIP: " + parse_error;
        keep_going = false;
        break;
      }
      stream.erase(0, consumed);
      wanted = 0;
      if (!request.is_request)
        continue;
      if (request.method == SipMethod::kRegister) {
        result.registered.store(true);
        const std::string response = MakeOkResponse(request);
        if (options.tls) {
          if (!peer.WriteFragmented(response, 23))
            keep_going = false;
        } else if (!peer.WriteAll(response.data(), response.size())) {
          keep_going = false;
        }
        if (keep_going && options.pause_before_big_message && !paused) {
          // Do not read while the client emits its large MESSAGE: the socket
          // buffer fills and its SSL_write must wait for writability.
          paused = true;
          SleepMs(600);
        }
      } else if (request.method == SipMethod::kMessage) {
        if (request.body.size() >= options.big_message_threshold) {
          result.big_message.store(true);
          std::printf("server: received large MESSAGE body=%zu bytes\n", request.body.size());
        }
        const std::string response = MakeOkResponse(request);
        if (options.tls) {
          if (!peer.WriteFragmented(response, 23))
            keep_going = false;
        } else if (!peer.WriteAll(response.data(), response.size())) {
          keep_going = false;
        }
      } else {
        const std::string response = MakeOkResponse(request);
        if (!peer.WriteAll(response.data(), response.size()))
          keep_going = false;
      }
    }
    if (!keep_going)
      break;
  }

  result.ok = result.registered.load() &&
              (result.big_message.load() || !options.pause_before_big_message);
  if (result.ok && result.error == "peer closed")
    result.error.clear();
  if (peer.tls && peer.session)
    peer.session->Shutdown(nullptr);
  ::close(fd);
}

// ---------------------------------------------------------------------------
// Client driver
// ---------------------------------------------------------------------------

struct ClientRun {
  bool saw_want_read = false;
  bool saw_want_write = false;
  bool send_ok = false;
  std::string tls_event;
  std::string cipher;
};

SipUaConfig MakeClientConfig(std::uint16_t port, const Authority& authority, const Leaf& signing,
                             const Leaf& encryption, bool tls, bool tls13) {
  SipUaConfig config;
  config.server_host = "127.0.0.1";
  config.server_port = port;
  config.bind_ip = "127.0.0.1";
  config.local_ip = "127.0.0.1";
  config.tcp = true;
  config.username = "34020000001320000001";
  config.server_id = "34020000002000000001";
  config.domain = "3402000000";
  config.expires_seconds = 3600;
  config.keepalive_seconds = 0;  // the test is short; no heartbeat needed
  config.initial_backoff_ms = 200;
  config.max_backoff_ms = 1000;
  config.connect_timeout_ms = 5000;
  config.request_timeout_ms = 5000;
  config.tls_gm = tls;
  config.tls13 = tls13;
  if (tls) {
    config.tls_sign_certificate_pem = signing.identity.certificate_pem;
    config.tls_sign_private_key_pem = signing.identity.key_pem;
    config.tls_enc_certificate_pem = encryption.identity.certificate_pem;
    config.tls_enc_private_key_pem = encryption.identity.key_pem;
    config.tls_ca_pem = authority.identity.certificate_pem;
    config.tls_verify_peer = true;
    config.tls_server_name = "localhost";
  }
  return config;
}

void DrainLoop(uvcpp::uvcpp_loop* loop) {
  for (int i = 0; i < 200; ++i) {
    loop->run(UV_RUN_NOWAIT);
    SleepMs(1);
  }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void TestTlsRegistration(const Authority& authority, const Leaf& signing,
                         const Leaf& encryption) {
  std::uint16_t port = 0;
  const int listener = CreateListener(&port);
  Check(listener >= 0, "tls: loopback listener");
  if (listener < 0)
    return;

  GmTlsOptions options;
  options.protocol = GmTlsProtocol::Tlcp;
  options.server_name = "localhost";
  options.require_client_cert = true;
  std::string error;
  auto context = GmTlsContext::CreateServer(
      MakeCredentials(authority.identity, signing, encryption, true), options, &error);
  Check(context != nullptr, "tls: server context (" + error + ")");
  if (!context) {
    ::close(listener);
    return;
  }

  ServerOptions server_options;
  server_options.tls = true;
  server_options.pause_before_big_message = true;
  server_options.context = context;
  ServerResult server_result;
  std::thread server([&] { RunServer(listener, server_options, &server_result); });

  uvcpp::uvcpp_loop loop;
  Check(loop.init() == 0, "tls: loop init");
  SipUserAgent ua(&loop, MakeClientConfig(port, authority, signing, encryption, true, false));

  ClientRun observed;
  std::atomic<bool> registered{false};
  std::atomic<bool> big_message_sent{false};
  std::atomic<bool> stop{false};
  std::string big_body(8 * 1024 * 1024, 'x');
  big_body = "<BigMessage>" + big_body + "</BigMessage>";

  SipUaCallbacks callbacks;
  callbacks.on_state = [&](SipRegistrationState state, const std::string&) {
    if (state == SipRegistrationState::kRegistered) {
      registered.store(true);
      if (!big_message_sent.exchange(true)) {
        std::string send_error;
        observed.send_ok =
            ua.SendSipMessage(big_body, "Application/MANSCDP+xml", &send_error);
        if (!observed.send_ok) {
          std::fprintf(stderr, "tls: large MESSAGE rejected: %s\n", send_error.c_str());
          stop.store(true);
        }
      }
    } else if (state == SipRegistrationState::kBackoff && registered.load()) {
      stop.store(true);
    }
  };
  callbacks.on_event = [&](const std::string& text) {
    if (text.find("tls-gm handshake complete") != std::string::npos)
      observed.tls_event = text;
    if (text.find("WantRead") != std::string::npos)
      observed.saw_want_read = true;
    if (text.find("WantWrite") != std::string::npos)
      observed.saw_want_write = true;
  };
  ua.set_callbacks(std::move(callbacks));

  const bool started = ua.Start(&error);
  Check(started, "tls: Start (" + error + ")");
  if (!started)
    stop.store(true);

  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(12);
  while (Clock::now() < deadline) {
    loop.run(UV_RUN_NOWAIT);
    if (stop.load())
      break;
    if (registered.load() && server_result.big_message.load())
      break;
    SleepMs(2);
  }

  observed.cipher = ua.tls_cipher();

  ua.Stop();
  DrainLoop(&loop);
  server.join();
  ::close(listener);

  Check(registered.load(), "tls: registered callback fired");
  Check(observed.cipher.find("ECDHE-SM2-SM4-CBC-SM3") != std::string::npos,
        "tls: client negotiated ECDHE-SM2-SM4-CBC-SM3 (got '" + observed.cipher + "')");
  Check(server_result.cipher.find("ECDHE-SM2-SM4-CBC-SM3") != std::string::npos,
        "tls: server negotiated ECDHE-SM2-SM4-CBC-SM3 (got '" + server_result.cipher + "')");
  Check(observed.cipher == server_result.cipher, "tls: both ends agree on the cipher");
  CheckContains(server_result.protocol, "NTLS", "tls: server protocol is NTLS (TLCP)");
  Check(server_result.peer_verified, "tls: server verified the client certificate");
  Check(observed.saw_want_read, "tls: WantRead path was exercised");
  Check(observed.saw_want_write, "tls: WantWrite path was exercised");
  Check(observed.send_ok, "tls: large MESSAGE was accepted");
  Check(server_result.big_message.load(),
        "tls: large MESSAGE reached the server after back-pressure");
  Check(server_result.ok, "tls: server finished cleanly (" + server_result.error + ")");

  std::printf("client: cipher=%s\n", observed.cipher.empty() ? "(none)" : observed.cipher.c_str());
  std::printf("client: %s\n", observed.tls_event.c_str());
  std::printf("client: registered=%s want_read=%s want_write=%s big_message=%s\n",
              registered.load() ? "yes" : "no", observed.saw_want_read ? "yes" : "no",
              observed.saw_want_write ? "yes" : "no",
              server_result.big_message.load() ? "yes" : "no");
}

void TestPlainTcpRegistration(const Authority& authority, const Leaf& signing,
                              const Leaf& encryption) {
  std::uint16_t port = 0;
  const int listener = CreateListener(&port);
  Check(listener >= 0, "tcp: loopback listener");
  if (listener < 0)
    return;

  ServerOptions server_options;
  server_options.tls = false;
  server_options.pause_before_big_message = false;
  ServerResult server_result;
  std::thread server([&] { RunServer(listener, server_options, &server_result); });

  uvcpp::uvcpp_loop loop;
  Check(loop.init() == 0, "tcp: loop init");
  SipUserAgent ua(&loop, MakeClientConfig(port, authority, signing, encryption, false, false));

  std::atomic<bool> registered{false};
  SipUaCallbacks callbacks;
  callbacks.on_state = [&](SipRegistrationState state, const std::string&) {
    if (state == SipRegistrationState::kRegistered)
      registered.store(true);
  };
  ua.set_callbacks(std::move(callbacks));

  std::string error;
  Check(ua.Start(&error), "tcp: Start (" + error + ")");

  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(8);
  while (Clock::now() < deadline && !registered.load()) {
    loop.run(UV_RUN_NOWAIT);
    SleepMs(2);
  }

  const std::string cipher = ua.tls_cipher();
  ua.Stop();
  DrainLoop(&loop);
  server.join();
  ::close(listener);

  Check(registered.load(), "tcp: registered callback fired (regression)");
  Check(cipher.empty(), "tcp: no TLS parameters on a plaintext transport");
  std::printf("plain tcp: registered=%s cipher='%s'\n", registered.load() ? "yes" : "no",
              cipher.c_str());
}

void TestMissingCredentials() {
  uvcpp::uvcpp_loop loop;
  Check(loop.init() == 0, "validation: loop init");
  SipUaConfig config;
  config.server_host = "127.0.0.1";
  config.server_port = 5060;
  config.username = "34020000001320000001";
  config.tcp = true;
  config.tls_gm = true;  // no certificates configured
  SipUserAgent ua(&loop, config);
  std::string error;
  const bool started = ua.Start(&error);
  Check(!started, "validation: tls-gm without certificates refuses to start");
  CheckContains(error, "gb28181.tls.sign_cert", "validation: error names the missing setting");
  std::printf("validation: %s\n", error.c_str());
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  Authority authority;
  Leaf signing;
  Leaf encryption;
  if (!CreateAuthority(&authority) ||
      !IssueLeaf(authority, 2, "critical,digitalSignature", &signing) ||
      !IssueLeaf(authority, 3, "critical,digitalSignature,keyEncipherment", &encryption)) {
    std::fprintf(stderr, "FAIL cannot build the SM2 test PKI\n");
    return 1;
  }

  TestMissingCredentials();
  TestTlsRegistration(authority, signing, encryption);
  TestPlainTcpRegistration(authority, signing, encryption);

  if (failures == 0) {
    std::printf("sip_gm_tls: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "sip_gm_tls: %d check(s) failed\n", failures);
  return 1;
}

#else  // _WIN32

int main() {
  std::printf("sip_gm_tls: skipped on Windows (POSIX loopback server)\n");
  return 0;
}

#endif  // _WIN32
