#include "gb35114/vkek.h"

#include "crypto/cert.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <utility>

namespace uvnvr::gb35114 {
namespace {

using gm::Bytes;

constexpr std::string_view kWireMagic = "GB35114V";
constexpr std::size_t kWireHeaderSize = 16;
constexpr std::size_t kMaxListEntries = 64;
constexpr std::size_t kMaxDeviceIdSize = 255;

constexpr std::uint8_t kTypeClientHello = 1;
constexpr std::uint8_t kTypeServerHello = 2;
constexpr std::uint8_t kTypeVkekDelivery = 3;
constexpr std::uint8_t kTypeVkekAck = 4;

constexpr std::string_view kDomainClientHello = "GB35114-VKEK-CH-1";
constexpr std::string_view kDomainServerHello = "GB35114-VKEK-SH-1";
constexpr std::string_view kDomainDelivery = "GB35114-VKEK-DL-1";
constexpr std::string_view kDomainAck = "GB35114-VKEK-ACK-1";
constexpr std::string_view kDomainTranscript = "GB35114-VKEK-TR-1";

void SetError(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
}

Bytes ToBytes(std::string_view text) {
  return Bytes(text.begin(), text.end());
}

std::string_view BytesView(const Bytes& bytes) {
  static const char kEmpty = '\0';
  return std::string_view(bytes.empty() ? &kEmpty : reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

void Wipe(Bytes* bytes) {
  if (!bytes)
    return;
  if (!bytes->empty())
    OPENSSL_cleanse(bytes->data(), bytes->size());
  bytes->clear();
}

void WipeString(std::string* text) {
  if (!text)
    return;
  if (!text->empty())
    OPENSSL_cleanse(text->data(), text->size());
  text->clear();
}

bool BytesEqual(const Bytes& a, const Bytes& b) {
  if (a.size() != b.size())
    return false;
  if (a.empty())
    return true;
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool RandomBytes(std::size_t size, Bytes* out, const char* what, std::string* error) {
  out->assign(size, 0);
  if (RAND_bytes(out->data(), static_cast<int>(size)) != 1) {
    SetError(error, std::string("cannot generate the random ") + what);
    return false;
  }
  return true;
}

class Writer {
public:
  void U8(std::uint8_t value) {
    data_.push_back(value);
  }
  void U16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>(value >> 8));
    data_.push_back(static_cast<std::uint8_t>(value));
  }
  void U32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
      data_.push_back(static_cast<std::uint8_t>(value >> shift));
  }
  void U64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
      data_.push_back(static_cast<std::uint8_t>(value >> shift));
  }
  void Raw(const Bytes& bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
  }
  void Var(const Bytes& bytes) {
    U16(static_cast<std::uint16_t>(bytes.size()));
    Raw(bytes);
  }
  Bytes& data() {
    return data_;
  }

private:
  Bytes data_;
};

class Reader {
public:
  Reader(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}

  bool U8(std::uint8_t* value) {
    if (remaining() < 1)
      return false;
    *value = data_[position_++];
    return true;
  }
  bool U16(std::uint16_t* value) {
    if (remaining() < 2)
      return false;
    *value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[position_]) << 8) |
                                        data_[position_ + 1]);
    position_ += 2;
    return true;
  }
  bool U32(std::uint32_t* value) {
    if (remaining() < 4)
      return false;
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i)
      result = (result << 8) | data_[position_ + static_cast<std::size_t>(i)];
    position_ += 4;
    *value = result;
    return true;
  }
  bool U64(std::uint64_t* value) {
    if (remaining() < 8)
      return false;
    std::uint64_t result = 0;
    for (int i = 0; i < 8; ++i)
      result = (result << 8) | data_[position_ + static_cast<std::size_t>(i)];
    position_ += 8;
    *value = result;
    return true;
  }
  bool Raw(std::size_t size, Bytes* out) {
    if (remaining() < size)
      return false;
    out->assign(data_ + position_, data_ + position_ + size);
    position_ += size;
    return true;
  }
  bool Var(Bytes* out) {
    std::uint16_t size = 0;
    return U16(&size) && Raw(size, out);
  }
  bool AtEnd() const {
    return position_ == size_;
  }

private:
  std::size_t remaining() const {
    return size_ - position_;
  }

  const unsigned char* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

bool SplitWire(const Bytes& wire, std::uint8_t expected_type, const unsigned char** body,
               std::size_t* body_size, std::string* error) {
  if (wire.size() < kWireHeaderSize) {
    SetError(error, "message is shorter than the 16-byte header");
    return false;
  }
  if (wire.size() > kMaxMessageSize) {
    SetError(error, "message exceeds the 64 KiB limit");
    return false;
  }
  if (std::memcmp(wire.data(), kWireMagic.data(), kWireMagic.size()) != 0) {
    SetError(error, "message magic is not GB35114V");
    return false;
  }
  Reader reader(wire.data() + kWireMagic.size(), wire.size() - kWireMagic.size());
  std::uint16_t version = 0;
  std::uint8_t type = 0;
  std::uint8_t flags = 0;
  std::uint32_t length = 0;
  if (!reader.U16(&version) || !reader.U8(&type) || !reader.U8(&flags) || !reader.U32(&length)) {
    SetError(error, "message header is truncated");
    return false;
  }
  if (version != kVkekProtocolVersion) {
    SetError(error, "unsupported protocol version " + std::to_string(version));
    return false;
  }
  if (type != expected_type) {
    SetError(error, "unexpected message type " + std::to_string(type));
    return false;
  }
  if (flags != 0) {
    SetError(error, "reserved header flags are not zero");
    return false;
  }
  if (length != wire.size() - kWireHeaderSize) {
    SetError(error, "message length does not match the body size");
    return false;
  }
  *body = wire.data() + kWireHeaderSize;
  *body_size = length;
  return true;
}

