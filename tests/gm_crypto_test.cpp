// Unit tests for the GM crypto module: SM3, SM4-CBC, SM2 sign/verify, CSR
// generation and device certificate import/validation.
//
// The binary also exposes three small modes so the results can be
// cross-checked against the openssl command line:
//
//   gm_crypto_test --digest <file>
//   gm_crypto_test --sign   <private-key.pem> <message-file> <signature-out>
//   gm_crypto_test --verify <public-key.pem>  <message-file> <signature-in>

#include "crypto/cert.h"
#include "crypto/sm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Expect(const std::string& name, const std::string& actual, const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
  ++failures;
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  if (haystack.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: '%s' does not contain '%s'\n", name.c_str(), haystack.c_str(),
               needle.c_str());
  ++failures;
}

void ExpectSize(const std::string& name, std::size_t actual, std::size_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %zu, expected %zu\n", name.c_str(), actual, expected);
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
struct X509ReqDeleter {
  void operator()(X509_REQ* value) const {
    if (value)
      X509_REQ_free(value);
  }
};
struct Pkcs12Deleter {
  void operator()(PKCS12* value) const {
    if (value)
      PKCS12_free(value);
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
using X509ReqPtr = std::unique_ptr<X509_REQ, X509ReqDeleter>;
using Pkcs12Ptr = std::unique_ptr<PKCS12, Pkcs12Deleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteAll(const std::filesystem::path& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  return output.good();
}

bool WriteAll(const std::filesystem::path& path, const uvnvr::gm::Bytes& data) {
  return WriteAll(path, std::string(reinterpret_cast<const char*>(data.data()), data.size()));
}

PkeyPtr LoadPrivateKey(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio)
    return {};
  return PkeyPtr(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::string PrivateKeyToPem(EVP_PKEY* key) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
    return {};
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  return length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string();
}

std::string CertificateToPem(X509* certificate) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1)
    return {};
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  return length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string();
}

uvnvr::gm::Bytes CertificateToDer(X509* certificate) {
  const int length = i2d_X509(certificate, nullptr);
  if (length <= 0)
    return {};
  uvnvr::gm::Bytes der(static_cast<std::size_t>(length));
  unsigned char* cursor = der.data();
  if (i2d_X509(certificate, &cursor) != length)
    return {};
  return der;
}

void AddCertificateExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer ? issuer : certificate, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
  if (!extension)
    return;
  X509_add_ext(certificate, extension, -1);
  X509_EXTENSION_free(extension);
}

X509* MakeCertificate(EVP_PKEY* subject_key, const std::string& common_name,
                      const std::string& organization, EVP_PKEY* issuer_key, X509* issuer,
                      const char* basic_constraints, long serial, long not_before_offset,
                      long not_after_offset, const std::string& subject_alt_names) {
  X509* certificate = X509_new();
  if (!certificate)
    return nullptr;
  X509_set_version(certificate, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate), serial);
  X509_gmtime_adj(X509_getm_notBefore(certificate), not_before_offset);
  X509_gmtime_adj(X509_getm_notAfter(certificate), not_after_offset);
  X509_set_pubkey(certificate, subject_key);

  X509_NAME* subject = X509_get_subject_name(certificate);
  X509_NAME_add_entry_by_txt(subject, "C", MBSTRING_UTF8,
                             reinterpret_cast<const unsigned char*>("CN"), -1, -1, 0);
  if (!organization.empty()) {
    X509_NAME_add_entry_by_txt(subject, "O", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char*>(organization.c_str()), -1, -1,
                               0);
  }
  X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8,
                             reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1,
                             0);
  X509_set_issuer_name(certificate, issuer ? X509_get_subject_name(issuer) : subject);

  AddCertificateExtension(certificate, issuer, NID_basic_constraints, basic_constraints);
  AddCertificateExtension(certificate, issuer, NID_key_usage,
                          std::strstr(basic_constraints, "CA:TRUE")
                              ? "critical,keyCertSign,cRLSign"
                              : "critical,digitalSignature,keyEncipherment");
  if (!subject_alt_names.empty())
    AddCertificateExtension(certificate, issuer, NID_subject_alt_name, subject_alt_names.c_str());

  if (X509_sign(certificate, issuer_key, EVP_sm3()) <= 0) {
    X509_free(certificate);
    return nullptr;
  }
  return certificate;
}

