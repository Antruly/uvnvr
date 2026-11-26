#include "engine_hyperlpr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "hyper_lpr_sdk.h"

namespace uvnvr {
namespace {

// HyperLPR3's plate types (HLPR_PlateType in hyper_lpr_sdk.h).
const char* PlateColorName(int type) {
  switch (type) {
  case 0: // PLATE_TYPE_BLUE
    return "蓝";
  case 1: // PLATE_TYPE_YELLOW_SINGLE
  case 9: // PLATE_TYPE_YELLOW_DOUBLE
    return "黄";
  case 2: // PLATE_TYPE_WHILE_SINGLE
    return "白";
  case 3: // PLATE_TYPE_GREEN
    return "绿";
  case 4: // PLATE_TYPE_BLACK_HK_MACAO
    return "黑";
  case 5: // PLATE_TYPE_HK_SINGLE
  case 6: // PLATE_TYPE_HK_DOUBLE
    return "香港";
  case 7: // PLATE_TYPE_MACAO_SINGLE
  case 8: // PLATE_TYPE_MACAO_DOUBLE
    return "澳门";
  default:
    return "";
  }
}

// The C API returns a fixed 128-byte buffer that is normally NUL terminated.
// strnlen keeps a malformed result from reading past it.
std::string PlateCode(const char* code) {
  return std::string(code, strnlen(code, 128));
}

struct BufferGuard {
  P_HLPR_DataBuffer buffer = nullptr;
  ~BufferGuard() {
    if (buffer != nullptr)
      HLPR_ReleaseDataBuffer(buffer);
  }
};

class HyperLprEngine final : public AiEngine {
public:
  HyperLprEngine() = default;
  ~HyperLprEngine() override {
    if (context_ != nullptr)
      HLPR_ReleaseContext(context_);
  }

  HyperLprEngine(const HyperLprEngine&) = delete;
  HyperLprEngine& operator=(const HyperLprEngine&) = delete;

  bool Initialize(const AiConfig& config, std::string* error) {
    if (config.model_dir.empty()) {
      if (error != nullptr)
        *error = "HyperLPR3 model directory is empty; set ai.plate.modelDir";
      return false;
    }
    const bool high = config.plate_det_level > 0;
    std::vector<std::string> models = {
        high ? "b640x_backbone_h.mnn" : "b320_backbone_h.mnn",
        high ? "b640x_head_h.mnn" : "b320_header_h.mnn",
        "litemodel_cls_96xh.mnn",
        "rpv3_mdict_160h.mnn",
    };
    for (const std::string& name : models) {
      const std::filesystem::path path =
          std::filesystem::path(config.model_dir) / name;
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        if (error != nullptr)
          *error = "missing HyperLPR3 model: " + path.string();
        return false;
      }
    }

    // HLPR_CreateContext copies the path during Initialize; the temporary
    // string lives until after the call returns.
    std::string models_path = config.model_dir;
    HLPR_ContextConfiguration configuration = {};
    configuration.models_path = models_path.data();
    configuration.max_num = 4;
    configuration.threads = 1;
    configuration.use_half = false;
    configuration.box_conf_threshold = 0.30f;
    configuration.nms_threshold = 0.50f;
    configuration.rec_confidence_threshold = 0.50f;
    configuration.det_level = high ? HLPR_DetectLevel::DETECT_LEVEL_HIGH
                                   : HLPR_DetectLevel::DETECT_LEVEL_LOW;

    context_ = HLPR_CreateContext(&configuration);
    if (context_ == nullptr) {
      if (error != nullptr)
        *error = "HLPR_CreateContext returned null";
      return false;
    }
    const HREESULT status = HLPR_ContextQueryStatus(context_);
    if (status != HResultCode::Ok) {
      if (error != nullptr)
        *error = "HyperLPR3 failed to load models from " + models_path;
      HLPR_ReleaseContext(context_);
      context_ = nullptr;
      return false;
    }
    ready_ = true;
    if (error != nullptr)
      error->clear();
    return true;
  }

  void SetMinDetectWidth(int width) { min_detect_width_ = width; }

  bool Ready() const override {
    return ready_;
  }

  const char* Name() const override {
    return "hyperlpr3";
  }

