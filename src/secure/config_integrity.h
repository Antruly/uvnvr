#pragma once

// Integrity verification for the startup configuration file.
//
// A sidecar manifest records the size and SM3 digest of the exact bytes of the
// configuration file, optionally with an SM2 signature over that metadata. At
// startup the file is hashed and compared; a mismatch means the file was
// edited after the manifest was written. A signed manifest additionally proves
// the digest came from the holder of the signing key, so an attacker who can
// write both files still cannot produce a manifest the device accepts.
//
// Manifest format (UTF-8, one "key value" per line; `signature` is optional):
//
//   uvnvr-config-integrity-v1
//   algorithm sm3
//   size <decimal byte count>
//   digest <64 lower-case hex chars>
//   signatureAlgorithm sm2-sm3
//   signature <DER signature, hex encoded>
//
// The signed message is the version line plus algorithm, size and digest in
// the order above, joined with '\n' (see ConfigManifestMessage). The manifest
// is generated out of band with `uvnvr --write-config-digest` (digest only) or
// `uvnvr --sign-config <private-key.pem>` (SM2 signed); the server only ever
// needs the public key or certificate.

#include "crypto/sm.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace uvnvr::secure {

inline constexpr std::string_view kConfigIntegrityVersion = "uvnvr-config-integrity-v1";
inline constexpr std::string_view kConfigManifestSuffix = ".sm3";

// What startup does when the check fails: nothing, warn and continue, or refuse
// to start. When the module is compiled in the default is kWarn.
enum class ConfigIntegrityMode { kOff, kWarn, kEnforce };

struct ConfigManifest {
  std::uint64_t config_size = 0;
  std::string config_sm3; // lower-case hex
  gm::Bytes signature;    // empty for a digest-only manifest
};

struct ConfigIntegrityResult {
  bool checked = false;        // the manifest was parsed and the file hashed
  bool ok = false;             // size and digest match, and signature if present
  bool signed_manifest = false;
  std::uint64_t config_size = 0;
  std::string expected_sm3;    // from the manifest
  std::string actual_sm3;      // computed from the file
  std::string message;         // human-readable outcome
};

bool ParseConfigIntegrityMode(std::string_view text, ConfigIntegrityMode* mode);
std::string_view ConfigIntegrityModeName(ConfigIntegrityMode mode);

// `<config path> + ".sm3"`.
std::string DefaultConfigManifestPath(std::string_view config_path);

// Deterministic message covered by the SM2 signature. Exposed so tooling and
// tests sign exactly what the verifier checks.
std::string ConfigManifestMessage(const ConfigManifest& manifest);

// Hashes `config_contents` and, when `private_key_pem` is non-empty, signs the
// manifest. An empty key produces a digest-only manifest.
bool CreateConfigManifest(std::string_view config_contents, std::string_view private_key_pem,
                          ConfigManifest* manifest, std::string* error);

bool ReadConfigManifest(const std::string& path, ConfigManifest* manifest, std::string* error);
bool WriteConfigManifest(const std::string& path, const ConfigManifest& manifest,
                         std::string* error);

// Create + write in one step. `private_key_pem` may be empty.
bool WriteConfigManifestForContents(std::string_view config_contents,
                                    const std::string& manifest_path,
                                    std::string_view private_key_pem, std::string* error);

// Verifies config bytes against a parsed manifest. Returns false when the
// verification could not be performed (for example an SM2-signed manifest with
// no public key); `result` is still filled in that case. Returns true with
// result.ok == false for an integrity mismatch.
//
// `public_key_or_certificate_pem` may be a PEM SubjectPublicKeyInfo or a PEM
// X.509 certificate. When it is provided, an unsigned manifest is rejected so a
// signed deployment cannot be downgraded to digest-only verification.
bool VerifyConfigContents(std::string_view config_contents, const ConfigManifest& manifest,
                          std::string_view public_key_or_certificate_pem,
                          ConfigIntegrityResult* result, std::string* error);

// Reads the manifest, then verifies `config_contents`.
bool VerifyConfigText(std::string_view config_contents, const std::string& manifest_path,
                      std::string_view public_key_or_certificate_pem,
                      ConfigIntegrityResult* result, std::string* error);

// Reads the configuration file, then calls VerifyConfigText.
bool VerifyConfigFile(const std::string& config_path, const std::string& manifest_path,
                      std::string_view public_key_or_certificate_pem,
                      ConfigIntegrityResult* result, std::string* error);

} // namespace uvnvr::secure
