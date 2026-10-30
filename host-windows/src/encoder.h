#pragma once

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "com_util.h"

namespace sst {

// H.264 encoder on the GPU's hardware video engine (NVENC / AMD VCN / Intel
// Quick Sync), driven through Media Foundation. Takes NV12 GPU textures and
// produces Annex-B H.264 with no B-frames, tuned for low latency.
class H264Encoder {
public:
    // Called on the encoder's thread for every encoded frame.
    using OutputFn = std::function<void(const uint8_t* data, size_t size, int64_t ptsUs, bool keyFrame)>;

    struct Config {
        UINT width = 0;
        UINT height = 0;
        UINT fps = 60;
        UINT bitrate = 8'000'000;
    };

    ~H264Encoder();

    void Init(ID3D11Device* device, const LUID& adapterLuid, const Config& config, OutputFn onOutput);

    // Queues one frame. Blocks until the encoder can accept it. The texture
    // must stay untouched until the encoder is done with it (use a ring of
    // textures). Returns false if the encoder has failed.
    bool Encode(ID3D11Texture2D* nv12, int64_t ptsUs, bool forceKeyFrame);

    void Shutdown();

    const std::string& Name() const { return name_; }

private:
    void EventLoop();
    void HandleOutput();
    void UpdateOutputStreamInfo();
    void SetCodecValue(const GUID& key, VARIANT value, const char* name, bool required);

    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<ICodecAPI> codecApi_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT resetToken_ = 0;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
    bool mftProvidesSamples_ = false;
    DWORD outputBufferSize_ = 0;
    Config config_;
    OutputFn onOutput_;
    std::string name_;

    std::thread eventThread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int inputCredits_ = 0;
    bool stopping_ = false;
    bool failed_ = false;
};

}  // namespace sst
