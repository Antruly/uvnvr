#include "crypto/cert.h"

#include "crypto/crypto_internal.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace uvnvr::gm {
namespace {

using internal::ClearOpenSslErrors;
using internal::SetError;

struct ExtensionStackDeleter {
  void operator()(STACK_OF(X509_EXTENSION) * value) const {
    if (value)
      sk_X509_EXTENSION_pop_free(value, X509_EXTENSION_free);
  }
};

using ExtensionStackPtr = std::unique_ptr<STACK_OF(X509_EXTENSION), ExtensionStackDeleter>;

bool ReadFile(const std::string& path, std::string* contents, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    SetError(error, "cannot open " + path);
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (input.bad()) {
    SetError(error, "cannot read " + path);
    return false;
  }
  *contents = buffer.str();
  return true;
}

std::string NameToString(X509_NAME* name) {
  if (!name)
    return {};
  internal::BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio)
    return {};
  if (X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253) <= 0)
    return {};
  return internal::BioToString(bio.get());
}

std::string NameEntry(X509_NAME* name, int nid) {
  if (!name)
    return {};
  const int index = X509_NAME_get_index_by_NID(name, nid, -1);
  if (index < 0)
    return {};
  X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, index);
  if (!entry)
    return {};
  ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
  if (!data)
    return {};
  unsigned char* utf8 = nullptr;
  const int length = ASN1_STRING_to_UTF8(&utf8, data);
  std::string result;
  if (length > 0 && utf8)
    result.assign(reinterpret_cast<const char*>(utf8), static_cast<std::size_t>(length));
  OPENSSL_free(utf8);
  return result;
}

std::string Asn1TimeToString(const ASN1_TIME* time) {
  if (!time)
    return {};
  struct tm parts {};
  if (ASN1_TIME_to_tm(time, &parts) != 1)
    return {};
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d%02d%02d%02dZ", parts.tm_year + 1900,
                parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
  return buffer;
}

std::string SerialToHex(X509* certificate) {
  if (!certificate)
    return {};
  BIGNUM* number = ASN1_INTEGER_to_BN(X509_get0_serialNumber(certificate), nullptr);
  if (!number)
    return {};
  char* hex = BN_bn2hex(number);
  std::string result = hex ? hex : "";
  OPENSSL_free(hex);
  BN_free(number);
  for (char& character : result)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

std::string IpToString(const unsigned char* data, int length) {
  if (!data || length <= 0)
    return {};
  if (length == 4) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", data[0], data[1], data[2], data[3]);
    return buffer;
  }
  std::string result;
  for (int i = 0; i < length; ++i) {
    if (i != 0 && i % 2 == 0)
      result.push_back(':');
    char byte[3];
    std::snprintf(byte, sizeof(byte), "%02x", data[i]);
    result += byte;
  }
  return result;
}

void AppendSubjectAltNames(X509* certificate, CertificateDetails* details) {
  GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
  if (!names)
    return;
  for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
    GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    int type = 0;
    void* value = GENERAL_NAME_get0_value(name, &type);
    if (!value)
      continue;
    if (type == GEN_DNS) {
      const ASN1_STRING* dns = static_cast<const ASN1_STRING*>(value);
      details->dns_sans.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(dns)),
                                     static_cast<std::size_t>(ASN1_STRING_length(dns)));
    } else if (type == GEN_IPADD) {
      const ASN1_STRING* address = static_cast<const ASN1_STRING*>(value);
      details->ip_sans.push_back(
          IpToString(ASN1_STRING_get0_data(address), ASN1_STRING_length(address)));
    }
  }
  GENERAL_NAMES_free(names);
}

bool AddSubjectEntry(X509_NAME* name, const char* field, const std::string& value,
                     std::string* error) {
  if (value.empty())
    return true;
  if (X509_NAME_add_entry_by_txt(name, field, MBSTRING_UTF8,
                                 reinterpret_cast<const unsigned char*>(value.c_str()), -1, -1,
                                 0) != 1) {
    SetError(error,
             std::string("cannot add ") + field + " to the subject: " + internal::OpenSslErrors());
    return false;
  }
  return true;
}