  bool DetectPlates(const cv::Mat& bgr, std::vector<AiPlateResult>* out,
                    std::string* error) override {
    if (out == nullptr) {
      if (error != nullptr)
        *error = "output vector is null";
      return false;
    }
    out->clear();
    if (!ready_ || context_ == nullptr) {
      if (error != nullptr)
        *error = "HyperLPR3 engine is not initialized";
      return false;
    }
    if (bgr.empty() || bgr.type() != CV_8UC3) {
      if (error != nullptr)
        *error = "HyperLPR3 expects a non-empty BGR 8UC3 frame";
      return false;
    }

    try {
      // A ROI crop is a non-continuous view; the C API's image buffer takes a
      // tightly packed pointer, so clone in that (cold) case only.
      cv::Mat frame = bgr.isContinuous() ? bgr : bgr.clone();

      // The detector works on a 320x320 / 640x640 letterboxed copy, so a plate
      // has to be roughly 100 px wide in the source frame to survive the
      // resize. Sub-stream frames (704x576, 1280x720) usually fail, while the
      // same scene upscaled to 1080p is recognised; upsample small frames
      // before detection instead of asking every camera for 4 MP.
      if (frame.cols < min_detect_width_) {
        const double scale = static_cast<double>(min_detect_width_) / frame.cols;
        cv::Mat upscaled;
        cv::resize(frame, upscaled, cv::Size(), scale, scale, cv::INTER_LINEAR);
        frame = upscaled;
      }

      HLPR_ImageData data = {};
      data.data = frame.ptr<uint8_t>(0);
      data.width = frame.cols;
      data.height = frame.rows;
      data.format = HLPR_ImageFormat::STREAM_BGR;
      data.rotation = HLPR_Rotation::CAMERA_ROTATION_0;

      BufferGuard buffer;
      buffer.buffer = HLPR_CreateDataBuffer(&data);
      if (buffer.buffer == nullptr) {
        if (error != nullptr)
          *error = "HLPR_CreateDataBuffer failed";
        return false;
      }

      HLPR_PlateResultList results = {};
      const HREESULT status =
          HLPR_ContextUpdateStream(context_, buffer.buffer, &results);
      if (status != HResultCode::Ok) {
        if (error != nullptr)
          *error = "HLPR_ContextUpdateStream failed";
        return false;
      }

      out->reserve(results.plate_size);
      for (unsigned long i = 0; i < results.plate_size; ++i) {
        const HLPR_PlateResult& plate = results.plates[i];
        const float left = std::min(plate.x1, plate.x2);
        const float top = std::min(plate.y1, plate.y2);
        const float right = std::max(plate.x1, plate.x2);
        const float bottom = std::max(plate.y1, plate.y2);
        const int x = static_cast<int>(std::lround(left));
        const int y = static_cast<int>(std::lround(top));
        const int w = static_cast<int>(std::lround(right - left));
        const int h = static_cast<int>(std::lround(bottom - top));
        if (w <= 0 || h <= 0)
          continue;

        // The detector answered in the (possibly upsampled) frame's coordinate
        // space; callers crop the original image, so map the box back.
        const double back =
            frame.cols > 0 ? static_cast<double>(bgr.cols) / frame.cols : 1.0;
        const int scaled_x = static_cast<int>(std::lround(x * back));
        const int scaled_y = static_cast<int>(std::lround(y * back));
        const int scaled_w = static_cast<int>(std::lround(w * back));
        const int scaled_h = static_cast<int>(std::lround(h * back));

        AiPlateResult result;
        result.text = PlateCode(plate.code);
        result.confidence = plate.text_confidence;
        result.x = std::max(0, std::min(scaled_x, bgr.cols - 1));
        result.y = std::max(0, std::min(scaled_y, bgr.rows - 1));
        result.w = std::min(scaled_w, bgr.cols - result.x);
        result.h = std::min(scaled_h, bgr.rows - result.y);
        if (result.w <= 0 || result.h <= 0)
          continue;
        const float x0 = static_cast<float>(result.x);
        const float y0 = static_cast<float>(result.y);
        const float x1 = static_cast<float>(result.x + result.w);
        const float y1 = static_cast<float>(result.y + result.h);
        result.points = {x0, y0, x1, y0, x1, y1, x0, y1};
        result.color = PlateColorName(static_cast<int>(plate.type));
        out->push_back(std::move(result));
      }
      return true;
    } catch (const std::exception& e) {
      out->clear();
      if (error != nullptr)
        *error = std::string("HyperLPR3 inference failed: ") + e.what();
      return false;
    } catch (...) {
      out->clear();
      if (error != nullptr)
        *error = "HyperLPR3 inference failed with an unknown error";
      return false;
    }
  }

  bool DetectFaces(const cv::Mat& bgr, std::vector<AiFaceResult>* out,
                   std::string* error) override {
    if (out != nullptr)
      out->clear();
    if (error != nullptr)
      *error = "the HyperLPR3 engine does not detect faces";
    return false;
  }

private:
  P_HLPR_Context context_ = nullptr;
  bool ready_ = false;
  // Frames narrower than this are upsampled before detection; see
  // DetectPlates for why the detector needs big plates, not a big sensor.
  int min_detect_width_ = 1600;
};

} // namespace

std::unique_ptr<AiEngine> CreateHyperLprPlateEngine(const AiConfig& config,
                                                   std::string* error) {
  auto engine = std::make_unique<HyperLprEngine>();
  if (config.plate_min_width > 0)
    engine->SetMinDetectWidth(config.plate_min_width);
  if (!engine->Initialize(config, error))
    return nullptr;
  return engine;
}

} // namespace uvnvr
