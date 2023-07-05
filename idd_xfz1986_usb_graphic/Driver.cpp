/*++

Copyright (c) Microsoft Corporation

Abstract:

    This module contains a sample implementation of an indirect display driver. See the included README.md file and the
    various TODO blocks throughout this file and all accompanying files for information on building a production driver.

    MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"
#include "Driver.tmh"
#include"tiny_jpeg.h"
#include <stdarg.h>
using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

void scale_for_320x240(uint32_t * dst, uint32_t * src, int line, int len);
NTSTATUS
idd_usbdisp_evt_device_prepareHardware(
	WDFDEVICE Device,
	WDFCMRESLIST ResourceList,
	WDFCMRESLIST ResourceListTranslated
	);
// Default modes reported for edid-less monitors. The first mode is set as preferred
static const struct IndirectSampleMonitor::SampleMonitorMode s_SampleDefaultModes[] = 
{
    { 320, 240, 60 },
    { 640,  480, 60 },
    { 800,  600, 60 },
	{ 1024,  600, 60 },
};

// FOR SAMPLE PURPOSES ONLY, Static info about monitors that will be reported to OS
static const struct IndirectSampleMonitor s_SampleMonitors[] =
{
    // Modified EDID from Dell S2719DGF
    {
        {
            0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,0x24,0x1D,0x01,
            0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54,0x00,0x02,0x00,
            0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,
            0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,
            0x00,0x37,0x4A,0x51,0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,
            0x53,0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,0x00,0x28,
            0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
        },
        {
            { 320, 240, 60 },
            { 640, 480,  60 },
            { 1024, 600,  60 },
        },
        0
    },
};
static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD IddSampleDeviceAdd;


EVT_WDF_DEVICE_D0_ENTRY IddSampleDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED IddSampleAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES IddSampleAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION IddSampleParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES IddSampleMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES IddSampleMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN IddSampleMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN IddSampleMonitorUnassignSwapChain;

#if 1
#define LOG(fmt,...) do {;} while(0)

#else

#define LOG _log
void _log(const char * fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	OutputDebugStringA(buf);
}

#endif

struct IndirectDeviceContextWrapper {
    IndirectDeviceContext* pContext;
    //usb disp
    WDFUSBDEVICE                    UsbDevice;

    WDFUSBINTERFACE                 UsbInterface;

    WDFUSBPIPE                      BulkReadPipe;
	ULONG max_in_pkg_size;

    WDFUSBPIPE                      BulkWritePipe;
	ULONG max_out_pkg_size;
    ULONG							UsbDeviceTraits;
    //

    void Cleanup() {
        delete pContext;
        pContext = nullptr;
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WPP_INIT_TRACING(pDriverObject, pRegistryPath);

    LOG("idd Driver 1986\n");

    WDF_DRIVER_CONFIG_INIT(&Config,

                           IddSampleDeviceAdd
                          );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if(!NT_SUCCESS(Status)) {
        return Status;
    }

    return Status;
}







_Use_decl_annotations_
NTSTATUS IddSampleDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{

    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);
	LOG("IddSampleDeviceAdd\n");
    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = IddSampleDeviceD0Entry;
    PnpPowerCallbacks.EvtDevicePrepareHardware = idd_usbdisp_evt_device_prepareHardware;

    //
    // These two callbacks start and stop the wdfusb pipe continuous reader
    // as we go in and out of the D0-working state.
    //

    //PnpPowerCallbacks.EvtDeviceD0Entry = idd_usbdisp_evt_device_D0Entry;
//    PnpPowerCallbacks.EvtDeviceD0Exit  = idd_usbdisp_evt_device_D0Exit;
//   PnpPowerCallbacks.EvtDeviceSelfManagedIoFlush = idd_usbdisp_evt_device_SelfManagedIoFlush;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
    // redirects IoDeviceControl requests to an internal queue. This sample does not need this.
    // IddConfig.EvtIddCxDeviceIoControl = IddSampleIoDeviceControl;

    IddConfig.EvtIddCxAdapterInitFinished = IddSampleAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = IddSampleParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = IddSampleMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = IddSampleMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = IddSampleAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = IddSampleMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = IddSampleMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if(!NT_SUCCESS(Status)) {
		LOG("IddCxDeviceInitConfig failed with Status code %x\n", Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if(pContext) {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if(!NT_SUCCESS(Status)) {
        LOG("WdfDeviceCreate failed with Status code %x\n", Status);
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);

    // Create a new device context object and attach it to the WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_
NTSTATUS IddSampleDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    // This function is called by WDF to start the device in the fully-on power state.

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

Direct3DDevice::Direct3DDevice()
{

}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if(FAILED(hr)) {
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if(FAILED(hr)) {
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if(FAILED(hr)) {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, WDFDEVICE  WdfDevice, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), mp_WdfDevice(WdfDevice), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate
    SetEvent(m_hTerminateEvent.Get());

    if(m_hThread.Get()) {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    NTSTATUS status;
    int i = 0;
    purb_itm_t purb;
	auto* pDeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(mp_WdfDevice);
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);
    InitializeSListHead(&urb_list);


    LOG("init urb list\n");
    jpg_quality = 6; //default
#define JPG_QUALITY_SIZE_HIGH (100*1024)
    target_quaility_size = JPG_QUALITY_SIZE_HIGH;
    // Insert into the list.
#define MAX_URB_SIZE 2
    for(i = 1; i <= MAX_URB_SIZE; i++) {
        purb = (urb_itm_t *)_aligned_malloc(sizeof(urb_itm_t),
                                            MEMORY_ALLOCATION_ALIGNMENT);
        if(NULL == purb) {
            LOG("Memory allocation failed.\n");
            break;
        }
        status = WdfRequestCreate(
                     WDF_NO_OBJECT_ATTRIBUTES,
                     NULL,
                     &purb->Request
                 );

        if(!NT_SUCCESS(status)) {
            LOG("create request NG\n");
            break;//return status;
        }
        purb->id = i;
		purb->max_ep_out_size = pDeviceContext->max_out_pkg_size;
        purb->urb_list = &urb_list;
        InterlockedPushEntrySList(&urb_list,
                                  &(purb->node));
        curr_urb = purb;
    }


    RunCore();

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    for(i = 1; i <= MAX_URB_SIZE; i++) {

        PSLIST_ENTRY 	pentry = InterlockedPopEntrySList(&urb_list);

        if(NULL == purb) {
            LOG("List is empty.\n");
            break;
        }

        purb = (urb_itm_t *)pentry;
        LOG("id is %d\n", purb->id);

        // This example assumes that the SLIST_ENTRY structure is the
        // first member of the structure. If your structure does not
        // follow this convention, you must compute the starting address
        // of the structure before calling the free function.
        WdfObjectDelete(purb->Request);
        _aligned_free(pentry);


    }


    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}



void SwapChainProcessor::RunCore()
{
    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if(FAILED(hr)) {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if(FAILED(hr)) {
        return;
    }

    // Acquire and release buffers in a loop
    for(;;) {
        ComPtr<IDXGIResource> AcquiredBuffer;
        int line_width = 0;
        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if(hr == E_PENDING) {
            // We must wait for a new buffer
            HANDLE WaitHandles [] = {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if(WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT) {
                // We have a new buffer, so try the AcquireBuffer again
                continue;
            } else if(WaitResult == WAIT_OBJECT_0 + 1) {
                // We need to terminate
                break;
            } else {
                // The wait was cancelled or something unexpected happened
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        } else if(SUCCEEDED(hr)) {
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            // ==============================
            // TODO: Process the frame here
            //
            // This is the most performance-critical section of code in an IddCx driver. It's important that whatever
            // is done with the acquired surface be finished as quickly as possible. This operation could be:
            //  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
            //  * a GPU encode operation
            //  * a GPU VPBlt to another surface
            //  * a GPU custom compute shader encode operation
            // ==============================
            //LOG("-dxgi fid:%d dirty:%d\n",Buffer.MetaData.PresentationFrameNumber,Buffer.MetaData.DirtyRectCount);

#define RESET_OBJECT(obj) { if(obj) obj->Release(); obj = NULL; }
            ID3D11Texture2D *hAcquiredDesktopImage = NULL;
            hr = AcquiredBuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&hAcquiredDesktopImage));
            if(FAILED(hr)) {
                LOG("dxgi 2d NG\n");
                goto next;
            }

            // copy old description
            //
            D3D11_TEXTURE2D_DESC frameDescriptor;
            hAcquiredDesktopImage->GetDesc(&frameDescriptor);


            //
            // create a new staging buffer for cpu accessable, or only GPU can access
            //
            ID3D11Texture2D *hNewDesktopImage = NULL;
            frameDescriptor.Usage = D3D11_USAGE_STAGING;
            frameDescriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            frameDescriptor.BindFlags = 0;
            frameDescriptor.MiscFlags = 0;
            frameDescriptor.MipLevels = 1;
            frameDescriptor.ArraySize = 1;
            frameDescriptor.SampleDesc.Count = 1;
            hr = m_Device->Device->CreateTexture2D(&frameDescriptor, NULL, &hNewDesktopImage);
            if(FAILED(hr)) {
                RESET_OBJECT(hAcquiredDesktopImage);

                LOG("dxgi create 2d NG\n");
                goto next;
            }

            //
            // copy next staging buffer to new staging buffer
            //
            m_Device->DeviceContext->CopyResource(hNewDesktopImage, hAcquiredDesktopImage);


            //
            // create staging buffer for map bits
            //
            IDXGISurface *hStagingSurf = NULL;
            hr = hNewDesktopImage->QueryInterface(__uuidof(IDXGISurface), (void **)(&hStagingSurf));
            RESET_OBJECT(hNewDesktopImage);//release it
            if(FAILED(hr)) {
                LOG("dxgi surf NG\n");
                goto next;
            }

            //
            // copy bits to user space
            //
            DXGI_MAPPED_RECT mappedRect;
            hr = hStagingSurf->Map(&mappedRect, DXGI_MAP_READ);
            if(SUCCEEDED(hr)) {

                if(640 == frameDescriptor.Width) {
                    scale_for_320x240((uint32_t *)this->fb_buf, (uint32_t *)mappedRect.pBits, mappedRect.Pitch / 4, frameDescriptor.Width * frameDescriptor.Height);
                    line_width = mappedRect.Pitch / 8;
                } else {
                    memcpy(this->fb_buf, mappedRect.pBits, frameDescriptor.Width * frameDescriptor.Height * 4);
                    line_width = mappedRect.Pitch / 4;
                }

                hStagingSurf->Unmap();
            } else {

                LOG("dxgi map NG %x\n", hr);
            }
			
            //issue urb
            {
                auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(this->mp_WdfDevice);
                PSLIST_ENTRY 	pentry =  InterlockedPopEntrySList(&urb_list);
                urb_itm_t* purb = (urb_itm_t*)pentry;
                if(NULL != purb)
                    usb_send_jpeg_image(purb, pContext->BulkWritePipe, purb->msg, purb->urb_msg, (pixel_type_t *)fb_buf, 0, 0, frameDescriptor.Width-1, frameDescriptor.Height-1, line_width);
            }
            RESET_OBJECT(hStagingSurf);
            RESET_OBJECT(hAcquiredDesktopImage);
next:

            AcquiredBuffer.Reset();
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if(FAILED(hr)) {
                break;
            }

            // ==============================
            // TODO: Report frame statistics once the asynchronous encode/send work is completed
            //
            // Drivers should report information about sub-frame timings, like encode time, send time, etc.
            // ==============================
            // IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
        } else {
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice)
{
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    m_ProcessingThread.reset();
}

#define NUM_VIRTUAL_DISPLAYS 1

void IndirectDeviceContext::InitAdapter()
{
    // ==============================
    // TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
    // numbers are used for telemetry and may be displayed to the user in some situations.
    //
    // This is also where static per-adapter capabilities are determined.
    // ==============================

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = NUM_VIRTUAL_DISPLAYS;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"IddSample Device";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"Microsoft";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"IddSample Model";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;


    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;


    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if(NT_SUCCESS(Status)) {
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit()
{
	LOG("IndirectDeviceContext::FinishInit\n");
    for(unsigned int i = 0; i < NUM_VIRTUAL_DISPLAYS; i++) {
        CreateMonitor(i);
    }
}

void IndirectDeviceContext::CreateMonitor(unsigned int ConnectorIndex)
{
    // ==============================
    // TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDID
    // provided here is purely for demonstration, as it describes only 640x480 @ 60 Hz and 800x600 @ 60 Hz. Monitor
    // manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS to optimize
    // settings like viewing distance and scale factor. Manufacturers should also use a unique serial number every
    // single device to ensure the OS can tell the monitors apart.
    // ==============================

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = ConnectorIndex;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    if (ConnectorIndex >= ARRAYSIZE(s_SampleMonitors))
    {
        MonitorInfo.MonitorDescription.DataSize = 0;
        MonitorInfo.MonitorDescription.pData = nullptr;
    }
    else
    {
        MonitorInfo.MonitorDescription.DataSize = IndirectSampleMonitor::szEdidBlock;
        MonitorInfo.MonitorDescription.pData = const_cast<BYTE*>(s_SampleMonitors[ConnectorIndex].pEdidBlock);
    }

    // ==============================
    // TODO: The monitor's container ID should be distinct from "this" device's container ID if the monitor is not
    // permanently attached to the display adapter device object. The container ID is typically made unique for each
    // monitor and can be used to associate the monitor with other devices, like audio or input devices. In this
    // sample we generate a random container ID GUID, but it's best practice to choose a stable container ID for a
    // unique monitor or to use "this" device's container ID for a permanent/integrated monitor.
    // ==============================

    // Create a container ID
    CoCreateGuid(&MonitorInfo.MonitorContainerId);

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE MonitorCreateOut;

    LOG("IddCxMonitorCreate\n");
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if(NT_SUCCESS(Status)) {
        m_Monitor = MonitorCreateOut.MonitorObject;

        // Associate the monitor with this device context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorCreateOut.MonitorObject);
        pContext->pContext = this;

        // Tell the OS that the monitor has been plugged in
        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        LOG("IddCxMonitorArrival\n");
        Status = IddCxMonitorArrival(m_Monitor, &ArrivalOut);
    }
}

void IndirectDeviceContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    LOG("AssignSwapChain\n");
    if(FAILED(Device->Init())) {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    } else {
        // Create a new swap-chain processing thread
        m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, this->m_WdfDevice, NewFrameEvent));
    }
}

void IndirectDeviceContext::UnassignSwapChain()
{
    // Stop processing the last swap-chain
    LOG("UnassignSwapChain\n");
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS IddSampleAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if(NT_SUCCESS(pInArgs->AdapterInitStatus)) {
        pContext->pContext->FinishInit();
    }

    LOG("IddSampleAdapterInitFinished\n");


    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

    // ==============================
    // TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
    // through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
    // should be turned off).
    // ==============================

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
    // this sample driver, we hard-code the EDID, so this function can generate known modes.
    // ==============================

    pOutArgs->MonitorModeBufferOutputCount = IndirectSampleMonitor::szModeList;
	LOG("%s %d %d\n",__func__, pInArgs->MonitorModeBufferInputCount , IndirectSampleMonitor::szModeList);
    if (pInArgs->MonitorModeBufferInputCount < IndirectSampleMonitor::szModeList)
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    else
    {
        // In the sample driver, we have reported some static information about connected monitors
        // Check which of the reported monitors this call is for by comparing it to the pointer of
        // our known EDID blocks.

        if (pInArgs->MonitorDescription.DataSize != IndirectSampleMonitor::szEdidBlock)
            return STATUS_INVALID_PARAMETER;

        DWORD SampleMonitorIdx = 0;
        for(; SampleMonitorIdx < ARRAYSIZE(s_SampleMonitors); SampleMonitorIdx++)
        {
            if (memcmp(pInArgs->MonitorDescription.pData, s_SampleMonitors[SampleMonitorIdx].pEdidBlock, IndirectSampleMonitor::szEdidBlock) == 0)
            {
                // Copy the known modes to the output buffer
                for (DWORD ModeIndex = 0; ModeIndex < IndirectSampleMonitor::szModeList; ModeIndex++)
                {
                    pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
                        s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].Width,
                        s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].Height,
                        s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].VSync,
                        IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
                    );
					LOG("%s %d %d %d\n", __func__, ModeIndex, s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].Width, s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].Height);
                }

                // Set the preferred mode as represented in the EDID
                pOutArgs->PreferredMonitorModeIdx = s_SampleMonitors[SampleMonitorIdx].ulPreferredModeIdx;
        
                return STATUS_SUCCESS;
            }
        }

        // This EDID block does not belong to the monitors we reported earlier
        return STATUS_INVALID_PARAMETER;
    }
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
    // Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
    // monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
    // than an EDID, those modes would also be reported here.
    // ==============================
	LOG("%s\n",__func__);

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(s_SampleDefaultModes); 
    }
    else
    {
        for (DWORD ModeIndex = 0; ModeIndex < ARRAYSIZE(s_SampleDefaultModes); ModeIndex++)
        {
            pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
                s_SampleDefaultModes[ModeIndex].Width,
                s_SampleDefaultModes[ModeIndex].Height,
                s_SampleDefaultModes[ModeIndex].VSync,
                IDDCX_MONITOR_MODE_ORIGIN_DRIVER
            );
			LOG("%s %d %d\n", __func__, s_SampleDefaultModes[ModeIndex].Width,s_SampleDefaultModes[ModeIndex].Height);
        }

        pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(s_SampleDefaultModes); 
        pOutArgs->PreferredMonitorModeIdx = 0;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    vector<IDDCX_TARGET_MODE> TargetModes;

    // Create a set of modes supported for frame processing and scan-out. These are typically not based on the
    // monitor's descriptor and instead are based on the static processing capability of the device. The OS will
    // report the available set of modes for a given output as the intersection of monitor modes with target modes.

    TargetModes.push_back(CreateIddCxTargetMode(1024, 600, 60));
    TargetModes.push_back(CreateIddCxTargetMode(800, 600, 60));
    TargetModes.push_back(CreateIddCxTargetMode(800, 480, 60));
    TargetModes.push_back(CreateIddCxTargetMode(640, 480, 60));
    TargetModes.push_back(CreateIddCxTargetMode(320, 240, 60));
 
    pOutArgs->TargetModeBufferOutputCount = (UINT) TargetModes.size();

    if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
    {
        copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);

    LOG("IddSampleMonitorAssignSwapChain\n");
    pContext->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
    LOG("IddSampleMonitorUnassignSwapChain\n");
    pContext->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

/*************usb part*******************/
NTSTATUS get_usb_dev_string_info(_In_ WDFDEVICE Device) {

	NTSTATUS status, ntStatus;
	USHORT  numCharacters;
	PUSHORT  stringBuf;
	WDFMEMORY  memoryHandle;
	USB_DEVICE_DESCRIPTOR udesc;
	auto* pDeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);

	WdfUsbTargetDeviceGetDeviceDescriptor(pDeviceContext->UsbDevice, &udesc);

	status = WdfUsbTargetDeviceQueryString(
		pDeviceContext->UsbDevice,
		NULL,
		NULL,
		NULL,
		&numCharacters,
		udesc.iProduct,
		0x0409
		);

	ntStatus = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		NonPagedPool,
		0,
		(numCharacters+1) * sizeof(WCHAR),
		&memoryHandle,
		(PVOID*)&stringBuf
		);
	if (!NT_SUCCESS(ntStatus)) {
		return ntStatus;
	}
	status = WdfUsbTargetDeviceQueryString(
		pDeviceContext->UsbDevice,
		NULL,
		NULL,
		stringBuf,
		&numCharacters,
		udesc.iProduct,
		0x0409
		);
	stringBuf[numCharacters] = '\0';
	LOG("product %d %S\n", numCharacters, stringBuf);
	return status;
}

