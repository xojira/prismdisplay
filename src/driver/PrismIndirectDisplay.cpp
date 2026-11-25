#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <avrt.h>

struct IndirectDeviceContext {
    WDFDEVICE WdfDevice;
    IDDCX_ADAPTER Adapter;
    IDDCX_MONITOR Monitor;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectDeviceContext, GetDeviceContext)

struct SwapChainContext {
    IDDCX_SWAPCHAIN SwapChain;
    HANDLE ThreadHandle;
    HANDLE TerminateEvent;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SwapChainContext, GetSwapChainContext)

static const BYTE s_KnownMonitorEdid[] = {
    0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x42,0x52,0x01,0x00,0x01,0x00,0x00,0x00,
    0x01,0x22,0x01,0x04,0xA5,0x1E,0x13,0x78,0x02,0xEE,0x95,0xA3,0x54,0x4C,0x99,0x26,
    0x0F,0x50,0x54,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x3A,0x80,0x18,0x71,0x38,0x2D,0x40,0x58,0x2C,
    0x45,0x00,0x2C,0xBE,0x10,0x00,0x00,0x1E,0x00,0x00,0x00,0xFD,0x00,0x1E,0x78,0x1E,
    0xA0,0x1E,0x00,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,0x50,
    0x72,0x69,0x73,0x6D,0x44,0x69,0x73,0x70,0x6C,0x61,0x79,0x0A,0x00,0x00,0x00,0x10,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x8A
};

EVT_WDF_DRIVER_DEVICE_ADD PrismDeviceAdd;
EVT_IDD_CX_ADAPTER_INIT_FINISHED PrismAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES PrismAdapterCommitModes;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES PrismMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES PrismMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN PrismMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN PrismMonitorUnassignSwapChain;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PrismDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS PrismDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit) {
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    IDD_CX_CLIENT_CONFIG iddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
    iddConfig.EvtIddCxAdapterInitFinished = PrismAdapterInitFinished;
    iddConfig.EvtIddCxAdapterCommitModes = PrismAdapterCommitModes;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = PrismMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes = PrismMonitorQueryModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain = PrismMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain = PrismMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(DeviceInit, &iddConfig);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContext);

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) return status;

    auto* ctx = GetDeviceContext(device);
    ctx->WdfDevice = device;

    IDDCX_ADAPTER_CAPS adapterCaps = {};
    adapterCaps.Size = sizeof(adapterCaps);
    adapterCaps.MaxMonitorsSupported = 2;
    adapterCaps.EndPointDiagnostics.Size = sizeof(adapterCaps.EndPointDiagnostics);
    adapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    adapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRELESS_MA_USB;

    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = device;
    adapterInit.pCaps = &adapterCaps;

    IDARG_OUT_ADAPTER_INIT adapterOut = {};
    status = IddCxAdapterInitAsync(&adapterInit, &adapterOut);
    if (NT_SUCCESS(status)) {
        ctx->Adapter = adapterOut.AdapterObject;
    }
    return status;
}

NTSTATUS PrismAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs) {
    if (!NT_SUCCESS(pInArgs->AdapterInitStatus)) return STATUS_SUCCESS;

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    monitorInfo.ConnectorIndex = 0;
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = sizeof(s_KnownMonitorEdid);
    monitorInfo.MonitorDescription.pData = const_cast<BYTE*>(s_KnownMonitorEdid);
    CoCreateGuid(&monitorInfo.MonitorContainerId);

    IDARG_IN_MONITORCREATE monCreate = {};
    monCreate.ObjectAttributes = nullptr;
    monCreate.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE monOut = {};
    NTSTATUS status = IddCxMonitorCreate(AdapterObject, &monCreate, &monOut);
    if (NT_SUCCESS(status)) {
        IDARG_OUT_MONITORARRIVAL arrivalOut = {};
        IddCxMonitorArrival(monOut.MonitorObject, &arrivalOut);
    }
    return status;
}