bool Wrap(std::uint8_t type, const Bytes& body, Bytes* wire, std::string* error) {
  if (body.size() > kMaxMessageSize - kWireHeaderSize ||
      body.size() > std::numeric_limits<std::uint32_t>::max()) {
    SetError(error, "the message body is too large");
    return false;
  }
  Writer writer;
  writer.Raw(ToBytes(kWireMagic));
  writer.U16(kVkekProtocolVersion);
  writer.U8(type);
  writer.U8(0);
  writer.U32(static_cast<std::uint32_t>(body.size()));
  writer.Raw(body);
  *wire = std::move(writer.data());
  return true;
}

Bytes SignedData(std::string_view domain, const Bytes& body) {
  Bytes data = ToBytes(domain);
  data.insert(data.end(), body.begin(), body.end());
  return data;
}

bool PutFixed(const Bytes& value, std::size_t expected, const char* name, Writer& writer,
              std::string* error) {
  if (value.size() != expected) {
    SetError(error, std::string(name) + " must be " + std::to_string(expected) + " bytes");
    return false;
  }
  writer.Raw(value);
  return true;
}

bool PutU16List(const std::vector<std::uint16_t>& values, const char* name, Writer& writer,
                std::string* error) {
  if (values.size() > kMaxListEntries) {
    SetError(error, std::string(name) + " has too many entries");
    return false;
  }
  writer.U16(static_cast<std::uint16_t>(values.size()));
  for (std::uint16_t value : values)
    writer.U16(value);
  return true;
}

bool ParseU16List(Reader& reader, const char* name, std::vector<std::uint16_t>* values,
                  std::string* error) {
  std::uint16_t count = 0;
  if (!reader.U16(&count)) {
    SetError(error, std::string(name) + " is truncated");
    return false;
  }
  if (count > kMaxListEntries) {
    SetError(error, std::string(name) + " has too many entries");
    return false;
  }
  values->clear();
  values->reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint16_t value = 0;
    if (!reader.U16(&value)) {
      SetError(error, std::string(name) + " is truncated");
      return false;
    }
    values->push_back(value);
  }
  return true;
}

bool SerializeClientHelloBody(const ClientHello& hello, bool include_signature, Bytes* body,
                              std::string* error) {
  if (hello.device_id.empty()) {
    SetError(error, "the device id is empty");
    return false;
  }
  if (hello.device_id.size() > kMaxDeviceIdSize) {
    SetError(error, "the device id is too long");
    return false;
  }
  Bytes fingerprint = hello.certificate_fingerprint;
  if (fingerprint.empty())
    fingerprint.assign(kFingerprintSize, 0);

  Writer writer;
  writer.Var(ToBytes(hello.device_id));
  if (!PutFixed(hello.device_nonce, kNonceSize, "the device nonce", writer, error))
    return false;
  writer.U64(static_cast<std::uint64_t>(hello.timestamp_ms));
  if (!PutU16List(hello.signature_suites, "the signature suite list", writer, error) ||
      !PutU16List(hello.cipher_suites, "the cipher suite list", writer, error) ||
      !PutU16List(hello.sign_modes, "the sign mode list", writer, error))
    return false;
  if (!PutFixed(fingerprint, kFingerprintSize, "the certificate fingerprint", writer, error))
    return false;
  if (include_signature) {
    if (hello.signature.size() > std::numeric_limits<std::uint16_t>::max()) {
      SetError(error, "the device signature is too long");
      return false;
    }
    writer.Var(hello.signature);
  }
  *body = std::move(writer.data());
  return true;
}

bool SerializeServerHelloBody(const ServerHello& hello, bool include_signature, Bytes* body,
                              std::string* error) {
  Writer writer;
  if (!PutFixed(hello.session_id, kSessionIdSize, "the session id", writer, error) ||
      !PutFixed(hello.platform_nonce, kNonceSize, "the platform nonce", writer, error) ||
      !PutFixed(hello.device_nonce, kNonceSize, "the echoed device nonce", writer, error))
    return false;
  writer.U16(hello.signature_suite);
  writer.U16(hello.cipher_suite);
  writer.U16(hello.sign_mode);
  writer.U16(hello.record_version);
  writer.U64(static_cast<std::uint64_t>(hello.timestamp_ms));
  if (include_signature) {
    if (hello.signature.size() > std::numeric_limits<std::uint16_t>::max()) {
      SetError(error, "the platform signature is too long");
      return false;
    }
    writer.Var(hello.signature);
  }
  *body = std::move(writer.data());
  return true;
}

bool SerializeDeliveryBody(const VkekDelivery& delivery, bool include_signature, Bytes* body,
                           std::string* error) {
  Writer writer;
  if (!PutFixed(delivery.session_id, kSessionIdSize, "the session id", writer, error) ||
      !PutFixed(delivery.device_nonce, kNonceSize, "the echoed device nonce", writer, error) ||
      !PutFixed(delivery.transcript_hash, gm::kSm3DigestSize, "the transcript hash", writer, error))
    return false;
  writer.U64(delivery.key_id);
  writer.U32(delivery.key_version);
  writer.U64(delivery.sequence);
  writer.U64(static_cast<std::uint64_t>(delivery.not_before_ms));
  writer.U64(static_cast<std::uint64_t>(delivery.not_after_ms));
  writer.U64(static_cast<std::uint64_t>(delivery.timestamp_ms));
  if (delivery.encrypted_vkek.size() > std::numeric_limits<std::uint16_t>::max()) {
    SetError(error, "the encrypted VKEK is too long");
    return false;
  }
  writer.Var(delivery.encrypted_vkek);
  if (include_signature) {
    if (delivery.signature.size() > std::numeric_limits<std::uint16_t>::max()) {
      SetError(error, "the platform signature is too long");
      return false;
    }
    writer.Var(delivery.signature);
  }
  *body = std::move(writer.data());
  return true;
}

