#pragma once

#include <string>

namespace sst {

// printf-style logging to stderr with a timestamp (seconds since start).
void Log(const char* fmt, ...);

std::string HrToString(long hr);
std::string Narrow(const std::wstring& s);

}  // namespace sst
