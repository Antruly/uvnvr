// Standalone InspireFace smoke/benchmark tool.
//
// The engine compiled here is the same one the AI service uses, so the output
// also validates the model directory before the service is started.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <vector>

#include "ai/engine_inspireface.h"

namespace {

void Usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage: %s --models <pack-or-dir> --image <file> [--iterations N]\n"
      "          [--pixel-level N] [--check-similarity V]\n"
      "  --models       InspireFace resource pack (.pak) or a directory holding one\n"
      "  --iterations   run the same image N times, default 1\n"
      "  --pixel-level  detector input level (multiple of 160), -1 = SDK default\n"
      "  --check-similarity  exit 3 if two runs differ by more than 1-V\n",
      argv0);
}

double CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || a.size() != b.size())
    return 0.0;
  double dot = 0.0;
  double norm_a = 0.0;
  double norm_b = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    norm_a += static_cast<double>(a[i]) * a[i];
    norm_b += static_cast<double>(b[i]) * b[i];
  }
  if (norm_a <= 1e-12 || norm_b <= 1e-12)
    return 0.0;
  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

bool ParseInt(const char* text, long low, long high, long* out) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < low || value > high)
    return false;
  *out = value;
  return true;
}

} // namespace

int main(int argc, char** argv) {
  std::string models;
  std::string image_path;
  long iterations = 1;
  long pixel_level = -1;
  double required_similarity = 0.0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--models" && i + 1 < argc) {
      models = argv[++i];
    } else if (arg == "--image" && i + 1 < argc) {
      image_path = argv[++i];
    } else if (arg == "--iterations" && i + 1 < argc) {
      if (!ParseInt(argv[++i], 1, 1000, &iterations)) {
        std::fprintf(stderr, "invalid --iterations '%s'\n", argv[i]);
        return 2;
      }
    } else if (arg == "--pixel-level" && i + 1 < argc) {
      if (!ParseInt(argv[++i], -1, 1280, &pixel_level)) {
        std::fprintf(stderr, "invalid --pixel-level '%s'\n", argv[i]);
        return 2;
      }
    } else if (arg == "--check-similarity" && i + 1 < argc) {
      char* end = nullptr;
      required_similarity = std::strtod(argv[++i], &end);
      if (end == argv[i] || *end != '\0' || required_similarity <= 0.0 ||
          required_similarity > 1.0) {
        std::fprintf(stderr, "invalid --check-similarity '%s'\n", argv[i]);
        return 2;
      }
    } else if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
      Usage(argv[0]);
      return 2;
    }
  }
  if (models.empty() || image_path.empty()) {
    Usage(argv[0]);
    return 2;
  }

  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty() || image.type() != CV_8UC3) {
    std::fprintf(stderr, "cannot read BGR image: %s\n", image_path.c_str());
    return 1;
  }

  uvnvr::AiConfig config;
  config.model_dir = models;
  config.face_pixel_level = static_cast<int>(pixel_level);
  std::string error;
  std::unique_ptr<uvnvr::AiEngine> engine = uvnvr::CreateInspireFaceEngine(config, &error);
  if (!engine) {
    std::fprintf(stderr, "engine init failed: %s\n", error.c_str());
    return 1;
  }
  if (std::strcmp(engine->Name(), "inspireface") != 0 || !engine->Ready()) {
    std::fprintf(stderr, "unexpected engine state: name=%s ready=%d\n", engine->Name(),
                 engine->Ready() ? 1 : 0);
    return 1;
  }
  std::printf("engine: %s, models: %s, pixel-level: %ld\n", engine->Name(), models.c_str(),
              pixel_level);
  std::printf("image: %s (%dx%d)\n", image_path.c_str(), image.cols, image.rows);

  std::vector<float> reference_feature;
  double total_ms = 0.0;
  double min_ms = 0.0;
  double max_ms = 0.0;
  double worst_similarity = 1.0;
  int first_pass_faces = 0;

  for (long iteration = 0; iteration < iterations; ++iteration) {
    std::vector<uvnvr::AiFaceResult> results;
    error.clear();
    const auto started = std::chrono::steady_clock::now();
    const bool ok = engine->DetectFaces(image, &results, &error);
    const auto finished = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(finished - started).count();
    if (!ok) {
      std::fprintf(stderr, "detection failed: %s\n", error.c_str());
      return 1;
    }
    total_ms += ms;
    if (iteration == 0 || ms < min_ms)
      min_ms = ms;
    if (iteration == 0 || ms > max_ms)
      max_ms = ms;

    if (iteration == 0) {
      first_pass_faces = static_cast<int>(results.size());
      std::printf("faces: %zu\n", results.size());
      for (size_t i = 0; i < results.size(); ++i) {
        const uvnvr::AiFaceResult& face = results[i];
        std::printf("  [%zu] box=(%d,%d,%d,%d) det=%.4f quality=%.4f featureDim=%zu\n", i + 1,
                    face.x, face.y, face.w, face.h, face.detConfidence, face.quality,
                    face.feature.size());
        if (face.landmark.size() == 10) {
          std::printf("       landmarks:");
          for (size_t p = 0; p < face.landmark.size(); p += 2)
            std::printf(" (%.1f,%.1f)", face.landmark[p], face.landmark[p + 1]);
          std::printf("\n");
        } else {
          std::printf("       landmarks: none\n");
        }
        std::printf("       feature[0..7]:");
        for (size_t k = 0; k < face.feature.size() && k < 8; ++k)
          std::printf(" %.6f", face.feature[k]);
        std::printf("\n");
      }
      if (!results.empty())
        reference_feature = results.front().feature;
    }

    double similarity = 1.0;
    if (!reference_feature.empty() && !results.empty()) {
      similarity = CosineSimilarity(reference_feature, results.front().feature);
      worst_similarity = std::min(worst_similarity, similarity);
      std::printf("  run %ld: faces=%zu firstFeatureSimilarity=%.9f  %.2f ms\n", iteration + 1,
                  results.size(), similarity, ms);
    } else {
      std::printf("  run %ld: faces=%zu firstFeatureSimilarity=n/a  %.2f ms\n", iteration + 1,
                  results.size(), ms);
    }
    if (iteration == 0)
      std::printf("  timing: %.2f ms\n", ms);
  }

  std::printf("summary: first-pass faces=%d, %ld iteration(s), avg %.2f ms, min %.2f ms, "
              "max %.2f ms, worst cosine similarity=%.9f\n",
              first_pass_faces, iterations, total_ms / iterations, min_ms, max_ms,
              worst_similarity);

  if (required_similarity > 0.0 && !reference_feature.empty() &&
      worst_similarity < required_similarity) {
    std::fprintf(stderr, "similarity check failed: %.9f < %.9f\n", worst_similarity,
                 required_similarity);
    return 3;
  }
  return 0;
}