std::string MakeSelfSignedPem(const uvnvr::gm::Sm2KeyPair& key_pair, const std::string& cn,
                              const std::string& organization, const std::string& subject_alt_names,
                              X509Ptr* owned_certificate) {
  PkeyPtr key = LoadPrivateKey(key_pair.private_key_pem);
  if (!key)
    return {};
  X509Ptr certificate(MakeCertificate(key.get(), cn, organization, key.get(), nullptr,
                                      "critical,CA:FALSE", 100, -3600, 365L * 86400,
                                      subject_alt_names));
  if (!certificate)
    return {};
  std::string pem = CertificateToPem(certificate.get());
  if (owned_certificate)
    *owned_certificate = std::move(certificate);
  return pem;
}

void RunSm3Tests() {
  using uvnvr::gm::Bytes;
  using uvnvr::gm::Sm3;
  using uvnvr::gm::Sm3Hex;
  using uvnvr::gm::ToHex;

  Expect("SM3(abc)", Sm3Hex("abc"),
         "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0");
  Expect("SM3(empty)", Sm3Hex(""),
         "1ab21d8355cfa17f8e61194831e81a8f22bec8c728fefb747ed035eb5082aa2b");

  // The standard 64-byte vector uses "abcd" repeated sixteen times.
  std::string abcd_repeated;
  for (int i = 0; i < 16; ++i)
    abcd_repeated += "abcd";
  Expect("SM3(abcd x16)", Sm3Hex(abcd_repeated),
         "debe9ff92275b8a138604889c18e5a4d6fdb70e5387e5765293dcba39c0c5732");

  Bytes digest = Sm3("abc");
  ExpectSize("SM3 digest length", digest.size(), uvnvr::gm::kSm3DigestSize);
  Expect("SM3 hex round-trip", ToHex(digest), Sm3Hex("abc"));

  Bytes decoded;
  ExpectBool("FromHex accepts the digest", uvnvr::gm::FromHex(Sm3Hex("abc"), &decoded), true);
  Expect("FromHex output", ToHex(decoded), Sm3Hex("abc"));
  ExpectBool("FromHex rejects odd length", uvnvr::gm::FromHex("abc", &decoded), false);
  ExpectBool("FromHex rejects non-hex", uvnvr::gm::FromHex("zz", &decoded), false);
}

void RunSm4Tests(const std::filesystem::path& artifacts) {
  using uvnvr::gm::Bytes;
  using uvnvr::gm::FromHex;
  using uvnvr::gm::Sm4CbcDecrypt;
  using uvnvr::gm::Sm4CbcEncrypt;
  using uvnvr::gm::ToHex;

  Bytes key;
  Bytes iv;
  ExpectBool("SM4 key hex", FromHex("0123456789abcdeffedcba9876543210", &key), true);
  ExpectBool("SM4 iv hex", FromHex("000102030405060708090a0b0c0d0e0f", &iv), true);

  const std::string text = "hello sm4 gm";
  Bytes plaintext(text.begin(), text.end());
  Bytes ciphertext;
  std::string error;
  ExpectBool("SM4 encrypt", Sm4CbcEncrypt(key, iv, plaintext, &ciphertext, &error), true);
  Expect("SM4-CBC known answer (same as openssl enc -sm4-cbc)", ToHex(ciphertext),
         "a82482ce08c43419f140e1958ccc39bb");
  ExpectSize("SM4 ciphertext length", ciphertext.size(), 16);

  Bytes decrypted;
  ExpectBool("SM4 decrypt", Sm4CbcDecrypt(key, iv, ciphertext, &decrypted, &error), true);
  Expect("SM4 round-trip", std::string(decrypted.begin(), decrypted.end()), text);

  // An empty plaintext still produces one PKCS#7 padding block.
  Bytes empty_plaintext;
  Bytes empty_cipher;
  ExpectBool("SM4 encrypt empty", Sm4CbcEncrypt(key, iv, empty_plaintext, &empty_cipher, &error),
             true);
  ExpectSize("SM4 empty ciphertext length", empty_cipher.size(), 16);
  Bytes empty_round_trip;
  ExpectBool("SM4 decrypt empty", Sm4CbcDecrypt(key, iv, empty_cipher, &empty_round_trip, &error),
             true);
  ExpectBool("SM4 empty round-trip", empty_round_trip.empty(), true);

  // Block-aligned plaintext gains a whole padding block.
  Bytes aligned(32, 'A');
  Bytes aligned_cipher;
  ExpectBool("SM4 encrypt aligned", Sm4CbcEncrypt(key, iv, aligned, &aligned_cipher, &error), true);
  ExpectSize("SM4 aligned ciphertext length", aligned_cipher.size(), 48);

  Bytes wrong_key = key;
  wrong_key[0] ^= 0xFF;
  Bytes unused;
  ExpectBool("SM4 wrong key is rejected", Sm4CbcDecrypt(wrong_key, iv, ciphertext, &unused, &error),
             false);
  ExpectBool("SM4 bad key length is rejected",
             Sm4CbcEncrypt(Bytes(8, 0), iv, plaintext, &unused, &error), false);
  ExpectBool("SM4 bad iv length is rejected",
             Sm4CbcEncrypt(key, Bytes(8, 0), plaintext, &unused, &error), false);
  ExpectBool("SM4 truncated ciphertext is rejected",
             Sm4CbcDecrypt(key, iv, Bytes(15, 0), &unused, &error), false);

  WriteAll(artifacts / "sm4_plain.bin", plaintext);
  WriteAll(artifacts / "sm4_cipher.bin", ciphertext);
  WriteAll(artifacts / "sm4_key.hex", ToHex(key) + "\n");
  WriteAll(artifacts / "sm4_iv.hex", ToHex(iv) + "\n");
}

