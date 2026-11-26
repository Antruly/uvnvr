#pragma once

// GB 35114 A-level video integrity (P6.3a).
//
// Every I frame (or, optionally, every access unit) of an Annex B H.264
// elementary stream is reduced to an SM3 digest and signed with the device SM2
// private key. The signature is carried out of band as a self-delimiting
// record stream, so the media bytes themselves are never modified. A player or
// gateway receives the original video on one channel and the signature records
// on a second channel; a recording stores them in a sidecar file next to the
// transport stream.
//
// The module reuses the P6.1a crypto primitives (crypto/sm.h) for SM2/SM3 and
// the P6.2c media pipeline model: MPEG-TS is demuxed into access units, and the
// same access-unit reader feeds both signing and verification, which keeps the
// frame index and frame boundaries deterministic.
//
// Record layout (all integers big-endian, fixed 88-byte header):
//
//   offset size field
//   0      8    magic "GB35114S"
//   8      2    record version (1)
//   10     2    header size (88)
//   12     4    flags (bit 0: key frame, bit 1: PTS present,
//                     bit 2: the sidecar covers every access unit)
//   16     8    channel id
//   24     8    access-unit index in the stream (0 based)
//   32     8    PTS in the 90 kHz clock (0 when bit 1 is clear)
//   40     8    signed payload size in bytes
//   48     32   SM3 digest of the payload
//   80     4    SM2 signature size (DER)
//   84     4    reserved (0)
//   88     ...  SM2 DER signature
//
// The signed message is not the payload itself but a canonical 80-byte binding
// of the domain separator, version, flags, channel, frame index, PTS, payload
// size and SM3 digest. SM2 signing then applies SM3 again, as GB/T 32918
// requires. Binding the metadata prevents moving a valid signature to a
// different frame, channel or timestamp.
//
// Compiled only with UVNVR_ENABLE_GB35114=ON (default off).

#include "crypto/sm.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb35114 {

inline constexpr std::size_t kSignatureHeaderSize = 88;
inline constexpr std::size_t kSignatureDigestSize = 32;
inline constexpr std::uint16_t kSignatureVersion = 1;
inline constexpr std::uint32_t kFlagKeyFrame = 1U << 0;
inline constexpr std::uint32_t kFlagHasPts = 1U << 1;
// Set on every record when the sidecar covers every access unit, not just key
// frames. It lets the verifier notice a deleted non-key-frame record.
inline constexpr std::uint32_t kFlagAllFrames = 1U << 2;

// Domain separator for the signed message. It is part of the cryptographic
// binding and must not change without a record version bump.
inline constexpr std::string_view kSignatureDomain = "GB35114-A1";

struct FrameSignature {
  std::uint32_t flags = 0;
  std::int64_t channel_id = 0;
  std::uint64_t frame_index = 0;
  std::int64_t pts = 0;
  std::uint64_t payload_size = 0;
  gm::Bytes digest;     // 32 bytes, SM3(payload)
  gm::Bytes signature;  // DER ECDSA-Sig-Value produced by SM2

  bool key_frame() const { return (flags & kFlagKeyFrame) != 0; }
  bool has_pts() const { return (flags & kFlagHasPts) != 0; }
};

// Serialises one record. Returns an empty vector and sets `error` when the
// record is malformed (wrong digest length, missing or oversized signature).
gm::Bytes SerializeFrameSignature(const FrameSignature& signature, std::string* error = nullptr);

// Parses one record from the front of `data`. `consumed` receives the exact
// number of bytes the record occupies so a caller can walk a concatenated
// stream. Extra header bytes from a future version are skipped, but the
// signature must fit in `size`.
bool ParseFrameSignature(const std::uint8_t* data, std::size_t size, FrameSignature* signature,
                         std::size_t* consumed, std::string* error = nullptr);

// Canonical 80-byte message that is handed to SM2. Exposed for tests and for
// independent verifiers; callers normally use CreateFrameSignature().
gm::Bytes BuildSignedMessage(const FrameSignature& signature);

// Digests `payload` with SM3 and signs the canonical binding with the device
// private key. The PTS flag is set from `has_pts`.
bool CreateFrameSignature(std::string_view private_key_pem, std::int64_t channel_id,
                          std::uint64_t frame_index, std::int64_t pts, bool has_pts,
                          bool key_frame, const void* payload, std::size_t size,
                          FrameSignature* signature, std::string* error = nullptr,
                          std::uint32_t extra_flags = 0);

