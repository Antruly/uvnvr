// Unit tests for the GB 35114 B-level module: SM2 VKEK wrapping, HKDF-SM3
// work-key derivation, access-unit splitting, SM4-CTR frame sealing and the
// authentication failure paths.
//
// Everything here is local; the end-to-end picture check (ffmpeg decode plus a
// PS/RTP round trip) lives in tests/integration/gb35114_sm4.py.

#include "crypto/sm.h"
#include "gb35114/es_crypto.h"
#include "gb35114/stream_keys.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uvnvr::gb35114::SealedFrameInfo;
using uvnvr::gb35114::VideoKeys;
using uvnvr::gb35114::StreamKeyEnvelope;
using uvnvr::gm::Bytes;

int failures = 0;

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

void ExpectBytes(const std::string& name, const Bytes& actual, const Bytes& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: byte strings differ (%zu vs %zu bytes)\n", name.c_str(),
               actual.size(), expected.size());
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

Bytes Hex(std::string_view hex) {
  Bytes result;
  uvnvr::gm::FromHex(hex, &result);
  return result;
}

struct PkeyDeleter {
  void operator()(EVP_PKEY* value) const {
    if (value)
      EVP_PKEY_free(value);
  }
};
struct X509Deleter {
  void operator()(X509* value) const {
    if (value)
      X509_free(value);
  }
};
struct BioDeleter {
  void operator()(BIO* value) const {
    if (value)
      BIO_free(value);
  }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

PkeyPtr LoadPrivateKey(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio)
    return {};
  return PkeyPtr(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::string MakeSelfSignedCertificate(const std::string& private_key_pem) {
  PkeyPtr key = LoadPrivateKey(private_key_pem);
  if (!key)
    return {};
  X509Ptr certificate(X509_new());
  if (!certificate)
    return {};
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600L * 24 * 30);
  X509_set_pubkey(certificate.get(), key.get());
  X509_NAME* name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                             reinterpret_cast<const unsigned char*>("34020000001320000001"), -1, -1,
                             0);
  X509_set_issuer_name(certificate.get(), name);
  if (X509_sign(certificate.get(), key.get(), EVP_sm3()) <= 0)
    return {};

  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_X509(bio.get(), certificate.get()) != 1)
    return {};
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  if (length <= 0 || !data)
    return {};
  return std::string(data, static_cast<std::size_t>(length));
}

void AppendNal(Bytes* out, std::uint8_t header, std::initializer_list<unsigned char> payload) {
  const unsigned char start_code[4] = {0, 0, 0, 1};
  out->insert(out->end(), start_code, start_code + 4);
  out->push_back(header);
  out->insert(out->end(), payload.begin(), payload.end());
}

Bytes MakeTestEs() {
  Bytes es;
  AppendNal(&es, 0x09, {0xF0});             // AUD
  AppendNal(&es, 0x67, {0x42, 0x00, 0x1E}); // SPS
  AppendNal(&es, 0x68, {0xCE, 0x3C, 0x80}); // PPS
  AppendNal(&es, 0x06, {0x05, 0x01});       // SEI
  AppendNal(&es, 0x65, {0x80, 0x11, 0x22}); // IDR, first_mb_in_slice = 0
  AppendNal(&es, 0x41, {0x80, 0x33});       // P, first_mb_in_slice = 0
  AppendNal(&es, 0x41, {0x80, 0x44});       // P, first_mb_in_slice = 0
  return es;
}

Bytes Flip(const Bytes& input, std::size_t offset) {
  Bytes copy = input;
  copy[offset] ^= 0x01;
  return copy;
}