#if 1
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterfaces(
	_In_ WDFDEVICE Device
	)
	/*++

	Routine Description:

	This helper routine selects the configuration, interface and
	creates a context for every pipe (end point) in that interface.

	Arguments:

	Device - Handle to a framework device

	Return Value:

	NT status value

	--*/
{
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
	NTSTATUS                            status = STATUS_SUCCESS;
	//PDEVICE_CONTEXT                     pDeviceContext;
	auto* pDeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	WDFUSBPIPE                          pipe;
	WDF_USB_PIPE_INFORMATION            pipeInfo;
	UCHAR                               index;
	UCHAR                               numberConfiguredPipes;
	WDFUSBINTERFACE                     usbInterface;

	PAGED_CODE();
	get_usb_dev_string_info(Device);

	//pDeviceContext = GetDeviceContext(Device);

	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

	usbInterface =
		WdfUsbTargetDeviceGetInterface(pDeviceContext->UsbDevice, 0);

	if (NULL == usbInterface) {
		status = STATUS_UNSUCCESSFUL;
		return status;
	}

	configParams.Types.SingleInterface.ConfiguredUsbInterface =
		usbInterface;

	configParams.Types.SingleInterface.NumberConfiguredPipes =
		WdfUsbInterfaceGetNumConfiguredPipes(usbInterface);

	pDeviceContext->UsbInterface =
		configParams.Types.SingleInterface.ConfiguredUsbInterface;

	numberConfiguredPipes = configParams.Types.SingleInterface.NumberConfiguredPipes;

	//
	// Get pipe handles
	//
	for (index = 0; index < numberConfiguredPipes; index++) {

		WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

		pipe = WdfUsbInterfaceGetConfiguredPipe(
			pDeviceContext->UsbInterface,
			index, //PipeIndex,
			&pipeInfo
			);
		//
		// Tell the framework that it's okay to read less than
		// MaximumPacketSize
		//
		WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);



		if (WdfUsbPipeTypeBulk == pipeInfo.PipeType &&
			WdfUsbTargetPipeIsInEndpoint(pipe)) {

			pDeviceContext->BulkReadPipe = pipe;
			pDeviceContext->max_in_pkg_size = pipeInfo.MaximumPacketSize;
			LOG("BulkInput Pipe is 0x%p ep_max:%d\n", pipe, pDeviceContext->max_in_pkg_size);
		}

		if (WdfUsbPipeTypeBulk == pipeInfo.PipeType &&
			WdfUsbTargetPipeIsOutEndpoint(pipe)) {
			
			pDeviceContext->BulkWritePipe = pipe;
			pDeviceContext->max_out_pkg_size = pipeInfo.MaximumPacketSize;
			LOG("BulkOutput Pipe is 0x%p ep_max:%d\n", pipe, pDeviceContext->max_out_pkg_size);
		}

	}

	//
	// If we didn't find all the 2 pipes, fail the start.
	//
	if (!(pDeviceContext->BulkWritePipe
		&& pDeviceContext->BulkReadPipe)) {
		status = STATUS_INVALID_DEVICE_STATE;
		LOG("Device is not configured properly %x\n",
			status);

		return status;
	}

	return status;
}

