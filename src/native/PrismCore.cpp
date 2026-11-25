// Compile (x64 Native Tools Command Prompt):
// cl /LD /O2 /std:c++20 PrismCore.cpp d3d11.lib dxgi.lib mfplat.lib mfuuid.lib mfreadwrite.lib strmiids.lib user32.lib

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <dshow.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma pack(push, 1)
struct PointerEventPacket {
    uint8_t ToolType;     // 1 = Finger, 2 = Stylus, 3 = Eraser
    uint8_t Action;       // 0 = Down, 1 = Move, 2 = Up, 3 = Hover
    uint16_t Buttons;     // 0x01 = Barrel/RightClick, 0x02 = EraserInvert
    float NormX;
    float NormY;
    float Pressure;
    int16_t TiltX;
    int16_t TiltY;
    uint32_t PointerId;
};
#pragma pack(pop)

typedef void(__stdcall* OnEncodedFrameCallback)(const uint8_t* data, int length, int64_t timestampUs, bool isKeyFrame);

class PrismEngine {
private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<IMFTransform> m_encoder;
    ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    UINT m_resetToken = 0;

    HSYNTHETICPOINTERDEVICE m_penDevice = nullptr;
    HSYNTHETICPOINTERDEVICE m_touchDevice = nullptr;
    RECT m_targetMonitorRect = { 0, 0, 1920, 1080 };
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;

public:
    bool Initialize(int monitorIndex, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps) {
        m_width = width;
        m_height = height;
        if (FAILED(MFStartup(MF_VERSION))) return false;

        D3D_FEATURE_LEVEL featureLevel;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context))) {
            return false;
        }

        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(m_device.As(&mt))) {
            mt->SetMultithreadProtected(TRUE);
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device.As(&dxgiDevice))) return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(monitorIndex, &output))) return false;

        DXGI_OUTPUT_DESC outDesc;
        if (SUCCEEDED(output->GetDesc(&outDesc))) {
            m_targetMonitorRect = outDesc.DesktopCoordinates;
        }

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) return false;
        if (FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication))) return false;

        m_penDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        m_touchDevice = CreateSyntheticPointerDevice(PT_TOUCH, 10, POINTER_FEEDBACK_DEFAULT);

        return SetupHardwareEncoder(width, height, fps, bitrateBps);
    }

    bool SetupHardwareEncoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps) {
        if (FAILED(MFCreateDXGIDeviceManager(&m_resetToken, &m_dxgiManager))) return false;
        if (FAILED(m_dxgiManager->ResetDevice(m_device.Get(), m_resetToken))) return false;

        MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** ppActivate = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &outType,
            &ppActivate,
            &count
        );
        if (FAILED(hr) || count == 0) return false;

        hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&m_encoder));
        for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);
        if (FAILED(hr) || !m_encoder) return false;

        m_encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiManager.Get()));

        ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(m_encoder.As(&codecApi))) {
            VARIANT var;
            var.vt = VT_BOOL;
            var.boolVal = VARIANT_TRUE;
            codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);

            var.vt = VT_UI4;
            var.ulVal = eAVEncCommonRateControlMode_CBR;
            codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

            var.ulVal = bitrateBps;
            codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

            var.ulVal = fps * 2;
            codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        }

        ComPtr<IMFMediaType> outMediaType;
        MFCreateMediaType(&outMediaType);
        outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outMediaType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
        MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
        outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        if (FAILED(m_encoder->SetOutputType(0, outMediaType.Get(), 0))) return false;

        ComPtr<IMFMediaType> inMediaType;
        MFCreateMediaType(&inMediaType);
        inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
        inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(m_encoder->SetInputType(0, inMediaType.Get(), 0))) return false;

        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void CaptureAndEncodeFrame(OnEncodedFrameCallback callback) {
        if (!m_duplication || !m_encoder || !callback) return;

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> desktopResource;
        HRESULT hr = m_duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
        if (FAILED(hr)) return;

        ComPtr<ID3D11Texture2D> gpuTex;
        if (SUCCEEDED(desktopResource.As(&gpuTex))) {
            D3D11_TEXTURE2D_DESC desc;
            gpuTex->GetDesc(&desc);
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = 0;

            ComPtr<ID3D11Texture2D> copyTex;
            if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &copyTex))) {
                m_context->CopyResource(copyTex.Get(), gpuTex.Get());

                ComPtr<IMFMediaBuffer> buffer;
                if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), copyTex.Get(), 0, FALSE, &buffer))) {
                    ComPtr<IMFSample> sample;
                    MFCreateSample(&sample);
                    sample->AddBuffer(buffer.Get());

                    LARGE_INTEGER qpc, freq;
                    QueryPerformanceCounter(&qpc);
                    QueryPerformanceFrequency(&freq);
                    int64_t time100Ns = (qpc.QuadPart * 10000000LL) / freq.QuadPart;
                    sample->SetSampleTime(time100Ns);
                    sample->SetSampleDuration(166666);

                    if (SUCCEEDED(m_encoder->ProcessInput(0, sample.Get(), 0))) {
                        MFT_OUTPUT_DATA_BUFFER outData = {};
                        outData.dwStreamID = 0;
                        DWORD status = 0;
                        while (SUCCEEDED(m_encoder->ProcessOutput(0, 1, &outData, &status))) {
                            if (outData.pSample) {
                                ComPtr<IMFMediaBuffer> outBuf;
                                if (SUCCEEDED(outData.pSample->ConvertToContiguousBuffer(&outBuf))) {
                                    BYTE* pBytes = nullptr;
                                    DWORD maxLen = 0, curLen = 0;
                                    outBuf->Lock(&pBytes, &maxLen, &curLen);

                                    UINT32 cleanPoint = 0;
                                    outData.pSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
                                    callback(pBytes, static_cast<int>(curLen), time100Ns / 10, cleanPoint != 0);

                                    outBuf->Unlock();
                                }
                                outData.pSample->Release();
                                outData.pSample = nullptr;
                            }
                        }
                    }
                }
            }
        }
        m_duplication->ReleaseFrame();
    }

    void InjectPointer(const PointerEventPacket& pkt) {
        long targetW = m_targetMonitorRect.right - m_targetMonitorRect.left;
        long targetH = m_targetMonitorRect.bottom - m_targetMonitorRect.top;
        long absX = m_targetMonitorRect.left + static_cast<long>(pkt.NormX * targetW);
        long absY = m_targetMonitorRect.top + static_cast<long>(pkt.NormY * targetH);

        POINTER_TYPE_INFO pti = {};
        POINTER_FLAGS flags = POINTER_FLAG_NONE;

        if (pkt.Action == 0) flags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        else if (pkt.Action == 1) flags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        else if (pkt.Action == 2) flags = POINTER_FLAG_UP | POINTER_FLAG_INRANGE;
        else if (pkt.Action == 3) flags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE;

        if ((pkt.ToolType == 2 || pkt.ToolType == 3) && m_penDevice) {
            pti.type = PT_PEN;
            pti.penInfo.pointerInfo.pointerType = PT_PEN;
            pti.penInfo.pointerInfo.pointerId = 0;
            pti.penInfo.pointerInfo.ptPixelLocation.x = absX;
            pti.penInfo.pointerInfo.ptPixelLocation.y = absY;
            pti.penInfo.pointerInfo.pointerFlags = flags;

            pti.penInfo.penFlags = PEN_FLAG_NONE;
            if (pkt.Buttons & 0x01) pti.penInfo.penFlags |= PEN_FLAG_BARREL;
            if (pkt.ToolType == 3 || (pkt.Buttons & 0x02)) pti.penInfo.penFlags |= PEN_FLAG_ERASER | PEN_FLAG_INVERTED;

            pti.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
            pti.penInfo.pressure = static_cast<UINT32>(pkt.Pressure * 1024.0f);
            pti.penInfo.tiltX = pkt.TiltX;
            pti.penInfo.tiltY = pkt.TiltY;

            InjectSyntheticPointerInput(m_penDevice, &pti, 1);
        } else if (m_touchDevice) {
            pti.type = PT_TOUCH;
            pti.touchInfo.pointerInfo.pointerType = PT_TOUCH;
            pti.touchInfo.pointerInfo.pointerId = pkt.PointerId % 10;
            pti.touchInfo.pointerInfo.ptPixelLocation.x = absX;
            pti.touchInfo.pointerInfo.ptPixelLocation.y = absY;
            pti.touchInfo.pointerInfo.pointerFlags = flags;
            pti.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
            pti.touchInfo.pressure = 512;
            pti.touchInfo.rcContact = { absX - 4, absY - 4, absX + 4, absY + 4 };

            InjectSyntheticPointerInput(m_touchDevice, &pti, 1);
        }
    }

    void ForceKeyFrame() {
        ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(m_encoder.As(&codecApi))) {
            VARIANT var;
            var.vt = VT_UI4;
            var.ulVal = 1;
            codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
        }
    }

    ~PrismEngine() {
        if (m_penDevice) DestroySyntheticPointerDevice(m_penDevice);
        if (m_touchDevice) DestroySyntheticPointerDevice(m_touchDevice);
        MFShutdown();
    }
};

extern "C" {
    __declspec(dllexport) PrismEngine* Prism_Create() { return new PrismEngine(); }
    __declspec(dllexport) bool Prism_Init(PrismEngine* e, int mon, uint32_t w, uint32_t h, uint32_t fps, uint32_t bps) { return e ? e->Initialize(mon, w, h, fps, bps) : false; }
    __declspec(dllexport) void Prism_Tick(PrismEngine* e, OnEncodedFrameCallback cb) { if (e) e->CaptureAndEncodeFrame(cb); }
    __declspec(dllexport) void Prism_Inject(PrismEngine* e, PointerEventPacket pkt) { if (e) e->InjectPointer(pkt); }
    __declspec(dllexport) void Prism_ForceIDR(PrismEngine* e) { if (e) e->ForceKeyFrame(); }
    __declspec(dllexport) void Prism_Destroy(PrismEngine* e) { delete e; }
}