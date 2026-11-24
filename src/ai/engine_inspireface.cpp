#include "engine_inspireface.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <inspireface.h>
#include <opencv2/core.hpp>

namespace uvnvr {
namespace {

namespace fs = std::filesystem;

constexpr int kMaxDetectFaces = 5;
constexpr int kDefaultFeatureLength = 512;

std::string DescribeStatus(long status) {
  char text[256] = {0};
  HInt32 required = 0;
  if (HFGetErrorMessage(static_cast<HResult>(status), text, static_cast<HInt32>(sizeof(text)),
                        &required) == HSUCCEED &&
      text[0] != '\0') {
    return std::string(text) + " (" + std::to_string(status) + ")";
  }
  return "result code " + std::to_string(status);
}

// The SDK accepts the resource-pack archive itself (a single file), not an
// extracted directory. The setting is a directory so a deployment can drop
// several packs next to each other; pick the first plausible archive.
std::string ResolveResourcePack(const std::string& configured, std::string* error) {
  if (configured.empty()) {
    if (error != nullptr)
      *error = "ai.face.modelDir is empty; point it at the InspireFace resource pack";
    return {};
  }
  std::error_code ec;
  const fs::path path(configured);
  if (!fs::exists(path, ec)) {
    if (error != nullptr)
      *error = "InspireFace resource path does not exist: " + configured;
    return {};
  }
  if (!fs::is_directory(path, ec))
    return path.string();

  struct Candidate {
    fs::path path;
    int rank = 0;
    std::string name;
  };
  std::vector<Candidate> candidates;
  for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec))
      continue;
    Candidate candidate;
    candidate.path = it->path();
    candidate.name = candidate.path.filename().string();
    std::string lower = candidate.name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".pak") == 0)
      candidate.rank = 3;
    else if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".pack") == 0)
      candidate.rank = 3;
    else if (lower.find("pikachu") != std::string::npos ||
             lower.find("inspire") != std::string::npos)
      candidate.rank = 2;
    else
      candidate.rank = 1;
    candidates.push_back(std::move(candidate));
  }
  if (candidates.empty()) {
    if (error != nullptr)
      *error = "no InspireFace resource pack found in " + configured;
    return {};
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     if (a.rank != b.rank)
                       return a.rank > b.rank;
                     return a.name < b.name;
                   });
  if (candidates.front().rank == 1 && candidates.size() > 1) {
    if (error != nullptr) {
      *error = "no .pak/.pack resource pack found in " + configured +
               " (expected the archive file downloaded with the SDK)";
    }
    return {};
  }
  return candidates.front().path.string();
}

// HFLaunchInspireFace is process-global and idempotent by contract. Serialise
// engine construction so one launch result is shared by every worker; a second
// worker must not launch a different pack behind the first worker's back.
struct LaunchState {
  std::mutex mutex;
  bool attempted = false;
  bool ready = false;
  std::string pack;
  std::string error;
};

LaunchState& GlobalLaunchState() {
  static LaunchState state;
  return state;
}

bool EnsureLaunched(const std::string& pack, std::string* error) {
  std::lock_guard<std::mutex> lock(GlobalLaunchState().mutex);
  LaunchState& state = GlobalLaunchState();
  if (state.attempted) {
    if (!state.ready) {
      if (error != nullptr)
        *error = state.error;
      return false;
    }
    if (!state.pack.empty() && state.pack != pack) {
      if (error != nullptr) {
        *error = "InspireFace is already launched with " + state.pack +
                 "; one process cannot load a second model pack";
      }
      return false;
    }
    return true;
  }
  state.attempted = true;
  const HResult status = HFLaunchInspireFace(pack.c_str());
  if (status != HSUCCEED) {
    state.error = "HFLaunchInspireFace(" + pack + ") failed: " + DescribeStatus(status);
    if (error != nullptr)
      *error = state.error;
    return false;
  }
  state.ready = true;
  state.pack = pack;
  return true;
}

class InspireFaceEngine final : public AiEngine {
public:
  InspireFaceEngine() = default;

  ~InspireFaceEngine() override {
    if (session_ != nullptr) {
      HFReleaseInspireFaceSession(session_);
      session_ = nullptr;
    }
  }

  InspireFaceEngine(const InspireFaceEngine&) = delete;
  InspireFaceEngine& operator=(const InspireFaceEngine&) = delete;

  bool Init(const AiConfig& config, std::string* error) {
    const std::string pack = ResolveResourcePack(config.model_dir, error);
    if (pack.empty())
      return false;
    if (!EnsureLaunched(pack, error))
      return false;

    HInt32 feature_length = 0;
    if (HFGetFeatureLength(&feature_length) == HSUCCEED && feature_length > 0)
      feature_length_ = static_cast<int>(feature_length);

    // The V1 constructor (HFCreateInspireFaceSession) is rejected by this SDK
    // with HERR_UNSUPPORTED (14); the fixed-layout V2 entry point is required.
    HFSessionConfigV2 session_config{};
    session_config.structSize = static_cast<HFUInt32>(sizeof(session_config));
    session_config.structVersion = HF_SESSION_CONFIG_V2_VERSION;
    session_config.featureMask = HF_ENABLE_FACE_RECOGNITION | HF_ENABLE_QUALITY;
    session_config.detectMode = HF_DETECT_MODE_ALWAYS_DETECT;
    session_config.maxDetectFaceNum = kMaxDetectFaces;
    session_config.detectPixelLevel = config.face_pixel_level;
    session_config.trackByDetectModeFPS = -1;

    HFStatus status = HFCreateInspireFaceSessionV2(&session_config, &session_);
    if (status == HSUCCEED) {
      quality_supported_ = true;
    } else {
      // A pack without the quality model is still usable: keep recognition
      // and derive the quality score from the detector confidence instead.
      session_ = nullptr;
      session_config.featureMask = HF_ENABLE_FACE_RECOGNITION;
      status = HFCreateInspireFaceSessionV2(&session_config, &session_);
      quality_supported_ = false;
    }
    if (status != HSUCCEED) {
      session_ = nullptr;
      if (error != nullptr)
        *error = "HFCreateInspireFaceSessionV2 failed: " + DescribeStatus(status);
      return false;
    }
    return true;
  }

