// Tests for startup configuration integrity: the SM3/SM2 manifest, tamper
// detection, the signed-manifest downgrade guard, and the unknown-key report
// produced by the real config parser.

#include "config.h"
#include "crypto/sm.h"
#include "secure/config_integrity.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void ExpectTrue(const std::string& name, bool condition) {
  if (condition)
    return;
  std::fprintf(stderr, "FAIL %s\n", name.c_str());
  ++failures;
}

void ExpectEqual(const std::string& name, const std::string& actual,
                 const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
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

bool WriteAll(const std::string& path, std::string_view data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  return output.good();
}

std::filesystem::path PrepareDirectory() {
  const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("uvnvr-config-integrity-test-" + std::to_string(stamp));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::fprintf(stderr, "cannot create %s: %s\n", directory.string().c_str(),
                 error.message().c_str());
  }
  return directory;
}

const char* kConfigText = R"({
  "http": {"port": 8080, "host": "0.0.0.0"},
  "rtsp": {"port": 8554},
  "private": {"port": 1515, "tcp": true, "quic": true},
  "logging": {"level": "info"}
})";

void TestDigestManifest(const std::filesystem::path& directory) {
  const std::string contents = kConfigText;
  uvnvr::secure::ConfigManifest manifest;
  std::string error;
  ExpectTrue("create digest manifest",
             uvnvr::secure::CreateConfigManifest(contents, "", &manifest, &error));
  ExpectTrue("digest-only manifest has no signature", manifest.signature.empty());
  ExpectEqual("manifest digest", manifest.config_sm3, uvnvr::gm::Sm3Hex(contents));

  const std::string manifest_path = (directory / "uvnvr.json.sm3").string();
  ExpectTrue("write digest manifest",
             uvnvr::secure::WriteConfigManifest(manifest_path, manifest, &error));
  const std::string text = ReadAll(manifest_path);
  ExpectContains("manifest header", text, "uvnvr-config-integrity-v1");
  ExpectContains("manifest algorithm", text, "algorithm sm3");
  ExpectContains("manifest digest line", text, "digest " + manifest.config_sm3);

  uvnvr::secure::ConfigManifest reread;
  ExpectTrue("reread manifest",
             uvnvr::secure::ReadConfigManifest(manifest_path, &reread, &error));
  ExpectEqual("reread digest", reread.config_sm3, manifest.config_sm3);
  ExpectTrue("reread size", reread.config_size == manifest.config_size);

  uvnvr::secure::ConfigIntegrityResult result;
  ExpectTrue("verify matching content",
             uvnvr::secure::VerifyConfigText(contents, manifest_path, "", &result, &error));
  ExpectTrue("matching content is ok", result.ok);
  ExpectTrue("manifest was checked", result.checked);
  ExpectTrue("digest manifest is not signed", !result.signed_manifest);
  ExpectEqual("result actual sm3", result.actual_sm3, manifest.config_sm3);
}

void TestTamperDetection(const std::filesystem::path& directory) {
  const std::string contents = kConfigText;
  uvnvr::secure::ConfigManifest manifest;
  std::string error;
  ExpectTrue("create manifest for tamper test",
             uvnvr::secure::CreateConfigManifest(contents, "", &manifest, &error));
  const std::string manifest_path = (directory / "tamper.sm3").string();
  ExpectTrue("write manifest for tamper test",
             uvnvr::secure::WriteConfigManifest(manifest_path, manifest, &error));

  // Same length as the original: catches a verifier that only compares sizes.
  std::string tampered = contents;
  const std::string from = "\"port\": 1515";
  const std::string to = "\"port\": 1516";
  const std::size_t position = tampered.find(from);
  ExpectTrue("tamper target exists", position != std::string::npos);
  if (position != std::string::npos)
    tampered.replace(position, from.size(), to);
  ExpectTrue("tampered content keeps its size", tampered.size() == contents.size());

  uvnvr::secure::ConfigIntegrityResult result;
  ExpectTrue("verify tampered content",
             uvnvr::secure::VerifyConfigText(tampered, manifest_path, "", &result, &error));
  ExpectTrue("tampered content is rejected", !result.ok);
  ExpectContains("tamper message", result.message, "changed");
  ExpectTrue("tamper expected digest is from the manifest",
             result.expected_sm3 == manifest.config_sm3);
  ExpectTrue("tamper actual digest differs", result.actual_sm3 != manifest.config_sm3);
}