bool AddExtension(STACK_OF(X509_EXTENSION) * extensions, int nid, const char* value, bool critical,
                  std::string* error) {
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, nullptr, nid, value);
  if (!extension) {
    SetError(error,
             "cannot build extension " + std::to_string(nid) + ": " + internal::OpenSslErrors());
    return false;
  }
  X509_EXTENSION_set_critical(extension, critical ? 1 : 0);
  if (sk_X509_EXTENSION_push(extensions, extension) == 0) {
    X509_EXTENSION_free(extension);
    SetError(error, "cannot append an extension to the request");
    return false;
  }
  return true;
}

bool SafeSanEntry(const std::string& value) {
  return value.find(',') == std::string::npos && value.find('\n') == std::string::npos &&
         value.find('\r') == std::string::npos;
}

bool ValidateChain(X509* leaf, std::string_view chain_pem, std::string_view trusted_roots_pem,
                   ChainValidationResult* result) {
  if (!result)
    return false;
  *result = ChainValidationResult{};

  std::string parse_error;
  std::vector<internal::X509Ptr> chain = internal::LoadCertificateChainPem(chain_pem, &parse_error);
  if (!chain_pem.empty() && chain.empty()) {
    result->error = parse_error;
    return false;
  }

  std::vector<internal::X509Ptr> roots =
      internal::LoadCertificateChainPem(trusted_roots_pem, &parse_error);
  if (roots.empty()) {
    result->error = trusted_roots_pem.empty() ? "trusted CA bundle is empty" : parse_error;
    return false;
  }

  internal::X509StorePtr store(X509_STORE_new());
  if (!store) {
    result->error = "cannot allocate an X.509 store";
    return false;
  }
  for (const auto& root : roots) {
    if (X509_STORE_add_cert(store.get(), root.get()) != 1) {
      const unsigned long code = ERR_peek_last_error();
      // Repeated roots are harmless; anything else is a real failure.
      if (ERR_GET_REASON(code) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        ClearOpenSslErrors();
        continue;
      }
      result->error = "cannot add a trust anchor: " + internal::OpenSslErrors();
      return false;
    }
  }

  STACK_OF(X509)* untrusted = sk_X509_new_null();
  if (!untrusted) {
    result->error = "cannot allocate the intermediate certificate stack";
    return false;
  }
  for (const auto& certificate : chain) {
    if (sk_X509_push(untrusted, certificate.get()) == 0) {
      sk_X509_free(untrusted);
      result->error = "cannot populate the intermediate certificate stack";
      return false;
    }
  }

  internal::X509StoreCtxPtr context(X509_STORE_CTX_new());
  if (!context) {
    sk_X509_free(untrusted);
    result->error = "cannot allocate an X.509 verification context";
    return false;
  }
  bool initialised = X509_STORE_CTX_init(context.get(), store.get(), leaf, untrusted) == 1;
  if (initialised) {
    X509_VERIFY_PARAM* parameters = X509_STORE_CTX_get0_param(context.get());
    if (parameters)
      X509_VERIFY_PARAM_set_depth(parameters, 16);
  }
  const int verified = initialised ? X509_verify_cert(context.get()) : 0;
  sk_X509_free(untrusted);

  if (verified != 1) {
    std::string message = "certificate chain validation failed";
    const int code = X509_STORE_CTX_get_error(context.get());
    if (code != X509_V_OK)
      message += std::string(": ") + X509_verify_cert_error_string(code);
    const int depth = X509_STORE_CTX_get_error_depth(context.get());
    message += " (depth " + std::to_string(depth) + ")";
    result->error = std::move(message);
    return false;
  }

  STACK_OF(X509)* verified_chain = X509_STORE_CTX_get1_chain(context.get());
  result->verified_chain_length = verified_chain ? sk_X509_num(verified_chain) : 0;
  if (verified_chain)
    sk_X509_pop_free(verified_chain, X509_free);
  result->ok = true;
  return true;
}

} // namespace

