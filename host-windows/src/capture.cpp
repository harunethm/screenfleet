#include "capture.h"

namespace sst {

void DesktopDuplicator::Init(ID3D11Device* device, IDXGIOutput* output) {
    device_ = device;
    device_->GetImmediateContext(&context_);
    output_ = output;
    // May fail while the lock screen or a UAC prompt is up; Acquire() retries.
    if (!TryCreateDuplication()) Log("capture not available yet, retrying");
}

bool DesktopDuplicator::TryCreateDuplication() {
    dup_.Reset();
    HRESULT hr;

    // DuplicateOutput1 hands us BGRA even for HDR / high bit-depth desktops.
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output_.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(device_.Get(), 0, ARRAYSIZE(formats), formats, &dup_);
    } else {
        ComPtr<IDXGIOutput1> output1;
        Check(output_.As(&output1), "IDXGIOutput1");
        hr = output1->DuplicateOutput(device_.Get(), &dup_);
    }
    if (FAILED(hr)) {
        // E_ACCESSDENIED while the secure desktop (UAC, lock screen) is shown;
        // the caller retries.
        Log("DuplicateOutput failed: %s", HrToString(hr).c_str());
        dup_.Reset();
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    dup_->GetDesc(&desc);
    if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
        !warnedRotation_) {
        Log("warning: display is rotated; rotation is not handled yet, the image will appear sideways");
        warnedRotation_ = true;
    }
    Log("capturing %ux%u", desc.ModeDesc.Width, desc.ModeDesc.Height);
    return true;
}

void DesktopDuplicator::EnsureTexture(UINT width, UINT height, bool clearBlack) {
    if (texture_ && width == width_ && height == height_) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_.Reset();
    Check(device_->CreateTexture2D(&desc, nullptr, &texture_), "CreateTexture2D(capture)");
    width_ = width;
    height_ = height;
    if (clearBlack) {
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device_->CreateRenderTargetView(texture_.Get(), nullptr, &rtv), "CreateRenderTargetView");
        const float black[4] = {0, 0, 0, 1};
        context_->ClearRenderTargetView(rtv.Get(), black);
    }
}

DesktopDuplicator::Result DesktopDuplicator::Acquire(UINT timeoutMs) {
    if (!dup_ && !TryCreateDuplication()) {
        Sleep(timeoutMs);
        return Result::NoChange;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::NoChange;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        Log("capture lost (%s), recreating", HrToString(hr).c_str());
        dup_.Reset();
        return Result::NoChange;
    }
    Check(hr, "AcquireNextFrame");

    Result result = Result::NoChange;
    // LastPresentTime == 0 means only the mouse moved; the image is unchanged.
    if (info.LastPresentTime.QuadPart != 0) {
        ComPtr<ID3D11Texture2D> frame;
        Check(resource.As(&frame), "IDXGIResource->ID3D11Texture2D");
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        EnsureTexture(desc.Width, desc.Height, false);
        context_->CopyResource(texture_.Get(), frame.Get());
        result = Result::NewFrame;
    }
    dup_->ReleaseFrame();
    return result;
}

}  // namespace sst