void TestSignedManifest(const std::filesystem::path& directory) {
  uvnvr::gm::Sm2KeyPair keys;
  std::string error;
  ExpectTrue("generate SM2 key pair", uvnvr::gm::GenerateSm2KeyPair(&keys, &error));
  if (keys.private_key_pem.empty()) {
    std::fprintf(stderr, "FAIL cannot continue signed-manifest test without keys: %s\n",
                 error.c_str());
    ++failures;
    return;
  }

  const std::string contents = kConfigText;
  uvnvr::secure::ConfigManifest manifest;
  ExpectTrue("create signed manifest",
             uvnvr::secure::CreateConfigManifest(contents, keys.private_key_pem, &manifest,
                                                 &error));
  ExpectTrue("signed manifest carries a signature", !manifest.signature.empty());
  const std::string manifest_path = (directory / "signed.sm3").string();
  ExpectTrue("write signed manifest",
             uvnvr::secure::WriteConfigManifest(manifest_path, manifest, &error));

  uvnvr::secure::ConfigIntegrityResult result;
  ExpectTrue("verify signed manifest",
             uvnvr::secure::VerifyConfigText(contents, manifest_path, keys.public_key_pem,
                                             &result, &error));
  ExpectTrue("signed manifest is ok", result.ok);
  ExpectTrue("signed manifest is marked signed", result.signed_manifest);

  // Signed manifest without a public key cannot be verified.
  uvnvr::secure::ConfigIntegrityResult no_key_result;
  std::string no_key_error;
  const bool performed = uvnvr::secure::VerifyConfigText(contents, manifest_path, "",
                                                         &no_key_result, &no_key_error);
  ExpectTrue("signed manifest without a key is not verified", !performed);
  ExpectContains("missing key error", no_key_error, "public key");
  ExpectTrue("missing key result is not ok", !no_key_result.ok);

  // A different key must not verify.
  uvnvr::gm::Sm2KeyPair other;
  ExpectTrue("generate second SM2 key pair", uvnvr::gm::GenerateSm2KeyPair(&other, &error));
  uvnvr::secure::ConfigIntegrityResult wrong_key_result;
  ExpectTrue("verify with wrong key performs",
             uvnvr::secure::VerifyConfigText(contents, manifest_path, other.public_key_pem,
                                             &wrong_key_result, &error));
  ExpectTrue("wrong key is rejected", !wrong_key_result.ok);
  ExpectContains("wrong key message", wrong_key_result.message, "signature");

  // A signed deployment must not silently accept a digest-only manifest.
  const std::string digest_only_path = (directory / "unsigned.sm3").string();
  uvnvr::secure::ConfigManifest digest_only;
  ExpectTrue("create digest-only manifest for downgrade test",
             uvnvr::secure::CreateConfigManifest(contents, "", &digest_only, &error));
  ExpectTrue("write digest-only manifest",
             uvnvr::secure::WriteConfigManifest(digest_only_path, digest_only, &error));
  uvnvr::secure::ConfigIntegrityResult downgrade;
  ExpectTrue("downgrade check performs",
             uvnvr::secure::VerifyConfigText(contents, digest_only_path, keys.public_key_pem,
                                             &downgrade, &error));
  ExpectTrue("digest-only manifest is rejected when a key is configured", !downgrade.ok);
  ExpectContains("downgrade message", downgrade.message, "not signed");
}

void TestMissingAndMalformedManifest(const std::filesystem::path& directory) {
  const std::string contents = kConfigText;
  uvnvr::secure::ConfigIntegrityResult result;
  std::string error;
  ExpectTrue("missing manifest does not verify",
             !uvnvr::secure::VerifyConfigText(contents, (directory / "absent.sm3").string(), "",
                                              &result, &error));
  ExpectContains("missing manifest error", error, "manifest");

  const std::string malformed_path = (directory / "malformed.sm3").string();
  ExpectTrue("write malformed manifest", WriteAll(malformed_path, "not a manifest\n"));
  uvnvr::secure::ConfigManifest manifest;
  ExpectTrue("malformed manifest is rejected",
             !uvnvr::secure::ReadConfigManifest(malformed_path, &manifest, &error));

  const std::string bad_digest_path = (directory / "bad-digest.sm3").string();
  ExpectTrue("write bad digest manifest",
             WriteAll(bad_digest_path, std::string(uvnvr::secure::kConfigIntegrityVersion) +
                                           "\nalgorithm sm3\nsize 1\ndigest zz\n"));
  ExpectTrue("bad digest manifest is rejected",
             !uvnvr::secure::ReadConfigManifest(bad_digest_path, &manifest, &error));
}

