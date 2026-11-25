#include "secure/storage_crypto.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <openssl/rand.h>
#include <sstream>
#include <utility>

namespace uvnvr::secure {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr)
    *error = std::move(message);
}

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

bool WriteFile(const std::string& path, const gm::Bytes& data, std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    SetError(error, "cannot create " + path);
    return false;
  }
  if (!data.empty())
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
  if (!output.good()) {
    SetError(error, "cannot write " + path);
    return false;
  }
  return true;
}

bool CheckKey(const gm::Bytes& key, std::string* error) {
  if (key.size() != gm::kSm4KeySize) {
    SetError(error, "the storage key must be 16 bytes");
    return false;
  }
  return true;
}

gm::Bytes BytesFromText(std::string_view text) {
  return gm::Bytes(text.begin(), text.end());
}

gm::Bytes DeriveFileKey(const gm::Bytes& master_key, const gm::Bytes& salt) {
  gm::Bytes material = BytesFromText("uvnvr-storage-file-v1");
  material.insert(material.end(), master_key.begin(), master_key.end());
  material.insert(material.end(), salt.begin(), salt.end());
  gm::Bytes digest = gm::Sm3(material.data(), material.size());
  digest.resize(gm::kSm4KeySize);
  return digest;
}

bool RandomBytes(gm::Bytes* out, std::size_t size, const char* what, std::string* error) {
  out->assign(size, 0);
  if (RAND_bytes(out->data(), static_cast<int>(size)) != 1) {
    SetError(error, std::string("cannot generate the random ") + what);
    return false;
  }
  return true;
}

} // namespace

bool LoadDeviceCertificate(const std::string& path, std::string* certificate_pem,
                           std::string* error) {
  if (certificate_pem == nullptr) {
    SetError(error, "certificate output is null");
    return false;
  }
  std::string contents;
  if (!ReadFile(path, &contents, error))
    return false;
  if (contents.find("BEGIN CERTIFICATE") == std::string::npos) {
    SetError(error, path + " does not contain a PEM certificate");
    return false;
  }
  *certificate_pem = std::move(contents);
  return true;
}

bool DeriveStorageKey(std::string_view certificate_pem, gm::Bytes* key, std::string* error) {
  if (key == nullptr) {
    SetError(error, "storage key output is null");
    return false;
  }
  if (certificate_pem.find("BEGIN CERTIFICATE") == std::string_view::npos) {
    SetError(error, "the device identity must be a PEM certificate");
    return false;
  }
  gm::Bytes material = BytesFromText("uvnvr-storage-key-v1\n");
  material.insert(material.end(), certificate_pem.begin(), certificate_pem.end());
  gm::Bytes digest = gm::Sm3(material.data(), material.size());
  digest.resize(gm::kSm4KeySize);
  *key = std::move(digest);
  return true;
}

bool DeriveStorageKeyFromFile(const std::string& certificate_path, gm::Bytes* key,
                              std::string* error) {
  std::string certificate;
  if (!LoadDeviceCertificate(certificate_path, &certificate, error))
    return false;
  return DeriveStorageKey(certificate, key, error);
}

bool IsSealedBlob(const gm::Bytes& data) {
  if (data.size() < kSealedHeaderSize)
    return false;
  return std::memcmp(data.data(), kSealedMagic.data(), kSealedMagic.size()) == 0;
}

bool SealBlob(const gm::Bytes& plaintext, const gm::Bytes& key, gm::Bytes* sealed,
              std::string* error) {
  if (sealed == nullptr) {
    SetError(error, "sealed output is null");
    return false;
  }
  if (!CheckKey(key, error))
    return false;

  gm::Bytes salt;
  gm::Bytes iv;
  if (!RandomBytes(&salt, kSealedSaltSize, "salt", error))
    return false;
  if (!RandomBytes(&iv, kSealedIvSize, "IV", error))
    return false;

  const gm::Bytes file_key = DeriveFileKey(key, salt);
  gm::Bytes ciphertext;
  if (!gm::Sm4CtrCrypt(file_key, iv, 0, plaintext, &ciphertext, error))
    return false;
  const gm::Bytes digest = gm::Sm3(plaintext.data(), plaintext.size());

  gm::Bytes result;
  result.reserve(kSealedHeaderSize + ciphertext.size());
  result.insert(result.end(), kSealedMagic.begin(), kSealedMagic.end());
  result.push_back(kSealedVersion);
  result.push_back(kSealedAlgorithmSm4Ctr);
  result.push_back(0);
  result.push_back(0);
  result.insert(result.end(), salt.begin(), salt.end());
  result.insert(result.end(), iv.begin(), iv.end());
  result.insert(result.end(), digest.begin(), digest.end());
  result.insert(result.end(), ciphertext.begin(), ciphertext.end());
  *sealed = std::move(result);
  return true;
}

