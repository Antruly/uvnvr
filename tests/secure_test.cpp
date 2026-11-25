// Unit and integration tests for the secure module: the SM3 audit chain, SM2
// firmware package verification and SM4 data-at-rest encryption.
//
// Modes for manual evidence collection:
//
//   secure_test --seal <device-cert.pem> <plain> <sealed>
//   secure_test --open <device-cert.pem> <sealed> <plain>
//   secure_test --make-firmware <key.pem> <payload> <product> <version> <out>

#include "crypto/sm.h"
#include "secure/audit_chain.h"
#include "secure/audit_store.h"
#include "secure/firmware.h"
#include "secure/storage_crypto.h"

#include <db/uvcpp_db.h>

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

std::string ReadAll(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteAll(const std::string& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  return output.good();
}

bool WriteAll(const std::string& path, const uvnvr::gm::Bytes& data) {
  return WriteAll(path, std::string(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::filesystem::path PrepareArtifacts() {
  const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("uvnvr-secure-test-" + std::to_string(stamp));
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

const char* kTestCertificate = R"(-----BEGIN CERTIFICATE-----
MIIBvTCCAWKgAwIBAgIUZHVtbXktZGV2aWNlLWNlcnRpZmljYXRlMB4XDTI2MDEw
MTAwMDAwMFoXDTM2MDEwMTAwMDAwMFowgZAxCzAJBgNVBAYTAkNOMQ4wDAYDVQQK
DAVVdk5WUjEOMAwGA1UECwwFVGVzdDERMA8GA1UEAwwIdXZudnItdGVzdDAeFw0y
NjAxMDEwMDAwMDBaFw0zNjAxMDEwMDAwMDBaMIGQMQswCQYDVQQGEwJDTjEOMAwG
A1UECgwFVXZOVlIxDjAMBgNVBAsMBVRlc3QxETAPBgNVBAMMCHV2bnZyLXRlc3Qw
WTATBgcqhkjOPQIBBggqgRzPVQGCLQNCAAT7J3hQ3n3jW1m0o4r0wHVxZ4I4c3Y2
u3Xx0K1l2m3n4o5p6q7r8s9t0u1v2w3x4y5z6a7b8c9d0e1f2g3h4i5j6k=
-----END CERTIFICATE-----
)";

uvnvr::secure::AuditRecord MakeRecord(int64_t id) {
  uvnvr::secure::AuditRecord record;
  record.id = id;
  record.ts = 1700000000 + id;
  record.category = "auth";
  record.level = "info";
  record.actor = "admin";
  record.ip = "192.0.2.10";
  record.user_agent = "secure-test/1.0";
  record.object_type = "user";
  record.object_id = std::to_string(id);
  record.action = "login";
  record.result = "success";
  record.error_code = "";
  record.detail = R"({"n":)" + std::to_string(id) + "}";
  record.request_id = "";
  return record;
}

void RunAuditChainTests(std::vector<uvnvr::secure::AuditRecord>* chain_out,
                        std::string* anchor_out, std::string* head_out) {
  using namespace uvnvr::secure;
  std::vector<AuditRecord> chain;
  std::string previous(kAuditGenesisHash);
  for (int i = 1; i <= 32; ++i) {
    AuditRecord record = MakeRecord(i);
    AppendToAuditChain(previous, &record);
    previous = record.entry_hash;
    chain.push_back(std::move(record));
  }
  *chain_out = chain;
  *anchor_out = chain.front().prev_hash;
  *head_out = chain.back().entry_hash;

  const AuditVerification intact = VerifyAuditChain(chain, *anchor_out, *head_out);
  ExpectBool("audit chain verifies", intact.ok, true);
  ExpectSize("audit chain verified count", intact.checked, chain.size());

  std::vector<AuditRecord> tampered = chain;
  tampered[7].action = "delete_user";
  const AuditVerification modified = VerifyAuditChain(tampered, *anchor_out, *head_out);
  ExpectBool("tampered audit entry fails", modified.ok, false);
  ExpectSize("tampered audit break index", modified.broken_index, 7);
  Expect("tampered audit break id", std::to_string(modified.broken_id), "8");
  ExpectContains("tampered audit message", modified.message, "SM3 hash");

  std::vector<AuditRecord> relinked = chain;
  relinked[5].prev_hash = "00";
  const AuditVerification broken_link = VerifyAuditChain(relinked, *anchor_out, *head_out);
  ExpectBool("broken prev_hash fails", broken_link.ok, false);
  ExpectSize("broken link index", broken_link.broken_index, 5);
  Expect("broken link id", std::to_string(broken_link.broken_id), "6");

  std::vector<AuditRecord> truncated(chain.begin(), chain.end() - 1);
  const AuditVerification tail = VerifyAuditChain(truncated, *anchor_out, *head_out);
  ExpectBool("truncated tail fails", tail.ok, false);
  ExpectContains("truncated tail message", tail.message, "truncated");

  std::vector<AuditRecord> legacy;
  legacy.push_back(MakeRecord(1));
  legacy.push_back(MakeRecord(2));
  const AuditVerification uninitialised = VerifyAuditChain(legacy);
  ExpectBool("legacy-only log has no chain to verify", uninitialised.ok, true);
  ExpectSize("legacy rows skipped", uninitialised.skipped_legacy, 2);

  std::printf("audit: %zu entries verified; tamper at index 7 detected (entry id %lld)\n",
              intact.checked, static_cast<long long>(modified.broken_id));
}

void RunAuditDatabaseTest(const std::filesystem::path& artifacts,
                          const std::vector<uvnvr::secure::AuditRecord>& chain,
                          const std::string& anchor, const std::string& head) {
  using namespace uvnvr::secure;
  const std::filesystem::path db_path = artifacts / "audit.db";
  std::error_code remove_error;
  std::filesystem::remove(db_path, remove_error);

  uvcpp::uvcpp_db_client db;
  const auto opened = db.open("sqlite://" + db_path.string());
  if (!uvcpp::uvcpp_db_ok(opened)) {
    Expect("open audit database", db.last_error(), "");
    return;
  }
  auto execute = [&db](const std::string& sql, const uvcpp::uvcpp_db_params& params) {
    const auto status = db.execute(sql, params);
    if (!uvcpp::uvcpp_db_ok(status)) {
      Expect("audit database exec: " + db.last_error(), "ok", "failed");
      return false;
    }
    return true;
  };
  if (!execute("CREATE TABLE audit_log ("
               "id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,"
               "category TEXT NOT NULL, level TEXT NOT NULL, actor TEXT, ip TEXT,"
               "user_agent TEXT, object_type TEXT, object_id TEXT, action TEXT NOT NULL,"
               "result TEXT NOT NULL, error_code TEXT, detail TEXT, request_id TEXT,"
               "prev_hash TEXT NOT NULL DEFAULT '', entry_hash TEXT NOT NULL DEFAULT '')",
               {})) {
    return;
  }
  if (!execute("CREATE TABLE system_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)", {}))
    return;

  for (const AuditRecord& record : chain) {
    uvcpp::uvcpp_db_params params;
    params.add(record.ts);
    params.add(record.category);
    params.add(record.level);
    params.add(record.actor);
    params.add(record.ip);
    params.add(record.user_agent);
    params.add(record.object_type);
    params.add(record.object_id);
    params.add(record.action);
    params.add(record.result);
    params.add(record.error_code);
    params.add(record.detail);
    params.add(record.request_id);
    params.add(record.prev_hash);
    params.add(record.entry_hash);
    if (!execute("INSERT INTO audit_log(ts, category, level, actor, ip, user_agent, "
                 "object_type, object_id, action, result, error_code, detail, request_id, "
                 "prev_hash, entry_hash) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                 params)) {
      return;
    }
  }
  {
    uvcpp::uvcpp_db_params params;
    params.add("audit_chain_anchor");
    params.add(anchor);
    execute("INSERT OR REPLACE INTO system_meta(key, value) VALUES(?, ?)", params);
    uvcpp::uvcpp_db_params params_head;
    params_head.add("audit_chain_head");
    params_head.add(head);
    execute("INSERT OR REPLACE INTO system_meta(key, value) VALUES(?, ?)", params_head);
  }

  AuditVerification verification;
  std::string error;
  ExpectBool("database audit chain verifies", VerifyAuditDatabase(db_path.string(), &verification,
                                                                  &error),
             true);
  ExpectBool("database audit chain is ok", verification.ok, true);

  // Modify the 4th row (id 4). The chain must point at that exact row.
  {
    uvcpp::uvcpp_db_params params;
    params.add("tampered");
    params.add(static_cast<int64_t>(4));
    if (!execute("UPDATE audit_log SET action = ? WHERE id = ?", params))
      return;
  }
  ExpectBool("database audit verify runs", VerifyAuditDatabase(db_path.string(), &verification,
                                                               &error),
             true);
  ExpectBool("database tamper detected", verification.ok, false);
  Expect("database tamper broken id", std::to_string(verification.broken_id), "4");
  ExpectSize("database tamper broken index", verification.broken_index, 3);
  std::printf("audit-db: tampered row id 4 -> %s\n", verification.message.c_str());
}

void RunFirmwareTests(const std::filesystem::path& artifacts) {
  using namespace uvnvr::secure;
  using uvnvr::gm::Bytes;

  std::string error;
  uvnvr::gm::Sm2KeyPair signer;
  uvnvr::gm::Sm2KeyPair other;
  if (!uvnvr::gm::GenerateSm2KeyPair(&signer, &error) ||
      !uvnvr::gm::GenerateSm2KeyPair(&other, &error)) {
    Expect("firmware key generation", error, "");
    return;
  }

  const std::string payload_text =
      "UvNVR firmware payload for the signature test\n"
      "second line: 0123456789\n";
  Bytes payload(payload_text.begin(), payload_text.end());
  Bytes package;
  if (!CreateFirmwarePackage(payload, "uvnvr", "0.19.1-test", signer.private_key_pem, &package,
                             &error)) {
    Expect("firmware package creation", error, "");
    return;
  }
  WriteAll((artifacts / "firmware.pkg").string(), package);
  WriteAll((artifacts / "firmware_pub.pem").string(), signer.public_key_pem);

  FirmwarePackage verified;
  ExpectBool("firmware package verifies",
             VerifyFirmwarePackage(package, signer.public_key_pem, "uvnvr", &verified, &error),
             true);
  Expect("firmware version", verified.version, "0.19.1-test");
  Expect("firmware product", verified.product, "uvnvr");

  ExpectBool("firmware rejects the wrong product",
             VerifyFirmwarePackage(package, signer.public_key_pem, "other", &verified, &error),
             false);
  ExpectBool("firmware rejects a different key",
             VerifyFirmwarePackage(package, other.public_key_pem, "uvnvr", &verified, &error),
             false);

  Bytes forged = package;
  forged.back() ^= 0x40;
  WriteAll((artifacts / "firmware_forged.pkg").string(), forged);
  ExpectBool("forged firmware payload fails",
             VerifyFirmwarePackage(forged, signer.public_key_pem, "uvnvr", &verified, &error),
             false);
  ExpectContains("forged firmware error", error, "payload");

  Bytes forged_signature = package;
  // The signature sits between the header and the payload; flipping one byte
  // of it keeps the container parseable but invalidates SM2.
  const std::uint32_t header_length =
      static_cast<std::uint32_t>(forged_signature[6]) |
      (static_cast<std::uint32_t>(forged_signature[7]) << 8) |
      (static_cast<std::uint32_t>(forged_signature[8]) << 16) |
      (static_cast<std::uint32_t>(forged_signature[9]) << 24);
  const std::size_t signature_offset = 14 + header_length;
  if (signature_offset < forged_signature.size())
    forged_signature[signature_offset] ^= 0x01;
  ExpectBool("forged firmware signature fails",
             VerifyFirmwarePackage(forged_signature, signer.public_key_pem, "uvnvr", &verified,
                                   &error),
             false);

  std::printf("firmware: valid package accepted; forged payload and signature rejected\n");
}

void RunStorageTests(const std::filesystem::path& artifacts) {
  using namespace uvnvr::secure;
  using uvnvr::gm::Bytes;

  Bytes key;
  std::string error;
  ExpectBool("storage key derivation", DeriveStorageKey(kTestCertificate, &key, &error), true);
  ExpectSize("storage key length", key.size(), uvnvr::gm::kSm4KeySize);

  Bytes other_key;
  DeriveStorageKey(kTestCertificate + std::string("\n"), &other_key, &error);
  ExpectBool("certificate bytes change the key", key != other_key, true);

  const std::string plaintext = "JPEG-ish snapshot contents\n\xff\xd8\xff\xe0 binary tail";
  Bytes plain(plaintext.begin(), plaintext.end());
  Bytes sealed;
  ExpectBool("seal snapshot", SealBlob(plain, key, &sealed, &error), true);
  ExpectContains("sealed magic", std::string(sealed.begin(), sealed.begin() + 4), "UVSE");
  ExpectSize("sealed size", sealed.size(), kSealedHeaderSize + plain.size());
  WriteAll((artifacts / "snapshot.jpg.enc").string(), sealed);

  Bytes opened;
  ExpectBool("open snapshot", OpenBlob(sealed, key, &opened, &error), true);
  Expect("snapshot round-trip", std::string(opened.begin(), opened.end()), plaintext);

  Bytes wrong;
  Bytes other_derived;
  DeriveStorageKey(kTestCertificate + std::string("\nextra"), &other_derived, &error);
  ExpectBool("wrong key is rejected", OpenBlob(sealed, other_derived, &wrong, &error), false);

  Bytes corrupted = sealed;
  corrupted.back() ^= 0x20;
  ExpectBool("corrupted sealed blob is rejected", OpenBlob(corrupted, key, &wrong, &error), false);

  // Recording container: same length, random access by plaintext offset.
  std::string recording;
  recording.reserve(300000);
  for (int i = 0; i < 300000; ++i)
    recording.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
  const std::filesystem::path plain_path = artifacts / "segment.ts.part";
  const std::filesystem::path enc_path = artifacts / "segment.ts.enc";
  WriteAll(plain_path.string(), recording);
  ExpectBool("encrypt recording",
             EncryptRecordingFile(plain_path.string(), enc_path.string(), key, &error), true);
  const auto plain_size = std::filesystem::file_size(plain_path);
  const auto enc_size = std::filesystem::file_size(enc_path);
  ExpectBool("recording ciphertext has the same size", plain_size == enc_size, true);

  for (const std::size_t offset : {std::size_t{0}, std::size_t{1}, std::size_t{12345},
                                   std::size_t{65536}, std::size_t{299000}}) {
    Bytes range;
    const std::size_t length = std::min<std::size_t>(4096, recording.size() - offset);
    ExpectBool("read decrypted recording range",
               ReadDecryptedRecordingRange(key, enc_path.string(), offset, length, &range, &error),
               true);
    Expect("recording range content at " + std::to_string(offset),
           std::string(range.begin(), range.end()), recording.substr(offset, length));
  }
  ExpectBool("encrypted path detection", IsEncryptedRecordingPath(enc_path.string()), true);
  ExpectBool("plain path is not encrypted", IsEncryptedRecordingPath(plain_path.string()), false);

  std::printf("storage: sealed header=");
  for (std::size_t i = 0; i < 8 && i < sealed.size(); ++i)
    std::printf("%02x", sealed[i]);
  std::printf(" (%zu bytes plaintext -> %zu bytes sealed); recording %llu bytes at rest\n",
              plain.size(), sealed.size(), static_cast<unsigned long long>(enc_size));
}

int RunSeal(const char* cert_path, const char* input_path, const char* output_path) {
  uvnvr::gm::Bytes key;
  std::string error;
  if (!uvnvr::secure::DeriveStorageKeyFromFile(cert_path, &key, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  const std::string plain = ReadAll(input_path);
  if (plain.empty()) {
    std::fprintf(stderr, "cannot read %s\n", input_path);
    return 2;
  }
  uvnvr::gm::Bytes sealed;
  if (!uvnvr::secure::SealBlob(uvnvr::gm::Bytes(plain.begin(), plain.end()), key, &sealed,
                               &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(output_path, sealed)) {
    std::fprintf(stderr, "cannot write %s\n", output_path);
    return 2;
  }
  std::printf("sealed %zu bytes -> %zu bytes\n", plain.size(), sealed.size());
  return 0;
}

int RunOpen(const char* cert_path, const char* input_path, const char* output_path) {
  uvnvr::gm::Bytes key;
  std::string error;
  if (!uvnvr::secure::DeriveStorageKeyFromFile(cert_path, &key, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  uvnvr::gm::Bytes plain;
  if (!uvnvr::secure::OpenSealedFile(input_path, key, &plain, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(output_path, plain)) {
    std::fprintf(stderr, "cannot write %s\n", output_path);
    return 2;
  }
  return 0;
}

int RunMakeFirmware(const char* key_path, const char* payload_path, const char* product,
                    const char* version, const char* output_path) {
  const std::string key = ReadAll(key_path);
  const std::string payload_text = ReadAll(payload_path);
  if (key.empty() || payload_text.empty()) {
    std::fprintf(stderr, "cannot read the key or payload\n");
    return 2;
  }
  uvnvr::gm::Bytes package;
  std::string error;
  if (!uvnvr::secure::CreateFirmwarePackage(
          uvnvr::gm::Bytes(payload_text.begin(), payload_text.end()), product, version, key,
          &package, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (!WriteAll(output_path, package)) {
    std::fprintf(stderr, "cannot write %s\n", output_path);
    return 2;
  }
  std::printf("wrote %zu-byte package\n", package.size());
  return 0;
}

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage:\n"
               "  %s                                            run the tests\n"
               "  %s --seal <cert.pem> <plain> <sealed>         seal a file\n"
               "  %s --open <cert.pem> <sealed> <plain>         open a sealed file\n"
               "  %s --make-firmware <key.pem> <payload> <product> <version> <out>\n",
               program, program, program, program);
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string mode = argv[1];
    if (mode == "--seal" && argc == 5)
      return RunSeal(argv[2], argv[3], argv[4]);
    if (mode == "--open" && argc == 5)
      return RunOpen(argv[2], argv[3], argv[4]);
    if (mode == "--make-firmware" && argc == 7)
      return RunMakeFirmware(argv[2], argv[3], argv[4], argv[5], argv[6]);
    PrintUsage(argv[0]);
    return 2;
  }

  const std::filesystem::path artifacts = PrepareArtifacts();
  std::vector<uvnvr::secure::AuditRecord> chain;
  std::string anchor;
  std::string head;
  RunAuditChainTests(&chain, &anchor, &head);
  RunAuditDatabaseTest(artifacts, chain, anchor, head);
  RunFirmwareTests(artifacts);
  RunStorageTests(artifacts);

  if (failures == 0) {
    std::printf("secure_test: all checks passed\n");
    std::printf("secure_test: artifacts in %s\n", artifacts.string().c_str());
    return 0;
  }
  std::fprintf(stderr, "secure_test: %d checks failed\n", failures);
  return 1;
}
