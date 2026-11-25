#include "crypto/sm.h"

#include "crypto/crypto_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <utility>

namespace uvnvr::gm {
namespace {

using internal::ClearOpenSslErrors;
using internal::SetError;

int HexValue(char character) {
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return character - 'a' + 10;
  if (character >= 'A' && character <= 'F')
    return character - 'A' + 10;
  return -1;
}

bool CheckSm4Inputs(const Bytes& key, const Bytes& iv, const char* operation, std::string* error) {
  if (key.size() != kSm4KeySize) {
    SetError(error, std::string("SM4 ") + operation + " requires a 16-byte key");
    return false;
  }
  if (iv.size() != kSm4IvSize) {
    SetError(error, std::string("SM4 ") + operation + " requires a 16-byte IV");
    return false;
  }
  return true;
}

bool IsSm2Key(EVP_PKEY* key) {
  return key && EVP_PKEY_is_a(key, "SM2") == 1;
}

bool DigestSign(EVP_PKEY* key, std::string_view message, bool use_user_id, std::string_view user_id,
                Bytes* signature, std::string* error) {
  if (!key) {
    SetError(error, "SM2 private key is null");
    return false;
  }
  if (!IsSm2Key(key)) {
    SetError(error, "the supplied private key is not an SM2 key");
    return false;
  }
  if (!signature) {
    SetError(error, "SM2 signature output is null");
    return false;
  }

  internal::MdCtxPtr context(EVP_MD_CTX_new());
  if (!context) {
    SetError(error, "cannot allocate an SM2 signing context");
    return false;
  }

  EVP_PKEY_CTX* key_context = nullptr;
  if (EVP_DigestSignInit(context.get(), &key_context, EVP_sm3(), nullptr, key) != 1) {
    SetError(error, "cannot initialise SM2 signing: " + internal::OpenSslErrors());
    return false;
  }
  if (use_user_id) {
    const char* id_data = user_id.empty() ? "" : user_id.data();
    if (EVP_PKEY_CTX_set1_id(key_context, id_data, static_cast<int>(user_id.size())) != 1) {
      SetError(error, "cannot set the SM2 user ID: " + internal::OpenSslErrors());
      return false;
    }
  }

  const unsigned char* data =
      reinterpret_cast<const unsigned char*>(message.empty() ? "" : message.data());

  std::size_t length = 0;
  if (EVP_DigestSign(context.get(), nullptr, &length, data, message.size()) != 1) {
    SetError(error, "cannot size the SM2 signature: " + internal::OpenSslErrors());
    return false;
  }
  Bytes result(length);
  if (EVP_DigestSign(context.get(), result.data(), &length, data, message.size()) != 1) {
    SetError(error, "cannot produce the SM2 signature: " + internal::OpenSslErrors());
    return false;
  }
  result.resize(length);
  *signature = std::move(result);
  return true;
}

bool DigestVerify(EVP_PKEY* key, std::string_view message, bool use_user_id,
                  std::string_view user_id, const Bytes& signature, std::string* error) {
  if (!key) {
    SetError(error, "SM2 public key is null");
    return false;
  }
  if (!IsSm2Key(key)) {
    SetError(error, "the supplied public key is not an SM2 key");
    return false;
  }
  if (signature.empty()) {
    SetError(error, "SM2 signature is empty");
    return false;
  }

  internal::MdCtxPtr context(EVP_MD_CTX_new());
  if (!context) {
    SetError(error, "cannot allocate an SM2 verification context");
    return false;
  }

  EVP_PKEY_CTX* key_context = nullptr;
  if (EVP_DigestVerifyInit(context.get(), &key_context, EVP_sm3(), nullptr, key) != 1) {
    SetError(error, "cannot initialise SM2 verification: " + internal::OpenSslErrors());
    return false;
  }
  if (use_user_id) {
    const char* id_data = user_id.empty() ? "" : user_id.data();
    if (EVP_PKEY_CTX_set1_id(key_context, id_data, static_cast<int>(user_id.size())) != 1) {
      SetError(error, "cannot set the SM2 user ID: " + internal::OpenSslErrors());
      return false;
    }
  }

  const unsigned char* data =
      reinterpret_cast<const unsigned char*>(message.empty() ? "" : message.data());
  const int result =
      EVP_DigestVerify(context.get(), signature.data(), signature.size(), data, message.size());
  if (result == 1)
    return true;
  if (result == 0) {
    SetError(error, "SM2 signature verification failed");
    return false;
  }
  SetError(error, "SM2 signature verification error: " + internal::OpenSslErrors());
  return false;
}

bool VerifyCertificate(X509* certificate, std::string_view message, const Bytes& signature,
                       std::string* error) {
  if (!certificate) {
    SetError(error, "SM2 certificate is null");
    return false;
  }
  internal::PkeyPtr public_key(X509_get_pubkey(certificate));
  if (!public_key) {
    SetError(error,
             "cannot extract the public key from the certificate: " + internal::OpenSslErrors());
    return false;
  }
  return DigestVerify(public_key.get(), message, false, {}, signature, error);
}

} // namespace

Bytes Sm3(std::string_view data) {
  static const unsigned char kEmpty = 0;
  return Sm3(data.empty() ? static_cast<const void*>(&kEmpty) : data.data(), data.size());
}

Bytes Sm3(const void* data, std::size_t size) {
  if (!data && size != 0)
    return {};
  Bytes digest(kSm3DigestSize);
  unsigned int length = 0;
  if (EVP_Digest(data, size, digest.data(), &length, EVP_sm3(), nullptr) != 1) {
    ClearOpenSslErrors();
    return {};
  }
  digest.resize(length);
  return digest;
}

std::string Sm3Hex(std::string_view data) {
  return ToHex(Sm3(data));
}

std::string ToHex(const Bytes& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

bool FromHex(std::string_view hex, Bytes* bytes) {
  if (!bytes)
    return false;
  if (hex.size() % 2 != 0)
    return false;
  Bytes out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = HexValue(hex[i]);
    const int low = HexValue(hex[i + 1]);
    if (high < 0 || low < 0)
      return false;
    out.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  *bytes = std::move(out);
  return true;
}

bool Sm4CbcEncrypt(const Bytes& key, const Bytes& iv, const Bytes& plaintext, Bytes* ciphertext,
                   std::string* error) {
  if (!ciphertext) {
    SetError(error, "SM4 ciphertext output is null");
    return false;
  }
  if (!CheckSm4Inputs(key, iv, "encryption", error))
    return false;

  internal::CipherCtxPtr context(EVP_CIPHER_CTX_new());
  if (!context) {
    SetError(error, "cannot allocate an SM4 encryption context");
    return false;
  }

  bool ok = EVP_EncryptInit_ex(context.get(), EVP_sm4_cbc(), nullptr, key.data(), iv.data()) == 1;
  Bytes result(plaintext.size() + kSm4BlockSize);
  int total = 0;
  if (ok && !plaintext.empty()) {
    int written = 0;
    ok = EVP_EncryptUpdate(context.get(), result.data(), &written, plaintext.data(),
                           static_cast<int>(plaintext.size())) == 1;
    total = written;
  }
  if (ok) {
    int written = 0;
    ok = EVP_EncryptFinal_ex(context.get(), result.data() + total, &written) == 1;
    total += written;
  }
  if (!ok) {
    SetError(error, "SM4 encryption failed: " + internal::OpenSslErrors());
    return false;
  }
  result.resize(static_cast<std::size_t>(total));
  *ciphertext = std::move(result);
  return true;
}

bool Sm4CbcDecrypt(const Bytes& key, const Bytes& iv, const Bytes& ciphertext, Bytes* plaintext,
                   std::string* error) {
  if (!plaintext) {
    SetError(error, "SM4 plaintext output is null");
    return false;
  }
  if (!CheckSm4Inputs(key, iv, "decryption", error))
    return false;
  if (ciphertext.empty() || ciphertext.size() % kSm4BlockSize != 0) {
    SetError(error, "SM4 ciphertext must be a non-empty multiple of 16 bytes");
    return false;
  }

  internal::CipherCtxPtr context(EVP_CIPHER_CTX_new());
  if (!context) {
    SetError(error, "cannot allocate an SM4 decryption context");
    return false;
  }

  bool ok = EVP_DecryptInit_ex(context.get(), EVP_sm4_cbc(), nullptr, key.data(), iv.data()) == 1;
  Bytes result(ciphertext.size());
  int total = 0;
  if (ok) {
    int written = 0;
    ok = EVP_DecryptUpdate(context.get(), result.data(), &written, ciphertext.data(),
                           static_cast<int>(ciphertext.size())) == 1;
    total = written;
  }
  if (ok) {
    int written = 0;
    ok = EVP_DecryptFinal_ex(context.get(), result.data() + total, &written) == 1;
    total += written;
  }
  if (!ok) {
    SetError(error, "SM4 decryption failed (wrong key, IV, or corrupted data)");
    ClearOpenSslErrors();
    return false;
  }
  result.resize(static_cast<std::size_t>(total));
  *plaintext = std::move(result);
  return true;
}

namespace {

void AdvanceCounter(unsigned char counter[kSm4BlockSize], std::uint64_t blocks) {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (int i = 0; i < 8; ++i)
    high = (high << 8) | counter[i];
  for (int i = 8; i < 16; ++i)
    low = (low << 8) | counter[i];
  const std::uint64_t previous = low;
  low += blocks;
  if (low < previous)
    ++high;
  for (int i = 7; i >= 0; --i) {
    counter[i] = static_cast<unsigned char>(high & 0xFF);
    high >>= 8;
  }
  for (int i = 15; i >= 8; --i) {
    counter[i] = static_cast<unsigned char>(low & 0xFF);
    low >>= 8;
  }
}

} // namespace

bool Sm4CtrCrypt(const Bytes& key, const Bytes& iv, std::uint64_t offset, const Bytes& input,
                 Bytes* output, std::string* error) {
  if (!output) {
    SetError(error, "SM4-CTR output is null");
    return false;
  }
  if (!CheckSm4Inputs(key, iv, "CTR operation", error))
    return false;
  output->assign(input.size(), 0);
  if (input.empty())
    return true;

  unsigned char counter[kSm4BlockSize];
  std::memcpy(counter, iv.data(), kSm4BlockSize);
  AdvanceCounter(counter, offset / kSm4BlockSize);
  const std::size_t intra = static_cast<std::size_t>(offset % kSm4BlockSize);

  internal::CipherCtxPtr context(EVP_CIPHER_CTX_new());
  if (!context) {
    SetError(error, "cannot allocate an SM4-CTR context");
    return false;
  }
  if (EVP_EncryptInit_ex(context.get(), EVP_sm4_ctr(), nullptr, key.data(), counter) != 1) {
    SetError(error, "cannot initialise SM4-CTR: " + internal::OpenSslErrors());
    return false;
  }

  std::size_t processed = 0;
  if (intra != 0) {
    // Generate one keystream block and use the tail of it, so the bulk update
    // starts at a block boundary of the already advanced counter.
    unsigned char keystream[kSm4BlockSize] = {0};
    int written = 0;
    if (EVP_EncryptUpdate(context.get(), keystream, &written, keystream, kSm4BlockSize) != 1 ||
        written != static_cast<int>(kSm4BlockSize)) {
      SetError(error, "cannot generate the SM4-CTR keystream: " + internal::OpenSslErrors());
      return false;
    }
    const std::size_t take =
        std::min<std::size_t>(kSm4BlockSize - intra, input.size());
    for (std::size_t i = 0; i < take; ++i)
      (*output)[i] = static_cast<unsigned char>(input[i] ^ keystream[intra + i]);
    processed = take;
  }

  if (processed < input.size()) {
    const std::size_t remaining = input.size() - processed;
    if (remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      SetError(error, "SM4-CTR input is too large for one call");
      return false;
    }
    int written = 0;
    if (EVP_EncryptUpdate(context.get(), output->data() + processed, &written,
                          input.data() + processed, static_cast<int>(remaining)) != 1 ||
        written != static_cast<int>(remaining)) {
      SetError(error, "SM4-CTR operation failed: " + internal::OpenSslErrors());
      return false;
    }
  }
  return true;
}

bool GenerateSm2KeyPair(Sm2KeyPair* key_pair, std::string* error) {
  if (!key_pair) {
    SetError(error, "SM2 key pair output is null");
    return false;
  }

  internal::PkeyCtxPtr context(EVP_PKEY_CTX_new_from_name(nullptr, "SM2", nullptr));
  if (!context) {
    SetError(error, "cannot create an SM2 keygen context: " + internal::OpenSslErrors());
    return false;
  }
  if (EVP_PKEY_keygen_init(context.get()) != 1) {
    SetError(error, "cannot initialise SM2 key generation: " + internal::OpenSslErrors());
    return false;
  }
  EVP_PKEY* raw_key = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw_key) != 1 || !raw_key) {
    SetError(error, "SM2 key generation failed: " + internal::OpenSslErrors());
    return false;
  }
  internal::PkeyPtr key(raw_key);

