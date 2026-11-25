// Platform-side helper for the GB 35114 B-level video encryption module.
//
// The device side (when the GB/T 28181 PS/RTP path lands) calls the same
// library functions between ES extraction and PS muxing. This tool exists so
// the key exchange, encryption and decryption can be exercised without a live
// SIP/PS session, and so the platform-side decryption path has a reference
// implementation.
//
//   gb35114_cli wrap <device-public-key-or-cert.pem> <stream-id> <key-id>
//                    <envelope-out> <platform-key-out>
//   gb35114_cli encrypt <device-private-key.pem> <envelope-in> <stream-id>
//                       <in.h264> <out.gbes>
//   gb35114_cli decrypt <platform-key-in> <stream-id> <in.gbes> <out.h264>
//   gb35114_cli info <in.gbes>
//
// The platform key file contains the VKEK and salt. It is written with mode
// 0600 and must be protected exactly like the video content.

#include "crypto/sm.h"
#include "gb35114/es_crypto.h"
#include "gb35114/vkek.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uvnvr::gm::Bytes;

constexpr std::string_view kPlatformKeyMagic = "GPK1";
constexpr std::uint8_t kPlatformKeyVersion = 1;
constexpr std::size_t kPlatformKeySize = 44;

void PrintUsage() {
  std::cerr << "usage:\n"
            << "  gb35114_cli wrap <device-public-key-or-cert.pem> <stream-id> <key-id> "
               "<envelope-out> <platform-key-out>\n"
            << "  gb35114_cli encrypt <device-private-key.pem> <envelope-in> <stream-id> "
               "<in.h264> <out.gbes>\n"
            << "  gb35114_cli decrypt <platform-key-in> <stream-id> <in.gbes> <out.h264>\n"
            << "  gb35114_cli info <in.gbes>\n";
}

bool ReadFile(const std::string& path, Bytes* data, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    *error = "cannot read " + path;
    return false;
  }
  return true;
}

bool ReadText(const std::string& path, std::string* data, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    *error = "cannot read " + path;
    return false;
  }
  return true;
}

bool WriteFile(const std::string& path, const Bytes& data, bool private_file, std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    *error = "cannot create " + path + ": " + std::strerror(errno);
    return false;
  }
  if (!data.empty())
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
  if (!output) {
    *error = "cannot write " + path;
    return false;
  }
  output.close();
  if (private_file) {
    std::error_code code;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, code);
    if (code) {
      *error = "cannot restrict permissions on " + path + ": " + code.message();
      return false;
    }
  }
  return true;
}

void WriteBe32(Bytes* out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    (*out)[offset + static_cast<std::size_t>(i)] =
        static_cast<unsigned char>((value >> (24 - 8 * i)) & 0xFF);
}

std::uint32_t ReadBe32(const Bytes& data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i)
    value = (value << 8) | data[offset + static_cast<std::size_t>(i)];
  return value;
}

bool ParseKeyId(const char* text, std::uint32_t* key_id) {
  if (!text || *text == '\0')
    return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value > 0xFFFFFFFFull)
    return false;
  *key_id = static_cast<std::uint32_t>(value);
  return true;
}

bool SerializePlatformKey(std::uint32_t key_id, const Bytes& salt, const Bytes& vkek, Bytes* out) {
  if (salt.size() != uvnvr::gb35114::kVkekSaltSize || vkek.size() != uvnvr::gb35114::kVkekSize) {
    return false;
  }
  out->assign(kPlatformKeySize, 0);
  std::memcpy(out->data(), kPlatformKeyMagic.data(), kPlatformKeyMagic.size());
  (*out)[4] = kPlatformKeyVersion;
  WriteBe32(out, 8, key_id);
  std::memcpy(out->data() + 12, salt.data(), salt.size());
  std::memcpy(out->data() + 28, vkek.data(), vkek.size());
  return true;
}

bool ParsePlatformKey(const Bytes& data, std::uint32_t* key_id, Bytes* salt, Bytes* vkek,
                      std::string* error) {
  if (data.size() != kPlatformKeySize ||
      std::memcmp(data.data(), kPlatformKeyMagic.data(), kPlatformKeyMagic.size()) != 0) {
    *error = "platform key file is not a GPK1 file";
    return false;
  }
  if (data[4] != kPlatformKeyVersion) {
    *error = "unsupported platform key version";
    return false;
  }
  *key_id = ReadBe32(data, 8);
  salt->assign(data.begin() + 12, data.begin() + 28);
  vkek->assign(data.begin() + 28, data.begin() + 44);
  return true;
}