bool OpenBlob(const gm::Bytes& sealed, const gm::Bytes& key, gm::Bytes* plaintext,
              std::string* error) {
  if (plaintext == nullptr) {
    SetError(error, "plaintext output is null");
    return false;
  }
  if (!CheckKey(key, error))
    return false;
  if (!IsSealedBlob(sealed)) {
    SetError(error, "the data is not a sealed UvNVR blob");
    return false;
  }
  if (sealed[4] != kSealedVersion) {
    SetError(error, "unsupported sealed blob version " + std::to_string(sealed[4]));
    return false;
  }
  if (sealed[5] != kSealedAlgorithmSm4Ctr) {
    SetError(error, "unsupported sealed blob algorithm");
    return false;
  }

  gm::Bytes salt(sealed.begin() + 8, sealed.begin() + 8 + kSealedSaltSize);
  gm::Bytes iv(sealed.begin() + 8 + kSealedSaltSize,
               sealed.begin() + 8 + kSealedSaltSize + kSealedIvSize);
  const gm::Bytes stored_digest(
      sealed.begin() + 8 + kSealedSaltSize + kSealedIvSize,
      sealed.begin() + kSealedHeaderSize);
  gm::Bytes ciphertext(sealed.begin() + kSealedHeaderSize, sealed.end());

  const gm::Bytes file_key = DeriveFileKey(key, salt);
  gm::Bytes result;
  if (!gm::Sm4CtrCrypt(file_key, iv, 0, ciphertext, &result, error))
    return false;
  const gm::Bytes digest = gm::Sm3(result.data(), result.size());
  if (digest != stored_digest) {
    SetError(error, "the sealed blob failed its SM3 integrity check (wrong key or corrupted data)");
    return false;
  }
  *plaintext = std::move(result);
  return true;
}

bool SealFile(const std::string& path, const gm::Bytes& plaintext, const gm::Bytes& key,
              std::string* error) {
  gm::Bytes sealed;
  if (!SealBlob(plaintext, key, &sealed, error))
    return false;
  return WriteFile(path, sealed, error);
}

bool OpenSealedFile(const std::string& path, const gm::Bytes& key, gm::Bytes* plaintext,
                    std::string* error) {
  gm::Bytes sealed;
  std::string contents;
  if (!ReadFile(path, &contents, error))
    return false;
  sealed.assign(contents.begin(), contents.end());
  return OpenBlob(sealed, key, plaintext, error);
}

bool IsEncryptedRecordingPath(std::string_view path) {
  return path.size() >= kEncryptedRecordingSuffix.size() &&
         path.substr(path.size() - kEncryptedRecordingSuffix.size()) ==
             kEncryptedRecordingSuffix;
}

gm::Bytes DeriveRecordingIv(const gm::Bytes& key, std::string_view final_path) {
  gm::Bytes material = BytesFromText("uvnvr-recording-iv-v1\n");
  material.insert(material.end(), key.begin(), key.end());
  material.insert(material.end(), final_path.begin(), final_path.end());
  gm::Bytes digest = gm::Sm3(material.data(), material.size());
  digest.resize(gm::kSm4IvSize);
  return digest;
}

bool EncryptRecordingFile(const std::string& plain_path, const std::string& final_path,
                          const gm::Bytes& key, std::string* error) {
  if (!CheckKey(key, error))
    return false;
  std::ifstream input(plain_path, std::ios::binary);
  if (!input) {
    SetError(error, "cannot open " + plain_path);
    return false;
  }
  std::ofstream output(final_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    SetError(error, "cannot create " + final_path);
    return false;
  }

  const gm::Bytes iv = DeriveRecordingIv(key, final_path);
  gm::Bytes buffer(256 * 1024);
  std::uint64_t offset = 0;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got <= 0)
      break;
    gm::Bytes plaintext(buffer.begin(), buffer.begin() + got);
    gm::Bytes ciphertext;
    if (!gm::Sm4CtrCrypt(key, iv, offset, plaintext, &ciphertext, error)) {
      output.close();
      std::error_code ec;
      std::filesystem::remove(final_path, ec);
      return false;
    }
    output.write(reinterpret_cast<const char*>(ciphertext.data()),
                 static_cast<std::streamsize>(ciphertext.size()));
    if (!output.good()) {
      SetError(error, "cannot write " + final_path);
      output.close();
      std::error_code ec;
      std::filesystem::remove(final_path, ec);
      return false;
    }
    offset += static_cast<std::uint64_t>(got);
  }
  if (input.bad()) {
    SetError(error, "cannot read " + plain_path);
    output.close();
    std::error_code ec;
    std::filesystem::remove(final_path, ec);
    return false;
  }
  output.close();
  if (!output.good()) {
    SetError(error, "cannot finalize " + final_path);
    std::error_code ec;
    std::filesystem::remove(final_path, ec);
    return false;
  }
  return true;
}

bool DecryptRecordingRange(const gm::Bytes& key, std::string_view final_path, std::uint64_t offset,
                           const gm::Bytes& ciphertext, gm::Bytes* plaintext,
                           std::string* error) {
  if (!CheckKey(key, error))
    return false;
  const gm::Bytes iv = DeriveRecordingIv(key, final_path);
  return gm::Sm4CtrCrypt(key, iv, offset, ciphertext, plaintext, error);
}

bool ReadDecryptedRecordingRange(const gm::Bytes& key, const std::string& final_path,
                                 std::uint64_t offset, std::size_t length, gm::Bytes* plaintext,
                                 std::string* error) {
  if (!CheckKey(key, error))
    return false;
  std::ifstream input(final_path, std::ios::binary);
  if (!input) {
    SetError(error, "cannot open " + final_path);
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset));
  gm::Bytes ciphertext(length);
  input.read(reinterpret_cast<char*>(ciphertext.data()), static_cast<std::streamsize>(length));
  const std::streamsize got = input.gcount();
  if (got < 0) {
    SetError(error, "cannot read " + final_path);
    return false;
  }
  ciphertext.resize(static_cast<std::size_t>(got));
  return DecryptRecordingRange(key, final_path, offset, ciphertext, plaintext, error);
}

} // namespace uvnvr::secure