  Sm2KeyPair result;
  result.private_key_pem = internal::PrivateKeyToPem(key.get());
  result.public_key_pem = internal::PublicKeyToPem(key.get());
  if (result.private_key_pem.empty() || result.public_key_pem.empty()) {
    SetError(error, "cannot encode the generated SM2 key pair");
    return false;
  }
  *key_pair = std::move(result);
  return true;
}

bool Sm2Sign(std::string_view private_key_pem, std::string_view message, Bytes* signature,
             std::string* error) {
  internal::PkeyPtr key = internal::LoadPrivateKeyPem(private_key_pem, error);
  if (!key)
    return false;
  return DigestSign(key.get(), message, false, {}, signature, error);
}

bool Sm2Sign(std::string_view private_key_pem, std::string_view message, std::string_view user_id,
             Bytes* signature, std::string* error) {
  internal::PkeyPtr key = internal::LoadPrivateKeyPem(private_key_pem, error);
  if (!key)
    return false;
  return DigestSign(key.get(), message, true, user_id, signature, error);
}

bool Sm2Verify(std::string_view public_key_pem, std::string_view message, const Bytes& signature,
               std::string* error) {
  internal::PkeyPtr key = internal::LoadPublicKeyPem(public_key_pem, error);
  if (!key)
    return false;
  return DigestVerify(key.get(), message, false, {}, signature, error);
}

bool Sm2Verify(std::string_view public_key_pem, std::string_view message, std::string_view user_id,
               const Bytes& signature, std::string* error) {
  internal::PkeyPtr key = internal::LoadPublicKeyPem(public_key_pem, error);
  if (!key)
    return false;
  return DigestVerify(key.get(), message, true, user_id, signature, error);
}

bool Sm2VerifyCertificate(std::string_view certificate_pem, std::string_view message,
                          const Bytes& signature, std::string* error) {
  internal::X509Ptr certificate = internal::LoadCertificatePem(certificate_pem, error);
  if (!certificate)
    return false;
  return VerifyCertificate(certificate.get(), message, signature, error);
}

bool Sm2VerifyCertificateDer(const Bytes& certificate_der, std::string_view message,
                             const Bytes& signature, std::string* error) {
  internal::X509Ptr certificate =
      internal::LoadCertificateDer(certificate_der.data(), certificate_der.size(), error);
  if (!certificate)
    return false;
  return VerifyCertificate(certificate.get(), message, signature, error);
}

} // namespace uvnvr::gm