int WrapCommand(int argc, char** argv) {
  if (argc != 7) {
    PrintUsage();
    return 1;
  }
  std::uint32_t key_id = 0;
  if (!ParseKeyId(argv[4], &key_id)) {
    std::cerr << "error: key-id must be an unsigned 32-bit decimal number\n";
    return 1;
  }
  std::string device_key;
  std::string error;
  if (!ReadText(argv[2], &device_key, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  Bytes envelope;
  Bytes vkek;
  Bytes salt;
  const bool is_certificate = device_key.find("BEGIN CERTIFICATE") != std::string::npos;
  const bool ok =
      is_certificate
          ? uvnvr::gb35114::CreateVkekEnvelopeForCertificate(device_key, key_id, &envelope, &vkek,
                                                             &salt, &error)
          : uvnvr::gb35114::CreateVkekEnvelope(device_key, key_id, &envelope, &vkek, &salt, &error);
  if (!ok) {
    std::cerr << "error: cannot wrap the VKEK: " << error << "\n";
    return 2;
  }

  Bytes platform_key;
  if (!SerializePlatformKey(key_id, salt, vkek, &platform_key)) {
    std::cerr << "error: cannot serialize the platform key\n";
    return 2;
  }
  if (!WriteFile(argv[5], envelope, false, &error) ||
      !WriteFile(argv[6], platform_key, true, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  std::cout << "wrapped VKEK key_id=" << key_id << " stream=" << argv[3]
            << " salt=" << uvnvr::gm::ToHex(salt) << " envelope=" << envelope.size()
            << " bytes platform_key=" << platform_key.size() << " bytes\n";
  return 0;
}

int EncryptCommand(int argc, char** argv) {
  if (argc != 7) {
    PrintUsage();
    return 1;
  }
  std::string device_key;
  std::string error;
  Bytes envelope;
  Bytes input;
  if (!ReadText(argv[2], &device_key, &error) || !ReadFile(argv[3], &envelope, &error) ||
      !ReadFile(argv[5], &input, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }
  if (input.empty()) {
    std::cerr << "error: input ES is empty\n";
    return 2;
  }

  Bytes vkek;
  uvnvr::gb35114::VkekEnvelope parsed;
  if (!uvnvr::gb35114::UnwrapVkek(device_key, envelope, &vkek, &parsed, &error)) {
    std::cerr << "error: cannot unwrap the VKEK: " << error << "\n";
    return 2;
  }
  uvnvr::gb35114::VideoKeys keys;
  if (!uvnvr::gb35114::DeriveVideoKeys(vkek, parsed, argv[4], &keys, &error)) {
    std::cerr << "error: cannot derive the video keys: " << error << "\n";
    return 2;
  }

  Bytes sealed;
  std::size_t frames = 0;
  if (!uvnvr::gb35114::SealEsStream(keys, input, &sealed, &frames, &error)) {
    std::cerr << "error: cannot encrypt the ES: " << error << "\n";
    return 2;
  }
  if (!WriteFile(argv[6], sealed, false, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  std::cout << "encrypted stream=" << argv[4] << " key_id=" << keys.key_id << " frames=" << frames
            << " es_bytes=" << input.size() << " sealed_bytes=" << sealed.size() << "\n";
  return 0;
}

int DecryptCommand(int argc, char** argv) {
  if (argc != 6) {
    PrintUsage();
    return 1;
  }
  std::string error;
  Bytes platform_key;
  Bytes sealed;
  if (!ReadFile(argv[2], &platform_key, &error) || !ReadFile(argv[4], &sealed, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  std::uint32_t key_id = 0;
  Bytes salt;
  Bytes vkek;
  if (!ParsePlatformKey(platform_key, &key_id, &salt, &vkek, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }
  uvnvr::gb35114::VideoKeys keys;
  if (!uvnvr::gb35114::DeriveVideoKeys(vkek, key_id, salt, argv[3], &keys, &error)) {
    std::cerr << "error: cannot derive the video keys: " << error << "\n";
    return 2;
  }

  Bytes plaintext;
  std::size_t frames = 0;
  if (!uvnvr::gb35114::OpenEsStream(keys, sealed, &plaintext, &frames, &error)) {
    std::cerr << "error: cannot decrypt the stream: " << error << "\n";
    return 2;
  }
  if (!WriteFile(argv[5], plaintext, false, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  std::cout << "decrypted stream=" << argv[3] << " key_id=" << key_id << " frames=" << frames
            << " es_bytes=" << plaintext.size() << "\n";
  return 0;
}

int InfoCommand(int argc, char** argv) {
  if (argc != 3) {
    PrintUsage();
    return 1;
  }
  std::string error;
  Bytes sealed;
  if (!ReadFile(argv[2], &sealed, &error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  std::size_t offset = 0;
  std::size_t frames = 0;
  std::size_t keyframes = 0;
  std::uint64_t plain_bytes = 0;
  std::uint64_t first_sequence = 0;
  std::uint64_t last_sequence = 0;
  std::set<std::uint32_t> key_ids;
  while (offset < sealed.size()) {
    std::size_t frame_size = 0;
    uvnvr::gb35114::SealedFrameInfo info;
    if (!uvnvr::gb35114::ReadSealedFrameInfo(sealed, offset, &info, &frame_size, &error)) {
      std::cerr << "error: frame " << frames << ": " << error << "\n";
      return 2;
    }
    if (frames == 0)
      first_sequence = info.sequence;
    last_sequence = info.sequence;
    if (info.keyframe)
      ++keyframes;
    key_ids.insert(info.key_id);
    plain_bytes += info.plain_size;
    ++frames;
    offset += frame_size;
  }

  std::cout << "frames=" << frames << " keyframes=" << keyframes << " plain_bytes=" << plain_bytes
            << " sealed_bytes=" << sealed.size() << " key_ids=";
  bool first = true;
  for (std::uint32_t key_id : key_ids) {
    if (!first)
      std::cout << ",";
    std::cout << key_id;
    first = false;
  }
  if (frames > 0)
    std::cout << " sequence=" << first_sequence << ".." << last_sequence;
  std::cout << "\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }
  const std::string_view command = argv[1];
  if (command == "wrap")
    return WrapCommand(argc, argv);
  if (command == "encrypt")
    return EncryptCommand(argc, argv);
  if (command == "decrypt")
    return DecryptCommand(argc, argv);
  if (command == "info")
    return InfoCommand(argc, argv);
  if (command == "help" || command == "--help" || command == "-h") {
    PrintUsage();
    return 0;
  }
  std::cerr << "error: unknown command '" << command << "'\n";
  PrintUsage();
  return 1;
}