struct Sm2Artifacts {
  uvnvr::gm::Sm2KeyPair key_pair;
  uvnvr::gm::Bytes signature;
  std::string certificate_pem;
  uvnvr::gm::Bytes certificate_der;
  std::string message = "GB/T 32918 SM2 sign/verify test message";
};

void RunSm2Tests(const std::filesystem::path& artifacts, Sm2Artifacts* out) {
  using uvnvr::gm::Bytes;
  using uvnvr::gm::GenerateSm2KeyPair;
  using uvnvr::gm::Sm2Sign;
  using uvnvr::gm::Sm2Verify;
  using uvnvr::gm::Sm2VerifyCertificate;
  using uvnvr::gm::Sm2VerifyCertificateDer;
  using uvnvr::gm::ToHex;

  std::string error;
  if (!GenerateSm2KeyPair(&out->key_pair, &error)) {
    Expect("SM2 key generation", error, "");
    return;
  }
  ExpectContains("SM2 private key is PEM", out->key_pair.private_key_pem, "BEGIN PRIVATE KEY");
  ExpectContains("SM2 public key is PEM", out->key_pair.public_key_pem, "BEGIN PUBLIC KEY");

  if (!Sm2Sign(out->key_pair.private_key_pem, out->message, &out->signature, &error)) {
    Expect("SM2 sign", error, "");
    return;
  }
  ExpectBool("SM2 signature is non-empty", !out->signature.empty(), true);
  ExpectBool("SM2 verify",
             Sm2Verify(out->key_pair.public_key_pem, out->message, out->signature, &error), true);
  ExpectBool("SM2 verify rejects a modified message",
             Sm2Verify(out->key_pair.public_key_pem, out->message + "x", out->signature, &error),
             false);

  Bytes tampered = out->signature;
  tampered.back() ^= 0x01;
  ExpectBool("SM2 verify rejects a modified signature",
             Sm2Verify(out->key_pair.public_key_pem, out->message, tampered, &error), false);

  Bytes custom_signature;
  ExpectBool("SM2 sign with a custom user ID",
             Sm2Sign(out->key_pair.private_key_pem, out->message, "GP-28181-28181-0001",
                     &custom_signature, &error),
             true);
  ExpectBool("SM2 verify with the same user ID",
             Sm2Verify(out->key_pair.public_key_pem, out->message, "GP-28181-28181-0001",
                       custom_signature, &error),
             true);
  ExpectBool("SM2 verify with the default user ID fails",
             Sm2Verify(out->key_pair.public_key_pem, out->message, custom_signature, &error),
             false);

  X509Ptr certificate;
  out->certificate_pem = MakeSelfSignedPem(out->key_pair, "34020000001320000001", "UvNVR Test",
                                           "DNS:nvr.example.test,IP:192.168.1.64", &certificate);
  if (!certificate) {
    Expect("SM2 self-signed certificate", "missing", "present");
    return;
  }
  out->certificate_der = CertificateToDer(certificate.get());
  ExpectBool("SM2 verify with a PEM certificate",
             Sm2VerifyCertificate(out->certificate_pem, out->message, out->signature, &error),
             true);
  ExpectBool("SM2 verify with a DER certificate",
             Sm2VerifyCertificateDer(out->certificate_der, out->message, out->signature, &error),
             true);
  ExpectBool("SM2 certificate verify rejects a modified message",
             Sm2VerifyCertificate(out->certificate_pem, out->message + "x", out->signature, &error),
             false);

  // A key generated independently must not verify the signature.
  uvnvr::gm::Sm2KeyPair other;
  GenerateSm2KeyPair(&other, &error);
  ExpectBool("SM2 verify rejects the wrong public key",
             Sm2Verify(other.public_key_pem, out->message, out->signature, &error), false);

  // SM2 public-key encryption, the primitive used to wrap a VKEK.
  Bytes vkek(16);
  for (std::size_t i = 0; i < vkek.size(); ++i)
    vkek[i] = static_cast<unsigned char>(0x40 + i);
  Bytes encrypted_vkek;
  ExpectBool("SM2 encrypt",
             uvnvr::gm::Sm2Encrypt(out->key_pair.public_key_pem, vkek, &encrypted_vkek, &error),
             true);
  ExpectBool("SM2 ciphertext is non-empty", encrypted_vkek.empty(), false);

  Bytes decrypted_vkek;
  ExpectBool(
      "SM2 decrypt",
      uvnvr::gm::Sm2Decrypt(out->key_pair.private_key_pem, encrypted_vkek, &decrypted_vkek, &error),
      true);
  Expect("SM2 encryption round-trip", ToHex(decrypted_vkek), ToHex(vkek));

  Bytes tampered_ciphertext = encrypted_vkek;
  tampered_ciphertext.back() ^= 0x01;
  Bytes unused_plaintext;
  ExpectBool("SM2 decrypt rejects a modified ciphertext",
             uvnvr::gm::Sm2Decrypt(out->key_pair.private_key_pem, tampered_ciphertext,
                                   &unused_plaintext, &error),
             false);
  ExpectBool(
      "SM2 decrypt rejects the wrong private key",
      uvnvr::gm::Sm2Decrypt(other.private_key_pem, encrypted_vkek, &unused_plaintext, &error),
      false);
  ExpectBool(
      "SM2 encrypt rejects an empty plaintext",
      uvnvr::gm::Sm2Encrypt(out->key_pair.public_key_pem, Bytes{}, &unused_plaintext, &error),
      false);

  WriteAll(artifacts / "sm2_vkek_plain.bin", vkek);
  WriteAll(artifacts / "sm2_vkek_cipher.der", encrypted_vkek);

  // The negotiation exchanges SM3(DER certificate) so the platform can choose
  // the enrolled device certificate.
  Bytes fingerprint;
  ExpectBool("certificate fingerprint",
             uvnvr::gm::CertificateFingerprintSm3(out->certificate_pem, &fingerprint, &error),
             true);
  ExpectSize("certificate fingerprint size", fingerprint.size(), uvnvr::gm::kSm3DigestSize);
  Expect("certificate fingerprint is SM3(DER)", ToHex(fingerprint),
         ToHex(uvnvr::gm::Sm3(out->certificate_der.data(), out->certificate_der.size())));

  WriteAll(artifacts / "sm2_key.pem", out->key_pair.private_key_pem);
  WriteAll(artifacts / "sm2_pub.pem", out->key_pair.public_key_pem);
  WriteAll(artifacts / "sm2_message.bin", out->message);
  WriteAll(artifacts / "sm2_signature.der", out->signature);
  WriteAll(artifacts / "sm2_cert.pem", out->certificate_pem);
  WriteAll(artifacts / "sm2_cert.der", out->certificate_der);
}

