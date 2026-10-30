#include "adb.h"

#include <windows.h>

#include <cctype>
#include <vector>

#include "log.h"

namespace sst {

namespace {

bool IsFile(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::wstring FindAdb(const std::wstring& explicitPath) {
    if (!explicitPath.empty()) return IsFile(explicitPath) ? explicitPath : L"";

    // The Android SDK's own copy first.
    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH)) {
        std::wstring sdk = std::wstring(localAppData) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (IsFile(sdk)) return sdk;
    }
    // Then the folders on PATH, in order. Only absolute entries: never the
    // current directory, where an adb.exe could have been planted.
    std::vector<wchar_t> path(32768);
    DWORD n = GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size()));
    if (n == 0 || n >= path.size()) return {};
    std::wstring all(path.data(), n);
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find(L';', start);
        if (end == std::wstring::npos) end = all.size();
        std::wstring dir = all.substr(start, end - start);
        start = end + 1;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        bool absolute = dir.size() >= 3 && ((dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/')) ||
                                            (dir[0] == L'\\' && dir[1] == L'\\'));
        if (!absolute) continue;
        std::wstring candidate = dir + (dir.back() == L'\\' ? L"" : L"\\") + L"adb.exe";
        if (IsFile(candidate)) return candidate;
    }
    return {};
}

namespace {

// Runs adb with the given arguments, without a console window; returns its
// exit code (or -1) and, if asked, what it printed.
int RunAdb(const std::wstring& adb, const std::wstring& args, std::string* output) {
    std::wstring cmd = L"\"" + adb + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    HANDLE readEnd = nullptr, writeEnd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (output && !CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    if (readEnd) SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (output) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, output != nullptr, CREATE_NO_WINDOW,
                                  nullptr, nullptr, &si, &pi);
    if (writeEnd) CloseHandle(writeEnd);
    if (!started) {
        if (readEnd) CloseHandle(readEnd);
        Log("adb: could not start %s", Narrow(adb).c_str());
        return -1;
    }
    if (output) {
        // adb's output is small; stop reading at 64 KB so a confused adb can't grow it without bound
        char buf[4096];
        DWORD n = 0;
        while (output->size() < 65536 && ReadFile(readEnd, buf, sizeof(buf), &n, nullptr) && n) output->append(buf, n);
        CloseHandle(readEnd);
    }
    DWORD exitCode = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exitCode);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(exitCode);
}

// Serials go on adb's command line: allow only what adb itself prints for
// USB and network devices (letters, digits, . : _ -).
bool SafeSerial(const std::string& serial) {
    if (serial.empty() || serial.size() > 128) return false;
    for (unsigned char c : serial)
        if (!isalnum(c) && c != '.' && c != ':' && c != '_' && c != '-') return false;
    return true;
}

}  // namespace

std::vector<std::string> AdbDevices(const std::wstring& adb) {
    std::string out;
    std::vector<std::string> serials;
    if (RunAdb(adb, L"devices", &out) != 0) return serials;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\n', pos);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto tab = line.find('\t');
        if (tab == std::string::npos || line.substr(tab + 1) != "device") continue;
        std::string serial = line.substr(0, tab);
        // Wireless debugging ("ip:port", "adb-…._adb-tls-connect._tcp") is not
        // the cable: those tablets are reached over Wi-Fi like any other.
        bool network = serial.find(':') != std::string::npos || serial.find("._adb-tls-connect.") != std::string::npos;
        if (!network && SafeSerial(serial)) serials.push_back(serial);
    }
    return serials;
}

bool AdbForward(const std::wstring& adb, const std::string& serial, uint16_t localPort, uint16_t remotePort) {
    if (!SafeSerial(serial)) return false;
    std::wstring args = L"-s " + std::wstring(serial.begin(), serial.end()) + L" forward tcp:" +
                        std::to_wstring(localPort) + L" tcp:" + std::to_wstring(remotePort);
    return RunAdb(adb, args, nullptr) == 0;
}

}  // namespace sst
