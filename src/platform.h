#pragma once

#include <cstdint>
#include <string>

namespace uvnvr::platform {

std::string ExecutablePath();
// Opens `url` in the user's default browser. Returns false when no handler
// could be launched (headless hosts, missing xdg-open).
bool OpenUrl(const std::string& url);
std::string HostName();
std::string OsDescription();
uint64_t TotalMemoryBytes();
uint64_t FreeMemoryBytes();
double UptimeSeconds();
int CpuCount();
double CpuUsagePercent(uint64_t* last_total, uint64_t* last_idle);

} // namespace uvnvr::platform