static void PopulateSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& info, UINT width, UINT height, UINT refresh) {
    info.totalSize.cx = info.activeSize.cx = width;
    info.totalSize.cy = info.activeSize.cy = height;
    info.AdditionalSignalInfo.vSyncFreqDivider = 0;
    info.AdditionalSignalInfo.videoStandard = 255;
    info.vSyncFreq.Numerator = refresh;
    info.vSyncFreq.Denominator = 1;
    info.hSyncFreq.Numerator = refresh * height;
    info.hSyncFreq.Denominator = 1;
    info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    info.pixelRate = static_cast<UINT64>(width) * height * refresh;
}

NTSTATUS PrismMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs) {
    UNREFERENCED_PARAMETER(MonitorObject);
    struct ModeSpec { UINT w, h, hz; };
    static const ModeSpec modes[] = { {1920, 1080, 60}, {1920, 1080, 120}, {2560, 1600, 60}, {2560, 1600, 120} };
    pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(modes);
    if (pInArgs->DefaultMonitorModeBufferInputCount < ARRAYSIZE(modes)) return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < ARRAYSIZE(modes); ++i) {
        pInArgs->pDefaultMonitorModes[i].Size = sizeof(IDDCX_MONITOR_MODE);
        pInArgs->pDefaultMonitorModes[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        PopulateSignalInfo(pInArgs->pDefaultMonitorModes[i].MonitorVideoSignalInfo, modes[i].w, modes[i].h, modes[i].hz);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

NTSTATUS PrismMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs) {
    UNREFERENCED_PARAMETER(MonitorObject);
    struct ModeSpec { UINT w, h, hz; };
    static const ModeSpec modes[] = { {1920, 1080, 60}, {1920, 1080, 120}, {2560, 1600, 60}, {2560, 1600, 120} };
    pOutArgs->TargetModeBufferOutputCount = ARRAYSIZE(modes);
    if (pInArgs->TargetModeBufferInputCount < ARRAYSIZE(modes)) return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < ARRAYSIZE(modes); ++i) {
        pInArgs->pTargetModes[i].Size = sizeof(IDDCX_TARGET_MODE);
        PopulateSignalInfo(pInArgs->pTargetModes[i].TargetVideoSignalInfo.targetVideoSignalInfo, modes[i].w, modes[i].h, modes[i].hz);
    }
    return STATUS_SUCCESS;
}

NTSTATUS PrismAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs) {
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    return STATUS_SUCCESS;
}

DWORD WINAPI SwapChainThreadProc(LPVOID param) {
    auto* ctx = static_cast<SwapChainContext*>(param);
    DWORD taskIndex = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);

    while (WaitForSingleObject(ctx->TerminateEvent, 0) == WAIT_TIMEOUT) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER bufferOut = {};
        NTSTATUS status = IddCxSwapChainReleaseAndAcquireBuffer(ctx->SwapChain, &bufferOut);
        if (status == STATUS_PENDING) {
            WaitForSingleObject(ctx->TerminateEvent, 16);
            continue;
        }
        if (NT_SUCCESS(status)) {
            IddCxSwapChainFinishedProcessingFrame(ctx->SwapChain);
        }
    }
    if (avrt) AvRevertMmThreadCharacteristics(avrt);
    return 0;
}

NTSTATUS PrismMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs) {
    UNREFERENCED_PARAMETER(MonitorObject);
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SwapChainContext);
    WdfObjectAllocateContext(pInArgs->hSwapChain, &attr, nullptr);

    auto* ctx = GetSwapChainContext(pInArgs->hSwapChain);
    ctx->SwapChain = pInArgs->hSwapChain;
    ctx->TerminateEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    ctx->ThreadHandle = CreateThread(nullptr, 0, SwapChainThreadProc, ctx, 0, nullptr);
    return STATUS_SUCCESS;
}

NTSTATUS PrismMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject) {
    UNREFERENCED_PARAMETER(MonitorObject);
    return STATUS_SUCCESS;
}
