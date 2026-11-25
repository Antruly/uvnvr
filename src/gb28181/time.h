#pragma once

// GB/T 28181 time helpers shared by MANSCDP RecordInfo and playback control.
//
// MANSCDP uses "YYYY-MM-DDTHH:MM:SS" in Query/Response/Control documents, while
// the `u=` line of a playback SDP offer uses the compact "YYYYMMDDHHMMSS"
// form. Both are device-local time unless the deployment runs with UTC. The
// helpers here are the single conversion point so the two code paths cannot
// drift apart.

#include <cstdint>
#include <string>
#include <string_view>

namespace uvnvr::gb28181 {

// Parses "YYYY-MM-DDTHH:MM:SS" (a space instead of 'T' is accepted). Returns
// false for malformed values or out-of-range fields.
bool ParseGbTime(std::string_view text, std::int64_t* epoch_ms, std::string* error = nullptr);

// Parses the compact 14-digit form used by playback `u=` lines.
bool ParseGbCompactTime(std::string_view text, std::int64_t* epoch_ms,
                        std::string* error = nullptr);

// Formats in device-local time. FormatGbTime emits "YYYY-MM-DDTHH:MM:SS";
// FormatGbCompactTime emits "YYYYMMDDHHMMSS".
std::string FormatGbTime(std::int64_t epoch_ms);
std::string FormatGbCompactTime(std::int64_t epoch_ms);

// Splits "start/end" (MANSCDP Range value) into the two times. Either side may
// be empty; an empty side leaves the corresponding output untouched.
bool ParseGbTimeRange(std::string_view text, std::int64_t* start_ms, std::int64_t* end_ms,
                      std::string* error = nullptr);

} // namespace uvnvr::gb28181