bool SerializeAckBody(const VkekAck& ack, bool include_signature, Bytes* body, std::string* error) {
  Writer writer;
  if (!PutFixed(ack.session_id, kSessionIdSize, "the session id", writer, error))
    return false;
  writer.U64(ack.key_id);
  writer.U32(ack.key_version);
  writer.U64(ack.sequence);
  writer.U16(ack.status);
  writer.U64(static_cast<std::uint64_t>(ack.timestamp_ms));
  if (include_signature) {
    if (ack.signature.size() > std::numeric_limits<std::uint16_t>::max()) {
      SetError(error, "the device signature is too long");
      return false;
    }
    writer.Var(ack.signature);
  }
  *body = std::move(writer.data());
  return true;
}

bool Canonical(const Bytes& wire, const Bytes& canonical, const char* what, std::string* error) {
  if (canonical.empty() || canonical != wire) {
    SetError(error, std::string(what) + " is not in canonical form");
    return false;
  }
  return true;
}

} // namespace

std::string_view SignatureSuiteName(std::uint16_t suite) {
  switch (suite) {
  case kSignatureSuiteSm2Sm3:
    return "SM2-SM3";
  default:
    return "unknown";
  }
}

std::string_view CipherSuiteName(std::uint16_t suite) {
  switch (suite) {
  case kCipherSuiteSm4Cbc:
    return "SM4-CBC";
  case kCipherSuiteSm4Ctr:
    return "SM4-CTR";
  default:
    return "unknown";
  }
}

std::string_view SignModeName(std::uint16_t mode) {
  switch (mode) {
  case kSignModeIFrames:
    return "I-frames";
  case kSignModeAllFrames:
    return "all-frames";
  default:
    return "unknown";
  }
}

std::string_view VkekStatusName(VkekStatus status) {
  switch (status) {
  case VkekStatus::kOk:
    return "ok";
  case VkekStatus::kNotConfigured:
    return "not-configured";
  case VkekStatus::kNoPendingOffer:
    return "no-pending-offer";
  case VkekStatus::kMalformed:
    return "malformed";
  case VkekStatus::kUnsupportedAlgorithm:
    return "unsupported-algorithm";
  case VkekStatus::kBadSignature:
    return "bad-signature";
  case VkekStatus::kBadNonce:
    return "bad-nonce";
  case VkekStatus::kBadTranscript:
    return "bad-transcript";
  case VkekStatus::kStaleTimestamp:
    return "stale-timestamp";
  case VkekStatus::kUnknownSession:
    return "unknown-session";
  case VkekStatus::kNotNegotiated:
    return "not-negotiated";
  case VkekStatus::kReplay:
    return "replay";
  case VkekStatus::kDecryptFailed:
    return "decrypt-failed";
  case VkekStatus::kInvalidKey:
    return "invalid-key";
  case VkekStatus::kKeyExpired:
    return "key-expired";
  case VkekStatus::kKeyRotationViolation:
    return "key-rotation-violation";
  case VkekStatus::kLocked:
    return "locked";
  case VkekStatus::kInternalError:
    return "internal-error";
  }
  return "unknown";
}

gm::Bytes SerializeClientHello(const ClientHello& hello, std::string* error) {
  Bytes body;
  if (!SerializeClientHelloBody(hello, true, &body, error))
    return {};
  Bytes wire;
  return Wrap(kTypeClientHello, body, &wire, error) ? wire : Bytes{};
}

gm::Bytes SerializeServerHello(const ServerHello& hello, std::string* error) {
  Bytes body;
  if (!SerializeServerHelloBody(hello, true, &body, error))
    return {};
  Bytes wire;
  return Wrap(kTypeServerHello, body, &wire, error) ? wire : Bytes{};
}

gm::Bytes SerializeVkekDelivery(const VkekDelivery& delivery, std::string* error) {
  Bytes body;
  if (!SerializeDeliveryBody(delivery, true, &body, error))
    return {};
  Bytes wire;
  return Wrap(kTypeVkekDelivery, body, &wire, error) ? wire : Bytes{};
}

gm::Bytes SerializeVkekAck(const VkekAck& ack, std::string* error) {
  Bytes body;
  if (!SerializeAckBody(ack, true, &body, error))
    return {};
  Bytes wire;
  return Wrap(kTypeVkekAck, body, &wire, error) ? wire : Bytes{};
}

bool ParseClientHello(const Bytes& wire, ClientHello* hello, std::string* error) {
  if (!hello) {
    SetError(error, "the client hello output is null");
    return false;
  }
  const unsigned char* body = nullptr;
  std::size_t body_size = 0;
  if (!SplitWire(wire, kTypeClientHello, &body, &body_size, error))
    return false;
  Reader reader(body, body_size);
  ClientHello parsed;
  Bytes device_id;
  if (!reader.Var(&device_id) || device_id.empty() || device_id.size() > kMaxDeviceIdSize) {
    SetError(error, "the client hello device id is invalid");
    return false;
  }
  parsed.device_id.assign(device_id.begin(), device_id.end());
  if (!reader.Raw(kNonceSize, &parsed.device_nonce)) {
    SetError(error, "the client hello device nonce is truncated");
    return false;
  }
  std::uint64_t timestamp = 0;
  if (!reader.U64(&timestamp)) {
    SetError(error, "the client hello timestamp is truncated");
    return false;
  }
  parsed.timestamp_ms = static_cast<std::int64_t>(timestamp);
  if (!ParseU16List(reader, "the signature suite list", &parsed.signature_suites, error) ||
      !ParseU16List(reader, "the cipher suite list", &parsed.cipher_suites, error) ||
      !ParseU16List(reader, "the sign mode list", &parsed.sign_modes, error))
    return false;
  if (!reader.Raw(kFingerprintSize, &parsed.certificate_fingerprint)) {
    SetError(error, "the certificate fingerprint is truncated");
    return false;
  }
  if (!reader.Var(&parsed.signature) || parsed.signature.empty()) {
    SetError(error, "the client hello signature is missing");
    return false;
  }
  if (!reader.AtEnd()) {
    SetError(error, "the client hello has trailing bytes");
    return false;
  }
  if (!Canonical(wire, SerializeClientHello(parsed), "the client hello", error))
    return false;
  *hello = std::move(parsed);
  return true;
}

