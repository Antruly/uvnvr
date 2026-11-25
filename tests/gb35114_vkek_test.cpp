// Unit tests for the GB 35114 signature negotiation and VKEK distribution
// module. A small platform simulator signs the server hello, encrypts the VKEK
// with the device's SM2 public key and signs the delivery, so the tests cover
// the real exchange rather than only local serialisation.

#include "crypto/sm.h"
#include "gb35114/vkek.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace {

using uvnvr::gb35114::CipherSuiteName;
using uvnvr::gb35114::ClientHello;
using uvnvr::gb35114::ClientHelloSignedData;
using uvnvr::gb35114::ComputeTranscriptHash;
using uvnvr::gb35114::ParseClientHello;
using uvnvr::gb35114::ParseVkekAck;
using uvnvr::gb35114::SerializeServerHello;
using uvnvr::gb35114::SerializeVkekDelivery;
using uvnvr::gb35114::ServerHello;
using uvnvr::gb35114::ServerHelloSignedData;
using uvnvr::gb35114::SessionInfo;
using uvnvr::gb35114::SignatureSuiteName;
using uvnvr::gb35114::SignModeName;
using uvnvr::gb35114::VkekAck;
using uvnvr::gb35114::VkekAckSignedData;
using uvnvr::gb35114::VkekConfig;
using uvnvr::gb35114::VkekDelivery;
using uvnvr::gb35114::VkekDeliverySignedData;
using uvnvr::gb35114::VkekManager;
using uvnvr::gb35114::VkekStatus;
using uvnvr::gb35114::VkekStatusName;

namespace gm = uvnvr::gm;

int failures = 0;

constexpr std::int64_t kNow = 1793000000000LL; // 2026-10-24T...Z, fixed for tests.

void Expect(const std::string& name, const std::string& actual, const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
  ++failures;
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectSize(const std::string& name, std::size_t actual, std::size_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %zu, expected %zu\n", name.c_str(), actual, expected);
  ++failures;
}

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  if (haystack.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: '%s' does not contain '%s'\n", name.c_str(), haystack.c_str(),
               needle.c_str());
  ++failures;
}

void ExpectStatus(const std::string& name, VkekStatus actual, VkekStatus expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(),
               std::string(VkekStatusName(actual)).c_str(),
               std::string(VkekStatusName(expected)).c_str());
  ++failures;
}

void Step(const std::string& text) {
  std::printf("[ok] %s\n", text.c_str());
}

gm::Bytes Pattern(std::size_t size, unsigned char seed) {
  gm::Bytes result(size);
  for (std::size_t i = 0; i < size; ++i)
    result[i] = static_cast<unsigned char>(seed + i * 7 + 1);
  return result;
}

