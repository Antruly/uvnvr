#include "secure/config_integrity.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace uvnvr::secure {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr)
    *error = std::move(message);
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return text;
}

std::string ManifestMessage(const ConfigManifest& manifest) {
  std::string message(kConfigIntegrityVersion);
  message += "\nalgorithm=sm3\nsize=";
  message += std::to_string(manifest.config_size);
  message += "\ndigest=";
  message += manifest.config_sm3;
  return message;
}

bool NormalizeDigest(std::string_view text, std::string* digest) {
  gm::Bytes bytes;
  if (!gm::FromHex(text, &bytes) || bytes.size() != gm::kSm3DigestSize)
    return false;
  *digest = gm::ToHex(bytes);
  return true;
}

bool ParseSize(std::string_view text, std::uint64_t* size) {
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    return false;
  *size = value;
  return true;
}

bool ParseManifestText(std::string_view text, const std::string& path, ConfigManifest* manifest,
                       std::string* error) {
  const std::string prefix = "configuration integrity manifest " + path + ": ";
  bool seen_header = false;
  bool have_algorithm = false;
  bool have_size = false;
  bool have_digest = false;
  bool have_signature = false;
  ConfigManifest parsed;

  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string_view raw =
        newline == std::string_view::npos ? text.substr(position)
                                          : text.substr(position, newline - position);
    position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;

    const std::string_view line = Trim(raw);
    if (line.empty())
      continue;

    if (!seen_header) {
      if (line != kConfigIntegrityVersion) {
        SetError(error, prefix + "missing or unsupported format header");
        return false;
      }
      seen_header = true;
      continue;
    }

    const std::size_t separator = line.find(' ');
    if (separator == std::string_view::npos || separator == 0) {
      SetError(error, prefix + "malformed line '" + std::string(line) + "'");
      return false;
    }
    const std::string_view key = line.substr(0, separator);
    const std::string_view value = Trim(line.substr(separator + 1));

    if (key == "algorithm") {
      if (have_algorithm) {
        SetError(error, prefix + "duplicate algorithm field");
        return false;
      }
      if (value != "sm3") {
        SetError(error, prefix + "unsupported digest algorithm '" + std::string(value) + "'");
        return false;
      }
      have_algorithm = true;
    } else if (key == "size") {
      if (have_size) {
        SetError(error, prefix + "duplicate size field");
        return false;
      }
      if (!ParseSize(value, &parsed.config_size)) {
        SetError(error, prefix + "size is not a decimal byte count");
        return false;
      }
      have_size = true;
    } else if (key == "digest") {
      if (have_digest) {
        SetError(error, prefix + "duplicate digest field");
        return false;
      }
      if (!NormalizeDigest(value, &parsed.config_sm3)) {
        SetError(error, prefix + "digest must be a 32-byte SM3 value in hex");
        return false;
      }
      have_digest = true;
    } else if (key == "signatureAlgorithm") {
      if (value != "sm2-sm3") {
        SetError(error, prefix + "unsupported signature algorithm '" + std::string(value) + "'");
        return false;
      }
    } else if (key == "signature") {
      if (have_signature) {
        SetError(error, prefix + "duplicate signature field");
        return false;
      }
      gm::Bytes signature;
      if (value.empty() || !gm::FromHex(value, &signature) || signature.empty()) {
        SetError(error, prefix + "signature is not valid hex");
        return false;
      }
      parsed.signature = std::move(signature);
      have_signature = true;
    }
    // Unknown fields are ignored so a newer generator can add metadata without
    // making older devices reject the manifest.
  }

  if (!seen_header) {
    SetError(error, prefix + "file is empty");
    return false;
  }
  if (!have_algorithm || !have_size || !have_digest) {
    SetError(error, prefix + "algorithm, size and digest are required");
    return false;
  }
  *manifest = std::move(parsed);
  return true;
}

bool ReadAll(const std::string& path, std::string* contents, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    SetError(error, "cannot open " + path);
    return false;
  }
  contents->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (input.bad()) {
    SetError(error, "cannot read " + path);
    return false;
  }
  return true;
}

} // namespace

bool ParseConfigIntegrityMode(std::string_view text, ConfigIntegrityMode* mode) {
  if (mode == nullptr)
    return false;
  if (text == "off")
    *mode = ConfigIntegrityMode::kOff;
  else if (text == "warn")
    *mode = ConfigIntegrityMode::kWarn;
  else if (text == "enforce")
    *mode = ConfigIntegrityMode::kEnforce;
  else
    return false;
  return true;
}

std::string_view ConfigIntegrityModeName(ConfigIntegrityMode mode) {
  switch (mode) {
  case ConfigIntegrityMode::kOff:
    return "off";
  case ConfigIntegrityMode::kWarn:
    return "warn";
  case ConfigIntegrityMode::kEnforce:
    return "enforce";
  }
  return "warn";
}

std::string DefaultConfigManifestPath(std::string_view config_path) {
  std::string path(config_path);
  path.append(kConfigManifestSuffix.data(), kConfigManifestSuffix.size());
  return path;
}

std::string ConfigManifestMessage(const ConfigManifest& manifest) {
  return ManifestMessage(manifest);
}

