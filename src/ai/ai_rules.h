#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr {

// Result of matching one plate against the configured black/white lists.
// Whitelist entries take precedence: a plate on both lists is suppressed.
struct PlateListVerdict {
  bool blacklisted = false;
  bool whitelisted = false;

  bool ShouldAlert() const {
    return blacklisted && !whitelisted;
  }

  // "black", "white" or "" (neither list matched). This string is carried in
  // the WebSocket event so the UI can explain why an alert fired.
  const char* MatchLabel() const {
    return whitelisted ? "white" : (blacklisted ? "black" : "");
  }
};

// Evaluates a plate against the comma/semicolon/whitespace separated lists.
// Both the probe and every entry go through NormalizePlate(), so
// "京a·12345" matches "京A12345". Empty or unnormalisable entries are ignored.
PlateListVerdict EvaluatePlateLists(const std::string& plate, const std::string& blacklist,
                                    const std::string& whitelist);

// One entry of the local face library, as loaded for 1:N comparison.
struct FaceLibraryEntry {
  int64_t id = 0;
  std::string name;
  std::vector<float> feature;
};

// Best result of a 1:N search. `score` always carries the highest cosine
// similarity found; `found` is true only when that score reaches the threshold
// and the entry has a positive id.
struct FaceMatchResult {
  bool found = false;
  int64_t id = 0;
  std::string name;
  double score = 0.0;
};

// Compares one probe feature against every library entry. Mismatched feature
// dimensions and non-finite values score 0. Ties keep the lower id because the
// library is expected to be ordered by id.
FaceMatchResult MatchFaceFeature(const std::vector<FaceLibraryEntry>& library,
                                 const std::vector<float>& feature, double threshold);

// Same comparison, but returns the best `limit` entries sorted by descending
// score. Every hit reports whether it independently reached the threshold.
std::vector<FaceMatchResult> RankFaceMatches(const std::vector<FaceLibraryEntry>& library,
                                             const std::vector<float>& feature, double threshold,
                                             int limit);

} // namespace uvnvr
