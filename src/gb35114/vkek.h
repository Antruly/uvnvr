#pragma once

// GB 35114 VKEK distribution and signature negotiation (P6.3).
//
// The platform authenticates the device and the device authenticates the
// platform with SM2/SM3, agrees on the A-level video signature parameters and
// then hands the device a VKEK (16-byte SM4 working key) encrypted with the
// device's SM2 public key. Three messages carry the exchange:
//
//   1. ClientHello    device -> platform, signed by the device
//   2. ServerHello    platform -> device, signed by the platform
//   3. VkekDelivery   platform -> device, signed by the platform and encrypted
//                     to the device; the device answers with a signed VkekAck
//
// Every message is a self-delimiting binary record. A SIP MESSAGE body carries
// one record base64-encoded (see EncodeMessageBase64); the same record can also
// travel over the private protocol or a WebSocket without changes. The wire
// format is documented in docs/gb35114-vkek.md.
//
// Security properties implemented here:
//   * mutual SM2 authentication over the canonical unsigned body;
//   * a device-generated 32-byte nonce echoed by the platform, plus a session
//     transcript hash, binds the VKEK to one negotiation;
//   * a strictly increasing per-session sequence number rejects replayed or
//     reordered VKEK deliveries;
//   * timestamps are checked against a configurable skew and the VKEK validity
//     window is enforced;
//   * a fresh VKEK must have a strictly greater key id and key version, and the
//     previous key is zeroised on rotation, so media encrypted under the old
//     key can no longer be decrypted;
//   * failed negotiations/deliveries that are bound to the current nonce and
//     session increment a counter and lock the manager after a configurable
//     threshold until an operator resets it. Unbound junk still increments the
//     statistics but cannot lock the device by itself.
//
// The module is compiled only with UVNVR_ENABLE_GB35114=ON (default off) and is
// not linked into the server, so the existing preview/recording/RTSP/ONVIF/
// private-protocol paths are unchanged.

#include "crypto/sm.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb35114 {

// ---- Protocol constants -------------------------------------------------

inline constexpr std::uint16_t kVkekProtocolVersion = 1;
inline constexpr std::size_t kVkekSize = 16;
inline constexpr std::size_t kNonceSize = 32;
inline constexpr std::size_t kSessionIdSize = 16;
inline constexpr std::size_t kFingerprintSize = 32;
inline constexpr std::size_t kMaxMessageSize = 64 * 1024;

// Negotiated video signature parameters. The values are part of the wire
// protocol; a future record format must use a new value rather than reusing an
// existing one.
inline constexpr std::uint16_t kSignatureSuiteSm2Sm3 = 1;
inline constexpr std::uint16_t kCipherSuiteSm4Cbc = 1;
inline constexpr std::uint16_t kCipherSuiteSm4Ctr = 2;
inline constexpr std::uint16_t kSignModeIFrames = 1;
inline constexpr std::uint16_t kSignModeAllFrames = 2;
inline constexpr std::uint16_t kSignatureRecordVersion = 1;

std::string_view SignatureSuiteName(std::uint16_t suite);
std::string_view CipherSuiteName(std::uint16_t suite);
std::string_view SignModeName(std::uint16_t mode);

// ---- Wire records -------------------------------------------------------

// ClientHello: the device's capabilities and nonce.
struct ClientHello {
  std::string device_id;
  gm::Bytes device_nonce;        // exactly 32 bytes
  std::int64_t timestamp_ms = 0; // Unix time, milliseconds
  std::vector<std::uint16_t> signature_suites;
  std::vector<std::uint16_t> cipher_suites;
  std::vector<std::uint16_t> sign_modes;
  gm::Bytes certificate_fingerprint; // SM3(DER certificate), 32 bytes or empty
  gm::Bytes signature;               // SM2 over ClientHelloSignedData()
};

// ServerHello: the platform's selection, its nonce, and a signature that also
// proves possession of the platform identity key.
struct ServerHello {
  gm::Bytes session_id; // exactly 16 bytes, non-zero
  std::uint16_t signature_suite = kSignatureSuiteSm2Sm3;
  std::uint16_t cipher_suite = kCipherSuiteSm4Ctr;
  std::uint16_t sign_mode = kSignModeIFrames;
  std::uint16_t record_version = kSignatureRecordVersion;
  gm::Bytes platform_nonce; // exactly 32 bytes
  std::int64_t timestamp_ms = 0;
  gm::Bytes device_nonce; // echo of ClientHello.device_nonce
  gm::Bytes signature;    // SM2 over ServerHelloSignedData()
};