bool ImportPemIdentity(std::string_view private_key_pem, std::string_view certificate_pem,
                       std::string_view chain_pem, DeviceIdentity* identity, std::string* error) {
  if (!identity) {
    SetError(error, "device identity output is null");
    return false;
  }

  internal::PkeyPtr private_key = internal::LoadPrivateKeyPem(private_key_pem, error);
  if (!private_key)
    return false;
  internal::X509Ptr certificate = internal::LoadCertificatePem(certificate_pem, error);
  if (!certificate)
    return false;

  if (X509_check_private_key(certificate.get(), private_key.get()) != 1) {
    SetError(error, "the private key does not match the certificate");
    ClearOpenSslErrors();
    return false;
  }

  std::string chain_out;
  if (!chain_pem.empty()) {
    std::string chain_error;
    std::vector<internal::X509Ptr> chain =
        internal::LoadCertificateChainPem(chain_pem, &chain_error);
    if (chain.empty()) {
      SetError(error, chain_error.empty() ? "the certificate chain PEM is invalid" : chain_error);
      return false;
    }
    chain_out = internal::CertificateChainToPem(chain);
    if (chain_out.empty()) {
      SetError(error, "cannot encode the certificate chain as PEM");
      return false;
    }
  }

  DeviceIdentity result;
  result.private_key_pem = internal::PrivateKeyToPem(private_key.get());
  result.certificate_pem = internal::CertificateToPem(certificate.get());
  if (result.private_key_pem.empty() || result.certificate_pem.empty()) {
    SetError(error, "cannot encode the device identity as PEM");
    return false;
  }
  result.chain_pem = std::move(chain_out);
  *identity = std::move(result);
  return true;
}

bool ImportPemIdentityFromFiles(const std::string& private_key_path,
                                const std::string& certificate_path, const std::string& chain_path,
                                DeviceIdentity* identity, std::string* error) {
  std::string private_key_pem;
  if (!ReadFile(private_key_path, &private_key_pem, error))
    return false;
  std::string certificate_pem;
  if (!ReadFile(certificate_path, &certificate_pem, error))
    return false;
  std::string chain_pem;
  if (!chain_path.empty() && !ReadFile(chain_path, &chain_pem, error))
    return false;
  return ImportPemIdentity(private_key_pem, certificate_pem, chain_pem, identity, error);
}

bool ImportPfxIdentity(const Bytes& pfx, std::string_view password, DeviceIdentity* identity,
                       std::string* error) {
  if (!identity) {
    SetError(error, "device identity output is null");
    return false;
  }
  if (pfx.empty()) {
    SetError(error, "the PKCS#12 container is empty");
    return false;
  }

  internal::BioPtr bio = internal::BioFromMemory(
      std::string_view(reinterpret_cast<const char*>(pfx.data()), pfx.size()));
  if (!bio) {
    SetError(error, "cannot allocate a BIO for the PKCS#12 container");
    return false;
  }
  PKCS12* container = d2i_PKCS12_bio(bio.get(), nullptr);
  if (!container) {
    SetError(error, "cannot parse the PKCS#12 container: " + internal::OpenSslErrors());
    return false;
  }

  EVP_PKEY* raw_key = nullptr;
  X509* raw_certificate = nullptr;
  STACK_OF(X509)* raw_chain = nullptr;
  const std::string password_text(password);
  const char* password_data = password_text.empty() ? nullptr : password_text.c_str();
  const int parsed = PKCS12_parse(container, password_data, &raw_key, &raw_certificate, &raw_chain);
  PKCS12_free(container);
  if (parsed != 1) {
    if (raw_key)
      EVP_PKEY_free(raw_key);
    if (raw_certificate)
      X509_free(raw_certificate);
    if (raw_chain)
      sk_X509_pop_free(raw_chain, X509_free);
    SetError(error, "cannot open the PKCS#12 container (wrong password or corrupt file): " +
                        internal::OpenSslErrors());
    return false;
  }

  internal::PkeyPtr private_key(raw_key);
  internal::X509Ptr certificate(raw_certificate);
  std::string chain_pem = internal::CertificateChainToPem(raw_chain);
  if (raw_chain)
    sk_X509_pop_free(raw_chain, X509_free);

  if (!private_key || !certificate) {
    SetError(error, "the PKCS#12 container must contain both a private key and a certificate");
    return false;
  }
  if (X509_check_private_key(certificate.get(), private_key.get()) != 1) {
    SetError(error, "the private key does not match the certificate in the PKCS#12 container");
    ClearOpenSslErrors();
    return false;
  }

  DeviceIdentity result;
  result.private_key_pem = internal::PrivateKeyToPem(private_key.get());
  result.certificate_pem = internal::CertificateToPem(certificate.get());
  if (result.private_key_pem.empty() || result.certificate_pem.empty()) {
    SetError(error, "cannot encode the PKCS#12 identity as PEM");
    return false;
  }
  result.chain_pem = std::move(chain_pem);
  *identity = std::move(result);
  return true;
}