struct VerifyOptions {
  std::string public_key_pem;
  std::string certificate_pem;  // used when public_key_pem is empty
};

// Verifies a record against the media payload:
//   1. the payload size must match the signed size;
//   2. SM3(payload) must match the signed digest (this is what detects a
//      modified frame);
//   3. the SM2 signature over the canonical binding must verify.
// The key frame flag and PTS are checked by the stream verifier, not here.
bool VerifyFrameSignature(const VerifyOptions& options, const void* payload, std::size_t size,
                          const FrameSignature& signature, std::string* error = nullptr);

// One Annex B H.264 access unit. `data` contains the original start codes and
// NAL bytes exactly as they appeared in the elementary stream.
struct AccessUnit {
  gm::Bytes data;
  bool key_frame = false;
  bool has_pts = false;
  std::int64_t pts = 0;
};

struct ReaderStats {
  std::uint64_t input_bytes = 0;
  std::uint64_t access_units = 0;
  std::uint64_t key_frames = 0;
  std::string codec = "h264";
};

// Input accepted by AccessUnitReader. kAuto looks at the first byte: 0x47 is
// MPEG-TS, anything else is treated as an Annex B elementary stream.
enum class InputFormat {
  kAuto,
  kMpegTs,
  kAnnexB,
};

// Streams MPEG-TS (or raw Annex B H.264) into access units. It is incremental:
// Push() may be called with arbitrary chunk boundaries, for example directly
// from MediaService::AddRawSubscriber. The sink returns false to abort reading.
class AccessUnitReader {
 public:
  using Sink = std::function<bool(AccessUnit&& unit, std::string* error)>;

  explicit AccessUnitReader(InputFormat format = InputFormat::kAuto);
  ~AccessUnitReader();

  AccessUnitReader(const AccessUnitReader&) = delete;
  AccessUnitReader& operator=(const AccessUnitReader&) = delete;

  void SetSink(Sink sink);
  bool Push(const std::uint8_t* data, std::size_t size, std::string* error = nullptr);
  bool Flush(std::string* error = nullptr);
  void Reset();

  const ReaderStats& stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct SigningOptions {
  std::string private_key_pem;
  std::string certificate_pem;  // optional; validated with the key when set
  std::int64_t channel_id = 0;
  bool sign_all_frames = false;
};

struct SigningStats : ReaderStats {
  std::uint64_t signed_frames = 0;
  std::uint64_t sign_ns_total = 0;
  std::uint64_t sign_ns_min = 0;
  std::uint64_t sign_ns_max = 0;
};

// Signs the access units produced by AccessUnitReader and emits one record per
// signed frame through the sink. The sink is the "independent channel": it can
// write a sidecar file, push a WebSocket/private-protocol message, or place the
// record in an RTP header extension.
class FrameSigner {
 public:
  using Sink = std::function<void(const FrameSignature& signature)>;

  FrameSigner(SigningOptions options, InputFormat format = InputFormat::kAuto);
  ~FrameSigner();

  FrameSigner(const FrameSigner&) = delete;
  FrameSigner& operator=(const FrameSigner&) = delete;

  // Parses the private key once so a bad identity fails before the stream
  // starts. Returns false with an OpenSSL-derived message.
  bool Configure(std::string* error = nullptr);
  void SetSink(Sink sink);

  bool Push(const std::uint8_t* data, std::size_t size, std::string* error = nullptr);
  bool Flush(std::string* error = nullptr);
  void Reset();

  const SigningStats& stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Convenience wrappers used by tools/tests. Each one drives the same
// incremental reader, so the frame indices match a live stream.
bool ReadAccessUnitsFromFile(const std::string& path, InputFormat format,
                             std::vector<AccessUnit>* units, ReaderStats* stats,
                             std::string* error = nullptr);
bool SignFile(const std::string& path, const SigningOptions& options, InputFormat format,
              std::vector<FrameSignature>* signatures, SigningStats* stats,
              std::string* error = nullptr);

// Reads/writes a concatenated sidecar record stream.
bool ReadSignatureFile(const std::string& path, std::vector<FrameSignature>* signatures,
                       std::string* error = nullptr);
bool WriteSignatureFile(const std::string& path, const std::vector<FrameSignature>& signatures,
                        std::string* error = nullptr);

// Reads a PEM file. Used by the command line tool and callers that keep the
// key on disk.
bool ReadPemFile(const std::string& path, std::string* pem, std::string* error = nullptr);

}  // namespace uvnvr::gb35114
