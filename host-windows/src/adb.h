#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sst {

// Finds adb.exe: explicit path, then PATH, then the Android SDK default
// location. Returns an empty string if not found.
std::wstring FindAdb(const std::wstring& explicitPath);

// Serials of the attached, authorised devices (`adb devices`, state "device").
std::vector<std::string> AdbDevices(const std::wstring& adb);

// Runs `adb -s <serial> forward tcp:<localPort> tcp:<remotePort>` so
// 127.0.0.1:<localPort> on this PC reaches the app on that device. Each
// device gets its own local port, so several can be connected at once.
// Works on Android 4.4+.
bool AdbForward(const std::wstring& adb, const std::string& serial, uint16_t localPort, uint16_t remotePort);

}  // namespace sst
