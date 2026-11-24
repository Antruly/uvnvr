#pragma once

#include <memory>
#include <string>

#include "ai_engine.h"

namespace uvnvr {

// Creates an InspireFace-backed face engine. The instance owns one SDK session
// and is not thread safe: create, use and destroy it on the same worker thread.
// The SDK itself is launched once per process on the first successful call.
//
// `config.model_dir` may name the resource-pack archive or a directory that
// contains one. On failure a null pointer is returned and *error describes why
// (pack missing, launch/session failure, ...). This function is only compiled
// when the build has UVNVR_ENABLE_INSPIREFACE.
std::unique_ptr<AiEngine> CreateInspireFaceEngine(const AiConfig& config, std::string* error);

} // namespace uvnvr