std::string_view BytesView(const gm::Bytes& bytes) {
  static const char kEmpty = '\0';
  return std::string_view(bytes.empty() ? &kEmpty : reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

std::string Hex(const gm::Bytes& bytes) {
  return gm::ToHex(bytes);
}

struct Fixture {
  gm::Sm2KeyPair device;
  gm::Sm2KeyPair platform;
  gm::Sm2KeyPair wrong;
  std::int64_t now = kNow;
  VkekManager manager;
  bool ready = false;

  explicit Fixture(int max_failures = 5) {
    std::string error;
    if (!gm::GenerateSm2KeyPair(&device, &error) || !gm::GenerateSm2KeyPair(&platform, &error) ||
        !gm::GenerateSm2KeyPair(&wrong, &error)) {
      std::fprintf(stderr, "FAIL key generation: %s\n", error.c_str());
      ++failures;
      return;
    }
    VkekConfig config;
    config.device_id = "34020000001320000001";
    config.device_private_key_pem = device.private_key_pem;
    config.platform_public_key_pem = platform.public_key_pem;
    config.allowed_clock_skew_ms = 60 * 1000;
    config.max_consecutive_failures = max_failures;
    config.now_ms = [this]() { return now; };
    if (!manager.Configure(std::move(config), &error)) {
      std::fprintf(stderr, "FAIL configure: %s\n", error.c_str());
      ++failures;
      return;
    }
    ready = true;
  }
};

struct HelloOptions {
  gm::Bytes session_id;
  gm::Bytes platform_nonce;
  std::uint16_t signature_suite = uvnvr::gb35114::kSignatureSuiteSm2Sm3;
  std::uint16_t cipher_suite = uvnvr::gb35114::kCipherSuiteSm4Ctr;
  std::uint16_t sign_mode = uvnvr::gb35114::kSignModeIFrames;
  std::uint16_t record_version = uvnvr::gb35114::kSignatureRecordVersion;
};

struct DeliveryOptions {
  gm::Bytes session_id;
  gm::Bytes device_nonce;
  gm::Bytes transcript;
  std::uint64_t key_id = 1;
  std::uint32_t key_version = 1;
  std::uint64_t sequence = 1;
  std::int64_t not_before_ms = 0;
  std::int64_t not_after_ms = 0;
  std::int64_t timestamp_ms = 0;
  gm::Bytes vkek;
  bool encrypt = true;
  gm::Bytes ciphertext;
  bool sign_with_device = false;
};

struct Handshake {
  gm::Bytes offer;
  gm::Bytes server_hello;
  gm::Bytes device_nonce;
  gm::Bytes platform_nonce;
  gm::Bytes session_id;
  gm::Bytes transcript;
};

bool MakeServerHello(const Fixture& fixture, const ClientHello& hello, const HelloOptions& options,
                     gm::Bytes* wire) {
  ServerHello reply;
  reply.session_id = options.session_id;
  reply.platform_nonce = options.platform_nonce;
  reply.device_nonce = hello.device_nonce;
  reply.signature_suite = options.signature_suite;
  reply.cipher_suite = options.cipher_suite;
  reply.sign_mode = options.sign_mode;
  reply.record_version = options.record_version;
  reply.timestamp_ms = fixture.now;
  std::string error;
  if (!gm::Sm2Sign(fixture.platform.private_key_pem, BytesView(ServerHelloSignedData(reply)),
                   &reply.signature, &error)) {
    std::fprintf(stderr, "FAIL server hello signing: %s\n", error.c_str());
    ++failures;
    return false;
  }
  *wire = SerializeServerHello(reply, &error);
  if (wire->empty()) {
    std::fprintf(stderr, "FAIL server hello serialisation: %s\n", error.c_str());
    ++failures;
    return false;
  }
  return true;
}

bool MakeDelivery(const Fixture& fixture, const DeliveryOptions& options, gm::Bytes* wire) {
  VkekDelivery delivery;
  delivery.session_id = options.session_id;
  delivery.key_id = options.key_id;
  delivery.key_version = options.key_version;
  delivery.sequence = options.sequence;
  delivery.not_before_ms = options.not_before_ms;
  delivery.not_after_ms = options.not_after_ms;
  delivery.timestamp_ms = options.timestamp_ms;
  delivery.device_nonce = options.device_nonce;
  delivery.transcript_hash = options.transcript;
  std::string error;
  if (options.encrypt) {
    if (!gm::Sm2Encrypt(fixture.device.public_key_pem, options.vkek, &delivery.encrypted_vkek,
                        &error)) {
      std::fprintf(stderr, "FAIL VKEK encryption: %s\n", error.c_str());
      ++failures;
      return false;
    }
  } else {
    delivery.encrypted_vkek = options.ciphertext;
  }
  const std::string& signing_key =
      options.sign_with_device ? fixture.device.private_key_pem : fixture.platform.private_key_pem;
  if (!gm::Sm2Sign(signing_key, BytesView(VkekDeliverySignedData(delivery)), &delivery.signature,
                   &error)) {
    std::fprintf(stderr, "FAIL VKEK delivery signing: %s\n", error.c_str());
    ++failures;
    return false;
  }
  *wire = SerializeVkekDelivery(delivery, &error);
  if (wire->empty()) {
    std::fprintf(stderr, "FAIL VKEK delivery serialisation: %s\n", error.c_str());
    ++failures;
    return false;
  }
  return true;
}

bool DoHandshake(Fixture* fixture, Handshake* out) {
  std::string error;
  if (!fixture->manager.CreateClientHello(&out->offer, &error)) {
    std::fprintf(stderr, "FAIL create client hello: %s\n", error.c_str());
    ++failures;
    return false;
  }
  ClientHello hello;
  if (!ParseClientHello(out->offer, &hello, &error)) {
    std::fprintf(stderr, "FAIL parse client hello: %s\n", error.c_str());
    ++failures;
    return false;
  }
  if (!gm::Sm2Verify(fixture->device.public_key_pem, BytesView(ClientHelloSignedData(hello)),
                     hello.signature, &error)) {
    std::fprintf(stderr, "FAIL device client hello signature: %s\n", error.c_str());
    ++failures;
    return false;
  }
  out->device_nonce = hello.device_nonce;
  out->session_id = Pattern(uvnvr::gb35114::kSessionIdSize, 0xA0);
  out->platform_nonce = Pattern(uvnvr::gb35114::kNonceSize, 0x30);

  HelloOptions options;
  options.session_id = out->session_id;
  options.platform_nonce = out->platform_nonce;
  if (!MakeServerHello(*fixture, hello, options, &out->server_hello))
    return false;
  if (fixture->manager.ProcessServerHello(out->server_hello, &error) != VkekStatus::kOk) {
    std::fprintf(stderr, "FAIL process server hello: %s\n", error.c_str());
    ++failures;
    return false;
  }
  out->transcript = ComputeTranscriptHash(out->offer, out->server_hello);
  return out->transcript.size() == gm::kSm3DigestSize;
}

DeliveryOptions DefaultDelivery(const Fixture& fixture, const Handshake& handshake,
                                gm::Bytes vkek) {
  DeliveryOptions options;
  options.session_id = handshake.session_id;
  options.device_nonce = handshake.device_nonce;
  options.transcript = handshake.transcript;
  options.key_id = 1;
  options.key_version = 1;
  options.sequence = 1;
  options.not_before_ms = fixture.now - 1000;
  options.not_after_ms = fixture.now + 3600 * 1000;
  options.timestamp_ms = fixture.now;
  options.vkek = std::move(vkek);
  return options;
}

void TestHandshakeAndDelivery() {
  Fixture fixture;
  if (!fixture.ready) {
    ExpectBool("handshake fixture ready", false, true);
    return;
  }
  Handshake handshake;
  if (!DoHandshake(&fixture, &handshake)) {
    ExpectBool("handshake completes", false, true);
    return;
  }
  Step("handshake: device signature verified, platform signature verified");

  SessionInfo session;
  std::string error;
  ExpectBool("session info available", fixture.manager.GetSessionInfo(&session, &error), true);
  ExpectBool("session negotiated flag", session.negotiated, true);
  ExpectSize("session id size", session.session_id.size(), uvnvr::gb35114::kSessionIdSize);
  Expect("negotiated signature suite", std::string(SignatureSuiteName(session.signature.suite)),
         "SM2-SM3");
  Expect("negotiated sign mode", std::string(SignModeName(session.signature.sign_mode)),
         "I-frames");
  Expect("negotiated cipher suite", std::string(CipherSuiteName(session.cipher_suite)), "SM4-CTR");
  Expect("negotiated record version", std::to_string(session.signature.record_version), "1");
  Step("signature negotiation: SM2-SM3, I-frames, record v1, cipher SM4-CTR");

  gm::Bytes vkek = Pattern(uvnvr::gb35114::kVkekSize, 0x51);
  DeliveryOptions options = DefaultDelivery(fixture, handshake, vkek);
  gm::Bytes delivery;
  ExpectBool("build VKEK delivery", MakeDelivery(fixture, options, &delivery), true);

  gm::Bytes ack_wire;
  ExpectStatus("VKEK delivery accepted",
               fixture.manager.ProcessVkekDelivery(delivery, &ack_wire, &error), VkekStatus::kOk);

  gm::Bytes active;
  ExpectBool("active key available", fixture.manager.CopyActiveKey(&active, nullptr, &error), true);
  Expect("active VKEK matches the distributed key", Hex(active), Hex(vkek));
  auto info = fixture.manager.active_key_info();
  ExpectBool("active key present", info.has_key, true);
  Expect("active key id", std::to_string(info.key_id), "1");
  Expect("active key version", std::to_string(info.key_version), "1");
  Step("VKEK key_id=1 version=1 installed (16-byte SM4 key)");

  ExpectBool("acknowledgement produced", !ack_wire.empty(), true);
  VkekAck ack;
  ExpectBool("acknowledgement parses", ParseVkekAck(ack_wire, &ack, &error), true);
  ExpectBool("acknowledgement status accepted", ack.status == uvnvr::gb35114::kVkekAckAccepted,
             true);
  ExpectBool("acknowledgement signature verifies",
             gm::Sm2Verify(fixture.device.public_key_pem, BytesView(VkekAckSignedData(ack)),
                           ack.signature, &error),
             true);
  Expect("acknowledgement key id", std::to_string(ack.key_id), "1");
  Step("device acknowledgement signed with the device SM2 key");

  const auto& stats = fixture.manager.stats();
  Expect("offers", std::to_string(stats.offers_created), "1");
  Expect("negotiations", std::to_string(stats.negotiations_completed), "1");
  Expect("accepted deliveries", std::to_string(stats.deliveries_accepted), "1");

  const std::string encoded = uvnvr::gb35114::EncodeMessageBase64(handshake.offer);
  gm::Bytes decoded;
  ExpectBool("base64 decodes", uvnvr::gb35114::DecodeMessageBase64(encoded, &decoded, &error),
             true);
  Expect("base64 round-trip", Hex(decoded), Hex(handshake.offer));
  ExpectBool("base64 decoder rejects an invalid length",
             uvnvr::gb35114::DecodeMessageBase64("abc", &decoded, &error), false);
  ExpectBool("base64 decoder rejects non-base64 characters",
             uvnvr::gb35114::DecodeMessageBase64("****", &decoded, &error), false);
  Step("SIP MESSAGE base64 body round-trip");
}

void TestReplayAndTampering() {
  Fixture fixture(100);
  if (!fixture.ready)
    return;
  Handshake handshake;
  if (!DoHandshake(&fixture, &handshake))
    return;

  DeliveryOptions options = DefaultDelivery(fixture, handshake, Pattern(16, 0x60));
  options.sequence = 5;
  gm::Bytes delivery;
  if (!MakeDelivery(fixture, options, &delivery))
    return;
  std::string error;
  ExpectStatus("delivery for replay test",
               fixture.manager.ProcessVkekDelivery(delivery, nullptr, &error), VkekStatus::kOk);

  ExpectStatus("replayed delivery rejected",
               fixture.manager.ProcessVkekDelivery(delivery, nullptr, &error), VkekStatus::kReplay);
  ExpectBool("replay counter incremented", fixture.manager.stats().replay_rejections >= 1, true);
  Step("replay of key_id=1 sequence=5 rejected: replay");

  DeliveryOptions stale = options;
  stale.sequence = 6;
  stale.vkek = Pattern(16, 0x61);
  stale.timestamp_ms = fixture.now - 10 * 60 * 1000;
  gm::Bytes wire;
  if (MakeDelivery(fixture, stale, &wire))
    ExpectStatus("stale delivery rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kStaleTimestamp);
  Step("stale timestamp rejected: stale-timestamp");

  DeliveryOptions wrong_nonce = options;
  wrong_nonce.sequence = 7;
  wrong_nonce.vkek = Pattern(16, 0x62);
  wrong_nonce.device_nonce = Pattern(uvnvr::gb35114::kNonceSize, 0xEE);
  if (MakeDelivery(fixture, wrong_nonce, &wire))
    ExpectStatus("wrong nonce rejected", fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kBadNonce);

  DeliveryOptions wrong_session = options;
  wrong_session.sequence = 8;
  wrong_session.vkek = Pattern(16, 0x63);
  wrong_session.session_id = Pattern(uvnvr::gb35114::kSessionIdSize, 0xEE);
  if (MakeDelivery(fixture, wrong_session, &wire))
    ExpectStatus("wrong session rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kUnknownSession);

  DeliveryOptions wrong_transcript = options;
  wrong_transcript.sequence = 9;
  wrong_transcript.vkek = Pattern(16, 0x64);
  wrong_transcript.transcript = Pattern(gm::kSm3DigestSize, 0xEE);
  if (MakeDelivery(fixture, wrong_transcript, &wire))
    ExpectStatus("wrong transcript rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kBadTranscript);
  Step("nonce/session/transcript binding rejects mismatched VKEK deliveries");

  DeliveryOptions bad_signature = options;
  bad_signature.sequence = 10;
  bad_signature.vkek = Pattern(16, 0x65);
  bad_signature.sign_with_device = true;
  if (MakeDelivery(fixture, bad_signature, &wire))
    ExpectStatus("wrong signer rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kBadSignature);
  Step("VKEK signed by a non-platform key rejected: bad-signature");

  gm::Bytes truncated(delivery.begin(), delivery.begin() + 10);
  ExpectStatus("truncated delivery rejected",
               fixture.manager.ProcessVkekDelivery(truncated, nullptr, &error),
               VkekStatus::kMalformed);
  gm::Bytes oversized(uvnvr::gb35114::kMaxMessageSize + 1, 0x47);
  ExpectStatus("oversized delivery rejected",
               fixture.manager.ProcessVkekDelivery(oversized, nullptr, &error),
               VkekStatus::kMalformed);

  // A second server hello cannot be replayed: the offer was already consumed.
  ExpectStatus("replayed server hello rejected",
               fixture.manager.ProcessServerHello(handshake.server_hello, &error),
               VkekStatus::kNoPendingOffer);
  Step("replayed server hello rejected: no-pending-offer");
}

void TestRotationRetiresOldKey() {
  Fixture fixture(100);
  if (!fixture.ready)
    return;
  Handshake handshake;
  if (!DoHandshake(&fixture, &handshake))
    return;

  std::string error;
  const gm::Bytes key_one = Pattern(uvnvr::gb35114::kVkekSize, 0x11);
  DeliveryOptions first = DefaultDelivery(fixture, handshake, key_one);
  gm::Bytes wire;
  if (!MakeDelivery(fixture, first, &wire))
    return;
  ExpectStatus("first VKEK installed", fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
               VkekStatus::kOk);

  // Media encrypted under the first key, using the crypto module directly.
  const gm::Bytes iv = Pattern(gm::kSm4IvSize, 0x90);
  const gm::Bytes plaintext = {'G', 'B', '3', '5', '1', '1', '4', '-', 'o', 'l', 'd'};
  gm::Bytes old_ciphertext;
  ExpectBool("old stream encrypted",
             gm::Sm4CbcEncrypt(key_one, iv, plaintext, &old_ciphertext, &error), true);

  const gm::Bytes key_two = Pattern(uvnvr::gb35114::kVkekSize, 0x22);
  DeliveryOptions second = DefaultDelivery(fixture, handshake, key_two);
  second.key_id = 2;
  second.key_version = 2;
  second.sequence = 2;
  if (!MakeDelivery(fixture, second, &wire))
    return;
  ExpectStatus("rotated VKEK installed", fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
               VkekStatus::kOk);
  Expect("rotation count", std::to_string(fixture.manager.stats().rotations), "1");
  Expect("destroyed key count", std::to_string(fixture.manager.stats().keys_destroyed), "1");
  Step("rotation: key_id=2 version=2 installed, key_id=1 zeroised");

  gm::Bytes active;
  ExpectBool("active key available after rotation",
             fixture.manager.CopyActiveKey(&active, nullptr, &error), true);
  Expect("active VKEK is the rotated key", Hex(active), Hex(key_two));

  gm::Bytes retired;
  ExpectBool("retired key id cannot be resolved",
             fixture.manager.GetKeyForStream(1, &retired, &error), false);
  ExpectContains("retired key error names the id", error, "retired");
  Step("GetKeyForStream(1) refused: key id 1 has been retired");

  gm::Bytes recovered;
  const bool recovered_ok = gm::Sm4CbcDecrypt(key_two, iv, old_ciphertext, &recovered, &error);
  ExpectBool("old stream is not decryptable with the new key",
             recovered_ok && recovered == plaintext, false);
  Step("ciphertext produced under key_id=1 no longer decrypts after rotation");

  ExpectStatus("replayed old delivery rejected",
               fixture.manager.ProcessVkekDelivery(wire, nullptr, &error), VkekStatus::kReplay);

  DeliveryOptions stale_id = DefaultDelivery(fixture, handshake, Pattern(16, 0x33));
  stale_id.key_id = 1;
  stale_id.key_version = 1;
  stale_id.sequence = 3;
  if (MakeDelivery(fixture, stale_id, &wire))
    ExpectStatus("old key id rejected", fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kKeyRotationViolation);

  DeliveryOptions stale_version = DefaultDelivery(fixture, handshake, Pattern(16, 0x34));
  stale_version.key_id = 3;
  stale_version.key_version = 2;
  stale_version.sequence = 3;
  if (MakeDelivery(fixture, stale_version, &wire))
    ExpectStatus("non-increasing key version rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kKeyRotationViolation);
  Step("non-increasing key id/version rejected: key-rotation-violation");

  DeliveryOptions expired = DefaultDelivery(fixture, handshake, Pattern(16, 0x35));
  expired.key_id = 3;
  expired.key_version = 3;
  expired.sequence = 3;
  expired.not_before_ms = fixture.now - 2000;
  expired.not_after_ms = fixture.now - 1000;
  if (MakeDelivery(fixture, expired, &wire))
    ExpectStatus("expired VKEK rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kKeyExpired);

  DeliveryOptions not_yet = DefaultDelivery(fixture, handshake, Pattern(16, 0x36));
  not_yet.key_id = 3;
  not_yet.key_version = 3;
  not_yet.sequence = 3;
  not_yet.not_before_ms = fixture.now + 1000;
  not_yet.not_after_ms = fixture.now + 5000;
  if (MakeDelivery(fixture, not_yet, &wire))
    ExpectStatus("not-yet-valid VKEK rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kInvalidKey);
  Step("VKEK validity window enforced");

  DeliveryOptions zero_key = DefaultDelivery(fixture, handshake, gm::Bytes(16, 0));
  zero_key.key_id = 3;
  zero_key.key_version = 3;
  zero_key.sequence = 3;
  if (MakeDelivery(fixture, zero_key, &wire))
    ExpectStatus("all-zero VKEK rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kInvalidKey);

  DeliveryOptions short_key = DefaultDelivery(fixture, handshake, Pattern(15, 0x37));
  short_key.key_id = 3;
  short_key.key_version = 3;
  short_key.sequence = 3;
  if (MakeDelivery(fixture, short_key, &wire))
    ExpectStatus("wrong-size VKEK rejected",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kInvalidKey);

  DeliveryOptions undecryptable = DefaultDelivery(fixture, handshake, {});
  undecryptable.key_id = 3;
  undecryptable.key_version = 3;
  undecryptable.sequence = 3;
  undecryptable.encrypt = false;
  undecryptable.ciphertext = Pattern(120, 0x77);
  if (MakeDelivery(fixture, undecryptable, &wire))
    ExpectStatus("ciphertext not decryptable by the device key",
                 fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
                 VkekStatus::kDecryptFailed);
  Step("invalid/undecryptable VKEK rejected");
}

void TestSignatureNegotiationFailures() {
  Fixture fixture(100);
  if (!fixture.ready)
    return;
  std::string error;
  gm::Bytes offer;
  if (!fixture.manager.CreateClientHello(&offer, &error))
    return;
  ClientHello hello;
  if (!ParseClientHello(offer, &hello, &error))
    return;

  HelloOptions unsupported;
  unsupported.session_id = Pattern(16, 0xA1);
  unsupported.platform_nonce = Pattern(32, 0x31);
  unsupported.signature_suite = 99;
  gm::Bytes wire;
  if (MakeServerHello(fixture, hello, unsupported, &wire))
    ExpectStatus("unsupported signature suite rejected",
                 fixture.manager.ProcessServerHello(wire, &error),
                 VkekStatus::kUnsupportedAlgorithm);

  HelloOptions unsupported_cipher = unsupported;
  unsupported_cipher.signature_suite = uvnvr::gb35114::kSignatureSuiteSm2Sm3;
  unsupported_cipher.cipher_suite = 99;
  if (MakeServerHello(fixture, hello, unsupported_cipher, &wire))
    ExpectStatus("unsupported cipher suite rejected",
                 fixture.manager.ProcessServerHello(wire, &error),
                 VkekStatus::kUnsupportedAlgorithm);

  HelloOptions unsupported_version = unsupported;
  unsupported_version.signature_suite = uvnvr::gb35114::kSignatureSuiteSm2Sm3;
  unsupported_version.record_version = 2;
  if (MakeServerHello(fixture, hello, unsupported_version, &wire))
    ExpectStatus("unsupported signature record version rejected",
                 fixture.manager.ProcessServerHello(wire, &error),
                 VkekStatus::kUnsupportedAlgorithm);
  Step("signature negotiation rejects unsupported suites/versions");

  HelloOptions valid;
  valid.session_id = Pattern(16, 0xA2);
  valid.platform_nonce = Pattern(32, 0x32);
  if (!MakeServerHello(fixture, hello, valid, &wire))
    return;
  gm::Bytes tampered = wire;
  tampered.back() ^= 0x01;
  ExpectStatus("modified platform signature rejected",
               fixture.manager.ProcessServerHello(tampered, &error), VkekStatus::kBadSignature);

  HelloOptions wrong_nonce;
  wrong_nonce.session_id = Pattern(16, 0xA3);
  wrong_nonce.platform_nonce = Pattern(32, 0x33);
  // The server hello always echoes the offer nonce; build one and patch it by
  // constructing the reply with a wrong device nonce through a manual sign.
  ServerHello reply;
  reply.session_id = wrong_nonce.session_id;
  reply.platform_nonce = wrong_nonce.platform_nonce;
  reply.device_nonce = Pattern(32, 0xEE);
  reply.timestamp_ms = fixture.now;
  if (gm::Sm2Sign(fixture.platform.private_key_pem, BytesView(ServerHelloSignedData(reply)),
                  &reply.signature, &error)) {
    gm::Bytes wrong_nonce_wire = SerializeServerHello(reply, &error);
    ExpectStatus("server hello with the wrong nonce rejected",
                 fixture.manager.ProcessServerHello(wrong_nonce_wire, &error),
                 VkekStatus::kBadNonce);
  }

  HelloOptions stale;
  stale.session_id = Pattern(16, 0xA4);
  stale.platform_nonce = Pattern(32, 0x34);
  // The helper always uses the current clock, so build a stale reply directly.
  ServerHello stale_reply;
  stale_reply.session_id = stale.session_id;
  stale_reply.platform_nonce = stale.platform_nonce;
  stale_reply.device_nonce = hello.device_nonce;
  stale_reply.timestamp_ms = fixture.now - 10 * 60 * 1000;
  if (gm::Sm2Sign(fixture.platform.private_key_pem, BytesView(ServerHelloSignedData(stale_reply)),
                  &stale_reply.signature, &error)) {
    gm::Bytes stale_wire = SerializeServerHello(stale_reply, &error);
    ExpectStatus("stale server hello rejected",
                 fixture.manager.ProcessServerHello(stale_wire, &error),
                 VkekStatus::kStaleTimestamp);
  }

  HelloOptions accepted;
  accepted.session_id = Pattern(16, 0xA5);
  accepted.platform_nonce = Pattern(32, 0x35);
  if (MakeServerHello(fixture, hello, accepted, &wire))
    ExpectStatus("negotiation succeeds after rejected candidates",
                 fixture.manager.ProcessServerHello(wire, &error), VkekStatus::kOk);
  Step("failed negotiation attempts do not consume the pending offer");
}

void TestFailureLockout() {
  Fixture fixture(2);
  if (!fixture.ready)
    return;
  Handshake handshake;
  if (!DoHandshake(&fixture, &handshake))
    return;

  std::string error;
  DeliveryOptions unbound = DefaultDelivery(fixture, handshake, Pattern(16, 0x40));
  unbound.session_id = Pattern(uvnvr::gb35114::kSessionIdSize, 0xEE);
  gm::Bytes unbound_wire;
  if (!MakeDelivery(fixture, unbound, &unbound_wire))
    return;
  for (int i = 0; i < 5; ++i) {
    ExpectStatus("unbound junk rejected",
                 fixture.manager.ProcessVkekDelivery(unbound_wire, nullptr, &error),
                 VkekStatus::kUnknownSession);
  }
  ExpectBool("unbound junk does not lock the device", fixture.manager.IsLocked(), false);
  Expect("unbound junk leaves the lock counter at zero",
         std::to_string(fixture.manager.consecutive_failures()), "0");
  Step("unbound junk is counted but cannot trip the lockout threshold");

  DeliveryOptions options = DefaultDelivery(fixture, handshake, Pattern(16, 0x41));
  options.sign_with_device = true;
  gm::Bytes wire;
  if (!MakeDelivery(fixture, options, &wire))
    return;
  ExpectStatus("first bad delivery", fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
               VkekStatus::kBadSignature);
  ExpectBool("not locked after one failure", fixture.manager.IsLocked(), false);
  ExpectStatus("second bad delivery", fixture.manager.ProcessVkekDelivery(wire, nullptr, &error),
               VkekStatus::kBadSignature);
  ExpectBool("locked after threshold", fixture.manager.IsLocked(), true);
  Expect("consecutive failures", std::to_string(fixture.manager.consecutive_failures()), "2");

  DeliveryOptions valid = DefaultDelivery(fixture, handshake, Pattern(16, 0x42));
  gm::Bytes valid_wire;
  if (MakeDelivery(fixture, valid, &valid_wire))
    ExpectStatus("delivery blocked while locked",
                 fixture.manager.ProcessVkekDelivery(valid_wire, nullptr, &error),
                 VkekStatus::kLocked);

  fixture.manager.ResetFailureLock();
  ExpectBool("reset clears the lock", fixture.manager.IsLocked(), false);
  if (!valid_wire.empty())
    ExpectStatus("delivery accepted after reset",
                 fixture.manager.ProcessVkekDelivery(valid_wire, nullptr, &error), VkekStatus::kOk);
  Step("failure lockout triggers at the configured threshold and resets explicitly");
}

void TestActiveKeyExpiry() {
  Fixture fixture(100);
  if (!fixture.ready)
    return;
  Handshake handshake;
  if (!DoHandshake(&fixture, &handshake))
    return;

  DeliveryOptions options = DefaultDelivery(fixture, handshake, Pattern(16, 0x71));
  options.not_before_ms = fixture.now - 1000;
  options.not_after_ms = fixture.now + 1000;
  gm::Bytes wire;
  if (!MakeDelivery(fixture, options, &wire))
    return;
  std::string error;
  ExpectStatus("short-lived VKEK installed",
               fixture.manager.ProcessVkekDelivery(wire, nullptr, &error), VkekStatus::kOk);

  gm::Bytes key;
  ExpectBool("key usable inside its window", fixture.manager.CopyActiveKey(&key, nullptr, &error),
             true);

  fixture.now += 2000;
  ExpectBool("expired active key refused", fixture.manager.CopyActiveKey(&key, nullptr, &error),
             false);
  ExpectContains("expiry error mentions the window", error, "expired");
  ExpectBool("stream lookup refused after expiry", fixture.manager.GetKeyForStream(1, &key, &error),
             false);
  Step("active VKEK is refused once its validity window ends");
}

void TestMalformedInputs() {
  Fixture fixture(100);
  if (!fixture.ready)
    return;
  std::string error;
  fixture.manager.CreateClientHello(nullptr, &error);
  ExpectContains("null output names the problem", error, "null");
  ExpectBool("manager reports configured", fixture.manager.IsConfigured(), true);
  Expect("status name replay", std::string(VkekStatusName(VkekStatus::kReplay)), "replay");
  Expect("status name rotation", std::string(VkekStatusName(VkekStatus::kKeyRotationViolation)),
         "key-rotation-violation");
  Step("configuration and status helpers");
}

} // namespace

int main() {
  TestHandshakeAndDelivery();
  TestReplayAndTampering();
  TestRotationRetiresOldKey();
  TestSignatureNegotiationFailures();
  TestFailureLockout();
  TestActiveKeyExpiry();
  TestMalformedInputs();

  if (failures == 0) {
    std::printf("gb35114_vkek_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "gb35114_vkek_test: %d checks failed\n", failures);
  return 1;
}