void TestFileRoundTrip(const std::filesystem::path& directory) {
  const std::string config_path = (directory / "uvnvr.json").string();
  const std::string manifest_path = (directory / "uvnvr.json.sm3").string();
  ExpectTrue("write config file", WriteAll(config_path, kConfigText));
  std::string error;
  ExpectTrue("write manifest for file",
             uvnvr::secure::WriteConfigManifestForContents(kConfigText, manifest_path, "", &error));

  uvnvr::secure::ConfigIntegrityResult result;
  ExpectTrue("verify config file",
             uvnvr::secure::VerifyConfigFile(config_path, manifest_path, "", &result, &error));
  ExpectTrue("config file verifies", result.ok);

  // The default sidecar path is what the CLI uses when --config-manifest is
  // not given.
  ExpectEqual("default manifest path",
              uvnvr::secure::DefaultConfigManifestPath(config_path), config_path + ".sm3");
}

void TestUnknownKeys() {
  const auto document = nlohmann::json::parse(R"({
    "http": {"port": 8080, "prot": 1, "tls": {"cert": "a", "mystery": true}},
    "logging": {"level": "info", "levl": "debug"},
    "typoRoot": 1,
    "config": {"path": "x", "loaded": true}
  })");
  ExpectTrue("unknown-key fixture parses", !document.is_discarded());

  uvnvr::Config config;
  std::string error;
  uvnvr::ConfigLoadReport report;
  ExpectTrue("apply config with unknown keys",
             uvnvr::ApplyConfigJson(document, &config, &error, &report));

  const auto contains = [&report](const std::string& key) {
    for (const std::string& entry : report.unknown_keys) {
      if (entry == key)
        return true;
    }
    return false;
  };
  ExpectTrue("root typo is reported", contains("typoRoot"));
  ExpectTrue("nested http typo is reported", contains("http.prot"));
  ExpectTrue("nested tls typo is reported", contains("http.tls.mystery"));
  ExpectTrue("nested logging typo is reported", contains("logging.levl"));
  ExpectTrue("known key is not reported", !contains("http.port"));
  ExpectTrue("known logging key is not reported", !contains("logging.level"));
  ExpectTrue("config diagnostics object is not reported", !contains("config"));
  ExpectTrue("config.path is not reported", !contains("config.path"));

  // LoadConfigText reports through the same path startup uses.
  uvnvr::Config loaded;
  uvnvr::ConfigLoadReport load_report;
  ExpectTrue("load config text",
             uvnvr::LoadConfigText(R"({"rtsp": {"port": 8554}, "bogus": 1})", "test.json",
                                   &loaded, &error, &load_report));
  ExpectTrue("LoadConfigText records unknown keys", load_report.unknown_keys.size() == 1);
  ExpectTrue("LoadConfigText sets the path", loaded.config_path == "test.json");
  ExpectTrue("LoadConfigText marks the config loaded", loaded.config_loaded);
}

void TestModeParsing() {
  uvnvr::secure::ConfigIntegrityMode mode = uvnvr::secure::ConfigIntegrityMode::kWarn;
  ExpectTrue("parse off",
             uvnvr::secure::ParseConfigIntegrityMode("off", &mode) &&
                 mode == uvnvr::secure::ConfigIntegrityMode::kOff);
  ExpectTrue("parse warn",
             uvnvr::secure::ParseConfigIntegrityMode("warn", &mode) &&
                 mode == uvnvr::secure::ConfigIntegrityMode::kWarn);
  ExpectTrue("parse enforce",
             uvnvr::secure::ParseConfigIntegrityMode("enforce", &mode) &&
                 mode == uvnvr::secure::ConfigIntegrityMode::kEnforce);
  ExpectTrue("reject unknown mode",
             !uvnvr::secure::ParseConfigIntegrityMode("block", &mode));
}

int RunUnknownKeyTestOnInvalidJson() {
  uvnvr::Config config;
  std::string error;
  uvnvr::ConfigLoadReport report;
  if (uvnvr::ApplyConfigJson(nlohmann::json::parse("[]"), &config, &error, &report))
    return 1;
  return error.empty() ? 1 : 0;
}

} // namespace

int main() {
  const std::filesystem::path directory = PrepareDirectory();
  TestModeParsing();
  TestDigestManifest(directory);
  TestTamperDetection(directory);
  TestSignedManifest(directory);
  TestMissingAndMalformedManifest(directory);
  TestFileRoundTrip(directory);
  TestUnknownKeys();
  if (RunUnknownKeyTestOnInvalidJson() != 0) {
    std::fprintf(stderr, "FAIL non-object config root is rejected with an error\n");
    ++failures;
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(directory, cleanup_error);

  if (failures != 0) {
    std::fprintf(stderr, "%d config integrity test(s) failed\n", failures);
    return 1;
  }
  std::printf("config integrity tests passed\n");
  return 0;
}
