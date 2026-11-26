#pragma once

#include <memory>
#include <string>

#include "ai_engine.h"

namespace uvnvr {

// Creates a HyperLPR3-backed plate engine. The returned instance owns one
// HLPR_Context and is not thread safe: create, use and destroy it on the same
// worker thread.
//
// On failure a null pointer is returned and *error describes why (missing
// models, MNN/session initialisation failure, ...). This function is only
// compiled when the build has UVNVR_ENABLE_HYPERLPR.
std::unique_ptr<AiEngine> CreateHyperLprPlateEngine(const AiConfig& config, std::string* error);

} // namespace uvnvr