void RunSm2EncryptionTests(uvnvr::gm::Sm2KeyPair* device) {
  std::string error;
  ExpectBool("generate an SM2 key pair", uvnvr::gm::GenerateSm2KeyPair(device, &error), true);

  const Bytes plaintext = Hex("00112233445566778899aabbccddeeff");
  Bytes ciphertext;
  ExpectBool("SM2 encrypt a 16-byte VKEK",
             uvnvr::gm::Sm2Encrypt(device->public_key_pem, plaintext, &ciphertext, &error), true);
  // C1 (65) + C3 (32) + C2 (16) in the standard wire form.
  ExpectSize("SM2 C1||C3||C2 length", ciphertext.size(), 65 + 32 + 16);
  if (!ciphertext.empty())
    ExpectSize("SM2 C1 uncompressed point", ciphertext.front(), 0x04);

  Bytes decrypted;
  ExpectBool("SM2 decrypt round trip",
             uvnvr::gm::Sm2Decrypt(device->private_key_pem, ciphertext, &decrypted, &error), true);
  ExpectBytes("SM2 plaintext round trip", decrypted, plaintext);

  uvnvr::gm::Sm2KeyPair other;
  uvnvr::gm::GenerateSm2KeyPair(&other, &error);
  ExpectBool("SM2 rejects the wrong private key",
             uvnvr::gm::Sm2Decrypt(other.private_key_pem, ciphertext, &decrypted, &error), false);
  if (!ciphertext.empty()) {
    ExpectBool("SM2 rejects a modified ciphertext",
               uvnvr::gm::Sm2Decrypt(device->private_key_pem,
                                     Flip(ciphertext, ciphertext.size() - 1), &decrypted, &error),
               false);
  }
  ExpectBool("SM2 rejects a truncated ciphertext",
             uvnvr::gm::Sm2Decrypt(
                 device->private_key_pem,
                 Bytes(ciphertext.begin(),
                       ciphertext.begin() + std::min<std::size_t>(ciphertext.size(), 20)),
                 &decrypted, &error),
             false);

  const std::string certificate = MakeSelfSignedCertificate(device->private_key_pem);
  ExpectBool("build an SM2 certificate fixture", !certificate.empty(), true);
  Bytes certificate_ciphertext;
  ExpectBool(
      "SM2 encrypt to a certificate",
      uvnvr::gm::Sm2EncryptCertificate(certificate, plaintext, &certificate_ciphertext, &error),
      true);
  ExpectBool(
      "SM2 decrypt the certificate-wrapped VKEK",
      uvnvr::gm::Sm2Decrypt(device->private_key_pem, certificate_ciphertext, &decrypted, &error),
      true);
  ExpectBytes("certificate round trip", decrypted, plaintext);

  // HMAC-SM3 known answer cross-checked with
  //   printf abc | openssl dgst -sm3 -mac HMAC -macopt hexkey:000102...0f
  const Bytes key = Hex("000102030405060708090a0b0c0d0e0f");
  const Bytes message = {'a', 'b', 'c'};
  Expect("HMAC-SM3 known answer", uvnvr::gm::HmacSm3Hex(key, message),
         "83fd35b3ff6211428a38c070431ad42c23a86eaca25a5ea81a1ded4704a12c7c");

  Bytes random_a;
  Bytes random_b;
  ExpectBool("CSPRNG produces bytes",
             uvnvr::gm::RandomBytes(16, &random_a, &error) &&
                 uvnvr::gm::RandomBytes(16, &random_b, &error),
             true);
  ExpectSize("CSPRNG output size", random_a.size(), 16);
  ExpectBool("CSPRNG output differs between calls", random_a != random_b, true);
}

