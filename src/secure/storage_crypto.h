#pragma once

// Data-at-rest encryption for snapshots and recordings.
//
// The SM4 key is derived from the provisioned device certificate, never stored
// next to the data. Two containers are used:
//
//   * Snapshots use a self-describing envelope: "UVSE" magic, version, random
//     salt and IV, the SM3 of the plaintext, then SM4-CTR ciphertext. The
//     envelope keeps the original JPEG intact in memory and lets a reader tell
//     an encrypted snapshot from a legacy plaintext one.
//   * Recordings are encrypted in place with SM4-CTR so the ciphertext has the
//     same length as the TS stream and playback can decrypt any byte range
//     without reading the whole segment. The per-file IV is derived from the
//     key and the final path; the ".enc" suffix marks the file. There is no
//     envelope header, which is what keeps plaintext and ciphertext offsets
//     identical for the existing seek logic.
//
// Neither container provides authenticity. The audit chain covers audit rows;
// a corrupted snapshot is rejected by its plaintext digest, while a corrupted
// recording simply fails to decode.

#include "crypto/sm.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::secure {

// Envelope constants, exposed for tests and for diagnostics.
inline constexpr std::string_view kSealedMagic = "UVSE";
inline constexpr std::uint8_t kSealedVersion = 1;
inline constexpr std::uint8_t kSealedAlgorithmSm4Ctr = 1;
inline constexpr std::size_t kSealedSaltSize = 16;
inline constexpr std::size_t kSealedIvSize = 16;
inline constexpr std::size_t kSealedDigestSize = 32;
inline constexpr std::size_t kSealedHeaderSize =
    4 + 1 + 1 + 2 + kSealedSaltSize + kSealedIvSize + kSealedDigestSize;
inline constexpr std::string_view kEncryptedRecordingSuffix = ".enc";

// Reads a PEM certificate from disk and derives the 16-byte SM4 master key
// from it. The PEM bytes are the key material, so a renewed certificate means
// the old files can no longer be opened; back up the certificate with the
// data.
bool LoadDeviceCertificate(const std::string& path, std::string* certificate_pem,
                           std::string* error);
bool DeriveStorageKey(std::string_view certificate_pem, gm::Bytes* key, std::string* error);
bool DeriveStorageKeyFromFile(const std::string& certificate_path, gm::Bytes* key,
                              std::string* error);

bool IsSealedBlob(const gm::Bytes& data);

bool SealBlob(const gm::Bytes& plaintext, const gm::Bytes& key, gm::Bytes* sealed,
              std::string* error);
bool OpenBlob(const gm::Bytes& sealed, const gm::Bytes& key, gm::Bytes* plaintext,
              std::string* error);

bool SealFile(const std::string& path, const gm::Bytes& plaintext, const gm::Bytes& key,
              std::string* error);
bool OpenSealedFile(const std::string& path, const gm::Bytes& key, gm::Bytes* plaintext,
                    std::string* error);

// ---- Recordings --------------------------------------------------------

bool IsEncryptedRecordingPath(std::string_view path);
gm::Bytes DeriveRecordingIv(const gm::Bytes& key, std::string_view final_path);

// Encrypts `plain_path` into `final_path` with SM4-CTR. `final_path` must be
// the path recorded in the database, because it is part of the IV derivation.
bool EncryptRecordingFile(const std::string& plain_path, const std::string& final_path,
                          const gm::Bytes& key, std::string* error);

// Decrypts `ciphertext` that was read at byte `offset` of `final_path`.
bool DecryptRecordingRange(const gm::Bytes& key, std::string_view final_path, std::uint64_t offset,
                           const gm::Bytes& ciphertext, gm::Bytes* plaintext,
                           std::string* error);

// Reads and decrypts [offset, offset + length) from an encrypted recording.
// Used by the playback keyframe scan.
bool ReadDecryptedRecordingRange(const gm::Bytes& key, const std::string& final_path,
                                 std::uint64_t offset, std::size_t length, gm::Bytes* plaintext,
                                 std::string* error);

} // namespace uvnvr::secure
