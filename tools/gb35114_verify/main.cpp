// Command line companion for the GB 35114 A-level I-frame signer.
//
//   gb35114_verify keygen --private-key device.key --public-key device.pub
//   gb35114_verify sign   --input rec.ts --private-key device.key \
//                         --output rec.ts.gbs [--cert device.pem] \
//                         [--channel 1] [--all-frames]
//   gb35114_verify verify --input rec.ts --signatures rec.ts.gbs \
//                         (--public-key device.pub | --cert device.pem)
//   gb35114_verify inspect --input rec.ts
//
// Input can be MPEG-TS (what the recorder writes), a raw Annex B H.264 file,
// or "auto" (the default: a leading 0x47 selects MPEG-TS). Sign uses the same
// incremental reader as the live pipeline; the sidecar is written as records
// are produced, not as one buffer at the end.

#include "gb35114/sign.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
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

struct Args {
  std::string command;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
  std::string error;

  bool Has(const std::string& name) const {
    return values.count(name) != 0 || flags.count(name) != 0;
  }
  std::string Get(const std::string& name, const std::string& fallback = {}) const {
    auto it = values.find(name);
    return it == values.end() ? fallback : it->second;
  }
};

bool ParseArgs(int argc, char** argv, Args* args) {
  if (argc < 2) {
    args->error = "missing command";
    return false;
  }
  args->command = argv[1];
  static const std::set<std::string> kBooleanFlags = {"json", "all-frames", "help"};
  for (int i = 2; i < argc; ++i) {
    std::string token = argv[i];
    if (token.rfind("--", 0) != 0) {
      args->error = "unexpected argument: " + token;
      return false;
    }
    std::string name = token.substr(2);
    std::string value;
    const std::size_t equals = name.find('=');
    if (equals != std::string::npos) {
      value = name.substr(equals + 1);
      name = name.substr(0, equals);
    } else if (kBooleanFlags.count(name) == 0) {
      if (i + 1 >= argc) {
        args->error = "missing value for --" + name;
        return false;
      }
      value = argv[++i];
    }
    if (kBooleanFlags.count(name) != 0)
      args->flags.insert(name);
    else
      args->values[name] = std::move(value);
  }
  return true;
}

InputFormat ParseFormat(const std::string& text, bool* ok) {
  *ok = true;
  if (text.empty() || text == "auto")
    return InputFormat::kAuto;
  if (text == "ts" || text == "mpegts")
    return InputFormat::kMpegTs;
  if (text == "es" || text == "h264" || text == "annexb")
    return InputFormat::kAnnexB;
  *ok = false;
  return InputFormat::kAuto;
}

std::string FormatName(InputFormat format) {
  switch (format) {
    case InputFormat::kMpegTs:
      return "mpegts";
    case InputFormat::kAnnexB:
      return "annexb";
    case InputFormat::kAuto:
    default:
      return "auto";
  }
}

bool ReadFile(const std::string& path, std::string* data, std::string* error) {
  return uvnvr::gb35114::ReadPemFile(path, data, error);
}

std::string EscapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

double ToMs(std::uint64_t ns) {
  return static_cast<double>(ns) / 1000000.0;
}

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool WriteAll(const std::string& path, const std::string& data, std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error)
      *error = "cannot create " + path;
    return false;
  }
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!output) {
    if (error)
      *error = "cannot write " + path;
    return false;
  }
  return true;
}

void PrintUsage() {
  std::printf(
      "usage:\n"
      "  gb35114_verify keygen --private-key <file.pem> --public-key <file.pem>\n"
      "  gb35114_verify sign   --input <recording> --private-key <file.pem> --output <file.gbs>\n"
      "                        [--cert <device.pem>] [--channel <id>] [--all-frames]\n"
      "                        [--format auto|ts|es] [--json]\n"
      "  gb35114_verify verify --input <recording> --signatures <file.gbs>\n"
      "                        (--public-key <file.pem> | --cert <device.pem>) [--format ...]\n"
      "                        [--json]\n"
      "  gb35114_verify inspect --input <recording> [--format auto|ts|es] [--json]\n"
      "\n"
      "By default only key frames are signed. --all-frames signs every access unit.\n");
}