void RunVkekTests(const uvnvr::gm::Sm2KeyPair& device) {
  std::string error;
  const std::string stream = "34020000001320000001";
  const std::uint32_t key_id = 7;

  Bytes envelope;
  Bytes platform_vkek;
  Bytes platform_salt;
  ExpectBool("create a VKEK envelope",
             uvnvr::gb35114::CreateStreamKeyEnvelope(device.public_key_pem, key_id, &envelope,
                                                &platform_vkek, &platform_salt, &error),
             true);
  ExpectSize("VKEK size", platform_vkek.size(), uvnvr::gb35114::kStreamVkekSize);
  ExpectSize("VKEK salt size", platform_salt.size(), uvnvr::gb35114::kStreamKeySaltSize);

  StreamKeyEnvelope parsed;
  ExpectBool("parse the VKEK envelope",
             uvnvr::gb35114::ParseStreamKeyEnvelope(envelope, &parsed, &error), true);
  ExpectSize("parsed key id", parsed.key_id, key_id);
  ExpectBytes("parsed salt", parsed.salt, platform_salt);

  Bytes device_vkek;
  StreamKeyEnvelope device_parsed;
  ExpectBool("device unwraps the VKEK",
             uvnvr::gb35114::UnwrapStreamVkek(device.private_key_pem, envelope, &device_vkek,
                                        &device_parsed, &error),
             true);
  ExpectBytes("unwrapped VKEK matches", device_vkek, platform_vkek);
  ExpectBytes("unwrapped salt matches", device_parsed.salt, platform_salt);

  VideoKeys platform_keys;
  VideoKeys device_keys;
  ExpectBool("platform derives its keys",
             uvnvr::gb35114::DeriveStreamKeys(platform_vkek, key_id, platform_salt, stream,
                                             &platform_keys, &error),
             true);
  ExpectBool(
      "device derives its keys",
      uvnvr::gb35114::DeriveStreamKeys(device_vkek, device_parsed, stream, &device_keys, &error),
      true);
  ExpectBytes("derived video key matches", platform_keys.video_key, device_keys.video_key);
  ExpectBytes("derived IV seed matches", platform_keys.iv_seed, device_keys.iv_seed);
  ExpectBytes("derived MAC key matches", platform_keys.mac_key, device_keys.mac_key);
  ExpectSize("derived video key size", platform_keys.video_key.size(), uvnvr::gm::kSm4KeySize);

  VideoKeys other_stream_keys;
  ExpectBool("derive keys for another stream",
             uvnvr::gb35114::DeriveStreamKeys(platform_vkek, key_id, platform_salt,
                                             "34020000001320000002", &other_stream_keys, &error),
             true);
  ExpectBool("stream id is bound into the key",
             platform_keys.video_key != other_stream_keys.video_key, true);

  VideoKeys other_id_keys;
  ExpectBool("derive keys for another key id",
             uvnvr::gb35114::DeriveStreamKeys(platform_vkek, key_id + 1, platform_salt, stream,
                                             &other_id_keys, &error),
             true);
  ExpectBool("key id is bound into the key", platform_keys.video_key != other_id_keys.video_key,
             true);

  uvnvr::gm::Sm2KeyPair other_device;
  uvnvr::gm::GenerateSm2KeyPair(&other_device, &error);
  Bytes ignored;
  ExpectBool(
      "wrong device key cannot unwrap the VKEK",
      uvnvr::gb35114::UnwrapStreamVkek(other_device.private_key_pem, envelope, &ignored, nullptr, &error),
      false);

  ExpectBool("tampered VKEK envelope is rejected",
             uvnvr::gb35114::UnwrapStreamVkek(device.private_key_pem, Flip(envelope, envelope.size() - 1),
                                        &ignored, nullptr, &error),
             false);
  Bytes truncated(envelope.begin(), envelope.begin() + 20);
  ExpectBool("truncated VKEK envelope is rejected",
             uvnvr::gb35114::ParseStreamKeyEnvelope(truncated, &parsed, &error), false);

  const std::string certificate = MakeSelfSignedCertificate(device.private_key_pem);
  Bytes certificate_envelope;
  ExpectBool("wrap a VKEK for the device certificate",
             uvnvr::gb35114::CreateStreamKeyEnvelopeForCertificate(
                 certificate, key_id + 1, &certificate_envelope, nullptr, nullptr, &error),
             true);
  ExpectBool("certificate-wrapped VKEK unwraps",
             uvnvr::gb35114::UnwrapStreamVkek(device.private_key_pem, certificate_envelope, &device_vkek,
                                        &device_parsed, &error),
             true);
  ExpectSize("certificate-wrapped key id", device_parsed.key_id, key_id + 1);
}

void RunSplitTests() {
  const Bytes es = MakeTestEs();
  const std::vector<uvnvr::gb35114::EsAccessUnit> units = uvnvr::gb35114::SplitH264AccessUnits(es);
  ExpectSize("access unit count", units.size(), 3);
  if (units.size() == 3) {
    ExpectBool("first access unit is a keyframe", units[0].keyframe, true);
    ExpectBool("second access unit is not a keyframe", units[1].keyframe, false);
    ExpectBool("third access unit is not a keyframe", units[2].keyframe, false);
  }
  Bytes joined;
  for (const auto& unit : units)
    joined.insert(joined.end(), unit.bytes.begin(), unit.bytes.end());
  ExpectBytes("access unit split preserves every byte", joined, es);

  Bytes multi_slice;
  AppendNal(&multi_slice, 0x65, {0x80}); // IDR, first slice
  AppendNal(&multi_slice, 0x65, {0x40}); // IDR, second slice (first_mb_in_slice = 1)
  AppendNal(&multi_slice, 0x41, {0x80}); // next picture
  const std::vector<uvnvr::gb35114::EsAccessUnit> slices =
      uvnvr::gb35114::SplitH264AccessUnits(multi_slice);
  ExpectSize("multi-slice picture stays one access unit", slices.size(), 2);
  ExpectBool("multi-slice access unit keeps the keyframe flag",
             !slices.empty() && slices[0].keyframe, true);

  Bytes prefixed = {0xAA, 0xBB};
  prefixed.insert(prefixed.end(), es.begin(), es.end());
  const std::vector<uvnvr::gb35114::EsAccessUnit> prefixed_units =
      uvnvr::gb35114::SplitH264AccessUnits(prefixed);
  Bytes prefixed_joined;
  for (const auto& unit : prefixed_units)
    prefixed_joined.insert(prefixed_joined.end(), unit.bytes.begin(), unit.bytes.end());
  ExpectBytes("leading junk is preserved in the first access unit", prefixed_joined, prefixed);

  ExpectSize("empty input has no access units",
             uvnvr::gb35114::SplitH264AccessUnits(Bytes{}).size(), 0);
}

