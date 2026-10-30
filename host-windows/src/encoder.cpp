#include "encoder.h"

#include <mferror.h>

#include <algorithm>
#include <chrono>

namespace sst {

namespace {

VARIANT U32Variant(ULONG v) {
    VARIANT var{};
    var.vt = VT_UI4;
    var.ulVal = v;
    return var;
}

VARIANT BoolVariant(bool v) {
    VARIANT var{};
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    return var;
}

}  // namespace

H264Encoder::~H264Encoder() { Shutdown(); }

void H264Encoder::SetCodecValue(const GUID& key, VARIANT value, const char* name, bool required) {
    HRESULT hr = codecApi_->SetValue(&key, &value);
    if (FAILED(hr)) {
        if (required) Check(hr, name);
        Log("encoder: %s not supported (%s)", name, HrToString(hr).c_str());
    }
}

void H264Encoder::Init(ID3D11Device* device, const LUID& adapterLuid, const Config& config, OutputFn onOutput) {
    config_ = config;
    onOutput_ = std::move(onOutput);

    // Find a hardware H.264 encoder on the same GPU that captures the screen,
    // so frames never cross between GPUs.
    ComPtr<IMFAttributes> enumAttrs;
    Check(MFCreateAttributes(&enumAttrs, 1), "MFCreateAttributes");
    Check(enumAttrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapterLuid), sizeof(LUID)),
          "SetBlob(MFT_ENUM_ADAPTER_LUID)");
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    Check(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo,
                   &outInfo, enumAttrs.Get(), &activates, &count),
          "MFTEnum2");
    for (UINT32 i = 0; i < count; ++i) {
        if (i == 0) activate_ = activates[i];
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    if (!activate_) throw std::runtime_error("no hardware H.264 encoder found on this GPU");

    wchar_t* friendly = nullptr;
    UINT32 friendlyLen = 0;
    if (SUCCEEDED(activate_->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &friendlyLen))) {
        name_ = Narrow(friendly);
        CoTaskMemFree(friendly);
    }

    Check(activate_->ActivateObject(IID_PPV_ARGS(&mft_)), "ActivateObject(encoder)");

    // Hardware encoders are asynchronous MFTs; they must be unlocked first.
    ComPtr<IMFAttributes> attrs;
    Check(mft_->GetAttributes(&attrs), "IMFTransform::GetAttributes");
    Check(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK");
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    HRESULT hr = mft_->GetStreamIDs(1, &inputStreamId_, 1, &outputStreamId_);
    if (hr == E_NOTIMPL) {
        inputStreamId_ = 0;
        outputStreamId_ = 0;
    } else {
        Check(hr, "GetStreamIDs");
    }

    // Share our D3D11 device so the encoder reads frames straight from GPU memory.
    Check(MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_), "MFCreateDXGIDeviceManager");
    Check(deviceManager_->ResetDevice(device, resetToken_), "IMFDXGIDeviceManager::ResetDevice");
    Check(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager_.Get())),
          "MFT_MESSAGE_SET_D3D_MANAGER");

    // Low-latency settings. Some encoders only accept these before the media
    // types are set.
    Check(mft_.As(&codecApi_), "ICodecAPI");
    SetCodecValue(CODECAPI_AVLowLatencyMode, BoolVariant(true), "AVLowLatencyMode", false);
    SetCodecValue(CODECAPI_AVEncCommonRateControlMode, U32Variant(eAVEncCommonRateControlMode_CBR),
                  "AVEncCommonRateControlMode", false);
    SetCodecValue(CODECAPI_AVEncCommonMeanBitRate, U32Variant(config_.bitrate), "AVEncCommonMeanBitRate", false);
    SetCodecValue(CODECAPI_AVEncMPVDefaultBPictureCount, U32Variant(0), "AVEncMPVDefaultBPictureCount", false);
    // Key frames come on demand; this is only a safety net.
    SetCodecValue(CODECAPI_AVEncMPVGOPSize, U32Variant(config_.fps * 10), "AVEncMPVGOPSize", false);

    ComPtr<IMFMediaType> outType;
    Check(MFCreateMediaType(&outType), "MFCreateMediaType");
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // Baseline profile: decodable by every Android hardware decoder.
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    Check(mft_->SetOutputType(outputStreamId_, outType.Get(), 0), "SetOutputType(H.264)");

    ComPtr<IMFMediaType> inType;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> candidate;
        hr = mft_->GetInputAvailableType(inputStreamId_, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        Check(hr, "GetInputAvailableType");
        GUID subtype{};
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            inType = candidate;
            break;
        }
    }
    if (!inType) throw std::runtime_error("encoder does not accept NV12 input");
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    Check(mft_->SetInputType(inputStreamId_, inType.Get(), 0), "SetInputType(NV12)");

    UpdateOutputStreamInfo();

    Check(mft_.As(&events_), "IMFMediaEventGenerator");
    Check(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "NOTIFY_BEGIN_STREAMING");
    Check(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "NOTIFY_START_OF_STREAM");

    stopping_ = false;
    failed_ = false;
    inputCredits_ = 0;
    eventThread_ = std::thread([this] { EventLoop(); });

    Log("encoder: %s, %ux%u @ %u fps, %u kbit/s", name_.c_str(), config_.width, config_.height, config_.fps,
        config_.bitrate / 1000);
}