bool ImportPfxIdentityFromFile(const std::string& pfx_path, std::string_view password,
                               DeviceIdentity* identity, std::string* error) {
  std::string contents;
  if (!ReadFile(pfx_path, &contents, error))
    return false;
  Bytes pfx(contents.begin(), contents.end());
  return ImportPfxIdentity(pfx, password, identity, error);
}

bool GenerateCsr(std::string_view private_key_pem, const CsrOptions& options, std::string* csr_pem,
                 std::string* error) {
  if (!csr_pem) {
    SetError(error, "CSR output is null");
    return false;
  }
  if (options.common_name.empty()) {
    SetError(error, "a CSR common name (CN) is required");
    return false;
  }

  internal::PkeyPtr private_key = internal::LoadPrivateKeyPem(private_key_pem, error);
  if (!private_key)
    return false;
  if (EVP_PKEY_is_a(private_key.get(), "SM2") != 1) {
    SetError(error, "the CSR signing key must be an SM2 key");
    return false;
  }

  internal::X509ReqPtr request(X509_REQ_new());
  if (!request) {
    SetError(error, "cannot allocate a certificate request");
    return false;
  }
  if (X509_REQ_set_version(request.get(), 0) != 1) {
    SetError(error, "cannot set the certificate request version");
    return false;
  }

  X509_NAME* subject = X509_REQ_get_subject_name(request.get());
  if (!subject) {
    SetError(error, "cannot access the certificate request subject");
    return false;
  }
  if (!AddSubjectEntry(subject, "C", options.country, error) ||
      !AddSubjectEntry(subject, "O", options.organization, error) ||
      !AddSubjectEntry(subject, "OU", options.organizational_unit, error) ||
      !AddSubjectEntry(subject, "CN", options.common_name, error)) {
    return false;
  }
  if (X509_REQ_set_pubkey(request.get(), private_key.get()) != 1) {
    SetError(error, "cannot attach the public key to the certificate request: " +
                        internal::OpenSslErrors());
    return false;
  }

  ExtensionStackPtr extensions(sk_X509_EXTENSION_new_null());
  if (!extensions) {
    SetError(error, "cannot allocate the certificate request extensions");
    return false;
  }

  std::string alternative_names;
  for (const std::string& dns : options.dns_sans) {
    if (dns.empty())
      continue;
    if (!SafeSanEntry(dns)) {
      SetError(error, "invalid DNS subjectAltName: " + dns);
      return false;
    }
    if (!alternative_names.empty())
      alternative_names += ",";
    alternative_names += "DNS:" + dns;
  }
  for (const std::string& address : options.ip_sans) {
    if (address.empty())
      continue;
    if (!SafeSanEntry(address)) {
      SetError(error, "invalid IP subjectAltName: " + address);
      return false;
    }
    if (!alternative_names.empty())
      alternative_names += ",";
    alternative_names += "IP:" + address;
  }
  if (!alternative_names.empty() && !AddExtension(extensions.get(), NID_subject_alt_name,
                                                  alternative_names.c_str(), false, error)) {
    return false;
  }
  if (!AddExtension(extensions.get(), NID_key_usage, "digitalSignature,keyEncipherment", true,
                    error) ||
      !AddExtension(extensions.get(), NID_basic_constraints, "CA:FALSE", true, error)) {
    return false;
  }

  if (X509_REQ_add_extensions(request.get(), extensions.get()) != 1) {
    SetError(error,
             "cannot attach extensions to the certificate request: " + internal::OpenSslErrors());
    return false;
  }

  if (X509_REQ_sign(request.get(), private_key.get(), EVP_sm3()) <= 0) {
    SetError(error, "cannot sign the certificate request: " + internal::OpenSslErrors());
    return false;
  }

  internal::BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_X509_REQ(bio.get(), request.get()) != 1) {
    SetError(error, "cannot encode the certificate request as PEM");
    ClearOpenSslErrors();
    return false;
  }
  *csr_pem = internal::BioToString(bio.get());
  if (csr_pem->empty()) {
    SetError(error, "the encoded certificate request is empty");
    return false;
  }
  return true;
}

