#pragma once

// Device identity handling for GB 35114: importing SM2 private keys with their
// X.509 certificate (PEM or PKCS#12), generating a certificate request with
// Subject Alternative Names, and validating a certificate against a CA chain.
//
// The module does not create trust anchors or self-sign pretending to be a CA;
// the certificate must come from an accredited CA.

#include "crypto/sm.h"

#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gm {

// A usable device identity: the private key, its leaf certificate, and the
// intermediate certificates that chain the leaf to the trust anchor.
struct DeviceIdentity {
  std::string private_key_pem;
  std::string certificate_pem;
  std::string chain_pem;
};

struct CsrOptions {
  std::string common_name;
  std::string organization;
  std::string organizational_unit;
  std::string country = "CN";
  std::vector<std::string> dns_sans;
  std::vector<std::string> ip_sans;
};

struct CertificateDetails {
  std::string subject;      // RFC 2253 form
  std::string issuer;       // RFC 2253 form
  std::string serial_hex;   // lower-case, no leading zeroes
  std::string common_name;  // first CN
  std::string organization; // first O
  std::string not_before;   // YYYYMMDDHHMMSSZ
  std::string not_after;    // YYYYMMDDHHMMSSZ
  bool is_ca = false;
  std::vector<std::string> dns_sans;
  std::vector<std::string> ip_sans;
};

struct ChainValidationResult {
  bool ok = false;
  int verified_chain_length = 0;
  std::string error;
};

// Imports a PEM private key and certificate, checks that they belong together,
// and normalises the key to unencrypted PKCS#8. `chain_pem` may be empty.
bool ImportPemIdentity(std::string_view private_key_pem, std::string_view certificate_pem,
                       std::string_view chain_pem, DeviceIdentity* identity,
                       std::string* error = nullptr);

// Same, reading each PEM from disk. An empty `chain_path` means "no chain".
bool ImportPemIdentityFromFiles(const std::string& private_key_path,
                                const std::string& certificate_path, const std::string& chain_path,
                                DeviceIdentity* identity, std::string* error = nullptr);

// Imports a PKCS#12 (.pfx/.p12) container. The private key and certificate must
// both be present; any extra certificates become the chain.
bool ImportPfxIdentity(const Bytes& pfx, std::string_view password, DeviceIdentity* identity,
                       std::string* error = nullptr);

bool ImportPfxIdentityFromFile(const std::string& pfx_path, std::string_view password,
                               DeviceIdentity* identity, std::string* error = nullptr);

// Builds a PKCS#10 certificate request signed with SM2/SM3. The request carries
// the configured subject, a subjectAltName extension, key usage and basic
// constraints. `options.common_name` is required.
bool GenerateCsr(std::string_view private_key_pem, const CsrOptions& options, std::string* csr_pem,
                 std::string* error = nullptr);

bool DescribeCertificate(std::string_view certificate_pem, CertificateDetails* details,
                         std::string* error = nullptr);

// SM3 digest of the certificate's DER encoding. This is the identity
// fingerprint exchanged during GB 35114 negotiation so the platform can pick
// the enrolled device certificate without sending the whole certificate.
bool CertificateFingerprintSm3(std::string_view certificate_pem, Bytes* digest,
                               std::string* error = nullptr);

// Validates `leaf` against `trusted_roots`, using `chain_pem` as untrusted
// intermediates. The result reports the verified chain length and, on failure,
// the OpenSSL verification error. Returns true only when the chain is valid.
bool ValidateCertificateChain(std::string_view leaf_pem, std::string_view chain_pem,
                              std::string_view trusted_roots_pem, ChainValidationResult* result);
bool ValidateCertificateChainDer(const Bytes& leaf_der, std::string_view chain_pem,
                                 std::string_view trusted_roots_pem, ChainValidationResult* result);

} // namespace uvnvr::gm
