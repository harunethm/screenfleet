#pragma once

#include <dxgi1_6.h>

#include <string>
#include <vector>

#include "com_util.h"

namespace sst {

// A monitor attached to the desktop, plus the GPU that drives it.
struct DisplayOutput {
    int index = 0;
    std::string gdiName;       // e.g. \\.\DISPLAY2
    std::string monitorName;   // e.g. "DELL U2720Q" (from EDID, may be empty)
    std::string adapterName;   // e.g. "NVIDIA GeForce RTX 4070"
    LUID adapterLuid{};
    RECT desktopRect{};
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;

    int Width() const { return desktopRect.right - desktopRect.left; }
    int Height() const { return desktopRect.bottom - desktopRect.top; }
};

std::vector<DisplayOutput> EnumerateOutputs();

}  // namespace sst
