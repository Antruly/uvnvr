// Unit tests for the GB 35114 A-level signer: Annex B access-unit splitting,
// MPEG-TS demuxing, record serialisation, SM2 signing/verification and
// single-byte tamper detection.
//
// The test builds a small H.264 stream with SPS/PPS/AUD/IDR/P NAL units and
// wraps it in a minimal PAT/PMT/PES transport stream, so the parser exercised
// here is the same one used by tools/gb35114_verify on real recordings.

#include "crypto/sm.h"
#include "gb35114/sign.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uvnvr::gb35114::AccessUnit;
using uvnvr::gb35114::AccessUnitReader;
using uvnvr::gb35114::FrameSignature;
using uvnvr::gb35114::FrameSigner;
using uvnvr::gb35114::InputFormat;
using uvnvr::gb35114::SigningOptions;
using uvnvr::gb35114::SigningStats;
using uvnvr::gb35114::VerifyOptions;

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

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  if (haystack.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: '%s' does not contain '%s'\n", name.c_str(), haystack.c_str(),
               needle.c_str());
  ++failures;
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteAll(const std::filesystem::path& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  return output.good();
}

std::filesystem::path PrepareArtifacts() {
  const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / ("uvnvr-gb35114-test-" + std::to_string(stamp));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::fprintf(stderr, "cannot create %s: %s\n", directory.string().c_str(),
                 error.message().c_str());
    return std::filesystem::temp_directory_path();
  }
  return directory;
}

void AppendStartCode(std::vector<unsigned char>* out) {
  out->push_back(0x00);
  out->push_back(0x00);
  out->push_back(0x00);
  out->push_back(0x01);
}

std::vector<unsigned char> MakeNal(int type, const std::vector<unsigned char>& payload) {
  std::vector<unsigned char> nal;
  AppendStartCode(&nal);
  nal.push_back(static_cast<unsigned char>(0x60 | type));
  nal.insert(nal.end(), payload.begin(), payload.end());
  return nal;
}

void Append(std::vector<unsigned char>* out, const std::vector<unsigned char>& data) {
  out->insert(out->end(), data.begin(), data.end());
}

// ue(v) encoding for the two slice-header fields the parser reads.
void AppendUe(std::vector<unsigned char>* bits, unsigned value) {
  unsigned length = 0;
  for (unsigned temp = value + 1; temp > 1; temp >>= 1)
    ++length;
  for (unsigned i = 0; i < length; ++i)
    bits->push_back(0);
  for (int i = static_cast<int>(length); i >= 0; --i)
    bits->push_back(static_cast<unsigned char>((value + 1) >> i) & 1U);
}

std::vector<unsigned char> PackBits(const std::vector<unsigned char>& bits) {
  std::vector<unsigned char> bytes((bits.size() + 7) / 8, 0);
  for (std::size_t i = 0; i < bits.size(); ++i)
    bytes[i / 8] = static_cast<unsigned char>(bytes[i / 8] | (bits[i] << (7 - (i % 8))));
  return bytes;
}

std::vector<unsigned char> MakeSlice(int nal_type, unsigned slice_type) {
  std::vector<unsigned char> bits;
  AppendUe(&bits, 0);  // first_mb_in_slice
  AppendUe(&bits, slice_type);
  return MakeNal(nal_type, PackBits(bits));
}

void PutU16(std::vector<unsigned char>* out, unsigned value) {
  out->push_back(static_cast<unsigned char>(value >> 8));
  out->push_back(static_cast<unsigned char>(value));
}

void PutPid(std::vector<unsigned char>* out, unsigned pid) {
  out->push_back(static_cast<unsigned char>(0xE0 | ((pid >> 8) & 0x1F)));
  out->push_back(static_cast<unsigned char>(pid & 0xFF));
}

std::vector<unsigned char> MakePat(unsigned pmt_pid) {
  std::vector<unsigned char> section;
  section.push_back(0x00);  // pointer_field
  section.push_back(0x00);  // table_id PAT
  PutU16(&section, 13);     // section_length
  PutU16(&section, 1);      // transport_stream_id
  section.push_back(0xC1);  // version 0, current_next 1
  section.push_back(0x00);  // section_number
  section.push_back(0x00);  // last_section_number
  PutU16(&section, 1);      // program_number
  PutPid(&section, pmt_pid);
  for (int i = 0; i < 4; ++i)
    section.push_back(0x00);  // CRC (not validated by the demuxer)
  return section;
}

