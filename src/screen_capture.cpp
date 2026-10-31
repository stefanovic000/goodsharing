#include "screen_capture.h"
#include "frame_pacer.h"
#include <windows.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <chrono>
#include <memory>
#include <optional>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics;
namespace CaptureDx=winrt::Windows::Graphics::DirectX;
namespace goodsharing {
namespace {
constexpr std::uint32_t maximumCaptureDimension=16384;
constexpr std::uint32_t maximumCaptureRowPitch=131072;
struct WakeState {
    HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::atomic_bool closed{false};
    ~WakeState() { if(ready) CloseHandle(ready); }
};
struct Apartment { Apartment(){winrt::init_apartment(winrt::apartment_type::multi_threaded);} ~Apartment(){winrt::uninit_apartment();} };
}
ScreenCapture::~ScreenCapture() { stop(); }
void ScreenCapture::start(std::uintptr_t handle,bool monitor,Output output,Error error,unsigned maximumFps,SourceState sourceState) {
    stop();if(maximumFps>240)throw std::invalid_argument("Invalid capture frame-rate limit");
    running_=true; worker_=std::thread(&ScreenCapture::run,this,handle,monitor,std::move(output),std::move(error),maximumFps,std::move(sourceState));
}
void ScreenCapture::stop() { running_=false; if(worker_.joinable()) worker_.join(); }
void ScreenCapture::run(std::uintptr_t handle,bool monitor,Output output,Error error,unsigned maximumFps,SourceState sourceState) {
    QString stage="initialize graphics capture";
    QString frameDetail;
    const auto failure=[&stage,&frameDetail](const QString& message) {
        auto prefix=stage;
        if(!frameDetail.isEmpty())prefix+=" ("+frameDetail+")";
        return prefix+": "+message;
    };
    try {
        stage="initialize WinRT apartment";
        Apartment apartment;
        auto pacing=maximumFps?std::make_unique<FramePacer>(maximumFps):nullptr;
        stage="get graphics capture item interop";
        auto interop=winrt::get_activation_factory<Capture::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        Capture::GraphicsCaptureItem item{nullptr};
        stage="query graphics capture support";
        if(!Capture::GraphicsCaptureSession::IsSupported())throw std::runtime_error("Windows Graphics Capture is not supported in this session");
        if(monitor) {
            stage="create capture item for monitor";
            winrt::check_hresult(interop->CreateForMonitor(reinterpret_cast<HMONITOR>(handle),winrt::guid_of<Capture::GraphicsCaptureItem>(),winrt::put_abi(item)));
        } else {
            stage="create capture item for window";
            winrt::check_hresult(interop->CreateForWindow(reinterpret_cast<HWND>(handle),winrt::guid_of<Capture::GraphicsCaptureItem>(),winrt::put_abi(item)));
        }
        auto size=item.Size();
        if(size.Width<1||size.Height<1||size.Width>maximumCaptureDimension||size.Height>maximumCaptureDimension)
            throw std::runtime_error(QString("Invalid capture item size: %1x%2").arg(size.Width).arg(size.Height).toStdString());
        stage="create D3D11 device";
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<IDXGIDevice> dxgi; winrt::check_hresult(device.As(&dxgi));
        winrt::com_ptr<IInspectable> inspectable; winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()));
        auto directDevice=inspectable.as<CaptureDx::Direct3D11::IDirect3DDevice>();
        stage="create capture frame pool";
        auto pool=Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(directDevice,CaptureDx::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
        auto session=pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(true);
        DWORD ownerPid=0;
        if(!monitor) GetWindowThreadProcessId(reinterpret_cast<HWND>(handle),&ownerPid);
        const auto state=std::make_shared<WakeState>();
        if(!state->ready) winrt::throw_last_error();
        const auto arrived=pool.FrameArrived([state](auto&,auto&) { SetEvent(state->ready); });
        const auto itemClosed=item.Closed([state](auto&,auto&) { state->closed=true; SetEvent(state->ready); });
        ComPtr<ID3D11Texture2D> staging;
        std::optional<bool> reportedState;
        try {
            stage="start capture session";
            session.StartCapture();
            while(running_&&!state->closed) {
                stage="wait for captured frame";
                if(!monitor) {
                    DWORD currentPid=0;
                    GetWindowThreadProcessId(reinterpret_cast<HWND>(handle),&currentPid);
                    if(!IsWindow(reinterpret_cast<HWND>(handle))||currentPid!=ownerPid) { state->closed=true; break; }
                }
                if(!monitor) {
                    // A minimized source stops producing frames; report the
                    // transition so viewers can wait instead of freezing.
                    const bool minimized=IsIconic(reinterpret_cast<HWND>(handle))!=FALSE;
                    if(sourceState&&(!reportedState.has_value()||*reportedState!=minimized)) {
                        reportedState=minimized;
                        try{sourceState(minimized);}catch(...){}
                    }
                }
                if(WaitForSingleObject(state->ready,50)!=WAIT_OBJECT_0) continue;
                // Rate-limit the owned readback worker before pulling/copying
                // textures. FrameArrived itself still only signals the event.
                if(pacing)pacing->wait();
                if(!running_)break;
                stage="acquire captured frame";
                auto frame=pool.TryGetNextFrame(); if(!frame) continue;
                // Prefer the newest frame instead of accumulating latency.
                for(;;) { auto next=pool.TryGetNextFrame(); if(!next) break; frame=std::move(next); }
                stage="read captured frame geometry";
                const auto content=frame.ContentSize();
                frameDetail=QString("frame=%1x%2 item=%3x%4").arg(content.Width).arg(content.Height).arg(size.Width).arg(size.Height);
                if(content.Width<1||content.Height<1) continue;
                if(content.Width>maximumCaptureDimension||content.Height>maximumCaptureDimension)
                    throw std::runtime_error("Captured frame dimensions exceed the supported bound");
                if(content.Width!=size.Width||content.Height!=size.Height) {
                    stage="recreate capture frame pool";
                    frame.Close(); size=content; staging.Reset();
                    pool.Recreate(directDevice,CaptureDx::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size); continue;
                }
                stage="access captured D3D texture";
                auto access=frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> texture; winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
                if(!staging) {
                    stage="create capture staging texture";
                    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
                    winrt::check_hresult(device->CreateTexture2D(&desc,nullptr,&staging));
                }
                stage="copy captured texture to staging";
                context->CopyResource(staging.Get(),texture.Get());
                stage="map captured staging texture";
                D3D11_MAPPED_SUBRESOURCE mapped{}; winrt::check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                frameDetail=QString("frame=%1x%2 item=%3x%4 row_pitch=%5")
                    .arg(content.Width).arg(content.Height).arg(size.Width).arg(size.Height).arg(mapped.RowPitch);
                const auto minimumRowPitch=static_cast<std::uint64_t>(content.Width)*4;
                if(mapped.RowPitch<minimumRowPitch||mapped.RowPitch>maximumCaptureRowPitch)
                    throw std::runtime_error("Captured frame row pitch is outside the supported bound");
                ScreenFrame captured;
                captured.timestampUs=frame.SystemRelativeTime().count()/10;
                stage="copy captured pixels";
                // DWM/Windows Graphics Capture surfaces are premultiplied BGRA;
                // labelling them that way keeps the conversion from applying the
                // alpha compositing twice.
                captured.image=QImage(static_cast<const uchar*>(mapped.pData),content.Width,content.Height,static_cast<qsizetype>(mapped.RowPitch),QImage::Format_ARGB32_Premultiplied).copy();
                stage="release captured staging texture";
                context->Unmap(staging.Get(),0); frame.Close();
                stage="deliver captured frame";
                output(std::move(captured));
            }
            stage="close capture session";
            session.Close(); pool.FrameArrived(arrived); item.Closed(itemClosed); pool.Close();
            if(state->closed&&running_) error("The selected capture source was closed.");
        } catch(...) {
            session.Close(); pool.FrameArrived(arrived); item.Closed(itemClosed); pool.Close(); throw;
        }
    } catch(const winrt::hresult_error& e) {
        error(failure(QString::fromWCharArray(e.message().c_str())));
    } catch(const std::exception& e) {
        error(failure(QString::fromUtf8(e.what())));
    }
    running_=false;
}
}

