#include "ai_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(UVNVR_ENABLE_INSPIREFACE) && UVNVR_ENABLE_INSPIREFACE
#include "engine_inspireface.h"
#endif
#if defined(UVNVR_ENABLE_HYPERLPR) && UVNVR_ENABLE_HYPERLPR
#include "engine_hyperlpr.h"
#endif

namespace uvnvr {
namespace {

bool StubRequested(const AiConfig& config) {
  if (config.stub)
    return true;
  const char* value = std::getenv("UVNVR_AI_STUB");
  if (value == nullptr || value[0] == '\0')
    return false;
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text == "1" || text == "true" || text == "yes" || text == "on";
}

int ClampInt(int value, int low, int high) {
  return std::max(low, std::min(value, high));
}

// Test-only artificial inference cost, so queue overflow/drop accounting can be
// exercised without a real model. UVNVR_AI_STUB_DELAY_MS=0 (default) is inert.
int StubDelayMs() {
  const char* value = std::getenv("UVNVR_AI_STUB_DELAY_MS");
  if (value == nullptr || value[0] == '\0')
    return 0;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed <= 0)
    return 0;
  return static_cast<int>(std::min<long>(parsed, 10000));
}

void ApplyStubDelay() {
  const int delay = StubDelayMs();
  if (delay > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

std::vector<float> StubFeature() {
  std::vector<float> feature(128);
  double norm = 0.0;
  for (size_t i = 0; i < feature.size(); ++i) {
    const double value =
        std::sin(static_cast<double>(i) * 0.37) + std::cos(static_cast<double>(i) * 0.11);
    feature[i] = static_cast<float>(value);
    norm += value * value;
  }
  norm = std::sqrt(std::max(norm, 1e-9));
  for (float& value : feature)
    value = static_cast<float>(value / norm);
  return feature;
}

class StubEngine final : public AiEngine {
public:
  StubEngine(bool fake, bool plate) : fake_(fake), plate_(plate) {}

  bool Ready() const override {
    return true;
  }
  const char* Name() const override {
    return "stub";
  }

  bool DetectPlates(const cv::Mat& bgr, std::vector<AiPlateResult>* out,
                    std::string* error) override {
    out->clear();
    if (bgr.empty() || bgr.type() != CV_8UC3) {
      if (error != nullptr)
        *error = "plate engine expects a non-empty BGR 8UC3 frame";
      return false;
    }
    if (!plate_ || !fake_)
      return true;
    ApplyStubDelay();

    const int width = bgr.cols;
    const int height = bgr.rows;
    AiPlateResult result;
    result.text = "京A12345";
    result.confidence = 0.93f;
    result.w = ClampInt(width / 4, 1, width);
    result.h = ClampInt(height / 10, 1, height);
    result.x = ClampInt(width / 5, 0, width - result.w);
    result.y = ClampInt(height / 2, 0, height - result.h);
    const float x0 = static_cast<float>(result.x);
    const float y0 = static_cast<float>(result.y);
    const float x1 = static_cast<float>(result.x + result.w);
    const float y1 = static_cast<float>(result.y + result.h);
    result.points = {x0, y0, x1, y0, x1, y1, x0, y1};
    result.color = "蓝";
    out->push_back(std::move(result));
    return true;
  }

  bool DetectFaces(const cv::Mat& bgr, std::vector<AiFaceResult>* out,
                   std::string* error) override {
    out->clear();
    if (bgr.empty() || bgr.type() != CV_8UC3) {
      if (error != nullptr)
        *error = "face engine expects a non-empty BGR 8UC3 frame";
      return false;
    }
    if (plate_ || !fake_)
      return true;
    ApplyStubDelay();

    const int width = bgr.cols;
    const int height = bgr.rows;
    AiFaceResult result;
    result.detConfidence = 0.90f;
    result.quality = 0.81f;
    result.w = ClampInt(width / 6, 1, width);
    result.h = ClampInt(height / 4, 1, height);
    result.x = ClampInt(width / 2 - result.w / 2, 0, width - result.w);
    result.y = ClampInt(height / 4, 0, height - result.h);
    result.landmark = {static_cast<float>(result.x + result.w / 3),
                       static_cast<float>(result.y + result.h / 3),
                       static_cast<float>(result.x + result.w * 2 / 3),
                       static_cast<float>(result.y + result.h / 3),
                       static_cast<float>(result.x + result.w / 2),
                       static_cast<float>(result.y + result.h / 2),
                       static_cast<float>(result.x + result.w / 3),
                       static_cast<float>(result.y + result.h * 2 / 3),
                       static_cast<float>(result.x + result.w * 2 / 3),
                       static_cast<float>(result.y + result.h * 2 / 3)};
    result.feature = StubFeature();
    out->push_back(std::move(result));
    return true;
  }

private:
  bool fake_ = false;
  bool plate_ = false;
};

std::unique_ptr<AiEngine> CreateStub(const AiConfig& config, bool plate, std::string* error) {
  // Phase 2/3 replace this branch with HyperLPR3 / InspireFace selection.
  // `error` is left untouched so a caller that fell back from a real engine
  // still sees why.
  (void)error;
  return std::make_unique<StubEngine>(StubRequested(config), plate);
}

} // namespace

std::unique_ptr<AiEngine> CreatePlateEngine(const AiConfig& config, std::string* error) {
  if (StubRequested(config))
    return CreateStub(config, true, error);

#if defined(UVNVR_ENABLE_HYPERLPR) && UVNVR_ENABLE_HYPERLPR
  {
    std::string hyper_error;
    std::unique_ptr<AiEngine> engine = CreateHyperLprPlateEngine(config, &hyper_error);
    if (engine)
      return engine;
    std::fprintf(stderr,
                 "ai: HyperLPR3 unavailable (%s); falling back to the stub plate engine\n",
                 hyper_error.c_str());
    if (error != nullptr)
      *error = hyper_error;
  }
#endif
  return CreateStub(config, true, error);
}

std::unique_ptr<AiEngine> CreateFaceEngine(const AiConfig& config, std::string* error) {
  if (StubRequested(config))
    return CreateStub(config, false, error);

#if defined(UVNVR_ENABLE_INSPIREFACE) && UVNVR_ENABLE_INSPIREFACE
  {
    std::string inspire_error;
    std::unique_ptr<AiEngine> engine = CreateInspireFaceEngine(config, &inspire_error);
    if (engine)
      return engine;
    std::fprintf(stderr,
                 "ai: InspireFace unavailable (%s); falling back to the stub face engine\n",
                 inspire_error.c_str());
    if (error != nullptr)
      *error = inspire_error;
  }
#endif
  return CreateStub(config, false, error);
}

} // namespace uvnvr
