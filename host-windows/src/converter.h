#pragma once

#include <d3d11.h>

#include <unordered_map>

#include "com_util.h"

namespace sst {

// Scales a BGRA texture to the stream size and converts it to NV12 (the
// encoder's input format) on the GPU's video engine. Keeps the aspect ratio,
// filling unused space with black.
class VideoConverter {
public:
    void Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT srcWidth, UINT srcHeight, UINT dstWidth,
              UINT dstHeight);
    void Convert(ID3D11Texture2D* srcBgra, ID3D11Texture2D* dstNv12);

    // Creates an NV12 texture the converter can write and the encoder can read.
    ComPtr<ID3D11Texture2D> CreateOutputTexture() const;

private:
    ID3D11VideoProcessorInputView* InputView(ID3D11Texture2D* texture);
    ID3D11VideoProcessorOutputView* OutputView(ID3D11Texture2D* texture);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inputViews_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outputViews_;
    UINT dstWidth_ = 0;
    UINT dstHeight_ = 0;
};

}  // namespace sst