#endif

NTSTATUS
idd_usbdisp_evt_device_prepareHardware(
	WDFDEVICE Device,
	WDFCMRESLIST ResourceList,
	WDFCMRESLIST ResourceListTranslated
	)
	/*++

	Routine Description:

	In this callback, the driver does whatever is necessary to make the
	hardware ready to use.  In the case of a USB device, this involves
	reading and selecting descriptors.

	Arguments:

	Device - handle to a device

	ResourceList - handle to a resource-list object that identifies the
	raw hardware resources that the PnP manager assigned
	to the device

	ResourceListTranslated - handle to a resource-list object that
	identifies the translated hardware resources
	that the PnP manager assigned to the device

	Return Value:

	NT status value

	--*/
{
	NTSTATUS                            status;
	//PDEVICE_CONTEXT                     pDeviceContext;
	auto* pDeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	WDF_USB_DEVICE_INFORMATION          deviceInfo;
	ULONG                               waitWakeEnable;

	UNREFERENCED_PARAMETER(ResourceList);
	UNREFERENCED_PARAMETER(ResourceListTranslated);
	waitWakeEnable = FALSE;
	PAGED_CODE();

	LOG("--> EvtDevicePrepareHardware\n");

	//pDeviceContext = GetDeviceContext(Device);


	//
	// Create a USB device handle so that we can communicate with the
	// underlying USB stack. The WDFUSBDEVICE handle is used to query,
	// configure, and manage all aspects of the USB device.
	// These aspects include device properties, bus properties,
	// and I/O creation and synchronization. We only create device the first
	// the PrepareHardware is called. If the device is restarted by pnp manager
	// for resource rebalance, we will use the same device handle but then select
	// the interfaces again because the USB stack could reconfigure the device on
	// restart.
	//
	if (pDeviceContext->UsbDevice == NULL) {

#if UMDF_VERSION_MINOR >= 25
		WDF_USB_DEVICE_CREATE_CONFIG createParams;

		WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createParams,
			USBD_CLIENT_CONTRACT_VERSION_602);

		status = WdfUsbTargetDeviceCreateWithParameters(
			Device,
			&createParams,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDeviceContext->UsbDevice);

#else
		status = WdfUsbTargetDeviceCreate(Device,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDeviceContext->UsbDevice);


#endif

		if (!NT_SUCCESS(status)) {
			LOG("WdfUsbTargetDeviceCreate failed with Status code %x\n", status);
			return status;
		}
	}

	//
	// Retrieve USBD version information, port driver capabilites and device
	// capabilites such as speed, power, etc.
	//
	WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);

	status = WdfUsbTargetDeviceRetrieveInformation(
		pDeviceContext->UsbDevice,
		&deviceInfo);
	if (NT_SUCCESS(status)) {


		waitWakeEnable = deviceInfo.Traits &
			WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

		//
		// Save these for use later.
		//
		pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
	}
	else {
		pDeviceContext->UsbDeviceTraits = 0;
	}