  bool Ready() const override {
    return session_ != nullptr;
  }

  const char* Name() const override {
    return "inspireface";
  }

  bool DetectPlates(const cv::Mat&, std::vector<AiPlateResult>* out,
                    std::string* error) override {
    out->clear();
    if (error != nullptr)
      *error = "the InspireFace engine does not detect plates";
    return false;
  }

  bool DetectFaces(const cv::Mat& bgr, std::vector<AiFaceResult>* out,
                   std::string* error) override {
    out->clear();
    if (bgr.empty() || bgr.type() != CV_8UC3) {
      if (error != nullptr)
        *error = "face engine expects a non-empty BGR 8UC3 frame";
      return false;
    }
    if (session_ == nullptr) {
      if (error != nullptr)
        *error = "InspireFace session is not initialised";
      return false;
    }

    // HFImageData has no stride field, so an ROI submatrix (step != width*3)
    // must be packed first or the detector sees skewed rows.
    cv::Mat packed;
    const cv::Mat* image = &bgr;
    if (!bgr.isContinuous()) {
      packed = bgr.clone();
      image = &packed;
    }

    HFImageData data{};
    data.data = image->data;
    data.width = image->cols;
    data.height = image->rows;
    data.format = HF_STREAM_BGR;
    data.rotation = HF_CAMERA_ROTATION_0;

    HFImageStream stream = nullptr;
    HResult status = HFCreateImageStream(&data, &stream);
    if (status != HSUCCEED) {
      if (error != nullptr)
        *error = "HFCreateImageStream failed: " + DescribeStatus(status);
      return false;
    }

    HFMultipleFaceData faces{};
    status = HFExecuteFaceTrack(session_, stream, &faces);
    if (status != HSUCCEED) {
      HFReleaseImageStream(stream);
      if (error != nullptr)
        *error = "HFExecuteFaceTrack failed: " + DescribeStatus(status);
      return false;
    }

    const int width = image->cols;
    const int height = image->rows;
    for (int i = 0; i < faces.detectedNum; ++i) {
      const HFaceRect& rect = faces.rects[i];
      const int x0 = std::max(0, std::min(rect.x, width));
      const int y0 = std::max(0, std::min(rect.y, height));
      const int x1 = std::max(0, std::min(rect.x + rect.width, width));
      const int y1 = std::max(0, std::min(rect.y + rect.height, height));
      if (x1 <= x0 || y1 <= y0)
        continue;

      AiFaceResult result;
      result.x = x0;
      result.y = y0;
      result.w = x1 - x0;
      result.h = y1 - y0;
      if (faces.detConfidence != nullptr)
        result.detConfidence = std::max(0.0f, std::min(1.0f, faces.detConfidence[i]));

      // The token is only valid until the next session call or image-stream
      // release; everything below is copied out before that happens.
      const HFFaceBasicToken token = faces.tokens[i];

      float quality = result.detConfidence;
      if (quality_supported_) {
        float measured = -1.0f;
        if (HFFaceQualityDetect(session_, token, &measured) == HSUCCEED && measured >= 0.0f)
          quality = std::max(0.0f, std::min(1.0f, measured));
      }
      result.quality = quality;

      HPoint2f points[5]{};
      if (HFGetFaceFiveKeyPointsFromFaceToken(token, points, 5) == HSUCCEED) {
        result.landmark.reserve(10);
        for (const HPoint2f& point : points) {
          result.landmark.push_back(point.x);
          result.landmark.push_back(point.y);
        }
      }

      if (feature_length_ > 0) {
        std::vector<float> feature(static_cast<size_t>(feature_length_), 0.0f);
        if (HFFaceFeatureExtractCpy(session_, stream, token, feature.data()) == HSUCCEED) {
          // The AI pipeline compares with cosine similarity and stores the
          // blob as float32; a unit vector keeps that contract explicit.
          double norm = 0.0;
          for (float value : feature)
            norm += static_cast<double>(value) * value;
          norm = std::sqrt(norm);
          if (norm > 1e-12) {
            for (float& value : feature)
              value = static_cast<float>(value / norm);
            result.feature = std::move(feature);
          }
        }
      }

      out->push_back(std::move(result));
    }

    HFReleaseImageStream(stream);
    return true;
  }

private:
  HFSession session_ = nullptr;
  int feature_length_ = kDefaultFeatureLength;
  bool quality_supported_ = false;
};

} // namespace

std::unique_ptr<AiEngine> CreateInspireFaceEngine(const AiConfig& config, std::string* error) {
  if (error != nullptr)
    error->clear();
  auto engine = std::make_unique<InspireFaceEngine>();
  if (!engine->Init(config, error))
    return nullptr;
  return engine;
}

} // namespace uvnvr