bool ParseServerHello(const Bytes& wire, ServerHello* hello, std::string* error) {
  if (!hello) {
    SetError(error, "the server hello output is null");
    return false;
  }
  const unsigned char* body = nullptr;
  std::size_t body_size = 0;
  if (!SplitWire(wire, kTypeServerHello, &body, &body_size, error))
    return false;
  Reader reader(body, body_size);
  ServerHello parsed;
  std::uint64_t timestamp = 0;
  if (!reader.Raw(kSessionIdSize, &parsed.session_id) ||
      !reader.Raw(kNonceSize, &parsed.platform_nonce) ||
      !reader.Raw(kNonceSize, &parsed.device_nonce) || !reader.U16(&parsed.signature_suite) ||
      !reader.U16(&parsed.cipher_suite) || !reader.U16(&parsed.sign_mode) ||
      !reader.U16(&parsed.record_version) || !reader.U64(&timestamp) ||
      !reader.Var(&parsed.signature) || parsed.signature.empty()) {
    SetError(error, "the server hello is truncated or has no signature");
    return false;
  }
  parsed.timestamp_ms = static_cast<std::int64_t>(timestamp);
  if (!reader.AtEnd()) {
    SetError(error, "the server hello has trailing bytes");
    return false;
  }
  if (!Canonical(wire, SerializeServerHello(parsed), "the server hello", error))
    return false;
  *hello = std::move(parsed);
  return true;
}

bool ParseVkekDelivery(const Bytes& wire, VkekDelivery* delivery, std::string* error) {
  if (!delivery) {
    SetError(error, "the VKEK delivery output is null");
    return false;
  }
  const unsigned char* body = nullptr;
  std::size_t body_size = 0;
  if (!SplitWire(wire, kTypeVkekDelivery, &body, &body_size, error))
    return false;
  Reader reader(body, body_size);
  VkekDelivery parsed;
  std::uint64_t not_before = 0;
  std::uint64_t not_after = 0;
  std::uint64_t timestamp = 0;
  if (!reader.Raw(kSessionIdSize, &parsed.session_id) ||
      !reader.Raw(kNonceSize, &parsed.device_nonce) ||
      !reader.Raw(gm::kSm3DigestSize, &parsed.transcript_hash) || !reader.U64(&parsed.key_id) ||
      !reader.U32(&parsed.key_version) || !reader.U64(&parsed.sequence) ||
      !reader.U64(&not_before) || !reader.U64(&not_after) || !reader.U64(&timestamp) ||
      !reader.Var(&parsed.encrypted_vkek) || parsed.encrypted_vkek.empty() ||
      !reader.Var(&parsed.signature) || parsed.signature.empty()) {
    SetError(error, "the VKEK delivery is truncated or missing fields");
    return false;
  }
  parsed.not_before_ms = static_cast<std::int64_t>(not_before);
  parsed.not_after_ms = static_cast<std::int64_t>(not_after);
  parsed.timestamp_ms = static_cast<std::int64_t>(timestamp);
  if (!reader.AtEnd()) {
    SetError(error, "the VKEK delivery has trailing bytes");
    return false;
  }
  if (!Canonical(wire, SerializeVkekDelivery(parsed), "the VKEK delivery", error))
    return false;
  *delivery = std::move(parsed);
  return true;
}

bool ParseVkekAck(const Bytes& wire, VkekAck* ack, std::string* error) {
  if (!ack) {
    SetError(error, "the VKEK acknowledgement output is null");
    return false;
  }
  const unsigned char* body = nullptr;
  std::size_t body_size = 0;
  if (!SplitWire(wire, kTypeVkekAck, &body, &body_size, error))
    return false;
  Reader reader(body, body_size);
  VkekAck parsed;
  std::uint64_t timestamp = 0;
  if (!reader.Raw(kSessionIdSize, &parsed.session_id) || !reader.U64(&parsed.key_id) ||
      !reader.U32(&parsed.key_version) || !reader.U64(&parsed.sequence) ||
      !reader.U16(&parsed.status) || !reader.U64(&timestamp) || !reader.Var(&parsed.signature) ||
      parsed.signature.empty()) {
    SetError(error, "the VKEK acknowledgement is truncated or missing fields");
    return false;
  }
  parsed.timestamp_ms = static_cast<std::int64_t>(timestamp);
  if (!reader.AtEnd()) {
    SetError(error, "the VKEK acknowledgement has trailing bytes");
    return false;
  }
  if (!Canonical(wire, SerializeVkekAck(parsed), "the VKEK acknowledgement", error))
    return false;
  *ack = std::move(parsed);
  return true;
}

gm::Bytes ClientHelloSignedData(const ClientHello& hello) {
  Bytes body;
  std::string error;
  if (!SerializeClientHelloBody(hello, false, &body, &error))
    return {};
  return SignedData(kDomainClientHello, body);
}

gm::Bytes ServerHelloSignedData(const ServerHello& hello) {
  Bytes body;
  std::string error;
  if (!SerializeServerHelloBody(hello, false, &body, &error))
    return {};
  return SignedData(kDomainServerHello, body);
}