bool DescribeCertificate(std::string_view certificate_pem, CertificateDetails* details,
                         std::string* error) {
  if (!details) {
    SetError(error, "certificate details output is null");
    return false;
  }
  internal::X509Ptr certificate = internal::LoadCertificatePem(certificate_pem, error);
  if (!certificate)
    return false;

  CertificateDetails result;
  X509_NAME* subject = X509_get_subject_name(certificate.get());
  X509_NAME* issuer = X509_get_issuer_name(certificate.get());
  result.subject = NameToString(subject);
  result.issuer = NameToString(issuer);
  result.serial_hex = SerialToHex(certificate.get());
  result.common_name = NameEntry(subject, NID_commonName);
  result.organization = NameEntry(subject, NID_organizationName);
  result.not_before = Asn1TimeToString(X509_get0_notBefore(certificate.get()));
  result.not_after = Asn1TimeToString(X509_get0_notAfter(certificate.get()));
  result.is_ca = X509_check_ca(certificate.get()) > 0;
  AppendSubjectAltNames(certificate.get(), &result);
  *details = std::move(result);
  return true;
}

bool CertificateFingerprintSm3(std::string_view certificate_pem, Bytes* digest,
                               std::string* error) {
  if (!digest) {
    SetError(error, "certificate fingerprint output is null");
    return false;
  }
  internal::X509Ptr certificate = internal::LoadCertificatePem(certificate_pem, error);
  if (!certificate)
    return false;
  const int length = i2d_X509(certificate.get(), nullptr);
  if (length <= 0) {
    SetError(error, "cannot encode the certificate as DER: " + internal::OpenSslErrors());
    return false;
  }
  Bytes der(static_cast<std::size_t>(length));
  unsigned char* cursor = der.data();
  if (i2d_X509(certificate.get(), &cursor) != length) {
    SetError(error, "cannot encode the certificate as DER: " + internal::OpenSslErrors());
    return false;
  }
  Bytes result = Sm3(der.data(), der.size());
  if (result.size() != kSm3DigestSize) {
    SetError(error, "cannot compute the certificate fingerprint");
    return false;
  }
  *digest = std::move(result);
  return true;
}

bool ValidateCertificateChain(std::string_view leaf_pem, std::string_view chain_pem,
                              std::string_view trusted_roots_pem, ChainValidationResult* result) {
  if (result)
    *result = ChainValidationResult{};
  std::string error;
  internal::X509Ptr leaf = internal::LoadCertificatePem(leaf_pem, &error);
  if (!leaf) {
    if (result)
      result->error = std::move(error);
    return false;
  }
  return ValidateChain(leaf.get(), chain_pem, trusted_roots_pem, result);
}

bool ValidateCertificateChainDer(const Bytes& leaf_der, std::string_view chain_pem,
                                 std::string_view trusted_roots_pem,
                                 ChainValidationResult* result) {
  if (result)
    *result = ChainValidationResult{};
  std::string error;
  internal::X509Ptr leaf = internal::LoadCertificateDer(leaf_der.data(), leaf_der.size(), &error);
  if (!leaf) {
    if (result)
      result->error = std::move(error);
    return false;
  }
  return ValidateChain(leaf.get(), chain_pem, trusted_roots_pem, result);
}

} // namespace uvnvr::gm