std::vector<unsigned char> MakePmt(unsigned video_pid) {
  std::vector<unsigned char> section;
  section.push_back(0x00);  // pointer_field
  section.push_back(0x02);  // table_id PMT
  PutU16(&section, 18);     // section_length
  PutU16(&section, 1);      // program_number
  section.push_back(0xC1);  // version 0, current_next 1
  section.push_back(0x00);  // section_number
  section.push_back(0x00);  // last_section_number
  PutPid(&section, video_pid);  // PCR PID
  PutU16(&section, 0);          // program_info_length
  section.push_back(0x1B);      // stream_type H.264
  PutPid(&section, video_pid);
  PutU16(&section, 0);          // ES_info_length
  for (int i = 0; i < 4; ++i)
    section.push_back(0x00);  // CRC
  return section;
}

void PacketizePsi(unsigned pid, const std::vector<unsigned char>& payload,
                  std::vector<unsigned char>* out) {
  std::vector<unsigned char> packet(188, 0xFF);
  packet[0] = 0x47;
  packet[1] = static_cast<unsigned char>(0x40 | ((pid >> 8) & 0x1F));
  packet[2] = static_cast<unsigned char>(pid & 0xFF);
  packet[3] = 0x10;
  std::memcpy(packet.data() + 4, payload.data(), payload.size());
  out->insert(out->end(), packet.begin(), packet.end());
}

std::vector<unsigned char> MakePes(std::int64_t pts, const std::vector<unsigned char>& payload) {
  std::vector<unsigned char> pes;
  pes.push_back(0x00);
  pes.push_back(0x00);
  pes.push_back(0x01);
  pes.push_back(0xE0);
  const std::size_t packet_length = 8 + payload.size();
  PutU16(&pes, static_cast<unsigned>(packet_length & 0xFFFF));
  pes.push_back(0x80);  // '10' marker, no scrambling
  pes.push_back(0x80);  // PTS only
  pes.push_back(0x05);  // PES header data length
  const std::uint64_t value = static_cast<std::uint64_t>(pts);
  pes.push_back(static_cast<unsigned char>(0x21 | (((value >> 30) & 0x07) << 1)));
  pes.push_back(static_cast<unsigned char>((value >> 22) & 0xFF));
  pes.push_back(static_cast<unsigned char>(0x01 | (((value >> 15) & 0x7F) << 1)));
  pes.push_back(static_cast<unsigned char>((value >> 7) & 0xFF));
  pes.push_back(static_cast<unsigned char>(0x01 | ((value & 0x7F) << 1)));
  pes.insert(pes.end(), payload.begin(), payload.end());
  return pes;
}

void PacketizeVideo(unsigned pid, unsigned counter, const std::vector<unsigned char>& pes,
                    std::vector<unsigned char>* out) {
  std::size_t offset = 0;
  bool first = true;
  while (offset < pes.size()) {
    const std::size_t chunk = std::min<std::size_t>(184, pes.size() - offset);
    std::vector<unsigned char> packet(188, 0xFF);
    packet[0] = 0x47;
    packet[1] = static_cast<unsigned char>(((first ? 0x40 : 0x00)) | ((pid >> 8) & 0x1F));
    packet[2] = static_cast<unsigned char>(pid & 0xFF);
    packet[3] = static_cast<unsigned char>(0x10 | (counter & 0x0F));
    std::memcpy(packet.data() + 4, pes.data() + offset, chunk);
    out->insert(out->end(), packet.begin(), packet.end());
    first = false;
    offset += chunk;
    counter = (counter + 1) & 0x0F;
  }
}

struct SyntheticStream {
  std::vector<std::vector<unsigned char>> access_units;
  std::vector<std::int64_t> pts;
};

SyntheticStream BuildSyntheticStream() {
  const std::vector<unsigned char> sps = MakeNal(7, {0x42, 0x00, 0x1E});
  const std::vector<unsigned char> pps = MakeNal(8, {0xCE, 0x3C, 0x80});
  const std::vector<unsigned char> aud = MakeNal(9, {0xF0});
  const std::vector<unsigned char> idr = MakeSlice(5, 2);
  const std::vector<unsigned char> p1 = MakeSlice(1, 0);
  const std::vector<unsigned char> p2 = MakeSlice(1, 0);

  SyntheticStream stream;
  std::vector<unsigned char> first;
  Append(&first, sps);
  Append(&first, pps);
  Append(&first, aud);
  Append(&first, idr);
  stream.access_units.push_back(std::move(first));
  stream.access_units.push_back(p1);
  stream.access_units.push_back(p2);
  std::vector<unsigned char> second;
  Append(&second, aud);
  Append(&second, idr);
  stream.access_units.push_back(std::move(second));
  stream.pts = {9000, 12000, 15000, 18000};
  return stream;
}

