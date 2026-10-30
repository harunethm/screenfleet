#include "converter.h"

#include <algorithm>

namespace sst {

void VideoConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT srcWidth, UINT srcHeight,
                          UINT dstWidth, UINT dstHeight) {
    device_ = device;
    dstWidth_ = dstWidth;
    dstHeight_ = dstHeight;
    inputViews_.clear();
    outputViews_.clear();
    processor_.Reset();
    enumerator_.Reset();

    Check(device_.As(&videoDevice_), "ID3D11VideoDevice");
    ComPtr<ID3D11DeviceContext> ctx(context);
    Check(ctx.As(&videoContext_), "ID3D11VideoContext");

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = srcWidth;
    content.InputHeight = srcHeight;
    content.OutputWidth = dstWidth;
    content.OutputHeight = dstHeight;
    content.InputFrameRate = {60, 1};
    content.OutputFrameRate = {60, 1};
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    Check(videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator_), "CreateVideoProcessorEnumerator");

    UINT flags = 0;
    Check(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags), "CheckVideoProcessorFormat");
    if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw std::runtime_error("GPU video processor cannot output NV12");

    Check(videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_), "CreateVideoProcessor");

    auto* vp = processor_.Get();
    videoContext_->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    // Desktop is full-range RGB; output is BT.709 limited-range YUV, which is
    // what the encoder signals in the stream.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range = 0;  // 0-255
    videoContext_->VideoProcessorSetStreamColorSpace(vp, 0, &in);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.YCbCr_Matrix = 1;  // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetOutputColorSpace(vp, &out);

    // Fit the source into the destination, keeping the aspect ratio.
    double scale = std::min(double(dstWidth) / srcWidth, double(dstHeight) / srcHeight);
    LONG w = std::min<LONG>(LONG(srcWidth * scale + 0.5) & ~1L, LONG(dstWidth));
    LONG h = std::min<LONG>(LONG(srcHeight * scale + 0.5) & ~1L, LONG(dstHeight));
    LONG x = ((LONG(dstWidth) - w) / 2) & ~1L;
    LONG y = ((LONG(dstHeight) - h) / 2) & ~1L;
    RECT srcRect{0, 0, LONG(srcWidth), LONG(srcHeight)};
    RECT dstRect{x, y, x + w, y + h};
    RECT target{0, 0, LONG(dstWidth), LONG(dstHeight)};
    videoContext_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &srcRect);
    videoContext_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dstRect);
    videoContext_->VideoProcessorSetOutputTargetRect(vp, TRUE, &target);
    D3D11_VIDEO_COLOR black{};
    black.RGBA = {0.0f, 0.0f, 0.0f, 1.0f};
    videoContext_->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);

    Log("converter: %ux%u -> %ux%u (picture %ldx%ld at %ld,%ld)", srcWidth, srcHeight, dstWidth, dstHeight, w, h,
        x, y);
}

ComPtr<ID3D11Texture2D> VideoConverter::CreateOutputTexture() const {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = dstWidth_;
    desc.Height = dstHeight_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    Check(device_->CreateTexture2D(&desc, nullptr, &tex), "CreateTexture2D(NV12)");
    return tex;
}

ID3D11VideoProcessorInputView* VideoConverter::InputView(ID3D11Texture2D* texture) {
    auto it = inputViews_.find(texture);
    if (it != inputViews_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> view;
    Check(videoDevice_->CreateVideoProcessorInputView(texture, enumerator_.Get(), &desc, &view),
          "CreateVideoProcessorInputView");
    return (inputViews_[texture] = view).Get();
}

ID3D11VideoProcessorOutputView* VideoConverter::OutputView(ID3D11Texture2D* texture) {
    auto it = outputViews_.find(texture);
    if (it != outputViews_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> view;
    Check(videoDevice_->CreateVideoProcessorOutputView(texture, enumerator_.Get(), &desc, &view),
          "CreateVideoProcessorOutputView");
    return (outputViews_[texture] = view).Get();
}

void VideoConverter::Convert(ID3D11Texture2D* srcBgra, ID3D11Texture2D* dstNv12) {
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = InputView(srcBgra);
    Check(videoContext_->VideoProcessorBlt(processor_.Get(), OutputView(dstNv12), 0, 1, &stream),
          "VideoProcessorBlt");
}

}  // namespace sst