gm::Bytes VkekDeliverySignedData(const VkekDelivery& delivery) {
  Bytes body;
  std::string error;
  if (!SerializeDeliveryBody(delivery, false, &body, &error))
    return {};
  return SignedData(kDomainDelivery, body);
}

gm::Bytes VkekAckSignedData(const VkekAck& ack) {
  Bytes body;
  std::string error;
  if (!SerializeAckBody(ack, false, &body, &error))
    return {};
  return SignedData(kDomainAck, body);
}

gm::Bytes ComputeTranscriptHash(const gm::Bytes& client_hello_wire,
                                const gm::Bytes& server_hello_wire) {
  if (client_hello_wire.size() > std::numeric_limits<std::uint32_t>::max() ||
      server_hello_wire.size() > std::numeric_limits<std::uint32_t>::max())
    return {};
  Writer writer;
  writer.Raw(ToBytes(kDomainTranscript));
  writer.U32(static_cast<std::uint32_t>(client_hello_wire.size()));
  writer.Raw(client_hello_wire);
  writer.U32(static_cast<std::uint32_t>(server_hello_wire.size()));
  writer.Raw(server_hello_wire);
  Bytes digest = gm::Sm3(writer.data().data(), writer.data().size());
  return digest.size() == gm::kSm3DigestSize ? digest : Bytes{};
}

std::string EncodeMessageBase64(const gm::Bytes& wire) {
  if (wire.empty() || wire.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return {};
  const std::size_t needed = 4 * ((wire.size() + 2) / 3);
  std::string text(needed + 1, '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text.data()), wire.data(),
                                      static_cast<int>(wire.size()));
  if (written <= 0)
    return {};
  text.resize(static_cast<std::size_t>(written));
  return text;
}

bool DecodeMessageBase64(std::string_view text, gm::Bytes* wire, std::string* error) {
  if (!wire) {
    SetError(error, "the base64 output is null");
    return false;
  }
  std::string compact;
  compact.reserve(text.size());
  for (char character : text) {
    if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
      continue;
    compact.push_back(character);
  }
  if (compact.empty()) {
    SetError(error, "the base64 message is empty");
    return false;
  }
  if (compact.size() % 4 != 0) {
    SetError(error, "the base64 message length is not a multiple of four");
    return false;
  }
  std::size_t padding = 0;
  if (compact.size() >= 1 && compact[compact.size() - 1] == '=')
    ++padding;
  if (compact.size() >= 2 && compact[compact.size() - 2] == '=')
    ++padding;

  std::vector<unsigned char> decoded(3 * (compact.size() / 4) + 1);
  const int written =
      EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(compact.data()),
                      static_cast<int>(compact.size()));
  if (written < 0 || static_cast<std::size_t>(written) < padding) {
    SetError(error, "the base64 message is invalid");
    return false;
  }
  decoded.resize(static_cast<std::size_t>(written) - padding);
  wire->assign(decoded.begin(), decoded.end());
  return true;
}

// ---------------------------------------------------------------------------
// VkekManager
// ---------------------------------------------------------------------------

struct VkekManager::Impl {
  VkekConfig config;
  bool configured = false;
  bool locked = false;
  int consecutive_failures = 0;
  VkekStats stats;

  bool has_pending = false;
  Bytes pending_wire;
  Bytes pending_nonce;

  bool has_session = false;
  Bytes session_id;
  Bytes device_nonce;
  Bytes platform_nonce;
  Bytes transcript;
  NegotiatedSignature signature;
  std::uint16_t cipher_suite = 0;

  bool delivery_seen = false;
  std::uint64_t last_sequence = 0;

  bool has_key = false;
  Bytes key;
  VkekKeyInfo key_info;

  ~Impl() {
    Wipe(&key);
    Wipe(&pending_nonce);
    WipeString(&config.device_private_key_pem);
  }