void RunCsrTests(const std::filesystem::path& artifacts, const Sm2Artifacts& material) {
  using uvnvr::gm::CsrOptions;
  using uvnvr::gm::GenerateCsr;

  CsrOptions options;
  options.common_name = "34020000001320000001";
  options.organization = "UvNVR Test";
  options.organizational_unit = "GB35114";
  options.dns_sans = {"nvr.example.test", "device.local"};
  options.ip_sans = {"192.168.1.64"};

  std::string csr_pem;
  std::string error;
  if (!GenerateCsr(material.key_pair.private_key_pem, options, &csr_pem, &error)) {
    Expect("GenerateCsr", error, "");
    return;
  }
  ExpectContains("CSR PEM header", csr_pem, "BEGIN CERTIFICATE REQUEST");
  WriteAll(artifacts / "device.csr", csr_pem);

  BioPtr bio(BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size())));
  X509ReqPtr request(bio ? PEM_read_bio_X509_REQ(bio.get(), nullptr, nullptr, nullptr) : nullptr);
  if (!request) {
    Expect("CSR parses", "no", "yes");
    return;
  }

  PkeyPtr public_key(X509_REQ_get_pubkey(request.get()));
  ExpectBool("CSR public key present", static_cast<bool>(public_key), true);
  ExpectBool("CSR signature verifies",
             public_key && X509_REQ_verify(request.get(), public_key.get()) == 1, true);

  X509_NAME* subject = X509_REQ_get_subject_name(request.get());
  char buffer[256];
  std::memset(buffer, 0, sizeof(buffer));
  X509_NAME_get_text_by_NID(subject, NID_commonName, buffer, sizeof(buffer));
  Expect("CSR CN", buffer, options.common_name);
  std::memset(buffer, 0, sizeof(buffer));
  X509_NAME_get_text_by_NID(subject, NID_organizationName, buffer, sizeof(buffer));
  Expect("CSR O", buffer, options.organization);
  std::memset(buffer, 0, sizeof(buffer));
  X509_NAME_get_text_by_NID(subject, NID_organizationalUnitName, buffer, sizeof(buffer));
  Expect("CSR OU", buffer, options.organizational_unit);

  STACK_OF(X509_EXTENSION)* extensions = X509_REQ_get_extensions(request.get());
  const int san_index =
      extensions ? X509v3_get_ext_by_NID(extensions, NID_subject_alt_name, -1) : -1;
  ExpectBool("CSR has subjectAltName", san_index >= 0, true);
  if (san_index >= 0) {
    X509_EXTENSION* extension = X509v3_get_ext(extensions, san_index);
    GENERAL_NAMES* names =
        extension ? static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(extension)) : nullptr;
    std::vector<std::string> dns_names;
    std::vector<std::string> ip_addresses;
    if (names) {
      for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
        GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
        int type = 0;
        void* value = GENERAL_NAME_get0_value(name, &type);
        if (!value)
          continue;
        if (type == GEN_DNS) {
          const ASN1_STRING* text = static_cast<const ASN1_STRING*>(value);
          dns_names.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(text)),
                                 static_cast<std::size_t>(ASN1_STRING_length(text)));
        } else if (type == GEN_IPADD) {
          const ASN1_STRING* address = static_cast<const ASN1_STRING*>(value);
          if (ASN1_STRING_length(address) == 4) {
            const unsigned char* bytes = ASN1_STRING_get0_data(address);
            char text[16];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2],
                          bytes[3]);
            ip_addresses.emplace_back(text);
          }
        }
      }
      GENERAL_NAMES_free(names);
    }
    ExpectSize("CSR DNS SAN count", dns_names.size(), options.dns_sans.size());
    ExpectBool("CSR contains nvr.example.test",
               std::find(dns_names.begin(), dns_names.end(), "nvr.example.test") != dns_names.end(),
               true);
    ExpectBool("CSR contains device.local",
               std::find(dns_names.begin(), dns_names.end(), "device.local") != dns_names.end(),
               true);
    ExpectBool("CSR contains 192.168.1.64",
               std::find(ip_addresses.begin(), ip_addresses.end(), "192.168.1.64") !=
                   ip_addresses.end(),
               true);
  }
  if (extensions)
    sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);

  CsrOptions missing_cn;
  std::string unused;
  ExpectBool("CSR rejects an empty CN",
             GenerateCsr(material.key_pair.private_key_pem, missing_cn, &unused, &error), false);
}

