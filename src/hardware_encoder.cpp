#include "hardware_encoder.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <wrl/client.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace goodsharing {
namespace {
void check(HRESULT hr,const char* step) {
    if(FAILED(hr)) throw std::runtime_error(QString("%1 failed: 0x%2").arg(step).arg(static_cast<quint32>(hr),8,16,QChar('0')).toStdString());
}
ComPtr<IMFMediaType> videoType(GUID subtype,int width,int height,int fps) {
    ComPtr<IMFMediaType> t; check(MFCreateMediaType(&t),"Create media type");
    check(t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Set major type");
    check(t->SetGUID(MF_MT_SUBTYPE,subtype),"Set subtype");
    check(MFSetAttributeSize(t.Get(),MF_MT_FRAME_SIZE,width,height),"Set size");
    check(MFSetAttributeRatio(t.Get(),MF_MT_FRAME_RATE,static_cast<UINT32>(fps),1),"Set rate");
    check(MFSetAttributeRatio(t.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1),"Set aspect");
    check(t->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"Set progressive");
    return t;
}
}
struct HardwareEncoder::Impl {
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ICodecAPI> codec;
    ComPtr<IMFActivate> activation;
    QString name;
    Output output;
    bool asynchronous=false;
    unsigned inputRequests=0;
    int width=0,height=0,frames=0;
    ~Impl() { if(transform) { transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0); transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0); ComPtr<IMFShutdown> shutdown; if(SUCCEEDED(transform.As(&shutdown))) shutdown->Shutdown(); } if(activation) activation->ShutdownObject(); }
    void set(const GUID& id,ULONG number,bool boolean=false,bool required=false) {
        if(!codec) { if(required) throw std::runtime_error("Encoder exposes no codec controls"); return; }
        VARIANT v{}; v.vt=boolean?VT_BOOL:VT_UI4; if(boolean) v.boolVal=number?VARIANT_TRUE:VARIANT_FALSE; else v.ulVal=number;
        const HRESULT hr=codec->SetValue(&id,&v); if(required) check(hr,"Set encoder control");
    }
    void receive() {
        MFT_OUTPUT_STREAM_INFO info{}; check(transform->GetOutputStreamInfo(0,&info),"Query encoded output");
        MFT_OUTPUT_DATA_BUFFER outputData{};
        ComPtr<IMFSample> allocated;
        if(!(info.dwFlags&MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            check(MFCreateSample(&allocated),"Create output sample");
            ComPtr<IMFMediaBuffer> b; check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize,width*height*2),&b),"Allocate output");
            check(allocated->AddBuffer(b.Get()),"Attach output"); outputData.pSample=allocated.Get();
        }
        DWORD status=0;
        const HRESULT hr=transform->ProcessOutput(0,1,&outputData,&status);
        if(outputData.pEvents) outputData.pEvents->Release();
        ComPtr<IMFSample> sample;
        if(!allocated&&outputData.pSample) sample.Attach(outputData.pSample); else sample=allocated;
        if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        check(hr,"Receive encoded frame");
        if(!sample) throw std::runtime_error("Hardware encoder returned no sample");
        ComPtr<IMFMediaBuffer> b; check(sample->ConvertToContiguousBuffer(&b),"Read encoded sample");
        BYTE* data=nullptr; DWORD length=0;
        check(b->Lock(&data,nullptr,&length),"Lock encoded sample");
        EncodedFrame frame; frame.bytes.assign(data,data+length); b->Unlock();
        LONGLONG timestamp=0; check(sample->GetSampleTime(&timestamp),"Read encoded timestamp"); frame.timestampUs=timestamp/10;
        UINT32 clean=0; sample->GetUINT32(MFSampleExtension_CleanPoint,&clean); frame.keyframe=clean!=0;
        frames++; output(std::move(frame));
    }
};
HardwareEncoder::HardwareEncoder(int width,int height,int bitrate,Output output,int fps):impl_(std::make_unique<Impl>()) {
    auto& p=*impl_; p.width=width; p.height=height; p.output=std::move(output);
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video,MFVideoFormat_NV12},out{MFMediaType_Video,MFVideoFormat_H264};
    IMFActivate** list=nullptr; UINT count=0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER,&input,&out,&list,&count),"Find hardware H.264 encoder");
    if(!count) { CoTaskMemFree(list); throw std::runtime_error("No hardware H.264 encoder supports NV12 input"); }
    p.activation=list[0]; for(UINT i=0;i<count;i++) list[i]->Release(); CoTaskMemFree(list);
    wchar_t* friendly=nullptr; UINT size=0;
    if(SUCCEEDED(p.activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,&friendly,&size))) { p.name=QString::fromWCharArray(friendly); CoTaskMemFree(friendly); }
    check(p.activation->ActivateObject(IID_PPV_ARGS(&p.transform)),"Activate hardware encoder");
    ComPtr<IMFAttributes> attributes; check(p.transform->GetAttributes(&attributes),"Query encoder attributes");
    UINT32 async=0; attributes->GetUINT32(MF_TRANSFORM_ASYNC,&async); p.asynchronous=async!=0;
    if(p.asynchronous) { check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK,TRUE),"Unlock encoder"); check(p.transform.As(&p.events),"Query encoder event queue"); }
    p.transform.As(&p.codec);
    p.set(CODECAPI_AVEncCommonRateControlMode,eAVEncCommonRateControlMode_CBR);
    p.set(CODECAPI_AVEncCommonMeanBitRate,bitrate);
    p.set(CODECAPI_AVEncMPVDefaultBPictureCount,0);
    // One keyframe per second was a visible hitch every second on screen shares.
    // Sparse IDRs plus explicit requests (periodic refresh, loss recovery) keep the
    // picture stable without that rhythm.
    p.set(CODECAPI_AVEncMPVGOPSize,fps*10);
    p.set(CODECAPI_AVLowLatencyMode,1,true);
    auto compressed=videoType(MFVideoFormat_H264,width,height,fps);
    check(compressed->SetUINT32(MF_MT_AVG_BITRATE,bitrate),"Set bitrate");
    check(compressed->SetUINT32(MF_MT_MPEG2_PROFILE,eAVEncH264VProfile_Base),"Set baseline profile");
    check(p.transform->SetOutputType(0,compressed.Get(),0),"Set hardware output format");
    auto raw=videoType(MFVideoFormat_NV12,width,height,fps);
    check(p.transform->SetInputType(0,raw.Get(),0),"Set hardware input format");
    check(p.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0),"Begin encoding");
    check(p.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0),"Start encoding");
}
HardwareEncoder::~HardwareEncoder()=default;
void HardwareEncoder::poll() {
    auto& p=*impl_;
    if(!p.asynchronous) { p.receive(); return; }
    for(;;) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr=p.events->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event);
        if(hr==MF_E_NO_EVENTS_AVAILABLE) return;
        check(hr,"Read encoder event");
        HRESULT eventStatus=S_OK; check(event->GetStatus(&eventStatus),"Read event status"); check(eventStatus,"Hardware encoder event");
        MediaEventType type; check(event->GetType(&type),"Read event type");
        if(type==METransformNeedInput) p.inputRequests++;
        if(type==METransformHaveOutput) p.receive();
    }
}
bool HardwareEncoder::submit(const std::vector<std::uint8_t>& nv12,std::int64_t timestampUs) {
    auto& p=*impl_; poll();
    if(nv12.size()!=static_cast<std::size_t>(p.width)*p.height*3/2) throw std::runtime_error("Invalid NV12 frame size");
    if(p.asynchronous&&!p.inputRequests) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(3);
        while(!p.inputRequests&&std::chrono::steady_clock::now()<deadline) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); poll(); }
        if(!p.inputRequests) return false;
    }
    ComPtr<IMFSample> sample; check(MFCreateSample(&sample),"Create input sample");
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()),&buffer),"Allocate input sample");
    BYTE* target=nullptr; check(buffer->Lock(&target,nullptr,nullptr),"Lock input"); std::memcpy(target,nv12.data(),nv12.size()); buffer->Unlock();
    check(buffer->SetCurrentLength(static_cast<DWORD>(nv12.size())),"Set input length");
    check(sample->AddBuffer(buffer.Get()),"Attach input"); check(sample->SetSampleTime(timestampUs*10),"Set frame timestamp"); check(sample->SetSampleDuration(10000000/60),"Set duration");
    const HRESULT hr=p.transform->ProcessInput(0,sample.Get(),0);
    if(hr==MF_E_NOTACCEPTING) return false;
    check(hr,"Submit hardware frame"); if(p.asynchronous) p.inputRequests--; return true;
}
void HardwareEncoder::requestKeyframe() { impl_->set(CODECAPI_AVEncVideoForceKeyFrame,1,false,true); }
void HardwareEncoder::setBitrate(std::uint64_t bitrate) { impl_->set(CODECAPI_AVEncCommonMeanBitRate,static_cast<ULONG>(std::clamp<std::uint64_t>(bitrate,100000,20000000)),false,true); }
QString HardwareEncoder::name() const { return impl_->name; }
int HardwareEncoder::framesEncoded() const { return impl_->frames; }
}
