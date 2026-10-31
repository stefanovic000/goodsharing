#include "wasapi_playout_sink.h"
#include "audio_endpoint_clock.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace goodsharing {
namespace {
std::int64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error(QString("WASAPI output error 0x%1").arg(static_cast<quint32>(hr),8,16,QChar('0')).toStdString());}
struct Event {HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}};
std::uint64_t qpc100ns(){LARGE_INTEGER q{},f{};if(!QueryPerformanceCounter(&q)||!QueryPerformanceFrequency(&f)||q.QuadPart<=0||f.QuadPart<=0)throw std::runtime_error("Missing QPC clock");
    return static_cast<std::uint64_t>(static_cast<long double>(q.QuadPart)*10000000/f.QuadPart);}
}
struct WasapiPlayoutSink::Impl {
    const DWORD owner=GetCurrentThreadId();
    ComPtr<IAudioClient> client;ComPtr<IAudioRenderClient> render;ComPtr<IAudioClock> clock;
    std::vector<std::int16_t> lastBlock;unsigned underrunHolds=0;
    Event event;BYTE* reserved=nullptr;UINT32 reservedFrames=0,bufferFrames=0,padding=0,highPadding=0;
    std::uint64_t frequency=0,written=0,lastPosition=0,lastQpc=0,commits=0,resets=0,cancels=0,starts=0,clockReads=0,mediaFrames=0,backpressure=0,resizes=0,discontinuities=0;
    std::uint64_t prepareCalls=0,commitCalls=0,noSpace=0,invalidClockReads=0,noDeadline=0,inconsistentDeadline=0,tooSoon=0,reservations=0;
    double submittedSignalAbs=0,submittedDifferenceAbs=0;
    std::int64_t reserveAt=0,lastDeadline=0,committedEnd=0,latencyUs=0,maxBridgeUs=0,maxCommitUs=0,maxLeadUs=0;
    std::uint64_t contiguousCorrections=0,inconsistentDeadlines=0;std::int64_t maxCorrectionUs=0;
    std::int64_t lastObservedLeadUs=0,lastClockAgeUs=0;
    std::int64_t lastPrepareUs=0,lastCommitUs=0,maximumPrepareGapUs=0,maximumCommitGapUs=0;
    bool running=false,failed=false;QString error;
    QJsonObject firstDiscontinuity;
    void thread()const{if(GetCurrentThreadId()!=owner)throw std::logic_error("WASAPI endpoint used from another thread");}
    void poison(const QString& reason){failed=true;error=reason;}
};
WasapiPlayoutSink::WasapiPlayoutSink():impl_(std::make_unique<Impl>()){
    auto& p=*impl_;if(!p.event.value)throw std::runtime_error("Output event creation failed");
    ComPtr<IMMDeviceEnumerator> devices;check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices)));
    ComPtr<IMMDevice> device;check(devices->GetDefaultAudioEndpoint(eRender,eMultimedia,&device));
    check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&p.client));
    WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=48000;
    format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=192000;
    check(p.client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,200000,0,&format,nullptr));
    check(p.client->SetEventHandle(p.event.value));check(p.client->GetBufferSize(&p.bufferFrames));
    if(p.bufferFrames<480||p.bufferFrames>1440)throw std::runtime_error("Output buffer outside supported 10-30 ms bound");
    REFERENCE_TIME latency=0;check(p.client->GetStreamLatency(&latency));p.latencyUs=latency/10;
    check(p.client->GetService(IID_PPV_ARGS(&p.render)));check(p.client->GetService(IID_PPV_ARGS(&p.clock)));
    check(p.clock->GetFrequency(&p.frequency));if(!p.frequency)throw std::runtime_error("Missing device clock frequency");
}
WasapiPlayoutSink::~WasapiPlayoutSink(){reset();}
std::optional<AudioPlayoutSink::Window> WasapiPlayoutSink::prepare(std::int64_t){
    auto& p=*impl_;p.thread();if(p.failed)throw std::runtime_error(p.error.toStdString());
    try{
    const auto callNow=nowUs();if(p.lastPrepareUs)p.maximumPrepareGapUs=std::max(p.maximumPrepareGapUs,callNow-p.lastPrepareUs);
    p.lastPrepareUs=callNow;p.prepareCalls++;
    if(p.reserved)throw std::logic_error("Outstanding endpoint reservation");
    if(!p.running){
        // No content is copied here, including if watch permission changes.
        BYTE* bytes=nullptr;check(p.render->GetBuffer(p.bufferFrames,&bytes));
        check(p.render->ReleaseBuffer(p.bufferFrames,AUDCLNT_BUFFERFLAGS_SILENT));p.written=p.bufferFrames;
        check(p.client->Start());p.running=true;p.starts++;return {};
    }
    check(p.client->GetCurrentPadding(&p.padding));p.highPadding=std::max(p.highPadding,p.padding);
    if(p.padding>p.bufferFrames)throw std::runtime_error("Invalid endpoint padding");
    const auto frames=std::min<UINT32>(480,p.bufferFrames-p.padding);if(!frames){p.noSpace++;return {};}
    UINT64 position=0,qpc=0;const auto before=nowUs();
    const HRESULT result=p.clock->GetPosition(&position,&qpc);check(result);
    const auto bridgeBefore=nowUs();const auto qpcNow=qpc100ns();const auto after=nowUs();p.maxBridgeUs=std::max(p.maxBridgeUs,after-before);
    if(result!=S_OK||after-before>2000||!position){p.invalidClockReads++;return {};}
    if(position<p.lastPosition||qpc<p.lastQpc)throw std::runtime_error("Device clock moved backward");
    p.lastPosition=position;p.lastQpc=qpc;p.clockReads++;
    const EndpointClockReading reading{position,p.frequency,qpc,qpcNow,p.written,(bridgeBefore+after)/2};
    const auto lead=endpointLeadUs(reading);
    p.lastClockAgeUs=qpcNow>=qpc?static_cast<std::int64_t>((qpcNow-qpc)/10):-1;
    p.lastObservedLeadUs=lead.value_or(INT64_MAX);
    if(lead&&*lead<0&&*lead>=-30000){
        if(p.firstDiscontinuity.isEmpty())p.firstDiscontinuity={{"local_us",static_cast<double>(after)},
            {"lead_us",static_cast<double>(*lead)},{"position",static_cast<double>(position)},{"frequency",static_cast<double>(p.frequency)},
            {"submitted_frames",static_cast<double>(p.written)},{"padding_frames",static_cast<int>(p.padding)},
            {"clock_age_us",static_cast<double>(p.lastClockAgeUs)},{"committed_end_us",static_cast<double>(p.committedEnd)}};
        throw EndpointDiscontinuity(-*lead);
    }
    // Leave time for a complete 10 ms captured block to arrive before its
    // 30 ms media deadline. Filling every available device frame races PCM
    // ingress and turns otherwise timely content into repeated silence.
    if(lead&&*lead>20000){p.backpressure++;return {};}
    const auto observed=endpointDeadline(reading);
    if(!observed){p.noDeadline++;throw std::runtime_error("Device clock/write position lost its bounded deadline");}
    const auto deadline=contiguousEndpointDeadline(*observed,p.committedEnd,after);
    if(!deadline){p.inconsistentDeadlines++;p.inconsistentDeadline++;return {};}
    if(*deadline>*observed){p.contiguousCorrections++;p.maxCorrectionUs=std::max(p.maxCorrectionUs,*deadline-*observed);}
    p.lastDeadline=*deadline;
    p.maxLeadUs=std::max(p.maxLeadUs,*deadline-after);
    // At an exhausted write position WASAPI can keep returning the same
    // positive-but-sub-millisecond lead. Waiting cannot advance that state:
    // the device repeats silence, the bounded PCM queue overflows and new
    // clocks are never accepted. Route this through the existing short-gap
    // recovery path so the endpoint is flushed and refilled explicitly.
    if(*deadline<after+1000){
        p.tooSoon++;
        if(p.firstDiscontinuity.isEmpty())p.firstDiscontinuity={{"reason","deadline_too_soon"},
            {"local_us",static_cast<double>(after)},{"deadline_us",static_cast<double>(*deadline)},
            {"lead_us",static_cast<double>(p.lastObservedLeadUs)},{"position",static_cast<double>(position)},
            {"frequency",static_cast<double>(p.frequency)},{"submitted_frames",static_cast<double>(p.written)},
            {"padding_frames",static_cast<int>(p.padding)},{"clock_age_us",static_cast<double>(p.lastClockAgeUs)},
            {"committed_end_us",static_cast<double>(p.committedEnd)}};
        throw EndpointDiscontinuity(1);
    }
    check(p.render->GetBuffer(frames,&p.reserved));p.reservedFrames=frames;p.reserveAt=nowUs();
    p.reservations++;
    return Window{*deadline,frames};
    }catch(const EndpointDiscontinuity&){p.discontinuities++;throw;}
    catch(const std::exception& e){p.poison(QString::fromUtf8(e.what()));throw;}
}
bool WasapiPlayoutSink::commit(std::span<const std::int16_t> pcm){
    auto& p=*impl_;p.thread();if(p.failed||!p.reserved||pcm.empty()||pcm.size()%2||pcm.size()>p.reservedFrames*2)return false;
    p.commitCalls++;
    const auto begin=nowUs();
    if(begin-p.reserveAt>5000||begin>p.lastDeadline){cancel();p.poison("Expired output reservation");return false;}
    if(pcm.size()!=p.reservedFrames*2){
        // Shared WASAPI permits zero-frame cancellation, not a partial release.
        // Reacquire the smaller packet inside the final permission lease.
        auto result=p.render->ReleaseBuffer(0,0);p.reserved=nullptr;p.reservedFrames=0;
        if(FAILED(result)){p.poison("Output resize cancellation failed");return false;}
        const auto frames=static_cast<UINT32>(pcm.size()/2);
        result=p.render->GetBuffer(frames,&p.reserved);
        if(FAILED(result)){p.poison("Output resize reservation failed");return false;}
        p.reservedFrames=frames;p.resizes++;
        if(nowUs()-p.reserveAt>5000||nowUs()>p.lastDeadline){cancel();p.poison("Expired resized output reservation");return false;}
    }
    return writeReserved(pcm);
}
bool WasapiPlayoutSink::writeReserved(std::span<const std::int16_t> pcm){
    auto& p=*impl_;
    const auto begin=nowUs();
    if(!p.reserved||pcm.size()!=p.reservedFrames*2)return false;
    for(std::size_t i=0;i+1<pcm.size();i+=2){
        const auto left=static_cast<double>(pcm[i]),right=static_cast<double>(pcm[i+1]);
        p.submittedSignalAbs+=(std::abs(left)+std::abs(right))/2;
        p.submittedDifferenceAbs+=std::abs(left-right);
    }
    std::memcpy(p.reserved,pcm.data(),pcm.size_bytes());
    const auto frames=p.reservedFrames;const auto result=p.render->ReleaseBuffer(frames,0);
    p.reserved=nullptr;p.reservedFrames=0;
    if(FAILED(result)){p.poison("Device commit failed");return false;}
    p.committedEnd=p.lastDeadline+static_cast<std::int64_t>(frames)*1000000/48000;
    const auto committedAt=nowUs();if(p.lastCommitUs)p.maximumCommitGapUs=std::max(p.maximumCommitGapUs,committedAt-p.lastCommitUs);
    p.lastCommitUs=committedAt;p.written+=frames;p.commits++;p.mediaFrames+=frames;p.maxCommitUs=std::max(p.maxCommitUs,committedAt-begin);
    p.lastBlock.assign(pcm.begin(),pcm.end());
    return true;
}
bool WasapiPlayoutSink::repeatLast(){
    auto& p=*impl_;
    if(p.failed||!p.reserved||p.lastBlock.empty()||p.reservedFrames*2!=p.lastBlock.size())return false;
    if(nowUs()-p.reserveAt>5000||nowUs()>p.lastDeadline)return false;
    p.underrunHolds++;
    return writeReserved(p.lastBlock);
}
void WasapiPlayoutSink::cancel()noexcept{
    auto& p=*impl_;if(GetCurrentThreadId()!=p.owner){p.poison("Wrong endpoint cancellation thread");return;}
    if(p.reserved){const auto result=p.render->ReleaseBuffer(0,0);p.reserved=nullptr;p.reservedFrames=0;p.cancels++;
        if(FAILED(result))p.poison("Device cancellation failed");}
}
bool WasapiPlayoutSink::reset()noexcept{
    auto& p=*impl_;if(GetCurrentThreadId()!=p.owner){p.poison("Wrong endpoint reset thread");return false;}
    cancel();if(!p.client)return false;
    const auto stopped=p.client->Stop();const auto reset=p.client->Reset();
    UINT32 padding=1;const auto queried=p.client->GetCurrentPadding(&padding);
    p.running=false;p.padding=padding;p.written=p.lastPosition=p.lastQpc=0;p.committedEnd=0;
    if(FAILED(stopped)||FAILED(reset)||FAILED(queried)||padding){p.poison("Device Stop/Reset did not flush output");return false;}
    p.resets++;return !p.failed;
}
void WasapiPlayoutSink::wait(unsigned milliseconds){auto& p=*impl_;p.thread();
    if(milliseconds>20)throw std::invalid_argument("Endpoint wait exceeds control polling bound");
    const auto result=WaitForSingleObject(p.event.value,milliseconds);if(result==WAIT_FAILED)throw std::runtime_error("Endpoint event wait failed");}