#if 1
	status = SelectInterfaces(Device);
	if (!NT_SUCCESS(status)) {
		LOG("SelectInterfaces failed 0x%x\n", status);
		return status;
	}
#endif
	LOG("<-- EvtDevicePrepareHardware\n");

	return status;
}

/**********************/

void dump_memory_bytes(char *prefix, uint8_t * hash, int bytes)
{
	char out[256] = { 0 };
	int len = 0, i = 0;

	for (i = 0; i < bytes; i++) {
		if (i && (i % 16 == 0)) {
			LOG("[%04d]%s %s\n", i, prefix, out);
			len = 0;
		}
		len += sprintf(out + len, "0x%02x,", hash[i] & 0xff);

	}
	LOG("[%04d]%s %s\n", i, prefix, out);

}


long get_os_us(void)
{

	SYSTEMTIME time;

	GetSystemTime(&time);
	long time_ms = (time.wSecond * 1000) + time.wMilliseconds;

	return time_ms * 1000;



}


long get_system_us(void)
{
	return get_os_us();
}



long SwapChainProcessor::get_fps(void)
{
	fps_mgr_t * mgr = &fps_mgr;
	if (mgr->cur < FPS_STAT_MAX)//we ignore first loop and also ignore rollback case due to a long period
		return mgr->last_fps;//if <0 ,please ignore it
	else {
		long a = mgr->tb[(mgr->cur - 1) % FPS_STAT_MAX];
		long b = mgr->tb[(mgr->cur) % FPS_STAT_MAX];
		long fps = (a - b) / (FPS_STAT_MAX - 1);
		fps = (1000000 * 10) / fps; //x10 for int
		mgr->last_fps = fps;
		return fps;
	}
}

