#include "ai_rules.h"

#include "plate_post.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace uvnvr {
namespace {

std::set<std::string> NormalizedList(const std::string& text) {
  std::set<std::string> out;
  std::string current;
  const auto flush = [&]() {
    if (!current.empty()) {
      const std::string normalized = NormalizePlate(current);
      if (!normalized.empty())
        out.insert(normalized);
      current.clear();
    }
  };
  for (const char c : text) {
    if (c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      flush();
    } else {
      current.push_back(c);
    }
  }
  flush();
  return out;
}

double CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || a.size() != b.size())
    return 0.0;
  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double x = static_cast<double>(a[i]);
    const double y = static_cast<double>(b[i]);
    if (!std::isfinite(x) || !std::isfinite(y))
      return 0.0;
    dot += x * y;
    na += x * x;
    nb += y * y;
  }
  if (na <= 1e-9 || nb <= 1e-9)
    return 0.0;
  const double score = dot / (std::sqrt(na) * std::sqrt(nb));
  // Floating-point error can put a perfect match a hair above 1.0; clamp so
  // callers can render the score as a percentage without sanity checks.
  return std::max(0.0, std::min(1.0, score));
}

// A feature of the wrong size, full of non-finite values, or all zeros has no
// meaningful cosine similarity. Skipping it keeps a threshold of 0 from
// declaring an empty entry a match.
bool UsableFeature(const std::vector<float>& feature) {
  if (feature.empty())
    return false;
  bool non_zero = false;
  for (const float value : feature) {
    if (!std::isfinite(value))
      return false;
    if (value != 0.0f)
      non_zero = true;
  }
  return non_zero;
}

} // namespace

PlateListVerdict EvaluatePlateLists(const std::string& plate, const std::string& blacklist,
                                    const std::string& whitelist) {
  PlateListVerdict verdict;
  const std::string normalized = NormalizePlate(plate);
  if (normalized.empty())
    return verdict;
  verdict.blacklisted = NormalizedList(blacklist).count(normalized) > 0;
  verdict.whitelisted = NormalizedList(whitelist).count(normalized) > 0;
  return verdict;
}

FaceMatchResult MatchFaceFeature(const std::vector<FaceLibraryEntry>& library,
                                 const std::vector<float>& feature, double threshold) {
  FaceMatchResult best;
  if (!UsableFeature(feature))
    return best;
  for (const auto& entry : library) {
    if (entry.id <= 0 || !UsableFeature(entry.feature) || entry.feature.size() != feature.size())
      continue;
    const double score = CosineSimilarity(feature, entry.feature);
    // Equal scores keep the lower id, matching RankFaceMatches and the
    // documented ordering so a search result is deterministic even when the
    // caller did not sort the library.
    if (best.id == 0 || score > best.score || (score == best.score && entry.id < best.id)) {
      best.id = entry.id;
      best.name = entry.name;
      best.score = score;
    }
  }
  best.found = best.id != 0 && best.score >= threshold;
  return best;
}

std::vector<FaceMatchResult> RankFaceMatches(const std::vector<FaceLibraryEntry>& library,
                                             const std::vector<float>& feature, double threshold,
                                             int limit) {
  std::vector<FaceMatchResult> hits;
  if (!UsableFeature(feature))
    return hits;
  hits.reserve(library.size());
  for (const auto& entry : library) {
    if (entry.id <= 0 || !UsableFeature(entry.feature) || entry.feature.size() != feature.size())
      continue;
    FaceMatchResult hit;
    hit.id = entry.id;
    hit.name = entry.name;
    hit.score = CosineSimilarity(feature, entry.feature);
    hit.found = hit.score >= threshold;
    hits.push_back(std::move(hit));
  }
  std::sort(hits.begin(), hits.end(), [](const FaceMatchResult& a, const FaceMatchResult& b) {
    if (a.score != b.score)
      return a.score > b.score;
    return a.id < b.id;
  });
  if (limit > 0 && hits.size() > static_cast<size_t>(limit))
    hits.resize(static_cast<size_t>(limit));
  return hits;
}

} // namespace uvnvr
