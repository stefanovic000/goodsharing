#include "mf_marker_decoder.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <QJsonArray>
#include <cstring>
#include <map>
#include <limits>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr,const char* step) {
    if(FAILED(hr)) throw std::runtime_error(QString("%1 failed: 0x%2").arg(step).arg(static_cast<quint32>(hr),8,16,QChar('0')).toStdString());
}
}
struct MfMarkerDecoder::Impl {
    ComPtr<IMFTransform> decoder;
    std::map<std::int64_t,unsigned> pending;
    int width=0,height=0,stride=0,frames=0,matches=0,priming=0;
    QJsonArray errors;
    void format() {
        for(DWORD i=0;;i++) {
            ComPtr<IMFMediaType> type;
            check(decoder->GetOutputAvailableType(0,i,&type),"Enumerate local decoder output");
            GUID subtype{}; check(type->GetGUID(MF_MT_SUBTYPE,&subtype),"Read local decoder subtype");
            if(subtype!=MFVideoFormat_NV12) continue;
            check(decoder->SetOutputType(0,type.Get(),0),"Set local decoder output");
            UINT32 w=0,h=0; check(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h),"Read decoder size");
            width=static_cast<int>(w); height=static_cast<int>(h);
            UINT32 s=0; if(FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&s))) s=w;
            stride=static_cast<int>(s); return;
        }
    }
    void receive() {
        for(;;) {
            MFT_OUTPUT_STREAM_INFO info{}; check(decoder->GetOutputStreamInfo(0,&info),"Read decoder output requirements");
            ComPtr<IMFSample> sample; check(MFCreateSample(&sample),"Create decoded sample");
            ComPtr<IMFMediaBuffer> buffer;
            check(MFCreateAlignedMemoryBuffer(info.cbSize,info.cbAlignment?info.cbAlignment-1:0,&buffer),"Allocate decoded sample");
            check(sample->AddBuffer(buffer.Get()),"Attach decoded buffer");
            MFT_OUTPUT_DATA_BUFFER result{}; result.pSample=sample.Get(); DWORD status=0;
            const HRESULT hr=decoder->ProcessOutput(0,1,&result,&status);
            if(result.pEvents) result.pEvents->Release();
            if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if(hr==MF_E_TRANSFORM_STREAM_CHANGE) { format(); continue; }
            check(hr,"Decode local H.264 frame");
            LONGLONG timestamp=0; check(sample->GetSampleTime(&timestamp),"Read local decoded timestamp");
            auto expected=pending.find(timestamp/10);
            if(expected==pending.end()) throw std::runtime_error("Unmatched local decoder timestamp");
            BYTE* bytes=nullptr; DWORD length=0;
            check(buffer->Lock(&bytes,nullptr,&length),"Read decoded NV12 buffer");
            if(width<640||height<21||stride<width||length<static_cast<DWORD>(stride*21)) { buffer->Unlock(); throw std::runtime_error("Invalid local decoder frame layout"); }
            unsigned actual=0;
            for(unsigned bit=0;bit<16;bit++) if(bytes[20*stride+bit*40+20]>128) actual|=1u<<bit;
            buffer->Unlock();
            if(expected->second==std::numeric_limits<unsigned>::max()) { priming++; pending.erase(expected); continue; }
            frames++;
            if(actual==(expected->second&0xffff)) matches++;
            else if(errors.size()<20) errors.append(QJsonObject{{"expected",static_cast<int>(expected->second&0xffff)},{"decoded",static_cast<int>(actual)},{"timestamp_us",static_cast<double>(timestamp/10)}});
            pending.erase(expected);
        }
    }
};
MfMarkerDecoder::MfMarkerDecoder(int width,int height):impl_(std::make_unique<Impl>()) {
    auto& p=*impl_;
    check(CoCreateInstance(CLSID_CMSH264DecoderMFT,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&p.decoder)),"Create independent H.264 decoder");
    ComPtr<IMFAttributes> attrs; check(p.decoder->GetAttributes(&attrs),"Get local decoder attributes");
    check(attrs->SetUINT32(CODECAPI_AVLowLatencyMode,TRUE),"Set low-latency decode");
    ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input),"Create local H.264 type");
    check(input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Set local major type");
    check(input->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264),"Set local H.264 subtype");
    check(MFSetAttributeSize(input.Get(),MF_MT_FRAME_SIZE,width,height),"Set decoder dimensions");
    check(MFSetAttributeRatio(input.Get(),MF_MT_FRAME_RATE,60,1),"Set decoder frame rate");
    check(input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"Set decoder interlace");
    check(p.decoder->SetInputType(0,input.Get(),0),"Set local decoder input");
    p.format();
    check(p.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0),"Begin local decode");
    check(p.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0),"Start local decode");
}
MfMarkerDecoder::~MfMarkerDecoder()=default;
void MfMarkerDecoder::submit(const goodsharing::EncodedFrame& frame,unsigned marker) {
    auto& p=*impl_; ComPtr<IMFSample> sample; check(MFCreateSample(&sample),"Create local encoded sample");
    ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(static_cast<DWORD>(frame.bytes.size()),&buffer),"Allocate local encoded sample");
    BYTE* bytes=nullptr; check(buffer->Lock(&bytes,nullptr,nullptr),"Lock local encoded sample");
    std::memcpy(bytes,frame.bytes.data(),frame.bytes.size()); buffer->Unlock();
    check(buffer->SetCurrentLength(static_cast<DWORD>(frame.bytes.size())),"Set local encoded length");
    check(sample->AddBuffer(buffer.Get()),"Attach local encoded sample");
    check(sample->SetSampleTime(frame.timestampUs*10),"Set local decode timestamp");
    check(sample->SetSampleDuration(10000000/60),"Set local decode duration");
    check(sample->SetUINT32(MFSampleExtension_CleanPoint,frame.keyframe),"Set local decode keyframe");
    p.pending[frame.timestampUs]=marker;
    const auto hr=p.decoder->ProcessInput(0,sample.Get(),0);
    if(hr==MF_E_NOTACCEPTING) { p.receive(); check(p.decoder->ProcessInput(0,sample.Get(),0),"Retry local decoder input"); }
    else check(hr,"Submit local decoder input");
    p.receive();
}
QJsonObject MfMarkerDecoder::report() {
    auto& p=*impl_; check(p.decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0),"Drain local decoder"); p.receive();
    return QJsonObject{{"frames",p.frames},{"priming_frames",p.priming},{"pixel_markers",p.matches},{"marker_errors",p.errors},{"pending_frames",static_cast<int>(p.pending.size())}};
}