void SwapChainProcessor::put_fps_data(long t)
{
	static int init = 0;
	fps_mgr_t * mgr = &fps_mgr;

	if (0 == init) {
		mgr->cur = 0;
		mgr->last_fps = -1;
		init = 1;
	}

	mgr->tb[mgr->cur%FPS_STAT_MAX] = t;
	mgr->cur++;//cur ptr to next

}


NTSTATUS usb_send_msg(WDFUSBPIPE pipeHandle, WDFREQUEST Request, PUCHAR msg, int tsize)
{
	WDF_MEMORY_DESCRIPTOR  writeBufDesc;
	WDFMEMORY  wdfMemory;
	ULONG  writeSize, bytesWritten;
	size_t  bufferSize;
	NTSTATUS status;
	PVOID  writeBuffer;

	writeSize = tsize;
	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		NonPagedPool,
		0,
		writeSize,
		&wdfMemory,
		NULL
		);
	if (!NT_SUCCESS(status)) {
		LOG("WdfMemoryCreate NG\n");
		return status;
	}

	writeBuffer = WdfMemoryGetBuffer(
		wdfMemory,
		&bufferSize
		);



	//memcpy(writeBuffer,msg, writeSize);
	WdfMemoryCopyFromBuffer(wdfMemory, 0, msg, writeSize);


	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&writeBufDesc,
		writeBuffer,
		writeSize
		);

	status = WdfUsbTargetPipeWriteSynchronously(
		pipeHandle,
		NULL,
		NULL,
		&writeBufDesc,
		&bytesWritten
		);
	LOG("WdfUsbTargetPipeWriteSynchronously %x\n", status);

	if (NULL != wdfMemory)
		WdfObjectDelete(wdfMemory);

	return status;

}


#if 1