bool CreateConfigManifest(std::string_view config_contents, std::string_view private_key_pem,
                          ConfigManifest* manifest, std::string* error) {
  if (manifest == nullptr) {
    SetError(error, "configuration manifest output is null");
    return false;
  }
  ConfigManifest created;
  created.config_size = static_cast<std::uint64_t>(config_contents.size());
  created.config_sm3 = gm::Sm3Hex(config_contents);
  if (!private_key_pem.empty()) {
    std::string sign_error;
    if (!gm::Sm2Sign(private_key_pem, ManifestMessage(created), &created.signature,
                     &sign_error)) {
      SetError(error, sign_error.empty() ? "cannot sign the configuration manifest with SM2"
                                         : sign_error);
      return false;
    }
  }
  *manifest = std::move(created);
  return true;
}

bool ReadConfigManifest(const std::string& path, ConfigManifest* manifest, std::string* error) {
  if (manifest == nullptr) {
    SetError(error, "configuration manifest output is null");
    return false;
  }
  std::string text;
  if (!ReadAll(path, &text, error))
    return false;
  return ParseManifestText(text, path, manifest, error);
}

bool WriteConfigManifest(const std::string& path, const ConfigManifest& manifest,
                         std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    SetError(error, "cannot open configuration integrity manifest for writing: " + path);
    return false;
  }
  output << kConfigIntegrityVersion << "\n"
         << "algorithm sm3\n"
         << "size " << manifest.config_size << "\n"
         << "digest " << manifest.config_sm3 << "\n";
  if (!manifest.signature.empty())
    output << "signatureAlgorithm sm2-sm3\n"
           << "signature " << gm::ToHex(manifest.signature) << "\n";
  output.flush();
  if (!output.good()) {
    SetError(error, "cannot write configuration integrity manifest: " + path);
    return false;
  }
  return true;
}

bool WriteConfigManifestForContents(std::string_view config_contents,
                                    const std::string& manifest_path,
                                    std::string_view private_key_pem, std::string* error) {
  ConfigManifest manifest;
  if (!CreateConfigManifest(config_contents, private_key_pem, &manifest, error))
    return false;
  return WriteConfigManifest(manifest_path, manifest, error);
}

bool VerifyConfigContents(std::string_view config_contents, const ConfigManifest& manifest,
                          std::string_view public_key_or_certificate_pem,
                          ConfigIntegrityResult* result, std::string* error) {
  if (result == nullptr) {
    SetError(error, "configuration integrity result output is null");
    return false;
  }

  ConfigIntegrityResult verification;
  verification.checked = true;
  verification.signed_manifest = !manifest.signature.empty();
  verification.config_size = static_cast<std::uint64_t>(config_contents.size());
  verification.expected_sm3 = manifest.config_sm3;
  verification.actual_sm3 = gm::Sm3Hex(config_contents);

  if (!manifest.signature.empty()) {
    if (public_key_or_certificate_pem.empty()) {
      *result = verification;
      SetError(error,
               "the configuration manifest is signed with SM2 but no public key was provided "
               "(use --config-public-key or UVNVR_CONFIG_PUBLIC_KEY)");
      return false;
    }
    std::string verify_error;
    const std::string message = ManifestMessage(manifest);
    const bool signature_ok =
        public_key_or_certificate_pem.find("BEGIN CERTIFICATE") != std::string_view::npos
            ? gm::Sm2VerifyCertificate(public_key_or_certificate_pem, message, manifest.signature,
                                       &verify_error)
            : gm::Sm2Verify(public_key_or_certificate_pem, message, manifest.signature,
                            &verify_error);
    if (!signature_ok) {
      verification.ok = false;
      verification.message = "SM2 signature verification failed";
      if (!verify_error.empty())
        verification.message += ": " + verify_error;
      *result = verification;
      return true;
    }
  } else if (!public_key_or_certificate_pem.empty()) {
    // Without a signature the digest could have been rewritten together with
    // the file, so a signed deployment must not fall back to it silently.
    verification.ok = false;
    verification.message =
        "a public key was configured but the manifest is not signed; refusing digest-only "
        "verification";
    *result = verification;
    return true;
  }

  if (verification.config_size != manifest.config_size ||
      verification.actual_sm3 != manifest.config_sm3) {
    verification.ok = false;
    verification.message =
        "configuration changed since the manifest was written (size or SM3 mismatch)";
  } else {
    verification.ok = true;
    verification.message = verification.signed_manifest
                               ? "configuration matches its SM2-signed SM3 manifest"
                               : "configuration matches its SM3 manifest";
  }
  *result = verification;
  return true;
}

bool VerifyConfigText(std::string_view config_contents, const std::string& manifest_path,
                      std::string_view public_key_or_certificate_pem,
                      ConfigIntegrityResult* result, std::string* error) {
  ConfigManifest manifest;
  std::string manifest_error;
  if (!ReadConfigManifest(manifest_path, &manifest, &manifest_error)) {
    if (result != nullptr)
      *result = ConfigIntegrityResult{};
    SetError(error, "cannot verify the configuration integrity manifest: " + manifest_error +
                        " (create or refresh it with --write-config-digest or --sign-config)");
    return false;
  }
  return VerifyConfigContents(config_contents, manifest, public_key_or_certificate_pem, result,
                              error);
}

bool VerifyConfigFile(const std::string& config_path, const std::string& manifest_path,
                      std::string_view public_key_or_certificate_pem,
                      ConfigIntegrityResult* result, std::string* error) {
  std::string contents;
  if (!ReadAll(config_path, &contents, error))
    return false;
  return VerifyConfigText(contents, manifest_path, public_key_or_certificate_pem, result, error);
}

} // namespace uvnvr::secure