QJsonObject WasapiPlayoutSink::report()const{
    const auto& p=*impl_;p.thread();return {{"wasapi_connected",true},{"running",p.running},{"failed",p.failed},{"error",p.error},
        {"sample_rate",48000},{"channels",2},{"buffer_frames",static_cast<int>(p.bufferFrames)},{"buffer_ms",p.bufferFrames/48.0},
        {"reported_stream_latency_ms",p.latencyUs/1000.0},{"padding_frames",static_cast<int>(p.padding)},
        {"padding_high_water_frames",static_cast<int>(p.highPadding)},{"clock_frequency",static_cast<double>(p.frequency)},
        {"clock_readings",static_cast<double>(p.clockReads)},{"commits",static_cast<double>(p.commits)},{"underrun_holds",static_cast<double>(p.underrunHolds)},
        {"prepare_calls",static_cast<double>(p.prepareCalls)},{"commit_calls",static_cast<double>(p.commitCalls)},
        {"prepare_no_space",static_cast<double>(p.noSpace)},{"prepare_invalid_clock",static_cast<double>(p.invalidClockReads)},
        {"prepare_no_deadline",static_cast<double>(p.noDeadline)},{"prepare_inconsistent_deadline",static_cast<double>(p.inconsistentDeadline)},
        {"prepare_too_soon",static_cast<double>(p.tooSoon)},{"prepare_reserved",static_cast<double>(p.reservations)},
        {"maximum_prepare_gap_us",static_cast<double>(p.maximumPrepareGapUs)},{"maximum_commit_gap_us",static_cast<double>(p.maximumCommitGapUs)},
        {"clock_backpressure_waits",static_cast<double>(p.backpressure)},
        {"shortened_packets",static_cast<double>(p.resizes)},
        {"submitted_channel_difference_ratio",p.submittedSignalAbs>0?p.submittedDifferenceAbs/p.submittedSignalAbs:0},
        {"short_discontinuities",static_cast<double>(p.discontinuities)},
        {"first_discontinuity",p.firstDiscontinuity},
        {"contiguous_deadline_corrections",static_cast<double>(p.contiguousCorrections)},
        {"maximum_contiguous_correction_us",static_cast<double>(p.maxCorrectionUs)},
        {"inconsistent_deadline_waits",static_cast<double>(p.inconsistentDeadlines)},
        {"contiguous_correction_limit_us",5000},
        {"target_maximum_first_sample_lead_ms",20},
        {"submitted_frames_including_silence",static_cast<double>(p.mediaFrames)},
        {"resets",static_cast<double>(p.resets)},{"cancels",static_cast<double>(p.cancels)},{"silent_starts",static_cast<double>(p.starts)},
        {"maximum_clock_bridge_us",static_cast<double>(p.maxBridgeUs)},{"maximum_commit_us",static_cast<double>(p.maxCommitUs)},
        {"last_clock_age_us",static_cast<double>(p.lastClockAgeUs)},{"last_observed_lead_us",static_cast<double>(p.lastObservedLeadUs)},
        {"maximum_estimated_first_sample_lead_ms",p.maxLeadUs/1000.0},{"reserved_frames",static_cast<int>(p.reservedFrames)},
        {"physical_playback_measured",false}};
}
}