int Keygen(const Args& args) {
  const std::string private_path = args.Get("private-key");
  const std::string public_path = args.Get("public-key");
  if (private_path.empty() || public_path.empty()) {
    std::fprintf(stderr, "keygen needs --private-key and --public-key\n");
    return 2;
  }
  uvnvr::gm::Sm2KeyPair pair;
  std::string error;
  if (!uvnvr::gm::GenerateSm2KeyPair(&pair, &error)) {
    std::fprintf(stderr, "key generation failed: %s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(private_path, pair.private_key_pem, &error) ||
      !WriteAll(public_path, pair.public_key_pem, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (args.Has("json")) {
    std::printf("{\"privateKey\":\"%s\",\"publicKey\":\"%s\"}\n", EscapeJson(private_path).c_str(),
                EscapeJson(public_path).c_str());
  } else {
    std::printf("private key: %s\npublic key:  %s\n", private_path.c_str(), public_path.c_str());
  }
  return 0;
}

bool LoadKeyMaterial(const Args& args, std::string* private_key, std::string* certificate,
                     std::string* error) {
  const std::string private_path = args.Get("private-key");
  if (private_path.empty()) {
    if (error)
      *error = "sign needs --private-key";
    return false;
  }
  if (!uvnvr::gb35114::ReadPemFile(private_path, private_key, error))
    return false;
  const std::string cert_path = args.Get("cert");
  if (!cert_path.empty() && !uvnvr::gb35114::ReadPemFile(cert_path, certificate, error))
    return false;
  return true;
}

int Sign(const Args& args) {
  const std::string input_path = args.Get("input");
  const std::string output_path = args.Get("output");
  if (input_path.empty() || output_path.empty()) {
    std::fprintf(stderr, "sign needs --input and --output\n");
    return 2;
  }
  bool format_ok = false;
  const InputFormat format = ParseFormat(args.Get("format", "auto"), &format_ok);
  if (!format_ok) {
    std::fprintf(stderr, "unknown --format; use auto, ts or es\n");
    return 2;
  }

  SigningOptions options;
  std::string error;
  if (!LoadKeyMaterial(args, &options.private_key_pem, &options.certificate_pem, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  options.channel_id = std::strtoll(args.Get("channel", "0").c_str(), nullptr, 10);
  options.sign_all_frames = args.Has("all-frames");

  std::ifstream input(input_path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot open %s\n", input_path.c_str());
    return 2;
  }

  const std::string temporary_path = output_path + ".tmp";
  std::ofstream sidecar(temporary_path, std::ios::binary | std::ios::trunc);
  if (!sidecar) {
    std::fprintf(stderr, "cannot create %s\n", temporary_path.c_str());
    return 2;
  }
  auto discard_sidecar = [&sidecar, &temporary_path] {
    sidecar.close();
    std::error_code ignored;
    std::filesystem::remove(temporary_path, ignored);
  };

  FrameSigner signer(options, format);
  if (!signer.Configure(&error)) {
    std::fprintf(stderr, "cannot use the signing identity: %s\n", error.c_str());
    discard_sidecar();
    return 2;
  }

  std::uint64_t sidecar_bytes = 0;
  std::string sink_error;
  signer.SetSink([&sidecar, &sidecar_bytes, &sink_error](const FrameSignature& record) {
    std::string serialize_error;
    const uvnvr::gm::Bytes bytes =
        uvnvr::gb35114::SerializeFrameSignature(record, &serialize_error);
    if (bytes.empty()) {
      if (sink_error.empty())
        sink_error = serialize_error;
      return;
    }
    sidecar.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!sidecar && sink_error.empty())
      sink_error = "cannot write the signature sidecar";
    sidecar_bytes += bytes.size();
  });

  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got > 0 &&
        !signer.Push(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                     static_cast<std::size_t>(got), &error)) {
      std::fprintf(stderr, "sign failed: %s\n", error.c_str());
      discard_sidecar();
      return 1;
    }
    if (!sink_error.empty()) {
      std::fprintf(stderr, "sign failed: %s\n", sink_error.c_str());
      discard_sidecar();
      return 1;
    }
  }
  if (!signer.Flush(&error)) {
    std::fprintf(stderr, "sign failed: %s\n", error.c_str());
    discard_sidecar();
    return 1;
  }
  sidecar.close();
  if (!sink_error.empty()) {
    std::fprintf(stderr, "sign failed: %s\n", sink_error.c_str());
    discard_sidecar();
    return 1;
  }

  const SigningStats& stats = signer.stats();
  if (stats.signed_frames == 0) {
    std::fprintf(stderr, "no key frames were found in %s (format %s)\n", input_path.c_str(),
                 FormatName(format).c_str());
    discard_sidecar();
    return 1;
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary_path, output_path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(output_path, ignored);
    rename_error.clear();
    std::filesystem::rename(temporary_path, output_path, rename_error);
  }
  if (rename_error) {
    std::fprintf(stderr, "cannot finalise %s: %s\n", output_path.c_str(),
                 rename_error.message().c_str());
    discard_sidecar();
    return 1;
  }

  const double total_ms = ToMs(stats.sign_ns_total);
  const double average_ms =
      stats.signed_frames == 0 ? 0.0 : total_ms / static_cast<double>(stats.signed_frames);
  if (args.Has("json")) {
    std::printf(
        "{\"ok\":true,\"input\":\"%s\",\"sidecar\":\"%s\",\"format\":\"%s\","
        "\"bytes\":%llu,\"accessUnits\":%llu,\"keyFrames\":%llu,\"signedFrames\":%llu,"
        "\"sidecarBytes\":%llu,\"signTotalMs\":%.3f,\"signAvgMs\":%.3f,"
        "\"signMinMs\":%.3f,\"signMaxMs\":%.3f}\n",
        EscapeJson(input_path).c_str(), EscapeJson(output_path).c_str(),
        FormatName(format).c_str(), static_cast<unsigned long long>(stats.input_bytes),
        static_cast<unsigned long long>(stats.access_units),
        static_cast<unsigned long long>(stats.key_frames),
        static_cast<unsigned long long>(stats.signed_frames),
        static_cast<unsigned long long>(sidecar_bytes), total_ms, average_ms,
        ToMs(stats.sign_ns_min), ToMs(stats.sign_ns_max));
  } else {
    std::printf("input:         %s\n", input_path.c_str());
    std::printf("format:        %s\n", FormatName(format).c_str());
    std::printf("access units:  %llu (key frames: %llu)\n",
                static_cast<unsigned long long>(stats.access_units),
                static_cast<unsigned long long>(stats.key_frames));
    std::printf("signed frames: %llu\n", static_cast<unsigned long long>(stats.signed_frames));
    std::printf("sign time:     %.3f ms total, %.3f ms average, %.3f ms min, %.3f ms max\n",
                total_ms, average_ms, ToMs(stats.sign_ns_min), ToMs(stats.sign_ns_max));
    std::printf("sidecar:       %s (%llu records, %llu bytes)\n", output_path.c_str(),
                static_cast<unsigned long long>(stats.signed_frames),
                static_cast<unsigned long long>(sidecar_bytes));
    std::printf("result:        OK\n");
  }
  return 0;
}

struct VerifyStatus {
  std::uint64_t access_units = 0;
  std::uint64_t key_frames = 0;
  std::uint64_t records = 0;
  std::uint64_t required_units = 0;
  std::uint64_t covered_units = 0;
  std::uint64_t verified = 0;
  std::uint64_t failed = 0;
  std::uint64_t verify_ns_total = 0;
  std::uint64_t verify_ns_min = 0;
  std::uint64_t verify_ns_max = 0;
  std::vector<std::string> first_errors;
};

void RecordFailure(VerifyStatus* status, std::uint64_t frame, const std::string& message) {
  ++status->failed;
  if (status->first_errors.size() < 20)
    status->first_errors.push_back("frame " + std::to_string(frame) + ": " + message);
}

int Verify(const Args& args) {
  const std::string input_path = args.Get("input");
  const std::string signatures_path = args.Get("signatures");
  if (input_path.empty() || signatures_path.empty()) {
    std::fprintf(stderr, "verify needs --input and --signatures\n");
    return 2;
  }
  bool format_ok = false;
  const InputFormat format = ParseFormat(args.Get("format", "auto"), &format_ok);
  if (!format_ok) {
    std::fprintf(stderr, "unknown --format; use auto, ts or es\n");
    return 2;
  }

  VerifyOptions options;
  std::string error;
  const std::string public_key_path = args.Get("public-key");
  const std::string cert_path = args.Get("cert");
  if (public_key_path.empty() && cert_path.empty()) {
    std::fprintf(stderr, "verify needs --public-key or --cert\n");
    return 2;
  }
  if (!public_key_path.empty() &&
      !uvnvr::gb35114::ReadPemFile(public_key_path, &options.public_key_pem, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  if (!cert_path.empty() &&
      !uvnvr::gb35114::ReadPemFile(cert_path, &options.certificate_pem, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }

  std::vector<FrameSignature> records;
  if (!uvnvr::gb35114::ReadSignatureFile(signatures_path, &records, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i].frame_index <= records[i - 1].frame_index) {
      std::fprintf(stderr, "signature file is not strictly ordered at record %zu\n", i);
      return 2;
    }
  }
  const bool all_frames =
      (records.front().flags & uvnvr::gb35114::kFlagAllFrames) != 0;
  for (const auto& record : records) {
    if (((record.flags & uvnvr::gb35114::kFlagAllFrames) != 0) != all_frames) {
      std::fprintf(stderr, "signature file mixes all-frame and key-frame records\n");
      return 2;
    }
  }

  std::ifstream input(input_path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot open %s\n", input_path.c_str());
    return 2;
  }

  VerifyStatus status;
  status.records = records.size();
  std::size_t next = 0;
  std::uint64_t frame_index = 0;
  AccessUnitReader reader(format);
  reader.SetSink([&](AccessUnit&& unit, std::string*) {
    const std::uint64_t index = frame_index++;
    ++status.access_units;
    if (unit.key_frame)
      ++status.key_frames;
    const bool required = all_frames || unit.key_frame;
    if (required)
      ++status.required_units;
    while (next < records.size() && records[next].frame_index < index) {
      RecordFailure(&status, records[next].frame_index,
                    "signature record is missing: the stream ended that frame before it appeared");
      ++next;
    }
    bool covered = false;
    if (next < records.size() && records[next].frame_index == index) {
      const FrameSignature& record = records[next++];
      covered = true;
      if (required)
        ++status.covered_units;
      if (record.key_frame() != unit.key_frame) {
        RecordFailure(&status, index, "key-frame flag does not match the access unit");
        return true;
      }
      if (record.has_pts() != unit.has_pts) {
        RecordFailure(&status, index, "PTS presence does not match the access unit");
        return true;
      }
      if (unit.has_pts && record.pts != unit.pts) {
        RecordFailure(&status, index, "signed PTS " + std::to_string(record.pts) +
                                          " does not match stream PTS " +
                                          std::to_string(unit.pts));
        return true;
      }
      const std::uint64_t start = NowNs();
      std::string verify_error;
      const bool ok = uvnvr::gb35114::VerifyFrameSignature(options, unit.data.data(),
                                                           unit.data.size(), record, &verify_error);
      const std::uint64_t elapsed = NowNs() - start;
      status.verify_ns_total += elapsed;
      if (status.verified + status.failed == 0 || elapsed < status.verify_ns_min)
        status.verify_ns_min = elapsed;
      status.verify_ns_max = std::max(status.verify_ns_max, elapsed);
      if (ok) {
        ++status.verified;
      } else {
        RecordFailure(&status, index, verify_error);
      }
    }
    if (required && !covered) {
      RecordFailure(&status, index,
                    all_frames ? "no signature record for this access unit"
                               : "no signature record for this key frame");
    }
    return true;
  });

  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got > 0 &&
        !reader.Push(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                     static_cast<std::size_t>(got), &error)) {
      std::fprintf(stderr, "verify failed: %s\n", error.c_str());
      return 1;
    }
  }
  if (!reader.Flush(&error)) {
    std::fprintf(stderr, "verify failed: %s\n", error.c_str());
    return 1;
  }
  while (next < records.size()) {
    RecordFailure(&status, records[next].frame_index,
                  "signature record points past the end of the recording");
    ++next;
  }
  if (status.covered_units != status.required_units) {
    RecordFailure(&status, 0,
                  "signature coverage is incomplete: " + std::to_string(status.covered_units) +
                      " of " + std::to_string(status.required_units) +
                      (all_frames ? " access units signed" : " key frames signed"));
  }
  if (status.records != status.covered_units) {
    RecordFailure(&status, 0,
                  "signature file contains records for frames the stream does not require");
  }
  if (status.verified + status.failed == 0) {
    status.first_errors.push_back("no signature records were checked");
    ++status.failed;
  }

  const bool ok = status.failed == 0 && status.verified == status.records;
  const double total_ms = ToMs(status.verify_ns_total);
  const double checked = static_cast<double>(status.verified + status.failed);
  const double average_ms = checked == 0.0 ? 0.0 : total_ms / checked;
  if (args.Has("json")) {
    std::printf(
        "{\"ok\":%s,\"input\":\"%s\",\"signatures\":\"%s\",\"accessUnits\":%llu,"
        "\"keyFrames\":%llu,\"allFrames\":%s,\"requiredFrames\":%llu,"
        "\"coveredFrames\":%llu,\"records\":%llu,\"verified\":%llu,\"failed\":%llu,"
        "\"verifyTotalMs\":%.3f,\"verifyAvgMs\":%.3f,\"verifyMinMs\":%.3f,"
        "\"verifyMaxMs\":%.3f,\"errors\":[",
        ok ? "true" : "false", EscapeJson(input_path).c_str(),
        EscapeJson(signatures_path).c_str(),
        static_cast<unsigned long long>(status.access_units),
        static_cast<unsigned long long>(status.key_frames), all_frames ? "true" : "false",
        static_cast<unsigned long long>(status.required_units),
        static_cast<unsigned long long>(status.covered_units),
        static_cast<unsigned long long>(status.records),
        static_cast<unsigned long long>(status.verified),
        static_cast<unsigned long long>(status.failed), total_ms, average_ms,
        ToMs(status.verify_ns_min), ToMs(status.verify_ns_max));
    for (std::size_t i = 0; i < status.first_errors.size(); ++i) {
      if (i != 0)
        std::printf(",");
      std::printf("\"%s\"", EscapeJson(status.first_errors[i]).c_str());
    }
    std::printf("]}\n");
  } else {
    std::printf("input:         %s\n", input_path.c_str());
    std::printf("format:        %s\n", FormatName(format).c_str());
    std::printf("access units:  %llu (key frames: %llu)\n",
                static_cast<unsigned long long>(status.access_units),
                static_cast<unsigned long long>(status.key_frames));
    std::printf("signatures:    %llu\n", static_cast<unsigned long long>(status.records));
    std::printf("coverage:      %llu/%llu %s signed\n",
                static_cast<unsigned long long>(status.covered_units),
                static_cast<unsigned long long>(status.required_units),
                all_frames ? "access units" : "key frames");
    for (const auto& message : status.first_errors)
      std::printf("FAIL %s\n", message.c_str());
    std::printf("verified:      %llu, failed: %llu\n",
                static_cast<unsigned long long>(status.verified),
                static_cast<unsigned long long>(status.failed));
    std::printf("verify time:   %.3f ms total, %.3f ms average, %.3f ms min, %.3f ms max\n",
                total_ms, average_ms, ToMs(status.verify_ns_min), ToMs(status.verify_ns_max));
    std::printf("result:        %s\n", ok ? "OK" : "FAILED");
  }
  return ok ? 0 : 1;
}

int Inspect(const Args& args) {
  const std::string input_path = args.Get("input");
  if (input_path.empty()) {
    std::fprintf(stderr, "inspect needs --input\n");
    return 2;
  }
  bool format_ok = false;
  const InputFormat format = ParseFormat(args.Get("format", "auto"), &format_ok);
  if (!format_ok) {
    std::fprintf(stderr, "unknown --format; use auto, ts or es\n");
    return 2;
  }
  std::ifstream input(input_path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot open %s\n", input_path.c_str());
    return 2;
  }
  AccessUnitReader reader(format);
  std::uint64_t units = 0;
  std::uint64_t key_frames = 0;
  std::uint64_t first_pts = 0;
  std::uint64_t last_pts = 0;
  bool have_pts = false;
  std::string error;
  reader.SetSink([&](AccessUnit&& unit, std::string*) {
    ++units;
    if (unit.key_frame)
      ++key_frames;
    if (unit.has_pts) {
      if (!have_pts)
        first_pts = static_cast<std::uint64_t>(unit.pts);
      last_pts = static_cast<std::uint64_t>(unit.pts);
      have_pts = true;
    }
    return true;
  });
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got > 0 &&
        !reader.Push(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                     static_cast<std::size_t>(got), &error)) {
      std::fprintf(stderr, "inspect failed: %s\n", error.c_str());
      return 1;
    }
  }
  if (!reader.Flush(&error)) {
    std::fprintf(stderr, "inspect failed: %s\n", error.c_str());
    return 1;
  }
  if (args.Has("json")) {
    std::printf(
        "{\"input\":\"%s\",\"format\":\"%s\",\"bytes\":%llu,\"accessUnits\":%llu,"
        "\"keyFrames\":%llu,\"hasPts\":%s,\"firstPts\":%llu,\"lastPts\":%llu}\n",
        EscapeJson(input_path).c_str(), FormatName(format).c_str(),
        static_cast<unsigned long long>(reader.stats().input_bytes),
        static_cast<unsigned long long>(units), static_cast<unsigned long long>(key_frames),
        have_pts ? "true" : "false", static_cast<unsigned long long>(first_pts),
        static_cast<unsigned long long>(last_pts));
  } else {
    std::printf("input:        %s\n", input_path.c_str());
    std::printf("format:       %s\n", FormatName(format).c_str());
    std::printf("bytes:        %llu\n",
                static_cast<unsigned long long>(reader.stats().input_bytes));
    std::printf("access units: %llu\n", static_cast<unsigned long long>(units));
    std::printf("key frames:   %llu\n", static_cast<unsigned long long>(key_frames));
    if (have_pts)
      std::printf("pts:          first %llu, last %llu (90 kHz)\n",
                  static_cast<unsigned long long>(first_pts),
                  static_cast<unsigned long long>(last_pts));
    else
      std::printf("pts:          none (raw elementary stream)\n");
    std::printf("result:       OK\n");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::fprintf(stderr, "%s\n\n", args.error.c_str());
    PrintUsage();
    return 2;
  }
  if (args.command == "help" || args.Has("help") || args.command == "--help") {
    PrintUsage();
    return 0;
  }
  if (args.command == "keygen")
    return Keygen(args);
  if (args.command == "sign")
    return Sign(args);
  if (args.command == "verify")
    return Verify(args);
  if (args.command == "inspect")
    return Inspect(args);
  std::fprintf(stderr, "unknown command: %s\n\n", args.command.c_str());
  PrintUsage();
  return 2;
}