void RunSealedFrameTests(const VideoKeys& keys) {
  std::string error;
  const Bytes es = MakeTestEs();
  const Bytes nonce = Hex("0001020304050607"); // fixed for the unit test
  Bytes sealed;
  ExpectBool("seal one access unit",
             uvnvr::gb35114::SealEsFrame(keys, 42, true, nonce, es, &sealed, &error), true);
  ExpectSize("sealed frame size", sealed.size(), uvnvr::gb35114::SealedFrameSize(es.size()));
  ExpectBool("sealed frame magic",
             std::memcmp(sealed.data(), uvnvr::gb35114::kSealedFrameMagic.data(), 4) == 0, true);
  ExpectBool("ciphertext differs from plaintext",
             std::memcmp(sealed.data() + uvnvr::gb35114::kSealedFrameHeaderSize, es.data(),
                         es.size()) != 0,
             true);

  Bytes opened;
  SealedFrameInfo info;
  ExpectBool("open the sealed frame",
             uvnvr::gb35114::OpenEsFrame(keys, sealed, &opened, &info, &error), true);
  ExpectBytes("sealed frame round trip", opened, es);
  ExpectSize("frame key id", info.key_id, keys.key_id);
  ExpectSize("frame sequence", info.sequence, 42);
  ExpectBool("frame keyframe flag", info.keyframe, true);
  ExpectSize("frame plaintext size", info.plain_size, es.size());

  SealedFrameInfo inspected;
  std::size_t inspected_size = 0;
  ExpectBool("inspect a sealed frame without a key",
             uvnvr::gb35114::ReadSealedFrameInfo(sealed, 0, &inspected, &inspected_size, &error),
             true);
  ExpectSize("inspected frame size", inspected_size, sealed.size());
  ExpectSize("inspected plaintext size", inspected.plain_size, es.size());

  // A wrong VKEK changes the video key, IV seed and MAC key together.
  VideoKeys wrong_key = keys;
  wrong_key.video_key[0] ^= 0x01;
  wrong_key.iv_seed[0] ^= 0x01;
  wrong_key.mac_key[0] ^= 0x01;
  ExpectBool("wrong key material is rejected",
             uvnvr::gb35114::OpenEsFrame(wrong_key, sealed, &opened, nullptr, &error), false);
  ExpectContains("wrong-key error is an authentication failure", error, "authentication failed");

  VideoKeys wrong_mac = keys;
  wrong_mac.mac_key[0] ^= 0x01;
  ExpectBool("wrong MAC key is rejected",
             uvnvr::gb35114::OpenEsFrame(wrong_mac, sealed, &opened, nullptr, &error), false);

  ExpectBool(
      "modified ciphertext is rejected",
      uvnvr::gb35114::OpenEsFrame(keys, Flip(sealed, sealed.size() - 1), &opened, nullptr, &error),
      false);
  ExpectBool("modified sequence is rejected",
             uvnvr::gb35114::OpenEsFrame(keys, Flip(sealed, 12), &opened, nullptr, &error), false);
  ExpectBool("modified tag is rejected",
             uvnvr::gb35114::OpenEsFrame(keys, Flip(sealed, 45), &opened, nullptr, &error), false);
  ExpectBool("truncated frame is rejected",
             uvnvr::gb35114::OpenEsFrame(keys,
                                         Bytes(sealed.begin(), sealed.begin() + sealed.size() - 1),
                                         &opened, nullptr, &error),
             false);
  Bytes trailing = sealed;
  trailing.push_back(0);
  ExpectBool("trailing bytes are rejected",
             uvnvr::gb35114::OpenEsFrame(keys, trailing, &opened, nullptr, &error), false);

  VideoKeys wrong_id = keys;
  wrong_id.key_id += 1;
  ExpectBool("key id mismatch is rejected",
             uvnvr::gb35114::OpenEsFrame(wrong_id, sealed, &opened, nullptr, &error), false);
  ExpectContains("key id mismatch is reported", error, "key id");

  Bytes fresh_nonce;
  ExpectBool("a second nonce yields different ciphertext",
             uvnvr::gb35114::SealEsFrame(keys, 42, true, Hex("0807060504030201"), es, &fresh_nonce,
                                         &error),
             true);
  ExpectBool("nonce changes the sealed frame", fresh_nonce != sealed, true);
}

