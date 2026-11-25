#include "gb28181/time.h"

#include <charconv>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace uvnvr::gb28181 {
namespace {

// mktime/localtime are not thread-safe when the time zone changes under a
// running process. The immutable parts are still guarded for the same reason
// the rest of the service guards them: tzset() writes process-global state.
std::mutex& TimeMutex() {
  static std::mutex mutex;
  return mutex;
}

void Localtime(std::time_t seconds, std::tm* out) {
#ifdef _WIN32
  localtime_s(out, &seconds);
#else
  localtime_r(&seconds, out);
#endif
}

bool ReadFixedDigits(std::string_view text, std::size_t offset, std::size_t count, int* value) {
  if (offset + count > text.size())
    return false;
  int parsed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9')
      return false;
    parsed = parsed * 10 + (c - '0');
  }
  *value = parsed;
  return true;
}

bool MakeEpochMs(int year, int month, int day, int hour, int minute, int second,
                 std::int64_t* epoch_ms, std::string* error) {
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 60) {
    if (error != nullptr)
      *error = "time field is out of range";
    return false;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = -1;
  std::time_t seconds = 0;
  {
    std::lock_guard<std::mutex> lock(TimeMutex());
    std::tm copy = tm;
    seconds = std::mktime(&copy);
  }
  if (seconds == static_cast<std::time_t>(-1)) {
    if (error != nullptr)
      *error = "time cannot be represented in the local time zone";
    return false;
  }
  // mktime normalises invalid dates (for example 31 April); reject values that
  // did not round-trip so a platform typo is not silently accepted.
  std::tm check{};
  {
    std::lock_guard<std::mutex> lock(TimeMutex());
    Localtime(seconds, &check);
  }
  if (check.tm_year != year - 1900 || check.tm_mon != month - 1 || check.tm_mday != day ||
      check.tm_hour != hour || check.tm_min != minute || check.tm_sec != second) {
    if (error != nullptr)
      *error = "time is not a valid calendar date";
    return false;
  }
  *epoch_ms = static_cast<std::int64_t>(seconds) * 1000;
  return true;
}

std::string Format(std::int64_t epoch_ms, const char* pattern) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm tm{};
  {
    std::lock_guard<std::mutex> lock(TimeMutex());
    Localtime(seconds, &tm);
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), pattern, &tm);
  return buffer;
}

} // namespace

bool ParseGbTime(std::string_view text, std::int64_t* epoch_ms, std::string* error) {
  if (epoch_ms == nullptr) {
    if (error != nullptr)
      *error = "time output is null";
    return false;
  }
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    text.remove_suffix(1);
  if (text.size() != 19 || (text[10] != 'T' && text[10] != ' ')) {
    if (error != nullptr)
      *error = "time must be YYYY-MM-DDTHH:MM:SS";
    return false;
  }
  if (text[4] != '-' || text[7] != '-' || text[13] != ':' || text[16] != ':') {
    if (error != nullptr)
      *error = "time must be YYYY-MM-DDTHH:MM:SS";
    return false;
  }
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!ReadFixedDigits(text, 0, 4, &year) || !ReadFixedDigits(text, 5, 2, &month) ||
      !ReadFixedDigits(text, 8, 2, &day) || !ReadFixedDigits(text, 11, 2, &hour) ||
      !ReadFixedDigits(text, 14, 2, &minute) || !ReadFixedDigits(text, 17, 2, &second)) {
    if (error != nullptr)
      *error = "time contains a non-digit";
    return false;
  }
  return MakeEpochMs(year, month, day, hour, minute, second, epoch_ms, error);
}

bool ParseGbCompactTime(std::string_view text, std::int64_t* epoch_ms, std::string* error) {
  if (epoch_ms == nullptr) {
    if (error != nullptr)
      *error = "time output is null";
    return false;
  }
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    text.remove_suffix(1);
  if (text.size() != 14) {
    if (error != nullptr)
      *error = "time must be YYYYMMDDHHMMSS";
    return false;
  }
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!ReadFixedDigits(text, 0, 4, &year) || !ReadFixedDigits(text, 4, 2, &month) ||
      !ReadFixedDigits(text, 6, 2, &day) || !ReadFixedDigits(text, 8, 2, &hour) ||
      !ReadFixedDigits(text, 10, 2, &minute) || !ReadFixedDigits(text, 12, 2, &second)) {
    if (error != nullptr)
      *error = "time contains a non-digit";
    return false;
  }
  return MakeEpochMs(year, month, day, hour, minute, second, epoch_ms, error);
}

std::string FormatGbTime(std::int64_t epoch_ms) {
  return Format(epoch_ms, "%Y-%m-%dT%H:%M:%S");
}

std::string FormatGbCompactTime(std::int64_t epoch_ms) {
  return Format(epoch_ms, "%Y%m%d%H%M%S");
}

bool ParseGbTimeRange(std::string_view text, std::int64_t* start_ms, std::int64_t* end_ms,
                      std::string* error) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos) {
    if (error != nullptr)
      *error = "Range must be start/end";
    return false;
  }
  const std::string_view start = text.substr(0, slash);
  const std::string_view end = text.substr(slash + 1);
  if (!start.empty()) {
    int64_t parsed = 0;
    if (!ParseGbTime(start, &parsed, error))
      return false;
    if (start_ms != nullptr)
      *start_ms = parsed;
  }
  if (!end.empty()) {
    int64_t parsed = 0;
    if (!ParseGbTime(end, &parsed, error))
      return false;
    if (end_ms != nullptr)
      *end_ms = parsed;
  }
  return true;
}

} // namespace uvnvr::gb28181