void RunIdentityTests(const std::filesystem::path& artifacts, const Sm2Artifacts& material) {
  using uvnvr::gm::DeviceIdentity;
  using uvnvr::gm::ImportPemIdentity;
  using uvnvr::gm::ImportPemIdentityFromFiles;
  using uvnvr::gm::ImportPfxIdentity;
  using uvnvr::gm::ImportPfxIdentityFromFile;

  std::string error;
  DeviceIdentity identity;
  ExpectBool("import PEM identity",
             ImportPemIdentity(material.key_pair.private_key_pem, material.certificate_pem, "",
                               &identity, &error),
             true);
  ExpectContains("imported key", identity.private_key_pem, "BEGIN PRIVATE KEY");
  ExpectContains("imported certificate", identity.certificate_pem, "BEGIN CERTIFICATE");

  uvnvr::gm::Sm2KeyPair unrelated;
  uvnvr::gm::GenerateSm2KeyPair(&unrelated, &error);
  ExpectBool(
      "import rejects a mismatched key",
      ImportPemIdentity(unrelated.private_key_pem, material.certificate_pem, "", &identity, &error),
      false);

  WriteAll(artifacts / "identity_key.pem", material.key_pair.private_key_pem);
  WriteAll(artifacts / "identity_cert.pem", material.certificate_pem);
  ExpectBool("import PEM identity from files",
             ImportPemIdentityFromFiles((artifacts / "identity_key.pem").string(),
                                        (artifacts / "identity_cert.pem").string(), "", &identity,
                                        &error),
             true);

  // Build a PKCS#12 container from the same key and certificate.
  PkeyPtr key = LoadPrivateKey(material.key_pair.private_key_pem);
  BioPtr cert_bio(BIO_new_mem_buf(material.certificate_pem.data(),
                                  static_cast<int>(material.certificate_pem.size())));
  X509Ptr certificate(cert_bio ? PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr)
                               : nullptr);
  if (!key || !certificate) {
    Expect("PKCS#12 fixture", "missing", "present");
    return;
  }
  Pkcs12Ptr container(PKCS12_create("s3cret", "uvnvr-device", key.get(), certificate.get(), nullptr,
                                    0, 0, 0, 0, 0));
  ExpectBool("PKCS#12 container created", static_cast<bool>(container), true);
  if (!container)
    return;

  const int length = i2d_PKCS12(container.get(), nullptr);
  uvnvr::gm::Bytes pfx(static_cast<std::size_t>(length > 0 ? length : 0));
  if (length > 0) {
    unsigned char* cursor = pfx.data();
    i2d_PKCS12(container.get(), &cursor);
  }
  ExpectBool("import PFX identity", ImportPfxIdentity(pfx, "s3cret", &identity, &error), true);
  ExpectBool("import PFX rejects a wrong password",
             ImportPfxIdentity(pfx, "wrong", &identity, &error), false);

  WriteAll(artifacts / "identity.pfx", pfx);
  ExpectBool(
      "import PFX identity from file",
      ImportPfxIdentityFromFile((artifacts / "identity.pfx").string(), "s3cret", &identity, &error),
      true);
}

