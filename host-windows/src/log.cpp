#include "log.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sst {

namespace {
std::mutex g_logMutex;
const auto g_start = std::chrono::steady_clock::now();
}  // namespace

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard lock(g_logMutex);
    std::fprintf(stderr, "[%8.3f] %s\n", t, buf);
    std::fflush(stderr);
}

std::string HrToString(long hr) {
    wchar_t* msg = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, DWORD(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    std::string result = code;
    if (len && msg) {
        std::wstring text(msg, len);
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
        result += " (" + Narrow(text) + ")";
    }
    if (msg) LocalFree(msg);
    return result;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace sst
