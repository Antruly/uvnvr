// In-process handshake tests for the GM/T 0024 / GM TLS 1.3 adapter.
//
// The tests build a small SM2 PKI (a self-signed CA issuing a signing leaf and
// an encryption leaf), run a TLCP (GB/T 38636-2020) handshake and an RFC 8998
// TLS 1.3 handshake over a socketpair, and check the negotiated cipher suite,
// peer verification and a small application data exchange. No network access
// or provisioned certificates are needed.

#include "gb35114/tls_gm.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#endif

namespace {

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

struct TestLeaf {
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
  if (!CertificateToPem(authority->certificate.get(), &authority->identity.certificate_pem) ||
      !KeyToPem(authority->key.get(), &authority->identity.key_pem)) {
    return false;
  }
  return true;
}

bool IssueLeaf(const Authority& authority, long serial, const char* key_usage, TestLeaf* leaf) {
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

uvnvr::gb35114::GmTlsCredentials MakeServerCredentials(const TestLeaf& signing,
                                                      const TestLeaf& encryption,
                                                      const Identity& ca,
                                                      bool require_client_cert) {
  uvnvr::gb35114::GmTlsCredentials credentials;
  credentials.sign_certificate_pem = signing.identity.certificate_pem;
  credentials.sign_private_key_pem = signing.identity.key_pem;
  credentials.enc_certificate_pem = encryption.identity.certificate_pem;
  credentials.enc_private_key_pem = encryption.identity.key_pem;
  credentials.sign_chain_pem = signing.chain_pem;
  credentials.enc_chain_pem = encryption.chain_pem;
  credentials.ca_pem = ca.certificate_pem;
  credentials.verify_peer = true;
  return credentials;
}

uvnvr::gb35114::GmTlsCredentials MakeClientCredentials(const Identity& ca,
                                                       const TestLeaf& signing,
                                                       const TestLeaf& encryption) {
  uvnvr::gb35114::GmTlsCredentials credentials;
  credentials.sign_certificate_pem = signing.identity.certificate_pem;
  credentials.sign_private_key_pem = signing.identity.key_pem;
  credentials.enc_certificate_pem = encryption.identity.certificate_pem;
  credentials.enc_private_key_pem = encryption.identity.key_pem;
  credentials.sign_chain_pem = signing.chain_pem;
  credentials.enc_chain_pem = encryption.chain_pem;
  credentials.ca_pem = ca.certificate_pem;
  credentials.verify_peer = true;
  return credentials;
}

struct PeerResult {
  bool ok = false;
  std::string error;
  std::string protocol;
  std::string cipher;
  bool peer_verified = false;
};

// Server side of one loopback handshake: accept, handshake, echo a line.
PeerResult RunServerPeer(const std::shared_ptr<uvnvr::gb35114::GmTlsContext>& context, int fd,
                         const std::string& expected) {
  using namespace uvnvr::gb35114;
  PeerResult result;
  std::string error;
  GmTlsSession session = GmTlsSession::Accept(context, fd, &error);
  if (!error.empty()) {
    result.error = error;
    return result;
  }
  if (HandshakeBlocking(session, &error) != GmTlsStatus::Ok) {
    result.error = "handshake: " + error;
    return result;
  }
  result.protocol = session.NegotiatedProtocol();
  result.cipher = session.NegotiatedCipher();
  result.peer_verified = session.PeerVerified();

  std::string received;
  char buffer[256];
  while (received.find('\n') == std::string::npos) {
    std::size_t got = 0;
    const GmTlsStatus status = session.Read(buffer, sizeof(buffer), &got, &error);
    if (status == GmTlsStatus::Ok) {
      received.append(buffer, got);
      continue;
    }
    if (status == GmTlsStatus::Closed && !received.empty())
      break;
    result.error = "read: " + error;
    return result;
  }

  const std::string reply = "echo:" + received;
  std::size_t sent = 0;
  if (session.Write(reply.data(), reply.size(), &sent, &error) != GmTlsStatus::Ok ||
      sent != reply.size()) {
    result.error = "write: " + error;
    return result;
  }
  if (received != expected) {
    result.error = "server received '" + received + "', expected '" + expected + "'";
    return result;
  }
  result.ok = true;
  return result;
}

const std::string kPing = "ping-from-gm-tls-test\n";

void RunHandshake(const char* label, uvnvr::gb35114::GmTlsProtocol protocol,
                  const Identity& ca, const TestLeaf& signing, const TestLeaf& encryption) {
  using namespace uvnvr::gb35114;

  GmTlsOptions server_options;
  server_options.protocol = protocol;
  server_options.server_name = "localhost";
  server_options.require_client_cert = true;
  std::string error;
  auto server_context =
      GmTlsContext::CreateServer(MakeServerCredentials(signing, encryption, ca, true),
                                 server_options, &error);
  Check(server_context != nullptr, std::string(label) + ": server context (" + error + ")");
  if (!server_context)
    return;

  GmTlsOptions client_options;
  client_options.protocol = protocol;
  client_options.server_name = "localhost";
  auto client_context = GmTlsContext::CreateClient(
      MakeClientCredentials(ca, signing, encryption), client_options, &error);
  Check(client_context != nullptr, std::string(label) + ": client context (" + error + ")");
  if (!client_context)
    return;

#ifndef _WIN32
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    Check(false, std::string(label) + ": socketpair");
    return;
  }

  PeerResult server_result;
  std::thread server([&] { server_result = RunServerPeer(server_context, fds[1], kPing); });

  GmTlsSession session = GmTlsSession::Connect(client_context, fds[0], &error);
  Check(error.empty(), std::string(label) + ": client session (" + error + ")");
  if (error.empty()) {
    const GmTlsStatus status = HandshakeBlocking(session, &error);
    Check(status == GmTlsStatus::Ok, std::string(label) + ": client handshake (" + error + ")");
    if (status == GmTlsStatus::Ok) {
      Check(session.IsHandshakeComplete(), std::string(label) + ": handshake complete");
      CheckContains(session.NegotiatedCipher(), "SM4",
                    std::string(label) + ": cipher is Chinese");
      Check(session.PeerVerified(), std::string(label) + ": peer verified");
      CheckContains(session.PeerSubject(), "localhost", std::string(label) + ": peer subject");
      if (protocol == GmTlsProtocol::Tlcp)
        CheckContains(session.NegotiatedProtocol(), "NTLS",
                      std::string(label) + ": TLCP protocol name");

      std::size_t sent = 0;
      const GmTlsStatus write_status = session.Write(kPing.data(), kPing.size(), &sent, &error);
      Check(write_status == GmTlsStatus::Ok && sent == kPing.size(),
            std::string(label) + ": client write (" + error + ")");

      std::string reply;
      char buffer[256];
      while (reply.find('\n') == std::string::npos) {
        std::size_t got = 0;
        const GmTlsStatus read_status = session.Read(buffer, sizeof(buffer), &got, &error);
        if (read_status == GmTlsStatus::Ok) {
          reply.append(buffer, got);
          continue;
        }
        Check(false, std::string(label) + ": client read (" + error + ")");
        break;
      }
      Check(reply == "echo:" + kPing, std::string(label) + ": echo payload");

      std::printf("%s: protocol=%s cipher=%s peer=%s\n", label,
                  session.NegotiatedProtocol().c_str(), session.NegotiatedCipher().c_str(),
                  session.PeerSubject().c_str());
    }
  }
  session.Shutdown(&error);
  // Wake a server peer that may still be blocked in SSL_read after a failed
  // client handshake or an early test exit.
  shutdown(fds[0], SHUT_RDWR);
  server.join();
  close(fds[0]);
  close(fds[1]);
  Check(server_result.ok, std::string(label) + ": server peer (" + server_result.error + ")");
  Check(server_result.peer_verified, std::string(label) + ": server verifies the client chain");
  if (server_result.ok) {
    CheckContains(server_result.cipher, "SM4", std::string(label) + ": server cipher");
    std::printf("%s: server protocol=%s cipher=%s\n", label, server_result.protocol.c_str(),
                server_result.cipher.c_str());
  }
#else
  (void)signing;
  (void)encryption;
  Check(false, std::string(label) + ": socketpair test is POSIX-only");
#endif
}

void RunCapabilityCheck() {
  const auto capabilities = uvnvr::gb35114::QueryGmTlsCapabilities();
  Check(capabilities.available, "capabilities: available");
  Check(capabilities.backend == "Tongsuo", "capabilities: Tongsuo backend");
  CheckContains(capabilities.version, "Tongsuo", "capabilities: version");
  Check(capabilities.tlcp, "capabilities: TLCP enabled");
  Check(capabilities.gm_tls13, "capabilities: GM TLS 1.3 enabled");
  CheckContains(capabilities.tlcp_cipher_suites[0], "SM4", "capabilities: TLCP suite");
  CheckContains(capabilities.tls13_cipher_suites[0], "SM4", "capabilities: TLS 1.3 suite");
  std::printf("backend=%s version=%s tlcp=%s gm_tls13=%s\n", capabilities.backend.c_str(),
              capabilities.version.c_str(), capabilities.tlcp ? "yes" : "no",
              capabilities.gm_tls13 ? "yes" : "no");
}

void RunCredentialValidation(const Identity& signing, const Identity& encryption) {
  using namespace uvnvr::gb35114;
  std::string error;
  GmTlsOptions options;
  options.protocol = GmTlsProtocol::Tlcp;
  GmTlsCredentials missing_encryption;
  missing_encryption.sign_certificate_pem = signing.certificate_pem;
  missing_encryption.sign_private_key_pem = signing.key_pem;
  Check(GmTlsContext::CreateServer(missing_encryption, options, &error) == nullptr,
        "validation: TLCP server rejects a missing encryption certificate");
  CheckContains(error, "encryption certificate", "validation: missing enc error");

  error.clear();
  GmTlsCredentials unverified;
  unverified.sign_certificate_pem = signing.certificate_pem;
  unverified.sign_private_key_pem = signing.key_pem;
  unverified.enc_certificate_pem = encryption.certificate_pem;
  unverified.enc_private_key_pem = encryption.key_pem;
  auto context = GmTlsContext::CreateServer(unverified, options, &error);
  Check(context != nullptr, "validation: server accepts a TLCP double certificate");
}

} // namespace