VOID EvtRequestWriteCompletionRoutine(
	_In_ WDFREQUEST                  Request,
	_In_ WDFIOTARGET                 Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
	_In_ WDFCONTEXT                  Context
	)
	/*++

	Routine Description:

	This is the completion routine for writes
	If the irp completes with success, we check if we
	need to recirculate this irp for another stage of
	transfer.

	Arguments:

	Context - Driver supplied context
	Device - Device handle
	Request - Request handle
	Params - request completion params

	Return Value:
	None

	--*/
{
	NTSTATUS    status;
	size_t      bytesWritten = 0;
	PWDF_USB_REQUEST_COMPLETION_PARAMS usbCompletionParams;

	UNREFERENCED_PARAMETER(Target);
	urb_itm_t * urb = (urb_itm_t *)Context;
	// UNREFERENCED_PARAMETER(Context);

	status = CompletionParams->IoStatus.Status;

	//
	// For usb devices, we should look at the Usb.Completion param.
	//
	usbCompletionParams = CompletionParams->Parameters.Usb.Completion;

	bytesWritten = usbCompletionParams->Parameters.PipeWrite.Length;

	if (NT_SUCCESS(status)) {
		;

	}
	else {
		LOG("Write failed: request Status 0x%x UsbdStatus 0x%x\n",
			status, usbCompletionParams->UsbdStatus);
	}

	if (NULL != urb->wdfMemory) {
		//LOG("urb cpl del wdfMemory\n", urb->wdfMemory);
		WdfObjectDelete(urb->wdfMemory);
		urb->wdfMemory = NULL;
	}





	LOG("pipe:%p cpl urb id:%d\n", urb->pipe, urb->id);
	InterlockedPushEntrySList(urb->urb_list,
		&(urb->node));

	return;
}


NTSTATUS usb_send_msg_async(urb_itm_t * urb, WDFUSBPIPE pipe, WDFREQUEST Request, PUCHAR msg, int tsize)
{
	WDF_MEMORY_DESCRIPTOR  writeBufDesc;
	WDFMEMORY  wdfMemory;
	ULONG  writeSize, bytesWritten;
	size_t  bufferSize;
	NTSTATUS status;
	PVOID  writeBuffer;
	;

	writeSize = tsize;


	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		NonPagedPool,
		0,
		writeSize,
		&wdfMemory,
		NULL
		);
	if (!NT_SUCCESS(status)) {
		LOG("WdfMemoryCreate NG\n");
		return status;
	}

	WdfMemoryCopyFromBuffer(wdfMemory, 0, msg, writeSize);


	status = WdfUsbTargetPipeFormatRequestForWrite(
		pipe,
		Request,
		wdfMemory,
		NULL // Offsets
		);
	if (!NT_SUCCESS(status)) {
		LOG("WdfUsbTargetPipeFormatRequestForWrite NG\n");
		goto Exit;
	}
	//
	// Set a CompletionRoutine callback function.
	//
	urb->pipe = pipe;
	urb->wdfMemory = wdfMemory;

	WdfRequestSetCompletionRoutine(
		Request,
		EvtRequestWriteCompletionRoutine,
		urb
		);
	//
	// Send the request. If an error occurs, complete the request.
	//
	if (WdfRequestSend(
		Request,
		WdfUsbTargetPipeGetIoTarget(pipe),
		WDF_NO_SEND_OPTIONS
		) == FALSE) {
		status = WdfRequestGetStatus(Request);
		LOG("WdfRequestSend NG %x\n", status);
		goto Exit;
	}
Exit:


	return status;

}
#endif

typedef unsigned __int32 uint32_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int8 uint8_t;



// -- Display Packets

#define USBDISP_CMD_BITBLT           2
#define USBDISP_CMD_BITBLT_JPEG       5


#define USBDISP_CMD_FLAG_START            (0x1<<7)
#define USBDISP_CMD_FLAG_END              (0x1<<6)


#ifdef _MSC_VER
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#endif

PACK(
	typedef    struct _usbdisp_disp_packet_header_t {

	uint8_t cmd_flag;

}  usbdisp_disp_packet_header_t;


typedef   struct _usbdisp_disp_bitblt_packet_t {
	usbdisp_disp_packet_header_t header;
	uint8_t  operation;
	uint16_t crc16;
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	uint32_t total_bytes;
	//padding 32bit align
}  usbdisp_disp_bitblt_packet_t;

);




struct bitblt_encoding_context_t {
	size_t  encoded_pos;
	size_t  packet_pos;
	int out_ep_max_size;
	uint8_t     * urbbuffer;

};


#define cpu_to_le16(x)  (uint16_t)(x)


static int _bitblt_encode_command_header(uint8_t * msg, int x, int y, int right, int bottom, uint8_t op_flg)
{
	usbdisp_disp_bitblt_packet_t * bitblt_header;


	// encoding the command header...

	bitblt_header = (usbdisp_disp_bitblt_packet_t *)msg;
	bitblt_header->header.cmd_flag = op_flg;
	bitblt_header->header.cmd_flag |= USBDISP_CMD_FLAG_START;

	bitblt_header->crc16 = 0;
	bitblt_header->x = cpu_to_le16(x);
	bitblt_header->y = cpu_to_le16(y);
	bitblt_header->width = cpu_to_le16(right + 1 - x);
	bitblt_header->height = cpu_to_le16(bottom + 1 - y);
	bitblt_header->operation = 0;


	return sizeof(usbdisp_disp_bitblt_packet_t);

}



static int _bitblt_encode_n_transfer_data(struct bitblt_encoding_context_t * ctx, const void * data, size_t count)
{
	const uint8_t * payload_in_bytes = (const uint8_t *)data;
	int len = 0;

	while (count) {
		// fill the buffer as much as possible...
		size_t buffer_avail_length = ctx->out_ep_max_size - ctx->encoded_pos;
		size_t size_to_copy = count > buffer_avail_length ? buffer_avail_length : count;

		memcpy(ctx->urbbuffer + ctx->encoded_pos, payload_in_bytes, size_to_copy);
		len += size_to_copy;
		payload_in_bytes += size_to_copy;
		ctx->encoded_pos += size_to_copy;


		count -= size_to_copy;

		if (buffer_avail_length == size_to_copy) {
			// current transfer block is full
			ctx->urbbuffer += ctx->out_ep_max_size;

			// encoding the header
			*ctx->urbbuffer = 0;
			((usbdisp_disp_packet_header_t *)ctx->urbbuffer)->cmd_flag = USBDISP_CMD_BITBLT;
			ctx->encoded_pos = sizeof(usbdisp_disp_packet_header_t);
			len += sizeof(usbdisp_disp_packet_header_t);

		}
	}

	return len;
}