void RunChainTests(const std::filesystem::path& artifacts, const Sm2Artifacts& material) {
  using uvnvr::gm::CertificateDetails;
  using uvnvr::gm::ChainValidationResult;
  using uvnvr::gm::DescribeCertificate;
  using uvnvr::gm::ValidateCertificateChain;
  using uvnvr::gm::ValidateCertificateChainDer;

  uvnvr::gm::Sm2KeyPair root_pair;
  uvnvr::gm::Sm2KeyPair intermediate_pair;
  uvnvr::gm::Sm2KeyPair leaf_pair;
  std::string error;
  if (!uvnvr::gm::GenerateSm2KeyPair(&root_pair, &error) ||
      !uvnvr::gm::GenerateSm2KeyPair(&intermediate_pair, &error) ||
      !uvnvr::gm::GenerateSm2KeyPair(&leaf_pair, &error)) {
    Expect("chain key generation", error, "");
    return;
  }

  PkeyPtr root_key = LoadPrivateKey(root_pair.private_key_pem);
  PkeyPtr intermediate_key = LoadPrivateKey(intermediate_pair.private_key_pem);
  PkeyPtr leaf_key = LoadPrivateKey(leaf_pair.private_key_pem);
  if (!root_key || !intermediate_key || !leaf_key) {
    Expect("chain key loading", "missing", "present");
    return;
  }

  X509Ptr root(MakeCertificate(root_key.get(), "UvNVR Test Root CA", "UvNVR Test", root_key.get(),
                               nullptr, "critical,CA:TRUE,pathlen:1", 1, -3600, 3650L * 86400, ""));
  X509Ptr intermediate(MakeCertificate(intermediate_key.get(), "UvNVR Test Device CA", "UvNVR Test",
                                       root_key.get(), root.get(), "critical,CA:TRUE,pathlen:0", 2,
                                       -3600, 1825L * 86400, ""));
  X509Ptr leaf(MakeCertificate(leaf_key.get(), "34020000001320000001", "UvNVR Test",
                               intermediate_key.get(), intermediate.get(), "critical,CA:FALSE", 3,
                               -3600, 365L * 86400, "DNS:nvr.example.test,IP:192.168.1.64"));
  X509Ptr expired(MakeCertificate(leaf_key.get(), "34020000001320000002", "UvNVR Test",
                                  intermediate_key.get(), intermediate.get(), "critical,CA:FALSE",
                                  4, -2L * 86400, -86400, ""));
  if (!root || !intermediate || !leaf || !expired) {
    Expect("chain certificate creation", "missing", "present");
    return;
  }

  const std::string root_pem = CertificateToPem(root.get());
  const std::string intermediate_pem = CertificateToPem(intermediate.get());
  const std::string leaf_pem = CertificateToPem(leaf.get());
  const std::string expired_pem = CertificateToPem(expired.get());
  const uvnvr::gm::Bytes leaf_der = CertificateToDer(leaf.get());

  WriteAll(artifacts / "chain_root.pem", root_pem);
  WriteAll(artifacts / "chain_intermediate.pem", intermediate_pem);
  WriteAll(artifacts / "chain_leaf.pem", leaf_pem);

  ChainValidationResult result;
  ExpectBool("validate leaf -> intermediate -> root",
             ValidateCertificateChain(leaf_pem, intermediate_pem, root_pem, &result), true);
  ExpectBool("chain result ok", result.ok, true);
  Expect("verified chain length", std::to_string(result.verified_chain_length), "3");

  ExpectBool("validate leaf from DER",
             ValidateCertificateChainDer(leaf_der, intermediate_pem, root_pem, &result), true);

  ExpectBool("missing intermediate is rejected",
             ValidateCertificateChain(leaf_pem, "", root_pem, &result), false);
  ExpectBool("missing-intermediate result is failure", result.ok, false);
  ExpectContains("missing-intermediate error", result.error, "validation failed");

  uvnvr::gm::Sm2KeyPair other_root_pair;
  uvnvr::gm::GenerateSm2KeyPair(&other_root_pair, &error);
  PkeyPtr other_root_key = LoadPrivateKey(other_root_pair.private_key_pem);
  X509Ptr other_root(MakeCertificate(other_root_key.get(), "Unrelated Root CA", "Other",
                                     other_root_key.get(), nullptr, "critical,CA:TRUE", 9, -3600,
                                     3650L * 86400, ""));
  const std::string other_root_pem = CertificateToPem(other_root.get());
  ExpectBool("unrelated trust anchor is rejected",
             ValidateCertificateChain(leaf_pem, intermediate_pem, other_root_pem, &result), false);

  ExpectBool("expired leaf is rejected",
             ValidateCertificateChain(expired_pem, intermediate_pem, root_pem, &result), false);
  ExpectContains("expired error", result.error, "expired");

  ExpectBool("empty trust anchors are rejected",
             ValidateCertificateChain(leaf_pem, intermediate_pem, "", &result), false);

  CertificateDetails details;
  ExpectBool("describe leaf certificate", DescribeCertificate(leaf_pem, &details, &error), true);
  Expect("leaf CN", details.common_name, "34020000001320000001");
  Expect("leaf O", details.organization, "UvNVR Test");
  ExpectContains("leaf issuer", details.issuer, "UvNVR Test Device CA");
  ExpectBool("leaf is not a CA", details.is_ca, false);
  ExpectSize("leaf DNS SAN count", details.dns_sans.size(), 1);
  Expect("leaf DNS SAN", details.dns_sans.empty() ? "" : details.dns_sans.front(),
         "nvr.example.test");
  ExpectSize("leaf IP SAN count", details.ip_sans.size(), 1);
  Expect("leaf IP SAN", details.ip_sans.empty() ? "" : details.ip_sans.front(), "192.168.1.64");
  ExpectBool("leaf validity is populated",
             !details.not_before.empty() && !details.not_after.empty(), true);

  CertificateDetails root_details;
  ExpectBool("describe root certificate", DescribeCertificate(root_pem, &root_details, &error),
             true);
  ExpectBool("root is a CA", root_details.is_ca, true);

  // The identity fixture's self-signed certificate is not issued by this root.
  ChainValidationResult unused_result;
  ExpectBool("unrelated self-signed leaf is rejected",
             ValidateCertificateChain(material.certificate_pem, "", root_pem, &unused_result),
             false);
}