// VkekDelivery: the platform's signed, device-encrypted working key.
struct VkekDelivery {
  gm::Bytes session_id;          // exactly 16 bytes
  std::uint64_t key_id = 0;      // strictly increasing across rotations
  std::uint32_t key_version = 0; // strictly increasing across rotations
  std::uint64_t sequence = 0;    // strictly increasing within the session
  std::int64_t not_before_ms = 0;
  std::int64_t not_after_ms = 0;
  std::int64_t timestamp_ms = 0; // message creation time, for replay checks
  gm::Bytes device_nonce;        // echo of ClientHello.device_nonce
  gm::Bytes transcript_hash;     // ComputeTranscriptHash(ClientHello, ServerHello)
  gm::Bytes encrypted_vkek;      // SM2 ciphertext, base-36 DER
  gm::Bytes signature;           // SM2 over VkekDeliverySignedData()
};

// VkekAck: the device's signed confirmation (or rejection) of a delivery.
inline constexpr std::uint16_t kVkekAckAccepted = 0;

struct VkekAck {
  gm::Bytes session_id;
  std::uint64_t key_id = 0;
  std::uint32_t key_version = 0;
  std::uint64_t sequence = 0;
  std::uint16_t status = kVkekAckAccepted; // 0 accepted, otherwise a VkekStatus
  std::int64_t timestamp_ms = 0;
  gm::Bytes signature; // SM2 over VkekAckSignedData()
};

enum class VkekStatus : std::uint16_t {
  kOk = 0,
  kNotConfigured = 1,
  kNoPendingOffer = 2,
  kMalformed = 3,
  kUnsupportedAlgorithm = 4,
  kBadSignature = 5,
  kBadNonce = 6,
  kBadTranscript = 7,
  kStaleTimestamp = 8,
  kUnknownSession = 9,
  kNotNegotiated = 10,
  kReplay = 11,
  kDecryptFailed = 12,
  kInvalidKey = 13,
  kKeyExpired = 14,
  kKeyRotationViolation = 15,
  kLocked = 16,
  kInternalError = 17,
};

std::string_view VkekStatusName(VkekStatus status);

// Serialised records. Serializers validate lengths and set `error` on failure.
gm::Bytes SerializeClientHello(const ClientHello& hello, std::string* error = nullptr);
gm::Bytes SerializeServerHello(const ServerHello& hello, std::string* error = nullptr);
gm::Bytes SerializeVkekDelivery(const VkekDelivery& delivery, std::string* error = nullptr);
gm::Bytes SerializeVkekAck(const VkekAck& ack, std::string* error = nullptr);

// Parsers consume exactly one record. They also reject non-canonical encodings,
// so a signature can never be over an ambiguous re-encoding of the same data.
bool ParseClientHello(const gm::Bytes& wire, ClientHello* hello, std::string* error = nullptr);
bool ParseServerHello(const gm::Bytes& wire, ServerHello* hello, std::string* error = nullptr);
bool ParseVkekDelivery(const gm::Bytes& wire, VkekDelivery* delivery, std::string* error = nullptr);
bool ParseVkekAck(const gm::Bytes& wire, VkekAck* ack, std::string* error = nullptr);

// Canonical byte strings that are handed to SM2. They begin with a domain
// separator so a signature from one message type can never verify as another.
gm::Bytes ClientHelloSignedData(const ClientHello& hello);
gm::Bytes ServerHelloSignedData(const ServerHello& hello);
gm::Bytes VkekDeliverySignedData(const VkekDelivery& delivery);
gm::Bytes VkekAckSignedData(const VkekAck& ack);

// SM3 over both complete wire records, prefixed with a domain separator. The
// platform includes this value in the VKEK delivery; the device recomputes it
// from the records it actually exchanged.
gm::Bytes ComputeTranscriptHash(const gm::Bytes& client_hello_wire,
                                const gm::Bytes& server_hello_wire);

// SIP MESSAGE helper: standard base64 with '=' padding. Decoding ignores ASCII
// whitespace and requires a multiple-of-four input after stripping.
std::string EncodeMessageBase64(const gm::Bytes& wire);
bool DecodeMessageBase64(std::string_view text, gm::Bytes* wire, std::string* error = nullptr);

// ---- Device-side manager ------------------------------------------------

