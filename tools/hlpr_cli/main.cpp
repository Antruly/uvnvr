// Standalone HyperLPR3 smoke/benchmark tool.
//
// The engine compiled here is the same one the AI service uses, so the output
// also validates the model directory before the service is started.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <vector>

#include "ai/engine_hyperlpr.h"
#include "ai/plate_post.h"

namespace {

void Usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage: %s --models <dir> --image <file> [--image <file> ...]\n"
      "          [--level low|high] [--iterations N]\n"
      "  --level low   320x detector (default)\n"
      "  --level high  640x detector, slower\n",
      argv0);
}

} // namespace

int main(int argc, char** argv) {
  std::string models;
  std::vector<std::string> images;
  int det_level = 0;
  int iterations = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--models" && i + 1 < argc) {
      models = argv[++i];
    } else if (arg == "--image" && i + 1 < argc) {
      images.emplace_back(argv[++i]);
    } else if (arg == "--level" && i + 1 < argc) {
      const std::string level = argv[++i];
      if (level == "low" || level == "0") {
        det_level = 0;
      } else if (level == "high" || level == "1") {
        det_level = 1;
      } else {
        std::fprintf(stderr, "invalid --level '%s'\n", level.c_str());
        return 2;
      }
    } else if (arg == "--iterations" && i + 1 < argc) {
      char* end = nullptr;
      const long parsed = std::strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || parsed < 1 || parsed > 1000) {
        std::fprintf(stderr, "invalid --iterations '%s'\n", argv[i]);
        return 2;
      }
      iterations = static_cast<int>(parsed);
    } else if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
      Usage(argv[0]);
      return 2;
    }
  }
  if (models.empty() || images.empty()) {
    Usage(argv[0]);
    return 2;
  }

  uvnvr::AiConfig config;
  config.model_dir = models;
  config.plate_det_level = det_level;
  std::string error;
  std::unique_ptr<uvnvr::AiEngine> engine = uvnvr::CreateHyperLprPlateEngine(config, &error);
  if (!engine) {
    std::fprintf(stderr, "engine init failed: %s\n", error.c_str());
    return 1;
  }
  if (std::strcmp(engine->Name(), "hyperlpr3") != 0 || !engine->Ready()) {
    std::fprintf(stderr, "unexpected engine state: name=%s ready=%d\n", engine->Name(),
                 engine->Ready() ? 1 : 0);
    return 1;
  }
  std::printf("engine: %s, level: %s (%s models), threads: 1\n", engine->Name(),
              det_level ? "high" : "low", det_level ? "640x" : "320x");

  int total_detections = 0;
  for (const std::string& path : images) {
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    if (image.empty() || image.type() != CV_8UC3) {
      std::fprintf(stderr, "cannot read BGR image: %s\n", path.c_str());
      return 1;
    }
    std::printf("\nimage: %s (%dx%d)\n", path.c_str(), image.cols, image.rows);

    double total_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
      std::vector<uvnvr::AiPlateResult> results;
      error.clear();
      const auto started = std::chrono::steady_clock::now();
      const bool ok = engine->DetectPlates(image, &results, &error);
      const auto finished = std::chrono::steady_clock::now();
      const double ms =
          std::chrono::duration<double, std::milli>(finished - started).count();
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
        total_detections += static_cast<int>(results.size());
        if (results.empty())
          std::printf("  no plates\n");
        for (size_t i = 0; i < results.size(); ++i) {
          const uvnvr::AiPlateResult& plate = results[i];
          const std::string normalized = uvnvr::NormalizePlate(plate.text);
          std::printf(
              "  [%zu] raw=%s normalized=%s valid=%s confidence=%.4f "
              "box=(%d,%d,%d,%d) color=%s\n",
              i + 1, plate.text.c_str(), normalized.c_str(),
              uvnvr::IsValidPlate(normalized) ? "yes" : "no", plate.confidence, plate.x,
              plate.y, plate.w, plate.h, plate.color.c_str());
        }
      }
    }
    std::printf("  timing: %d iteration(s), avg %.2f ms, min %.2f ms, max %.2f ms\n",
                iterations, total_ms / iterations, min_ms, max_ms);
  }

  std::printf("\ntotal plates on first pass: %d\n", total_detections);
  return 0;
}
