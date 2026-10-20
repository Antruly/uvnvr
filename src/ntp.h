#pragma once

#include <cstdint>
#include <string>

namespace uvnvr {

bool QueryNtpOffset(const std::string& server, int64_t* offset_ms, std::string* error);
bool ApplyTimeOffset(int64_t offset_ms, std::string* error);

} // namespace uvnvr
