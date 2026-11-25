#pragma once

// Shared OpenSSL helpers for the GM crypto module. This header is internal to
// src/crypto: the public surface lives in sm.h and cert.h.

#include <cstddef>
#include <memory>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uvnvr::gm::internal {

struct BioDeleter {
  void operator()(BIO* value) const {
    if (value)
      BIO_free(value);
  }
};

struct PkeyDeleter {
  void operator()(EVP_PKEY* value) const {
    if (value)
      EVP_PKEY_free(value);
  }
};

struct PkeyCtxDeleter {
  void operator()(EVP_PKEY_CTX* value) const {
    if (value)
      EVP_PKEY_CTX_free(value);
  }
};

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* value) const {
    if (value)
      EVP_MD_CTX_free(value);
  }
};

struct CipherCtxDeleter {
  void operator()(EVP_CIPHER_CTX* value) const {
    if (value)
      EVP_CIPHER_CTX_free(value);
  }
};

struct X509Deleter {
  void operator()(X509* value) const {
    if (value)
      X509_free(value);
  }
};

struct X509ReqDeleter {
  void operator()(X509_REQ* value) const {
    if (value)
      X509_REQ_free(value);
  }
};

struct X509StoreDeleter {
  void operator()(X509_STORE* value) const {
    if (value)
      X509_STORE_free(value);
  }
};

struct X509StoreCtxDeleter {
  void operator()(X509_STORE_CTX* value) const {
    if (value)
      X509_STORE_CTX_free(value);
  }
};

using BioPtr = std::unique_ptr<BIO, BioDeleter>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using X509ReqPtr = std::unique_ptr<X509_REQ, X509ReqDeleter>;
using X509StorePtr = std::unique_ptr<X509_STORE, X509StoreDeleter>;
using X509StoreCtxPtr = std::unique_ptr<X509_STORE_CTX, X509StoreCtxDeleter>;

inline void SetError(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
}

// Drains the OpenSSL error queue into a single human-readable string.
inline std::string OpenSslErrors() {
  std::string result;
  unsigned long code = 0;
  char buffer[256];
  while ((code = ERR_get_error()) != 0) {
    ERR_error_string_n(code, buffer, sizeof(buffer));
    if (!result.empty())
      result += "; ";
    result += buffer;
  }
  return result.empty() ? std::string("unknown OpenSSL error") : result;
}

inline void ClearOpenSslErrors() {
  ERR_clear_error();
}

inline BioPtr BioFromMemory(std::string_view data) {
  // BIO_new_mem_buf() does not accept a NULL pointer even for an empty range.
  static const char kEmpty = '\0';
  const char* pointer = data.empty() ? &kEmpty : data.data();
  return BioPtr(BIO_new_mem_buf(pointer, static_cast<int>(data.size())));
}

inline std::string BioToString(BIO* bio) {
  if (!bio)
    return {};
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio, &data);
  if (length <= 0 || !data)
    return {};
  return std::string(data, static_cast<std::size_t>(length));
}

inline PkeyPtr LoadPrivateKeyPem(std::string_view pem, std::string* error) {
  if (pem.empty()) {
    SetError(error, "private key PEM is empty");
    return {};
  }
  BioPtr bio = BioFromMemory(pem);
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the private key");
    return {};
  }
  PkeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  if (!key) {
    SetError(error, "cannot parse PEM private key: " + OpenSslErrors());
    return {};
  }
  return key;
}

inline PkeyPtr LoadPublicKeyPem(std::string_view pem, std::string* error) {
  if (pem.empty()) {
    SetError(error, "public key PEM is empty");
    return {};
  }
  BioPtr bio = BioFromMemory(pem);
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the public key");
    return {};
  }
  PkeyPtr key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
  if (!key) {
    SetError(error, "cannot parse PEM public key: " + OpenSslErrors());
    return {};
  }
  return key;
}

inline X509Ptr LoadCertificatePem(std::string_view pem, std::string* error) {
  if (pem.empty()) {
    SetError(error, "certificate PEM is empty");
    return {};
  }
  BioPtr bio = BioFromMemory(pem);
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the certificate");
    return {};
  }
  X509Ptr cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!cert) {
    SetError(error, "cannot parse PEM certificate: " + OpenSslErrors());
    return {};
  }
  return cert;
}

inline X509Ptr LoadCertificateDer(const unsigned char* der, std::size_t size, std::string* error) {
  if (!der || size == 0) {
    SetError(error, "certificate DER is empty");
    return {};
  }
  const unsigned char* cursor = der;
  X509Ptr cert(d2i_X509(nullptr, &cursor, static_cast<long>(size)));
  if (!cert) {
    SetError(error, "cannot parse DER certificate: " + OpenSslErrors());
    return {};
  }
  return cert;
}

// Parses zero or more certificates from a PEM blob. An empty input yields an
// empty vector; malformed non-empty input sets `error` and returns empty.
inline std::vector<X509Ptr> LoadCertificateChainPem(std::string_view pem, std::string* error) {
  std::vector<X509Ptr> certs;
  if (pem.empty())
    return certs;
  BioPtr bio = BioFromMemory(pem);
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the certificate chain");
    return {};
  }
  while (true) {
    X509* raw = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (!raw) {
      ClearOpenSslErrors();
      break;
    }
    certs.emplace_back(raw);
  }
  if (certs.empty())
    SetError(error, "cannot parse certificate chain PEM");
  return certs;
}

inline std::string PrivateKeyToPem(EVP_PKEY* key) {
  if (!key)
    return {};
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio ||
      PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    ClearOpenSslErrors();
    return {};
  }
  return BioToString(bio.get());
}

inline std::string PublicKeyToPem(EVP_PKEY* key) {
  if (!key)
    return {};
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), key) != 1) {
    ClearOpenSslErrors();
    return {};
  }
  return BioToString(bio.get());
}

inline std::string CertificateToPem(X509* cert) {
  if (!cert)
    return {};
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_X509(bio.get(), cert) != 1) {
    ClearOpenSslErrors();
    return {};
  }
  return BioToString(bio.get());
}

inline std::string CertificateChainToPem(const STACK_OF(X509) * certs) {
  if (!certs)
    return {};
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio)
    return {};
  for (int i = 0; i < sk_X509_num(certs); ++i) {
    if (PEM_write_bio_X509(bio.get(), sk_X509_value(certs, i)) != 1) {
      ClearOpenSslErrors();
      return {};
    }
  }
  return BioToString(bio.get());
}

inline std::string CertificateChainToPem(const std::vector<X509Ptr>& certs) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio)
    return {};
  for (const auto& cert : certs) {
    if (PEM_write_bio_X509(bio.get(), cert.get()) != 1) {
      ClearOpenSslErrors();
      return {};
    }
  }
  return BioToString(bio.get());
}

} // namespace uvnvr::gm::internal
