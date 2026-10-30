#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace sst {

class HrError : public std::runtime_error {
public:
    HrError(const char* what, HRESULT hr)
        : std::runtime_error(std::string(what) + " failed: " + HrToString(hr)), hr(hr) {}
    HRESULT hr;
};

inline void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw HrError(what, hr);
}

}  // namespace sst
