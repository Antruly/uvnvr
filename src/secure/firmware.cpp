#include "secure/firmware.h"

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace uvnvr::secure {
namespace {

using json = nlohmann::json;

void SetError(std::string* error, std::string message) {
  if (error != nullptr)
    *error = std::move(message);
}

std::uint32_t ReadU32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

void AppendU32(gm::Bytes* out, std::uint32_t value) {
  out->push_back(static_cast<unsigned char>(value & 0xFF));
  out->push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
  out->push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
  out->push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
}

bool ReadFile(const std::string& path, gm::Bytes* data, std::string* error) {
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
  const std::string contents = buffer.str();
  data->assign(contents.begin(), contents.end());
  return true;
}

bool VerifySignature(std::string_view signer_pem, std::string_view header_json,
                     const gm::Bytes& signature, std::string* error) {
  if (signer_pem.find("BEGIN CERTIFICATE") != std::string_view::npos) {
    return gm::Sm2VerifyCertificate(signer_pem, header_json, signature, error);
  }
  return gm::Sm2Verify(signer_pem, header_json, signature, error);
}

bool ValidatePayload(const FirmwarePackage& package, std::string* error) {
  const std::string digest = gm::ToHex(gm::Sm3(package.payload.data(), package.payload.size()));
  if (digest != package.payload_sm3) {
    SetError(error, "payload SM3 does not match the signed header (package is corrupted or forged)");
    return false;
  }
  return true;
}

} // namespace

bool ParseFirmwarePackage(const gm::Bytes& package, FirmwarePackage* out, std::string* error) {
  if (out == nullptr) {
    SetError(error, "firmware package output is null");
    return false;
  }
  if (package.size() < kFirmwareFixedHeaderSize) {
    SetError(error, "firmware package is shorter than its fixed header");
    return false;
  }
  if (std::string_view(reinterpret_cast<const char*>(package.data()), 4) != kFirmwareMagic) {
    SetError(error, "not a UvNVR firmware package (bad magic)");
    return false;
  }
  if (package[4] != kFirmwareFormatVersion) {
    SetError(error, "unsupported firmware package version " + std::to_string(package[4]));
    return false;
  }
  if (package[5] != 0) {
    SetError(error, "unsupported firmware package flags");
    return false;
  }

  const std::uint32_t header_length = ReadU32(package.data() + 6);
  const std::uint32_t signature_length = ReadU32(package.data() + 10);
  const std::uint64_t body_start =
      static_cast<std::uint64_t>(kFirmwareFixedHeaderSize) + header_length + signature_length;
  if (header_length == 0 || signature_length == 0 || body_start > package.size()) {
    SetError(error, "firmware package lengths are inconsistent");
    return false;
  }

  FirmwarePackage parsed;
  parsed.header_json.assign(reinterpret_cast<const char*>(package.data() +
                                                          kFirmwareFixedHeaderSize),
                            header_length);
  parsed.signature.assign(package.begin() + kFirmwareFixedHeaderSize + header_length,
                          package.begin() + kFirmwareFixedHeaderSize + header_length +
                              signature_length);
  parsed.payload.assign(package.begin() + static_cast<std::ptrdiff_t>(body_start),
                        package.end());

  json header = json::parse(parsed.header_json, nullptr, false);
  if (header.is_discarded() || !header.is_object()) {
    SetError(error, "firmware package header is not valid JSON");
    return false;
  }
  if (!header.contains("product") || !header["product"].is_string() ||
      !header.contains("version") || !header["version"].is_string() ||
      !header.contains("payloadSize") || !header["payloadSize"].is_number_integer() ||
      !header.contains("payloadSm3") || !header["payloadSm3"].is_string()) {
    SetError(error, "firmware package header is missing required fields");
    return false;
  }
  parsed.product = header["product"].get<std::string>();
  parsed.version = header["version"].get<std::string>();
  parsed.payload_sm3 = header["payloadSm3"].get<std::string>();
  if (header.contains("createdAt") && header["createdAt"].is_number_integer())
    parsed.created_at = header["createdAt"].get<std::int64_t>();

  const std::int64_t declared_size = header["payloadSize"].get<std::int64_t>();
  if (declared_size < 0 || static_cast<std::uint64_t>(declared_size) != parsed.payload.size()) {
    SetError(error, "firmware payload size does not match the signed header");
    return false;
  }
  if (parsed.product.empty() || parsed.version.empty() || parsed.payload_sm3.empty()) {
    SetError(error, "firmware package header fields must not be empty");
    return false;
  }
  if (!ValidatePayload(parsed, error))
    return false;

  *out = std::move(parsed);
  return true;
}

bool ReadFirmwarePackageFile(const std::string& path, FirmwarePackage* out, std::string* error) {
  gm::Bytes data;
  if (!ReadFile(path, &data, error))
    return false;
  return ParseFirmwarePackage(data, out, error);
}

bool VerifyFirmwarePackage(const FirmwarePackage& package,
                           std::string_view public_key_or_certificate_pem,
                           std::string_view expected_product, std::string* error) {
  if (!expected_product.empty() && package.product != expected_product) {
    SetError(error, "firmware package is for product '" + package.product + "', expected '" +
                        std::string(expected_product) + "'");
    return false;
  }
  if (package.header_json.empty() || package.signature.empty()) {
    SetError(error, "firmware package has no header or signature");
    return false;
  }
  if (!ValidatePayload(package, error))
    return false;
  if (!VerifySignature(public_key_or_certificate_pem, package.header_json, package.signature,
                       error)) {
    if (error != nullptr && error->empty())
      *error = "firmware signature verification failed";
    return false;
  }
  return true;
}

bool VerifyFirmwarePackage(const gm::Bytes& package,
                           std::string_view public_key_or_certificate_pem,
                           std::string_view expected_product, FirmwarePackage* out,
                           std::string* error) {
  FirmwarePackage parsed;
  if (!ParseFirmwarePackage(package, &parsed, error))
    return false;
  if (!VerifyFirmwarePackage(parsed, public_key_or_certificate_pem, expected_product, error))
    return false;
  if (out != nullptr)
    *out = std::move(parsed);
  return true;
}

bool VerifyFirmwareFile(const std::string& path,
                        std::string_view public_key_or_certificate_pem,
                        std::string_view expected_product, FirmwarePackage* out,
                        std::string* error) {
  gm::Bytes package;
  if (!ReadFile(path, &package, error))
    return false;
  return VerifyFirmwarePackage(package, public_key_or_certificate_pem, expected_product, out,
                               error);
}

bool CreateFirmwarePackage(const gm::Bytes& payload, std::string_view product,
                           std::string_view version, std::string_view private_key_pem,
                           gm::Bytes* package, std::string* error) {
  if (package == nullptr) {
    SetError(error, "firmware package output is null");
    return false;
  }
  if (product.empty() || version.empty()) {
    SetError(error, "firmware product and version are required");
    return false;
  }
  const std::string payload_sm3 = gm::ToHex(gm::Sm3(payload.data(), payload.size()));
  json header{
      {"product", std::string(product)},
      {"version", std::string(version)},
      {"payloadSize", payload.size()},
      {"payloadSm3", payload_sm3},
      {"createdAt",
       std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count()},
  };
  const std::string header_json = header.dump();

  gm::Bytes signature;
  if (!gm::Sm2Sign(private_key_pem, header_json, &signature, error))
    return false;

  gm::Bytes result;
  result.reserve(kFirmwareFixedHeaderSize + header_json.size() + signature.size() + payload.size());
  result.insert(result.end(), kFirmwareMagic.begin(), kFirmwareMagic.end());
  result.push_back(kFirmwareFormatVersion);
  result.push_back(0);
  AppendU32(&result, static_cast<std::uint32_t>(header_json.size()));
  AppendU32(&result, static_cast<std::uint32_t>(signature.size()));
  result.insert(result.end(), header_json.begin(), header_json.end());
  result.insert(result.end(), signature.begin(), signature.end());
  result.insert(result.end(), payload.begin(), payload.end());
  *package = std::move(result);
  return true;
}

} // namespace uvnvr::secure