void H264Encoder::UpdateOutputStreamInfo() {
    MFT_OUTPUT_STREAM_INFO info{};
    Check(mft_->GetOutputStreamInfo(outputStreamId_, &info), "GetOutputStreamInfo");
    mftProvidesSamples_ =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputBufferSize_ = std::max<DWORD>(info.cbSize, config_.width * config_.height * 3 / 2);
}

void H264Encoder::EventLoop() {
    for (;;) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = events_->GetEvent(0, &event);  // blocks
        if (FAILED(hr)) {
            if (hr != MF_E_SHUTDOWN) Log("encoder: GetEvent failed: %s", HrToString(hr).c_str());
            break;
        }
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            {
                std::lock_guard lock(mutex_);
                ++inputCredits_;
            }
            cv_.notify_all();
        } else if (type == METransformHaveOutput) {
            try {
                HandleOutput();
            } catch (const std::exception& e) {
                Log("encoder: %s", e.what());
                break;
            }
        } else if (type == MEError) {
            HRESULT status = S_OK;
            event->GetStatus(&status);
            Log("encoder: error event %s", HrToString(status).c_str());
            break;
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (!stopping_) failed_ = true;
    }
    cv_.notify_all();
}

void H264Encoder::HandleOutput() {
    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = outputStreamId_;
    ComPtr<IMFSample> ownSample;
    if (!mftProvidesSamples_) {
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateSample(&ownSample), "MFCreateSample");
        Check(MFCreateMemoryBuffer(outputBufferSize_, &buffer), "MFCreateMemoryBuffer");
        ownSample->AddBuffer(buffer.Get());
        out.pSample = ownSample.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) out.pEvents->Release();

    // When the MFT allocated the sample, we now own a reference to it.
    ComPtr<IMFSample> sample;
    if (mftProvidesSamples_)
        sample.Attach(out.pSample);
    else
        sample = ownSample;

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outputStreamId_, 0, &type)))
            mft_->SetOutputType(outputStreamId_, type.Get(), 0);
        UpdateOutputStreamInfo();
        return;
    }
    if (FAILED(hr)) {
        Log("encoder: ProcessOutput failed: %s", HrToString(hr).c_str());
        return;
    }
    if (!sample) return;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return;
    LONGLONG timeHns = 0;
    sample->GetSampleTime(&timeHns);
    bool keyFrame = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
    if (onOutput_ && length > 0) onOutput_(data, length, timeHns / 10, keyFrame);
    buffer->Unlock();
}

bool H264Encoder::Encode(ID3D11Texture2D* nv12, int64_t ptsUs, bool forceKeyFrame) {
    {
        std::unique_lock lock(mutex_);
        while (inputCredits_ == 0 && !stopping_ && !failed_) {
            if (!cv_.wait_for(lock, std::chrono::seconds(2), [this] {
                    return inputCredits_ > 0 || stopping_ || failed_;
                })) {
                Log("encoder: still waiting for the encoder to accept input");
            }
        }
        if (stopping_ || failed_) return false;
        --inputCredits_;
    }

    if (forceKeyFrame) SetCodecValue(CODECAPI_AVEncVideoForceKeyFrame, U32Variant(1), "AVEncVideoForceKeyFrame", false);

    ComPtr<IMFMediaBuffer> buffer;
    Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer),
          "MFCreateDXGISurfaceBuffer");
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        DWORD length = 0;
        if (SUCCEEDED(buffer2d->GetContiguousLength(&length))) buffer->SetCurrentLength(length);
    }
    ComPtr<IMFSample> sample;
    Check(MFCreateSample(&sample), "MFCreateSample");
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ptsUs * 10);
    sample->SetSampleDuration(10'000'000LL / config_.fps);

    HRESULT hr = mft_->ProcessInput(inputStreamId_, sample.Get(), 0);
    if (FAILED(hr)) {
        Log("encoder: ProcessInput failed: %s", HrToString(hr).c_str());
        return false;
    }
    return true;
}

void H264Encoder::Shutdown() {
    if (!mft_) return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    // Unblocks GetEvent in the event thread with MF_E_SHUTDOWN.
    ComPtr<IMFShutdown> shutdown;
    if (SUCCEEDED(mft_.As(&shutdown))) shutdown->Shutdown();
    if (eventThread_.joinable()) eventThread_.join();

    events_.Reset();
    codecApi_.Reset();
    mft_.Reset();
    if (activate_) activate_->ShutdownObject();
    activate_.Reset();
    deviceManager_.Reset();
}

}  // namespace sst
