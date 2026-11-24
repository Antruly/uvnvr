#pragma once

#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace uvnvr {

struct AiConfig {
  // When true the engine returns predictable fake detections. The
  // UVNVR_AI_STUB=1 environment variable has the same effect and is what the
  // end-to-end checks use.
  bool stub = false;
  std::string model_dir;
};

struct AiPlateResult {
  std::string text;
  float confidence = 0.0f;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  // Four corners, each x/y pair: (x1,y1,x2,y2,x3,y3,x4,y4).
  std::vector<float> points;
  std::string color;
};

struct AiFaceResult {
  float detConfidence = 0.0f;
  float quality = 0.0f;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  // Ten values: five landmarks, each x/y pair.
  std::vector<float> landmark;
  std::vector<float> feature;
};

// A recognition engine instance is NOT thread safe. Each worker thread creates,
// uses and destroys its own instance; the input Mat stays owned by the caller
// and is only borrowed for the duration of one call.
class AiEngine {
public:
  virtual ~AiEngine() = default;
  virtual bool Ready() const = 0;
  virtual const char* Name() const = 0;
  // Input is BGR 8UC3.
  virtual bool DetectPlates(const cv::Mat& bgr, std::vector<AiPlateResult>* out,
                            std::string* error) = 0;
  virtual bool DetectFaces(const cv::Mat& bgr, std::vector<AiFaceResult>* out,
                           std::string* error) = 0;
};

std::unique_ptr<AiEngine> CreatePlateEngine(const AiConfig& config, std::string* error);
std::unique_ptr<AiEngine> CreateFaceEngine(const AiConfig& config, std::string* error);

} // namespace uvnvr