std::filesystem::path PrepareArtifactsDirectory() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "uvnvr_gm_crypto_test";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::fprintf(stderr, "warning: cannot prepare %s: %s\n", directory.string().c_str(),
                 error.message().c_str());
  }
  return directory;
}

int RunDigest(const char* path) {
  if (!std::filesystem::exists(path)) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  const std::string contents = ReadAll(path);
  std::printf("%s\n", uvnvr::gm::Sm3Hex(contents).c_str());
  return 0;
}

int RunSign(const char* key_path, const char* message_path, const char* signature_path) {
  if (!std::filesystem::exists(key_path) || !std::filesystem::exists(message_path)) {
    std::fprintf(stderr, "cannot read the key or message\n");
    return 2;
  }
  const std::string key = ReadAll(key_path);
  const std::string message = ReadAll(message_path);
  if (key.empty()) {
    std::fprintf(stderr, "cannot read %s\n", key_path);
    return 2;
  }
  uvnvr::gm::Bytes signature;
  std::string error;
  if (!uvnvr::gm::Sm2Sign(key, message, &signature, &error)) {
    std::fprintf(stderr, "sign failed: %s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(signature_path, signature)) {
    std::fprintf(stderr, "cannot write %s\n", signature_path);
    return 2;
  }
  std::printf("signed %zu message bytes, %zu signature bytes\n", message.size(), signature.size());
  return 0;
}

int RunVerify(const char* key_path, const char* message_path, const char* signature_path) {
  const std::string key = ReadAll(key_path);
  const std::string message = ReadAll(message_path);
  const std::string signature_text = ReadAll(signature_path);
  if (key.empty() || signature_text.empty()) {
    std::fprintf(stderr, "cannot read the key or signature\n");
    return 2;
  }
  const uvnvr::gm::Bytes signature(signature_text.begin(), signature_text.end());
  std::string error;
  if (!uvnvr::gm::Sm2Verify(key, message, signature, &error)) {
    std::fprintf(stderr, "verify failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("verified OK\n");
  return 0;
}

int RunEncrypt(const char* key_path, const char* plain_path, const char* cipher_path) {
  const std::string key = ReadAll(key_path);
  const std::string plain_text = ReadAll(plain_path);
  if (key.empty() || plain_text.empty()) {
    std::fprintf(stderr, "cannot read the public key or plaintext\n");
    return 2;
  }
  uvnvr::gm::Bytes plaintext(plain_text.begin(), plain_text.end());
  uvnvr::gm::Bytes ciphertext;
  std::string error;
  if (!uvnvr::gm::Sm2Encrypt(key, plaintext, &ciphertext, &error)) {
    std::fprintf(stderr, "encrypt failed: %s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(cipher_path, ciphertext)) {
    std::fprintf(stderr, "cannot write %s\n", cipher_path);
    return 2;
  }
  std::printf("encrypted %zu plaintext bytes, %zu ciphertext bytes\n", plaintext.size(),
              ciphertext.size());
  return 0;
}

int RunDecrypt(const char* key_path, const char* cipher_path, const char* plain_path) {
  const std::string key = ReadAll(key_path);
  const std::string cipher_text = ReadAll(cipher_path);
  if (key.empty() || cipher_text.empty()) {
    std::fprintf(stderr, "cannot read the private key or ciphertext\n");
    return 2;
  }
  const uvnvr::gm::Bytes ciphertext(cipher_text.begin(), cipher_text.end());
  uvnvr::gm::Bytes plaintext;
  std::string error;
  if (!uvnvr::gm::Sm2Decrypt(key, ciphertext, &plaintext, &error)) {
    std::fprintf(stderr, "decrypt failed: %s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(plain_path, plaintext)) {
    std::fprintf(stderr, "cannot write %s\n", plain_path);
    return 2;
  }
  std::printf("decrypted %zu ciphertext bytes, %zu plaintext bytes\n", ciphertext.size(),
              plaintext.size());
  return 0;
}

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage:\n"
               "  %s                                      run the unit tests\n"
               "  %s --digest <file>                      print the SM3 hex digest\n"
               "  %s --sign <key.pem> <msg> <sig-out>     sign a file with SM2/SM3\n"
               "  %s --verify <key.pem> <msg> <sig>       verify a signature with SM2/SM3\n"
               "  %s --encrypt <pub.pem> <plain> <out>    encrypt a file with SM2\n"
               "  %s --decrypt <key.pem> <cipher> <out>   decrypt a file with SM2\n",
               program, program, program, program, program, program);
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string mode = argv[1];
    if (mode == "--digest" && argc == 3)
      return RunDigest(argv[2]);
    if (mode == "--sign" && argc == 5)
      return RunSign(argv[2], argv[3], argv[4]);
    if (mode == "--verify" && argc == 5)
      return RunVerify(argv[2], argv[3], argv[4]);
    if (mode == "--encrypt" && argc == 5)
      return RunEncrypt(argv[2], argv[3], argv[4]);
    if (mode == "--decrypt" && argc == 5)
      return RunDecrypt(argv[2], argv[3], argv[4]);
    PrintUsage(argv[0]);
    return 2;
  }

  const std::filesystem::path artifacts = PrepareArtifactsDirectory();
  Sm2Artifacts material;
  RunSm3Tests();
  RunSm4Tests(artifacts);
  RunSm2Tests(artifacts, &material);
  RunCsrTests(artifacts, material);
  RunIdentityTests(artifacts, material);
  RunChainTests(artifacts, material);

  if (failures == 0) {
    std::printf("gm_crypto_test: all checks passed\n");
    std::printf("gm_crypto_test: artifacts in %s\n", artifacts.string().c_str());
    return 0;
  }
  std::fprintf(stderr, "gm_crypto_test: %d checks failed\n", failures);
  return 1;
}