int encoder_ctx_init(struct bitblt_encoding_context_t * ctx, uint8_t *  buf, int msg_size,int ep_size)
{

	// init the context...
	ctx->packet_pos = 0;
	ctx->urbbuffer = (uint8_t *)buf;
	ctx->encoded_pos = 0;
	ctx->out_ep_max_size = ep_size;

	return 0;

}


int encode_urb_msg(WDFUSBPIPE pipeHandle, uint8_t * msg_data, uint8_t * urb_data, int msg_size,int ep_size)
{
	struct bitblt_encoding_context_t encoder_ctx;
	int urb_len = 0;

	if (encoder_ctx_init(&encoder_ctx, urb_data, msg_size,ep_size) < 0) return -1;

	urb_len = _bitblt_encode_n_transfer_data(&encoder_ctx, msg_data, msg_size);

	if (urb_len < 0) {
		// abort the operation...

		return -2;
	}

	LOG("usb msg_size:%d urb_len:%d\n", msg_size, urb_len);


	return urb_len;


}

static int _bitblt_encode_command_header_total_bytes(uint8_t * msg, int total_bytes, uint16_t crc)
{
	usbdisp_disp_bitblt_packet_t * bitblt_header;
	bitblt_header = (usbdisp_disp_bitblt_packet_t *)msg;
	bitblt_header->total_bytes = total_bytes;
	bitblt_header->crc16 = crc;
	LOG("crc16:%x\n", crc);
	return sizeof(usbdisp_disp_bitblt_packet_t);
}




#define rgb565(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))


pixel_type_t lcd_color_test_pattern[640 * 480] = { 0 };

#define TEST_COLOR_RED 0xff
#define TEST_COLOR_GREEN 0xff00
#define TEST_COLOR_BLUE 0xff0000

void fill_color(pixel_type_t *buf, int len, pixel_type_t color)
{
	int i = 0;
	for (i = 0; i <= len; i++) {
		buf[i] = color;
	}
	//dump_memory_bytes("-",buf,16);
}


void fill_color_bar(pixel_type_t *buf, int len)
{
#if 0
	fill_color(buf, len / 3, TEST_COLOR_RED);
#else
	fill_color(buf, len / 12, TEST_COLOR_RED);
	fill_color(&buf[len / 12], len / 12, TEST_COLOR_GREEN);
	fill_color(&buf[(len * 2) / 12], len / 12, TEST_COLOR_BLUE);
#endif
}

void fill_color565(uint16_t *buf, int len, uint16_t color)
{
	int i = 0;
	for (i = 0; i <= len; i++) {
		buf[i] = color;
	}
	//dump_memory_bytes("-",buf,16);
}

void fill_color565_bar(uint16_t *buf, int len)
{
#if 0
	fill_color565(buf, len / 3, TEST_COLOR_RED);
#else
	fill_color565(buf, len / 3, rgb565(0xff, 0, 0));
	fill_color565(&buf[len / 3], len / 3, rgb565(0, 0xff, 0));
	fill_color565(&buf[(len * 2) / 3], len / 3, rgb565(0, 0, 0xff));
#endif
}


uint16_t crc16_calc_multi(uint16_t crc_reg, unsigned char *puchMsg, unsigned int usDataLen)
{
	uint32_t i, j, check;
	for (i = 0; i < usDataLen; i++) {
		crc_reg = (crc_reg >> 8) ^ puchMsg[i];
		for (j = 0; j < 8; j++) {
			check = crc_reg & 0x0001;
			crc_reg >>= 1;
			if (check == 0x0001) {
				crc_reg ^= 0xA001;
			}
		}
	}
	return crc_reg;
}


uint16_t crc16_calc(unsigned char *puchMsg, unsigned int usDataLen)
{
	return crc16_calc_multi(0xFFFF, puchMsg, usDataLen);
}


#if 0
int usb_send_image(WDFUSBPIPE pipeHandle, uint8_t * msg, uint8_t * urb_msg, uint32_t * framebuffer, int x, int y, int right, int bottom, int line_width)
{
	int    last_copied_x, last_copied_y;
	int pos = 0;
	uint16_t * pix_msg;
	uint16_t crc;
	uint32_t total_bytes = 0;
	int msg_pos = 0;
	int color_cnt = 0;

#define FB_MAX_WIDTH 159
#define FB_MAX_HEIGHT 119

	if ((right - x) > FB_MAX_WIDTH) {
		LOG("width over %d\n", FB_MAX_WIDTH);
		right = FB_MAX_WIDTH + x;
		//return -1;
	}
	if ((bottom - y) > FB_MAX_HEIGHT) {
		LOG("bottom over %d\n", FB_MAX_HEIGHT);
		bottom = FB_MAX_HEIGHT + y;
		//return -1;
	}
	// estimate how many tickets are needed
	const size_t image_size = (right - x + 1) * (bottom - y + 1) * (FB_DISP_DEFAULT_PIXEL_BITS / 8);

	// do not transmit zero size image
	if (!image_size) return -1;

	LOG("image_size %d\n", image_size);


	msg_pos = _bitblt_encode_command_header(msg, x, y, right, bottom, USBDISP_CMD_BITBLT);
	pix_msg = (uint16_t *)&msg[msg_pos];
	// locate to the begining...
	framebuffer += (y * line_width + x);

	//LOG("fb %p\n",framebuffer);

	for (last_copied_y = y; last_copied_y <= bottom; ++last_copied_y) {

		for (last_copied_x = x; last_copied_x <= right; ++last_copied_x) {

#ifdef PIXEL_32BIT
			pixel_type_t pix = *framebuffer;
			uint8_t r, g, b;
			//LOG("fb %p\n",framebuffer);
			r = pix & 0xff;
			g = (pix >> 8) & 0xff;
			b = (pix >> 16) & 0xff;
			uint16_t current_pixel_le = rgb565(b, g, r);
			current_pixel_le = (current_pixel_le >> 8) | (current_pixel_le << 8);
			*pix_msg = current_pixel_le;
			pix_msg++;
			msg_pos += sizeof(uint16_t);

#else
			pixel_type_t current_pixel_le = cpu_to_le16(*framebuffer);
			*pix_msg = current_pixel_le;
			pix_msg++;
			msg_pos += sizeof(pixel_type_t);

#endif


			++framebuffer;

		}
		framebuffer += line_width - right - 1 + x;
	}

	total_bytes = msg_pos;

	//we only calc playload crc16, or it easy cause confuse
	crc = crc16_calc(&msg[sizeof(usbdisp_disp_bitblt_packet_t)], total_bytes - sizeof(usbdisp_disp_bitblt_packet_t));
	_bitblt_encode_command_header_total_bytes(msg, total_bytes, crc);

	LOG("img:%d %d %d %d crc:%x\n", x, y, right, bottom, crc);
	int urb_len = encode_urb_msg(pipeHandle, msg, urb_msg, total_bytes);
	if (urb_len > 0)
		return usb_send_msg(pipeHandle, NULL, urb_msg, urb_len);
	else
		return -1;


}
#endif