std::vector<unsigned char> BuildAnnexB(const SyntheticStream& stream) {
  std::vector<unsigned char> es;
  for (const auto& unit : stream.access_units)
    Append(&es, unit);
  return es;
}

std::vector<unsigned char> BuildTransportStream(const SyntheticStream& stream,
                                                unsigned video_pid = 0x1011) {
  std::vector<unsigned char> ts;
  PacketizePsi(0, MakePat(0x1000), &ts);
  PacketizePsi(0x1000, MakePmt(video_pid), &ts);
  unsigned counter = 0;
  for (std::size_t i = 0; i < stream.access_units.size(); ++i) {
    PacketizeVideo(video_pid, counter, MakePes(stream.pts[i], stream.access_units[i]), &ts);
    counter = (counter + 3) & 0x0F;
  }
  return ts;
}

std::string AsString(const std::vector<unsigned char>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void TestAnnexBSplit(const std::filesystem::path& directory) {
  const SyntheticStream stream = BuildSyntheticStream();
  const std::vector<unsigned char> es = BuildAnnexB(stream);
  const std::filesystem::path path = directory / "synthetic.h264";
  ExpectBool("write synthetic ES", WriteAll(path, AsString(es)), true);

  std::vector<AccessUnit> units;
  uvnvr::gb35114::ReaderStats stats;
  std::string error;
  ExpectBool("read synthetic ES", uvnvr::gb35114::ReadAccessUnitsFromFile(
                                      path.string(), InputFormat::kAuto, &units, &stats, &error),
             true);
  ExpectSize("annex-b access units", units.size(), 4);
  ExpectSize("annex-b key frames", stats.key_frames, 2);
  if (units.size() == 4) {
    ExpectBool("au0 key", units[0].key_frame, true);
    ExpectBool("au1 not key", units[1].key_frame, false);
    ExpectBool("au2 not key", units[2].key_frame, false);
    ExpectBool("au3 key", units[3].key_frame, true);
    ExpectBool("au0 has no pts", units[0].has_pts, false);
    ExpectBool("au0 starts with sps", (units[0].data.size() > 4 && units[0].data[4] == 0x67),
               true);
    ExpectBool("au3 starts with aud", (units[3].data.size() > 4 && units[3].data[4] == 0x69),
               true);
  }
}

void TestTsDemux(const std::filesystem::path& directory) {
  const SyntheticStream stream = BuildSyntheticStream();
  const std::vector<unsigned char> ts = BuildTransportStream(stream);
  const std::filesystem::path path = directory / "synthetic.ts";
  ExpectBool("write synthetic TS", WriteAll(path, AsString(ts)), true);

  std::vector<AccessUnit> units;
  uvnvr::gb35114::ReaderStats stats;
  std::string error;
  ExpectBool("read synthetic TS", uvnvr::gb35114::ReadAccessUnitsFromFile(
                                      path.string(), InputFormat::kAuto, &units, &stats, &error),
             true);
  ExpectSize("ts access units", units.size(), 4);
  ExpectSize("ts key frames", stats.key_frames, 2);
  if (units.size() == 4) {
    for (std::size_t i = 0; i < units.size(); ++i) {
      ExpectBool("ts au has pts " + std::to_string(i), units[i].has_pts, true);
      ExpectBool("ts au pts " + std::to_string(i), units[i].pts == stream.pts[i], true);
      ExpectBool("ts au bytes " + std::to_string(i), units[i].data == stream.access_units[i],
                 true);
    }
  }
}

void TestSignatureRoundTrip(const std::filesystem::path& directory) {
  uvnvr::gm::Sm2KeyPair pair;
  std::string error;
  ExpectBool("generate SM2 key", uvnvr::gm::GenerateSm2KeyPair(&pair, &error), true);

  const SyntheticStream stream = BuildSyntheticStream();
  const std::vector<unsigned char>& payload = stream.access_units[0];
  FrameSignature record;
  ExpectBool("create signature",
             uvnvr::gb35114::CreateFrameSignature(pair.private_key_pem, 7, 0, 9000, true, true,
                                                 payload.data(), payload.size(), &record, &error),
             true);
  ExpectSize("record digest size", record.digest.size(), 32);
  ExpectBool("record key flag", record.key_frame(), true);
  ExpectBool("record pts flag", record.has_pts(), true);
  ExpectSize("record payload size", static_cast<std::size_t>(record.payload_size), payload.size());

  const uvnvr::gm::Bytes serialized =
      uvnvr::gb35114::SerializeFrameSignature(record, &error);
  ExpectBool("serialize record", !serialized.empty(), true);
  ExpectSize("serialized size", serialized.size(),
             uvnvr::gb35114::kSignatureHeaderSize + record.signature.size());

  FrameSignature parsed;
  std::size_t consumed = 0;
  ExpectBool("parse record", uvnvr::gb35114::ParseFrameSignature(serialized.data(),
                                                                 serialized.size(), &parsed,
                                                                 &consumed, &error),
             true);
  ExpectSize("parsed consumed", consumed, serialized.size());
  ExpectBool("parsed channel", parsed.channel_id == 7, true);
  ExpectBool("parsed index", parsed.frame_index == 0, true);
  ExpectBool("parsed pts", parsed.pts == 9000, true);
  ExpectBool("parsed digest", parsed.digest == record.digest, true);

  VerifyOptions options;
  options.public_key_pem = pair.public_key_pem;
  ExpectBool("verify original", uvnvr::gb35114::VerifyFrameSignature(
                                    options, payload.data(), payload.size(), parsed, &error),
             true);

  // Flip one byte of the media payload. The SM2 signature itself is intact,
  // but the recomputed SM3 digest no longer matches the signed digest.
  std::vector<unsigned char> tampered = payload;
  tampered[tampered.size() / 2] ^= 0x01;
  std::string tamper_error;
  ExpectBool("verify tampered payload",
             uvnvr::gb35114::VerifyFrameSignature(options, tampered.data(), tampered.size(),
                                                  parsed, &tamper_error),
             false);
  ExpectContains("tamper error", tamper_error, "SM3 digest mismatch");

  // Flip a byte of the signature: the digest still matches, so this exercises
  // the SM2 verification path.
  FrameSignature bad_signature = parsed;
  bad_signature.signature[bad_signature.signature.size() / 2] ^= 0x01;
  std::string signature_error;
  ExpectBool("verify tampered signature",
             uvnvr::gb35114::VerifyFrameSignature(options, payload.data(), payload.size(),
                                                  bad_signature, &signature_error),
             false);
  ExpectContains("signature error", signature_error, "SM2 signature verification failed");

  // Moving a signature to another PTS must break the signed binding.
  FrameSignature moved = parsed;
  moved.pts = 9001;
  std::string moved_error;
  ExpectBool("verify moved record",
             uvnvr::gb35114::VerifyFrameSignature(options, payload.data(), payload.size(), moved,
                                                  &moved_error),
             false);
  ExpectContains("moved record error", moved_error, "SM2 signature verification failed");

  // A bad key must not verify.
  uvnvr::gm::Sm2KeyPair other;
  ExpectBool("generate other key", uvnvr::gm::GenerateSm2KeyPair(&other, &error), true);
  VerifyOptions other_options;
  other_options.public_key_pem = other.public_key_pem;
  std::string other_error;
  ExpectBool("verify with other key",
             uvnvr::gb35114::VerifyFrameSignature(other_options, payload.data(), payload.size(),
                                                  parsed, &other_error),
             false);
}

void TestStreamingSigner(const std::filesystem::path& directory) {
  const SyntheticStream stream = BuildSyntheticStream();
  const std::vector<unsigned char> ts = BuildTransportStream(stream);
  const std::filesystem::path path = directory / "stream.ts";
  ExpectBool("write stream TS", WriteAll(path, AsString(ts)), true);

  uvnvr::gm::Sm2KeyPair pair;
  std::string error;
  ExpectBool("stream generate key", uvnvr::gm::GenerateSm2KeyPair(&pair, &error), true);

  SigningOptions options;
  options.private_key_pem = pair.private_key_pem;
  options.channel_id = 42;
  options.sign_all_frames = true;

  FrameSigner signer(options, InputFormat::kAuto);
  ExpectBool("configure stream signer", signer.Configure(&error), true);
  std::vector<FrameSignature> records;
  signer.SetSink([&records](const FrameSignature& record) { records.push_back(record); });

  // Deliberately awkward chunk sizes: the reader has to cope with TS packets
  // and Annex B start codes split across Push() calls.
  const std::size_t chunk = 97;
  for (std::size_t offset = 0; offset < ts.size(); offset += chunk) {
    const std::size_t size = std::min(chunk, ts.size() - offset);
    ExpectBool("stream push", signer.Push(ts.data() + offset, size, &error), true);
  }
  ExpectBool("stream flush", signer.Flush(&error), true);
  ExpectSize("stream records", records.size(), 4);
  for (std::size_t i = 0; i < records.size(); ++i) {
    ExpectBool("stream index " + std::to_string(i), records[i].frame_index == i, true);
    ExpectBool("stream channel " + std::to_string(i), records[i].channel_id == 42, true);
    ExpectBool("stream pts " + std::to_string(i),
               records[i].has_pts() && records[i].pts == stream.pts[i], true);
  }
  ExpectBool("stream key record 0", records[0].key_frame(), true);
  ExpectBool("stream key record 3", records[3].key_frame(), true);
  ExpectBool("stream non-key record 1", records[1].key_frame(), false);
  ExpectBool("stream all-frames coverage flag",
             (records[0].flags & uvnvr::gb35114::kFlagAllFrames) != 0, true);
  ExpectSize("stream key frames stat", signer.stats().key_frames, 2);
  ExpectSize("stream signed stat", signer.stats().signed_frames, 4);

  // Key-frame-only mode is the production default.
  options.sign_all_frames = false;
  FrameSigner key_only(options, InputFormat::kAuto);
  ExpectBool("configure key-only signer", key_only.Configure(&error), true);
  std::vector<FrameSignature> key_records;
  key_only.SetSink(
      [&key_records](const FrameSignature& record) { key_records.push_back(record); });
  ExpectBool("key-only push", key_only.Push(ts.data(), ts.size(), &error), true);
  ExpectBool("key-only flush", key_only.Flush(&error), true);
  ExpectSize("key-only records", key_records.size(), 2);
  if (key_records.size() == 2) {
    ExpectBool("key-only first index", key_records[0].frame_index == 0, true);
    ExpectBool("key-only second index", key_records[1].frame_index == 3, true);
    ExpectBool("key-only has no all-frames flag",
               (key_records[0].flags & uvnvr::gb35114::kFlagAllFrames) == 0, true);
  }

  // Sidecar write/read round trip.
  const std::filesystem::path sidecar = directory / "stream.ts.gbs";
  ExpectBool("write sidecar",
             uvnvr::gb35114::WriteSignatureFile(sidecar.string(), records, &error), true);
  std::vector<FrameSignature> loaded;
  ExpectBool("read sidecar", uvnvr::gb35114::ReadSignatureFile(sidecar.string(), &loaded, &error),
             true);
  ExpectSize("sidecar records", loaded.size(), records.size());

  // Verify every record against the units produced by a fresh reader.
  std::vector<AccessUnit> units;
  uvnvr::gb35114::ReaderStats stats;
  ExpectBool("re-read units",
             uvnvr::gb35114::ReadAccessUnitsFromFile(path.string(), InputFormat::kAuto, &units,
                                                     &stats, &error),
             true);
  VerifyOptions verify_options;
  verify_options.public_key_pem = pair.public_key_pem;
  for (std::size_t i = 0; i < loaded.size() && i < units.size(); ++i) {
    ExpectBool("verify stream record " + std::to_string(i),
               uvnvr::gb35114::VerifyFrameSignature(verify_options, units[i].data.data(),
                                                    units[i].data.size(), loaded[i], &error),
               true);
  }
}

void TestFileSigning(const std::filesystem::path& directory) {
  const SyntheticStream stream = BuildSyntheticStream();
  const std::vector<unsigned char> es = BuildAnnexB(stream);
  const std::filesystem::path path = directory / "file.h264";
  ExpectBool("write file ES", WriteAll(path, AsString(es)), true);

  uvnvr::gm::Sm2KeyPair pair;
  std::string error;
  ExpectBool("file generate key", uvnvr::gm::GenerateSm2KeyPair(&pair, &error), true);
  SigningOptions options;
  options.private_key_pem = pair.private_key_pem;
  options.channel_id = 1;
  std::vector<FrameSignature> records;
  SigningStats stats;
  ExpectBool("sign file",
             uvnvr::gb35114::SignFile(path.string(), options, InputFormat::kAuto, &records, &stats,
                                      &error),
             true);
  ExpectSize("file signed key frames", records.size(), 2);
  ExpectSize("file access units", stats.access_units, 4);
  ExpectBool("file sign timing recorded", stats.sign_ns_total > 0, true);
}

}  // namespace

int main() {
  const std::filesystem::path directory = PrepareArtifacts();
  TestAnnexBSplit(directory);
  TestTsDemux(directory);
  TestSignatureRoundTrip(directory);
  TestStreamingSigner(directory);
  TestFileSigning(directory);
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("gb35114 sign tests: OK\n");
  return 0;
}
