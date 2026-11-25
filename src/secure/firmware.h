#pragma once

// Signed firmware packages for the upgrade entry.
//
// Layout (little-endian for the fixed-width integers):
//
//   offset 0   "UVFW"                     4 bytes magic
//   offset 4   format version             u8   (1)
//   offset 5   flags                      u8   (0)
//   offset 6   header length              u32
//   offset 10  signature length           u32
//   offset 14  header                     JSON, exactly as signed
//   ...        signature                  SM2/SM3 DER, over the header bytes
//   ...        payload                    firmware bytes
//
// The header records product, version, payloadSize, payloadSm3 and createdAt.
// Verification checks the payload size and SM3 against the header and then the
// SM2 signature over the untouched header bytes. Signing the header (rather
// than the payload) keeps verification streaming-friendly once a real writer
// exists, while the digest still binds the payload.
//
// Signing is done by the release process with a key that is not on the device.
// The device only ever imports the corresponding public key or certificate.

#include "crypto/sm.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::secure {

inline constexpr std::string_view kFirmwareMagic = "UVFW";
inline constexpr std::uint8_t kFirmwareFormatVersion = 1;
inline constexpr std::size_t kFirmwareFixedHeaderSize = 14;

struct FirmwarePackage {
  std::string product;
  std::string version;
  std::int64_t created_at = 0;
  std::string header_json; // exact bytes covered by the signature
  std::string payload_sm3; // lower-case hex, from the header
  gm::Bytes signature;
  gm::Bytes payload;
};

// Parses the container and validates its internal consistency: magic, format,
// lengths, header JSON, payload size and payload SM3. It does not check the
// signature; call VerifyFirmwarePackage for that.
bool ParseFirmwarePackage(const gm::Bytes& package, FirmwarePackage* out, std::string* error);
bool ReadFirmwarePackageFile(const std::string& path, FirmwarePackage* out, std::string* error);

// Verifies the SM2 signature over the header. `public_key_or_certificate_pem`
// may be either a PEM SubjectPublicKeyInfo or a PEM X.509 certificate. When
// `expected_product` is non-empty the package's product must match exactly.
bool VerifyFirmwarePackage(const FirmwarePackage& package,
                           std::string_view public_key_or_certificate_pem,
                           std::string_view expected_product, std::string* error);

// Parse + verify in one step; the convenience entry used by the HTTP upgrade
// handler.
bool VerifyFirmwarePackage(const gm::Bytes& package,
                           std::string_view public_key_or_certificate_pem,
                           std::string_view expected_product, FirmwarePackage* out,
                           std::string* error);

bool VerifyFirmwareFile(const std::string& path,
                        std::string_view public_key_or_certificate_pem,
                        std::string_view expected_product, FirmwarePackage* out, std::string* error);

// Builds a signed package. This is the release-side helper used by the tests
// and by the small `--sign-firmware` mode; devices never need the private key.
bool CreateFirmwarePackage(const gm::Bytes& payload, std::string_view product,
                           std::string_view version, std::string_view private_key_pem,
                           gm::Bytes* package, std::string* error);

} // namespace uvnvr::secure