int SwapChainProcessor::usb_send_jpeg_image(urb_itm_t * urb, WDFUSBPIPE pipeHandle, uint8_t * msg, uint8_t * urb_msg, uint32_t * framebuffer, int x, int y, int right, int bottom, int line_width)
{
	int last_copied_x, last_copied_y;
	int ret = 0;
	int pos = 0;
	uint8_t * pix_msg;
	stream_mgr_t m_mgr;
	stream_mgr_t * mgr = &m_mgr;
	uint32_t total_bytes = 0; //ovf bug 
	int msg_pos = 0;


	// estimate how many tickets are needed
	size_t image_size = (right - x + 1) * (bottom - y + 1) * (FB_DISP_DEFAULT_PIXEL_BITS / 8);

	// do not transmit zero size image
	if (!image_size) return -1;


	msg_pos = _bitblt_encode_command_header(msg, x, y, right, bottom, USBDISP_CMD_BITBLT_JPEG);

	pix_msg = &msg[msg_pos];
	// locate to the begining...
	framebuffer += (y * line_width + x);

	for (last_copied_y = y; last_copied_y <= bottom; ++last_copied_y) {

		for (last_copied_x = x; last_copied_x <= right; ++last_copied_x) {

#ifdef PIXEL_32BIT
			pixel_type_t pix = *framebuffer;
			uint8_t r, g, b;
			b = pix & 0xff;
			g = (pix >> 8) & 0xff;
			r = (pix >> 16) & 0xff;

			*pix_msg++ = r;
			*pix_msg++ = g;
			*pix_msg++ = b;
			msg_pos += 3;

#else
			pixel_type_t pix = *framebuffer;
			uint8_t r, g, b;
			b = (pix << 3) & 0xf8;
			g = (pix >> 3) & 0xfc;
			r = (pix >> 8) & 0xf8;

			*pix_msg++ = r;
			*pix_msg++ = g;
			*pix_msg++ = b;
			msg_pos += 3;
#endif


			++framebuffer;

		}
		framebuffer += line_width - right - 1 + x;
	}
	long fps = get_fps();
	int low_quality = -1;;
	int high_quality = -1;

redo: {
	//ok we use jpeg transfer data

	mgr->data = urb_msg;
	mgr->max = JPEG_MAX_SIZE;
	mgr->dp = 0;
	if (!tje_encode_to_ctx(mgr, (right - x + 1), (bottom - y + 1), 3, &msg[sizeof(usbdisp_disp_bitblt_packet_t)], jpg_quality)) {
		LOG("Could not encode JPEG\n");
	}
}
	  LOG("%p jpg: total:%d %d\n", pipeHandle, mgr->dp, jpg_quality);
	  if (fps >= 60) { //we think this may video case
#define ABS(x) ((x) < 0 ? -(x) : (x))


		  if (ABS(mgr->dp - target_quaility_size) < 2 * 1024)
			  goto next;

		  if (high_quality > 0 && low_quality > 0)
			  goto next;

		  if (mgr->dp > target_quaility_size) {
			  high_quality = jpg_quality;
			  if (jpg_quality >= 2) {
				  jpg_quality--;
				  goto next;
			  }

		  }
		  else {
			  low_quality = jpg_quality;
			  if (jpg_quality <= 5) {
				  jpg_quality++;
				  goto next;
			  }
		  }
	  }
	  else {
		  jpg_quality = 5;
	  }
  next:
	  memcpy(&msg[sizeof(usbdisp_disp_bitblt_packet_t)], mgr->data, mgr->dp);

	  total_bytes = sizeof(usbdisp_disp_bitblt_packet_t) + mgr->dp;
	  _bitblt_encode_command_header_total_bytes(msg, total_bytes, crc16_calc(mgr->data, mgr->dp));
	  //encode to urb protocol
	  int urb_len = encode_urb_msg(pipeHandle, msg, urb_msg, total_bytes,urb->max_ep_out_size);
	  if (urb_len > 0) {
		  gfid++;
		  NTSTATUS ret = usb_send_msg_async(urb, pipeHandle, urb->Request, urb_msg, urb_len);
		  put_fps_data(get_system_us());
		  LOG("%p jpg: total:%d fps:%d(x10) %d\n", pipeHandle, total_bytes, fps, jpg_quality);
		  return ret;
	  }
	  else
		  return -1;

}


void scale_for_320x240(uint32_t * dst, uint32_t * src, int line, int len)
{
	int i = 0, j = 0;

	for (i = 0; i < len / 4; i++) {
		*dst++ = *src++;
		src++;
		j += 2;
		if (j >= line) {
			j = 0;
			src += line;
		}
	}
}
#pragma endregion