void RunStreamTests(const VideoKeys& keys) {
  std::string error;
  const Bytes es = MakeTestEs();
  Bytes sealed;
  std::size_t frames = 0;
  ExpectBool("seal the whole ES stream",
             uvnvr::gb35114::SealEsStream(keys, es, &sealed, &frames, &error), true);
  ExpectSize("sealed stream frame count", frames, 3);

  std::size_t offset = 0;
  std::size_t walked = 0;
  std::size_t keyframes = 0;
  while (offset < sealed.size()) {
    SealedFrameInfo info;
    std::size_t size = 0;
    if (!uvnvr::gb35114::ReadSealedFrameInfo(sealed, offset, &info, &size, &error))
      break;
    if (info.keyframe)
      ++keyframes;
    offset += size;
    ++walked;
  }
  ExpectSize("sealed stream walks to its end", offset, sealed.size());
  ExpectSize("sealed stream walk frame count", walked, 3);
  ExpectSize("sealed stream keyframe count", keyframes, 1);

  Bytes opened;
  std::size_t opened_frames = 0;
  ExpectBool("open the whole ES stream",
             uvnvr::gb35114::OpenEsStream(keys, sealed, &opened, &opened_frames, &error), true);
  ExpectSize("opened stream frame count", opened_frames, 3);
  ExpectBytes("sealed stream round trip", opened, es);

  VideoKeys wrong = keys;
  wrong.video_key[15] ^= 0x80;
  wrong.iv_seed[15] ^= 0x80;
  wrong.mac_key[15] ^= 0x80;
  Bytes ignored;
  ExpectBool("the wrong key cannot open the stream",
             uvnvr::gb35114::OpenEsStream(wrong, sealed, &ignored, nullptr, &error), false);

  Bytes empty_sealed;
  std::size_t empty_frames = 99;
  ExpectBool("empty input seals to an empty stream",
             uvnvr::gb35114::SealEsStream(keys, Bytes{}, &empty_sealed, &empty_frames, &error),
             true);
  ExpectSize("empty stream frame count", empty_frames, 0);
  ExpectBool("empty stream opens", empty_sealed.empty(), true);

  Bytes truncated(sealed.begin(), sealed.end() - 5);
  ExpectBool("a truncated stream is rejected",
             uvnvr::gb35114::OpenEsStream(keys, truncated, &ignored, nullptr, &error), false);
}

} // namespace

int main() {
  std::string error;
  uvnvr::gm::Sm2KeyPair device;
  RunSm2EncryptionTests(&device);
  RunVkekTests(device);
  RunSplitTests();

  const std::string stream = "34020000001320000001";
  Bytes envelope;
  Bytes vkek;
  Bytes salt;
  if (!uvnvr::gb35114::CreateStreamKeyEnvelope(device.public_key_pem, 1, &envelope, &vkek, &salt,
                                          &error)) {
    std::fprintf(stderr, "FAIL cannot build the frame-test VKEK: %s\n", error.c_str());
    return 1;
  }
  uvnvr::gb35114::VideoKeys keys;
  if (!uvnvr::gb35114::DeriveStreamKeys(vkek, 1, salt, stream, &keys, &error)) {
    std::fprintf(stderr, "FAIL cannot derive the frame-test keys: %s\n", error.c_str());
    return 1;
  }
  RunSealedFrameTests(keys);
  RunStreamTests(keys);

  if (failures == 0) {
    std::printf("gb35114_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "gb35114_test: %d checks failed\n", failures);
  return 1;
}
