#include "ai/ai_rules.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectString(const std::string& name, const std::string& actual,
                  const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
  ++failures;
}

void ExpectNear(const std::string& name, double actual, double expected, double tolerance) {
  if (std::fabs(actual - expected) <= tolerance)
    return;
  std::fprintf(stderr, "FAIL %s: got %.6f, expected %.6f\n", name.c_str(), actual, expected);
  ++failures;
}

uvnvr::FaceLibraryEntry Entry(int64_t id, const std::string& name,
                              std::initializer_list<float> values) {
  uvnvr::FaceLibraryEntry entry;
  entry.id = id;
  entry.name = name;
  entry.feature.assign(values.begin(), values.end());
  return entry;
}

void TestPlateLists() {
  const std::string blacklist = "京A12345, 粤B12345;沪C12345";
  const std::string whitelist = "粤B12345\n京A00001";

  auto verdict = uvnvr::EvaluatePlateLists("京A12345", blacklist, whitelist);
  ExpectBool("blacklist: alert", verdict.ShouldAlert(), true);
  ExpectBool("blacklist: black flag", verdict.blacklisted, true);
  ExpectBool("blacklist: not white", verdict.whitelisted, false);
  ExpectString("blacklist: label", verdict.MatchLabel(), "black");

  // The probe is normalised before comparison.
  verdict = uvnvr::EvaluatePlateLists("京a·12345", blacklist, whitelist);
  ExpectBool("normalised probe: alert", verdict.ShouldAlert(), true);

  // Whitelist wins when a plate is on both lists.
  verdict = uvnvr::EvaluatePlateLists("粤B12345", blacklist, whitelist);
  ExpectBool("both lists: no alert", verdict.ShouldAlert(), false);
  ExpectBool("both lists: black flag", verdict.blacklisted, true);
  ExpectBool("both lists: white flag", verdict.whitelisted, true);
  ExpectString("both lists: label", verdict.MatchLabel(), "white");

  verdict = uvnvr::EvaluatePlateLists("京A99999", blacklist, whitelist);
  ExpectBool("unlisted: no alert", verdict.ShouldAlert(), false);
  ExpectString("unlisted: empty label", verdict.MatchLabel(), "");

  verdict = uvnvr::EvaluatePlateLists("", blacklist, whitelist);
  ExpectBool("empty probe: no alert", verdict.ShouldAlert(), false);

  verdict = uvnvr::EvaluatePlateLists("京A12345", "", "");
  ExpectBool("empty lists: no alert", verdict.ShouldAlert(), false);

  // A malformed entry must not accidentally match.
  verdict = uvnvr::EvaluatePlateLists("京A12345", " , ;\n", "");
  ExpectBool("blank entries: no alert", verdict.ShouldAlert(), false);
}

void TestFaceMatch1toN() {
  const std::vector<uvnvr::FaceLibraryEntry> library = {
      Entry(3, "alice", {1.0f, 0.0f, 0.0f}),
      Entry(7, "bob", {0.9f, 0.1f, 0.0f}),
      Entry(9, "carol", {0.0f, 1.0f, 0.0f}),
  };

  auto match = uvnvr::MatchFaceFeature(library, {1.0f, 0.0f, 0.0f}, 0.75);
  ExpectBool("1:N: found", match.found, true);
  ExpectBool("1:N: best id", match.id == 3, true);
  ExpectString("1:N: best name", match.name, "alice");
  ExpectNear("1:N: perfect score", match.score, 1.0, 1e-6);

  // bob's score is cos(0.9,0.1) ~= 0.9939, still above the threshold.
  match = uvnvr::MatchFaceFeature(library, {0.9f, 0.1f, 0.0f}, 0.75);
  ExpectBool("1:N: second entry", match.id == 7, true);

  // Similarity to bob (0.9,0.1) is higher than to alice, so bob is the best hit.
  match = uvnvr::MatchFaceFeature(library, {1.0f, 1.0f, 0.0f}, 0.99);
  ExpectBool("1:N: below threshold", match.found, false);
  ExpectNear("1:N: best score kept", match.score, 0.780869, 1e-5);

  match = uvnvr::MatchFaceFeature(library, {1.0f, 0.0f, 0.0f}, 1.0);
  ExpectBool("1:N: threshold boundary accepted", match.found, true);

  match = uvnvr::MatchFaceFeature(library, {1.0f, 0.0f, 0.0f}, 1.0001);
  ExpectBool("1:N: above boundary rejected", match.found, false);

  match = uvnvr::MatchFaceFeature({}, {1.0f, 0.0f, 0.0f}, 0.0);
  ExpectBool("1:N: empty library", match.found, false);

  match = uvnvr::MatchFaceFeature(library, {}, 0.0);
  ExpectBool("1:N: empty probe", match.found, false);

  match = uvnvr::MatchFaceFeature(library, {1.0f, 0.0f}, 0.0);
  ExpectBool("1:N: dimension mismatch", match.found, false);

  const std::vector<uvnvr::FaceLibraryEntry> zero = {Entry(1, "zero", {0.0f, 0.0f})};
  match = uvnvr::MatchFaceFeature(zero, {1.0f, 0.0f}, 0.0);
  ExpectBool("1:N: zero vector", match.found, false);
}

void TestRankFaces() {
  const std::vector<uvnvr::FaceLibraryEntry> library = {
      Entry(3, "alice", {1.0f, 0.0f, 0.0f}),
      Entry(7, "bob", {0.9f, 0.1f, 0.0f}),
      Entry(9, "carol", {0.0f, 1.0f, 0.0f}),
  };
  const auto ranked = uvnvr::RankFaceMatches(library, {1.0f, 0.0f, 0.0f}, 0.75, 2);
  ExpectBool("rank: limited size", ranked.size() == 2, true);
  ExpectBool("rank: first is best", ranked[0].id == 3, true);
  ExpectBool("rank: first matched", ranked[0].found, true);
  ExpectBool("rank: second is bob", ranked[1].id == 7, true);

  const auto all = uvnvr::RankFaceMatches(library, {0.0f, 1.0f, 0.0f}, 0.75, 0);
  ExpectBool("rank: zero limit returns all", all.size() == library.size(), true);
  ExpectBool("rank: carol first", all[0].id == 9, true);

  const auto none = uvnvr::RankFaceMatches(library, {}, 0.0, 10);
  ExpectBool("rank: empty probe", none.empty(), true);
}

} // namespace

int main() {
  TestPlateLists();
  TestFaceMatch1toN();
  TestRankFaces();
  if (failures == 0) {
    std::printf("ai_rules_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "ai_rules_test: %d check(s) failed\n", failures);
  return 1;
}