struct VkekConfig {
  std::string device_id;
  std::string device_private_key_pem;   // SM2 PKCS#8 PEM
  std::string device_certificate_pem;   // optional; used for the fingerprint
  std::string platform_certificate_pem; // pinned platform leaf certificate
  std::string platform_chain_pem;       // optional intermediates for the leaf
  std::string platform_public_key_pem;  // alternative to a certificate
  // When both are set, the platform leaf is validated against these roots in
  // addition to being used for signature verification.
  std::string trusted_roots_pem;
  std::int64_t allowed_clock_skew_ms = 60 * 1000;
  int max_consecutive_failures = 5;
  // Injected clock for deterministic tests; empty means system_clock.
  std::function<std::int64_t()> now_ms;
};

struct NegotiatedSignature {
  std::uint16_t suite = kSignatureSuiteSm2Sm3;
  std::uint16_t sign_mode = kSignModeIFrames;
  std::uint16_t record_version = kSignatureRecordVersion;
};

struct VkekKeyInfo {
  bool has_key = false;
  std::uint64_t key_id = 0;
  std::uint32_t key_version = 0;
  std::uint64_t sequence = 0;
  std::int64_t not_before_ms = 0;
  std::int64_t not_after_ms = 0;
  std::size_t key_size = 0;
};

struct SessionInfo {
  bool negotiated = false;
  gm::Bytes session_id;
  gm::Bytes device_nonce;
  gm::Bytes platform_nonce;
  NegotiatedSignature signature;
  std::uint16_t cipher_suite = 0;
  VkekKeyInfo key;
};

struct VkekStats {
  std::uint64_t offers_created = 0;
  std::uint64_t negotiations_completed = 0;
  std::uint64_t deliveries_accepted = 0;
  std::uint64_t deliveries_rejected = 0;
  std::uint64_t rotations = 0;
  std::uint64_t keys_destroyed = 0;
  std::uint64_t replay_rejections = 0;
  std::uint64_t expired_rejections = 0;
  std::uint64_t failures = 0;
};

// One instance per device identity, driven from the SIP/event-loop thread.
// The class holds private key material, so it is non-copyable; the destructor
// zeroises live key material.
class VkekManager {
public:
  VkekManager();
  ~VkekManager();

  VkekManager(const VkekManager&) = delete;
  VkekManager& operator=(const VkekManager&) = delete;
  VkekManager(VkekManager&&) noexcept;
  VkekManager& operator=(VkekManager&&) noexcept;

  // Validates the configured device key (and, when an identity certificate is
  // supplied, that it matches the key). A platform chain is validated here
  // when trusted roots are configured.
  bool Configure(VkekConfig config, std::string* error = nullptr);
  bool IsConfigured() const;
  bool IsLocked() const;
  // Operator action after investigating the failure; clears the lock and the
  // consecutive-failure counter but not the session or installed key.
  void ResetFailureLock();

  // Builds a signed ClientHello and stores its nonce for the reply check. A
  // new call replaces an uncompleted offer.
  bool CreateClientHello(gm::Bytes* wire, std::string* error = nullptr);
  const gm::Bytes& pending_client_hello() const;
  const gm::Bytes& pending_device_nonce() const;

  // Verifies the platform signature, echoed nonce, freshness and selection.
  // On success the negotiated session parameters are available to the media
  // signing path.
  VkekStatus ProcessServerHello(const gm::Bytes& wire, std::string* error = nullptr);

  // Verifies, decrypts and installs a VKEK. On success an optional signed
  // acknowledgement is produced. The previous key, if any, is zeroised.
  VkekStatus ProcessVkekDelivery(const gm::Bytes& wire, gm::Bytes* ack_wire = nullptr,
                                 std::string* error = nullptr);

  // ---- Media-path accessors -------------------------------------------

  bool GetSessionInfo(SessionInfo* info, std::string* error = nullptr) const;
  VkekKeyInfo active_key_info() const;
  // Copies the active key after checking its validity window. Returns false
  // when no key is installed, the key is not yet valid, or it has expired.
  bool CopyActiveKey(gm::Bytes* key, VkekKeyInfo* info = nullptr,
                     std::string* error = nullptr) const;
  // Only the active key id resolves. After a rotation a stream tagged with the
  // retired key id is refused: this is the "old stream cannot be decrypted"
  // guarantee, because the old key bytes are gone.
  bool GetKeyForStream(std::uint64_t key_id, gm::Bytes* key, std::string* error = nullptr) const;

  const VkekStats& stats() const;
  int consecutive_failures() const;
  std::int64_t now_ms() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr::gb35114