  std::int64_t NowMs() const {
    if (config.now_ms)
      return config.now_ms();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  bool FreshTimestamp(std::int64_t timestamp) const {
    if (timestamp <= 0)
      return false;
    const std::int64_t now = NowMs();
    const std::int64_t skew = config.allowed_clock_skew_ms;
    if (timestamp > now)
      return timestamp - now <= skew;
    return now - timestamp <= skew;
  }

  bool VerifyPlatform(std::string_view signed_data, const Bytes& signature_bytes,
                      std::string* error) const {
    if (signed_data.empty()) {
      SetError(error, "the signed message is empty");
      return false;
    }
    if (!config.platform_certificate_pem.empty())
      return gm::Sm2VerifyCertificate(config.platform_certificate_pem, signed_data, signature_bytes,
                                      error);
    return gm::Sm2Verify(config.platform_public_key_pem, signed_data, signature_bytes, error);
  }

  // `bound` is true only after the message has proved knowledge of the current
  // session's random nonce/transcript. Counting unbound junk towards the lock
  // would let any host on the signalling network lock the device.
  VkekStatus Fail(VkekStatus status, bool delivery, bool bound, std::string detail,
                  std::string* error) {
    ++stats.failures;
    if (delivery)
      ++stats.deliveries_rejected;
    if (status == VkekStatus::kReplay)
      ++stats.replay_rejections;
    if (status == VkekStatus::kKeyExpired)
      ++stats.expired_rejections;
    if (bound) {
      ++consecutive_failures;
      if (consecutive_failures >= config.max_consecutive_failures)
        locked = true;
    }
    if (error) {
      *error = std::move(detail);
      *error += " (";
      *error += VkekStatusName(status);
      *error += ")";
    }
    return status;
  }

  bool BuildAck(const VkekDelivery& delivery, std::uint16_t status, Bytes* wire,
                std::string* error) const {
    VkekAck ack;
    ack.session_id = delivery.session_id;
    ack.key_id = delivery.key_id;
    ack.key_version = delivery.key_version;
    ack.sequence = delivery.sequence;
    ack.status = status;
    ack.timestamp_ms = NowMs();
    if (!gm::Sm2Sign(config.device_private_key_pem, BytesView(VkekAckSignedData(ack)),
                     &ack.signature, error))
      return false;
    Bytes result = SerializeVkekAck(ack, error);
    if (result.empty())
      return false;
    *wire = std::move(result);
    return true;
  }

  void ClearNegotiation() {
    has_session = false;
    session_id.clear();
    device_nonce.clear();
    platform_nonce.clear();
    transcript.clear();
    signature = NegotiatedSignature{};
    cipher_suite = 0;
    delivery_seen = false;
    last_sequence = 0;
  }

  void ClearPending() {
    has_pending = false;
    pending_wire.clear();
    Wipe(&pending_nonce);
  }

  void InstallKey(gm::Bytes key_bytes, const VkekDelivery& delivery) {
    if (has_key) {
      Wipe(&key);
      ++stats.keys_destroyed;
      ++stats.rotations;
    }
    key = std::move(key_bytes);
    key_info = VkekKeyInfo{};
    key_info.has_key = true;
    key_info.key_id = delivery.key_id;
    key_info.key_version = delivery.key_version;
    key_info.sequence = delivery.sequence;
    key_info.not_before_ms = delivery.not_before_ms;
    key_info.not_after_ms = delivery.not_after_ms;
    key_info.key_size = key.size();
    has_key = true;
  }
};

VkekManager::VkekManager() : impl_(std::make_unique<Impl>()) {}
VkekManager::~VkekManager() = default;
VkekManager::VkekManager(VkekManager&&) noexcept = default;
VkekManager& VkekManager::operator=(VkekManager&&) noexcept = default;

bool VkekManager::Configure(VkekConfig config, std::string* error) {
  if (config.device_id.empty()) {
    SetError(error, "the device id is required");
    return false;
  }
  if (config.device_id.size() > kMaxDeviceIdSize) {
    SetError(error, "the device id is too long");
    return false;
  }
  if (config.device_private_key_pem.empty()) {
    SetError(error, "the device SM2 private key is required");
    return false;
  }
  if (config.platform_certificate_pem.empty() && config.platform_public_key_pem.empty()) {
    SetError(error, "a platform certificate or public key is required");
    return false;
  }
  if (!config.trusted_roots_pem.empty() && config.platform_certificate_pem.empty()) {
    SetError(error, "trusted roots require a platform certificate");
    return false;
  }
  if (config.allowed_clock_skew_ms < 0) {
    SetError(error, "the allowed clock skew cannot be negative");
    return false;
  }
  if (config.max_consecutive_failures < 1)
    config.max_consecutive_failures = 1;
  if (config.max_consecutive_failures > 1000)
    config.max_consecutive_failures = 1000;
  if (!config.trusted_roots_pem.empty()) {
    gm::ChainValidationResult result;
    if (!gm::ValidateCertificateChain(config.platform_certificate_pem, config.platform_chain_pem,
                                      config.trusted_roots_pem, &result)) {
      SetError(error, "the platform certificate chain is invalid: " + result.error);
      return false;
    }
  }

  // Prove that the private key is SM2 and, when an identity certificate is
  // provisioned, that the certificate's public key belongs to this key.
  static constexpr std::string_view kProbe = "uvnvr-gb35114-vkek-config-v1";
  Bytes probe_signature;
  if (!gm::Sm2Sign(config.device_private_key_pem, kProbe, &probe_signature, error))
    return false;
  if (!config.device_certificate_pem.empty() &&
      !gm::Sm2VerifyCertificate(config.device_certificate_pem, kProbe, probe_signature, error)) {
    std::string detail = error ? *error : std::string();
    SetError(error, "the device certificate does not match the private key: " + detail);
    return false;
  }

  Wipe(&impl_->key);
  WipeString(&impl_->config.device_private_key_pem);
  impl_->has_key = false;
  impl_->key_info = VkekKeyInfo{};
  impl_->ClearPending();
  impl_->ClearNegotiation();
  impl_->config = std::move(config);
  impl_->configured = true;
  impl_->locked = false;
  impl_->consecutive_failures = 0;
  return true;
}

bool VkekManager::IsConfigured() const {
  return impl_->configured;
}
bool VkekManager::IsLocked() const {
  return impl_->locked;
}

void VkekManager::ResetFailureLock() {
  impl_->locked = false;
  impl_->consecutive_failures = 0;
}

bool VkekManager::CreateClientHello(gm::Bytes* wire, std::string* error) {
  if (!wire) {
    SetError(error, "the client hello output is null");
    return false;
  }
  if (!impl_->configured) {
    SetError(error, "the VKEK manager is not configured");
    return false;
  }
  if (impl_->locked) {
    SetError(error, "the VKEK manager is locked after repeated failures");
    return false;
  }

  ClientHello hello;
  hello.device_id = impl_->config.device_id;
  if (!RandomBytes(kNonceSize, &hello.device_nonce, "device nonce", error))
    return false;
  hello.timestamp_ms = impl_->NowMs();
  hello.signature_suites = {kSignatureSuiteSm2Sm3};
  hello.cipher_suites = {kCipherSuiteSm4Cbc, kCipherSuiteSm4Ctr};
  hello.sign_modes = {kSignModeIFrames, kSignModeAllFrames};
  if (!impl_->config.device_certificate_pem.empty()) {
    if (!gm::CertificateFingerprintSm3(impl_->config.device_certificate_pem,
                                       &hello.certificate_fingerprint, error))
      return false;
  } else {
    hello.certificate_fingerprint.assign(kFingerprintSize, 0);
  }
  if (!gm::Sm2Sign(impl_->config.device_private_key_pem, BytesView(ClientHelloSignedData(hello)),
                   &hello.signature, error))
    return false;
  Bytes encoded = SerializeClientHello(hello, error);
  if (encoded.empty())
    return false;

  impl_->ClearPending();
  impl_->ClearNegotiation();
  impl_->has_pending = true;
  impl_->pending_wire = encoded;
  impl_->pending_nonce = hello.device_nonce;
  ++impl_->stats.offers_created;
  *wire = std::move(encoded);
  return true;
}

const gm::Bytes& VkekManager::pending_client_hello() const {
  return impl_->pending_wire;
}
const gm::Bytes& VkekManager::pending_device_nonce() const {
  return impl_->pending_nonce;
}

VkekStatus VkekManager::ProcessServerHello(const gm::Bytes& wire, std::string* error) {
  if (!impl_->configured)
    return impl_->Fail(VkekStatus::kNotConfigured, false, false,
                       "the VKEK manager is not configured", error);
  if (impl_->locked) {
    SetError(error, "the VKEK manager is locked after repeated failures");
    return VkekStatus::kLocked;
  }
  if (!impl_->has_pending)
    return impl_->Fail(VkekStatus::kNoPendingOffer, false, false,
                       "no ClientHello is waiting for a platform reply", error);

  ServerHello hello;
  std::string parse_error;
  if (!ParseServerHello(wire, &hello, &parse_error))
    return impl_->Fail(VkekStatus::kMalformed, false, false, parse_error, error);
  if (!BytesEqual(hello.device_nonce, impl_->pending_nonce))
    return impl_->Fail(VkekStatus::kBadNonce, false, false,
                       "the platform did not echo the current device nonce", error);
  if (!impl_->FreshTimestamp(hello.timestamp_ms))
    return impl_->Fail(VkekStatus::kStaleTimestamp, false, true,
                       "the server hello timestamp is outside the allowed clock skew", error);

  // The device only offers SM2/SM3, SM4-CBC/CTR and both A-level signing
  // granularities, so the reply is checked against those explicit lists.
  const bool suite_ok = hello.signature_suite == kSignatureSuiteSm2Sm3;
  const bool cipher_ok =
      hello.cipher_suite == kCipherSuiteSm4Cbc || hello.cipher_suite == kCipherSuiteSm4Ctr;
  const bool mode_ok = hello.sign_mode == kSignModeIFrames || hello.sign_mode == kSignModeAllFrames;
  if (!suite_ok || !cipher_ok || !mode_ok || hello.record_version != kSignatureRecordVersion)
    return impl_->Fail(VkekStatus::kUnsupportedAlgorithm, false, true,
                       "the platform selected an unsupported suite, mode or record version", error);

  bool all_zero = std::all_of(hello.session_id.begin(), hello.session_id.end(),
                              [](unsigned char value) { return value == 0; });
  if (all_zero)
    return impl_->Fail(VkekStatus::kMalformed, false, true, "the session id is all zeroes", error);

  if (!impl_->VerifyPlatform(BytesView(ServerHelloSignedData(hello)), hello.signature,
                             &parse_error))
    return impl_->Fail(VkekStatus::kBadSignature, false, true,
                       "the platform server hello signature is invalid: " + parse_error, error);

  Bytes transcript = ComputeTranscriptHash(impl_->pending_wire, wire);
  if (transcript.size() != gm::kSm3DigestSize)
    return impl_->Fail(VkekStatus::kInternalError, false, false,
                       "cannot compute the negotiation transcript hash", error);

  impl_->ClearNegotiation();
  impl_->has_session = true;
  impl_->session_id = hello.session_id;
  impl_->device_nonce = hello.device_nonce;
  impl_->platform_nonce = hello.platform_nonce;
  impl_->transcript = std::move(transcript);
  impl_->signature.suite = hello.signature_suite;
  impl_->signature.sign_mode = hello.sign_mode;
  impl_->signature.record_version = hello.record_version;
  impl_->cipher_suite = hello.cipher_suite;
  impl_->ClearPending();
  ++impl_->stats.negotiations_completed;
  impl_->consecutive_failures = 0;
  return VkekStatus::kOk;
}

VkekStatus VkekManager::ProcessVkekDelivery(const gm::Bytes& wire, gm::Bytes* ack_wire,
                                            std::string* error) {
  if (!impl_->configured)
    return impl_->Fail(VkekStatus::kNotConfigured, true, false,
                       "the VKEK manager is not configured", error);
  if (impl_->locked) {
    SetError(error, "the VKEK manager is locked after repeated failures");
    return VkekStatus::kLocked;
  }
  if (!impl_->has_session)
    return impl_->Fail(VkekStatus::kNotNegotiated, true, false,
                       "no platform session has been negotiated", error);

  VkekDelivery delivery;
  std::string detail;
  if (!ParseVkekDelivery(wire, &delivery, &detail))
    return impl_->Fail(VkekStatus::kMalformed, true, false, detail, error);
  if (!BytesEqual(delivery.session_id, impl_->session_id))
    return impl_->Fail(VkekStatus::kUnknownSession, true, false,
                       "the VKEK belongs to a different session", error);
  if (!BytesEqual(delivery.device_nonce, impl_->device_nonce))
    return impl_->Fail(VkekStatus::kBadNonce, true, false,
                       "the VKEK does not echo the negotiated device nonce", error);
  if (!BytesEqual(delivery.transcript_hash, impl_->transcript))
    return impl_->Fail(VkekStatus::kBadTranscript, true, false,
                       "the VKEK transcript does not match this negotiation", error);
  if (!impl_->FreshTimestamp(delivery.timestamp_ms))
    return impl_->Fail(VkekStatus::kStaleTimestamp, true, true,
                       "the VKEK timestamp is outside the allowed clock skew", error);

  if (!impl_->VerifyPlatform(BytesView(VkekDeliverySignedData(delivery)), delivery.signature,
                             &detail))
    return impl_->Fail(VkekStatus::kBadSignature, true, true,
                       "the VKEK signature is invalid: " + detail, error);

  const std::int64_t now = impl_->NowMs();
  if (delivery.not_before_ms <= 0 || delivery.not_after_ms <= 0 ||
      delivery.not_before_ms >= delivery.not_after_ms)
    return impl_->Fail(VkekStatus::kInvalidKey, true, true,
                       "the VKEK validity window is empty, inverted or negative", error);
  if (now < delivery.not_before_ms)
    return impl_->Fail(VkekStatus::kInvalidKey, true, true, "the VKEK is not valid yet", error);
  if (now > delivery.not_after_ms)
    return impl_->Fail(VkekStatus::kKeyExpired, true, true, "the VKEK has expired", error);

  if (delivery.sequence == 0)
    return impl_->Fail(VkekStatus::kMalformed, true, true, "the VKEK sequence is zero", error);
  if (impl_->delivery_seen && delivery.sequence <= impl_->last_sequence)
    return impl_->Fail(VkekStatus::kReplay, true, true,
                       "the VKEK sequence was already accepted or superseded", error);

  if (impl_->has_key) {
    if (delivery.key_id <= impl_->key_info.key_id ||
        delivery.key_version <= impl_->key_info.key_version) {
      return impl_->Fail(VkekStatus::kKeyRotationViolation, true, true,
                         "a rotated VKEK must increase both key id and key version", error);
    }
  } else if (delivery.key_id == 0 || delivery.key_version == 0) {
    return impl_->Fail(VkekStatus::kInvalidKey, true, true,
                       "a VKEK must have a non-zero key id and version", error);
  }

  Bytes key;
  if (!gm::Sm2Decrypt(impl_->config.device_private_key_pem, delivery.encrypted_vkek, &key,
                      &detail)) {
    return impl_->Fail(VkekStatus::kDecryptFailed, true, true,
                       "cannot decrypt the VKEK with the device private key: " + detail, error);
  }
  if (key.size() != kVkekSize) {
    Wipe(&key);
    return impl_->Fail(VkekStatus::kInvalidKey, true, true, "the decrypted VKEK is not 16 bytes",
                       error);
  }
  const bool key_all_zero =
      std::all_of(key.begin(), key.end(), [](unsigned char value) { return value == 0; });
  if (key_all_zero) {
    Wipe(&key);
    return impl_->Fail(VkekStatus::kInvalidKey, true, true, "the decrypted VKEK is all zeroes",
                       error);
  }

  Bytes ack;
  if (ack_wire != nullptr && !impl_->BuildAck(delivery, kVkekAckAccepted, &ack, &detail)) {
    Wipe(&key);
    return impl_->Fail(VkekStatus::kInternalError, true, false,
                       "cannot build the VKEK acknowledgement: " + detail, error);
  }

  impl_->InstallKey(std::move(key), delivery);
  impl_->delivery_seen = true;
  impl_->last_sequence = delivery.sequence;
  impl_->consecutive_failures = 0;
  ++impl_->stats.deliveries_accepted;
  if (ack_wire != nullptr)
    *ack_wire = std::move(ack);
  return VkekStatus::kOk;
}

bool VkekManager::GetSessionInfo(SessionInfo* info, std::string* error) const {
  if (!info) {
    SetError(error, "the session info output is null");
    return false;
  }
  if (!impl_->has_session) {
    SetError(error, "no platform session has been negotiated");
    return false;
  }
  SessionInfo result;
  result.negotiated = true;
  result.session_id = impl_->session_id;
  result.device_nonce = impl_->device_nonce;
  result.platform_nonce = impl_->platform_nonce;
  result.signature = impl_->signature;
  result.cipher_suite = impl_->cipher_suite;
  result.key = impl_->key_info;
  *info = std::move(result);
  return true;
}

VkekKeyInfo VkekManager::active_key_info() const {
  return impl_->key_info;
}

bool VkekManager::CopyActiveKey(gm::Bytes* key, VkekKeyInfo* info, std::string* error) const {
  if (!key) {
    SetError(error, "the VKEK output is null");
    return false;
  }
  if (!impl_->has_key) {
    SetError(error, "no VKEK has been installed");
    return false;
  }
  const std::int64_t now = impl_->NowMs();
  if (now < impl_->key_info.not_before_ms) {
    SetError(error, "the active VKEK is not valid yet");
    return false;
  }
  if (now > impl_->key_info.not_after_ms) {
    SetError(error, "the active VKEK has expired");
    return false;
  }
  *key = impl_->key;
  if (info)
    *info = impl_->key_info;
  return true;
}

bool VkekManager::GetKeyForStream(std::uint64_t key_id, gm::Bytes* key, std::string* error) const {
  if (!impl_->has_key) {
    SetError(error, "no VKEK has been installed");
    return false;
  }
  if (key_id != impl_->key_info.key_id) {
    SetError(error, "key id " + std::to_string(key_id) +
                        " has been retired; the active key id is " +
                        std::to_string(impl_->key_info.key_id));
    return false;
  }
  return CopyActiveKey(key, nullptr, error);
}

const VkekStats& VkekManager::stats() const {
  return impl_->stats;
}
int VkekManager::consecutive_failures() const {
  return impl_->consecutive_failures;
}
std::int64_t VkekManager::now_ms() const {
  return impl_->NowMs();
}

} // namespace uvnvr::gb35114
