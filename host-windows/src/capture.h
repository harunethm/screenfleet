#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>

#include "com_util.h"

namespace sst {

// Captures one monitor with DXGI Desktop Duplication into a GPU texture.
// The image never leaves the GPU.
class DesktopDuplicator {
public:
    enum class Result { NewFrame, NoChange };

    void Init(ID3D11Device* device, IDXGIOutput* output);

    // Waits up to timeoutMs for the screen to change. On NewFrame, Texture()
    // holds the latest image (BGRA). Recovers by itself from mode changes,
    // UAC prompts and full-screen apps taking over the output.
    Result Acquire(UINT timeoutMs);

    // When no picture arrives (e.g. the monitor is asleep and nothing redraws),
    // starts from a black image of this size; real frames replace it.
    void EnsureTexture(UINT width, UINT height, bool clearBlack);

    ID3D11Texture2D* Texture() const { return texture_.Get(); }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }

private:
    bool TryCreateDuplication();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> texture_;
    UINT width_ = 0;
    UINT height_ = 0;
    bool warnedRotation_ = false;
};

}  // namespace sst