int main() {
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  RunCapabilityCheck();

  Authority authority;
  TestLeaf signing;
  TestLeaf encryption;
  if (!CreateAuthority(&authority) || !IssueLeaf(authority, 2, "critical,digitalSignature", &signing) ||
      !IssueLeaf(authority, 3, "critical,digitalSignature,keyEncipherment", &encryption)) {
    std::fprintf(stderr, "FAIL cannot build the SM2 test PKI\n");
    ++failures;
    return failures == 0 ? 0 : 1;
  }

  RunCredentialValidation(signing.identity, encryption.identity);
  // Optional dump for reproducing the external gm_tls_cli capture. These are
  // ephemeral test keys generated in this process, never production material.
  if (const char* dump = std::getenv("GM_TLS_TEST_DUMP")) {
    std::ofstream(dump + std::string("/ca.pem")) << authority.identity.certificate_pem;
    std::ofstream(dump + std::string("/sign.pem")) << signing.identity.certificate_pem;
    std::ofstream(dump + std::string("/signkey.pem")) << signing.identity.key_pem;
    std::ofstream(dump + std::string("/enc.pem")) << encryption.identity.certificate_pem;
    std::ofstream(dump + std::string("/enckey.pem")) << encryption.identity.key_pem;
  }
  RunHandshake("tlcp", uvnvr::gb35114::GmTlsProtocol::Tlcp, authority.identity, signing, encryption);
  RunHandshake("tls13", uvnvr::gb35114::GmTlsProtocol::GmTls13, authority.identity, signing,
               encryption);

  if (failures == 0)
    std::printf("gm_tls: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
