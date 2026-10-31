#include "audio_playback.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace goodsharing {
namespace {
void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error(QString("Playback Windows error 0x%1").arg(static_cast<quint32>(hr),8,16,QChar('0')).toStdString()); }
struct Event { HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(value) CloseHandle(value);} };
}
struct AudioPlayback::Impl {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::int16_t> queue;
    std::jthread worker;
    QString error;
    bool initialized=false,accepting=false;
    std::uint64_t rendered=0,rejected=0,underrun=0;
    std::size_t highWater=0;
    unsigned deviceFrames=0;
    static constexpr std::size_t capacity=4800*2; // 100 ms, never unbounded.
};
AudioPlayback::AudioPlayback():impl_(std::make_unique<Impl>()) {}
AudioPlayback::~AudioPlayback(){stop();}
void AudioPlayback::start(float volume) {
    stop();
    if(!std::isfinite(volume)||volume<0||volume>1) throw std::invalid_argument("Playback volume must be between zero and one");
    auto& p=*impl_;
    { std::lock_guard lock(p.mutex); p.error.clear(); p.initialized=false; p.rendered=p.rejected=p.underrun=0; p.highWater=0; p.deviceFrames=0; }
    p.worker=std::jthread([&p,volume](std::stop_token stop) {
        const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try {
            check(initialized);
            ComPtr<IMMDeviceEnumerator> devices;
            check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices)));
            ComPtr<IMMDevice> device; check(devices->GetDefaultAudioEndpoint(eRender,eMultimedia,&device));
            ComPtr<IAudioClient> client; check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));
            WAVEFORMATEX format{}; format.wFormatTag=WAVE_FORMAT_PCM; format.nChannels=2; format.nSamplesPerSec=48000; format.wBitsPerSample=16; format.nBlockAlign=4; format.nAvgBytesPerSec=192000;
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,200000,0,&format,nullptr));
            Event event; if(!event.value) throw std::runtime_error("Playback event creation failed");
            check(client->SetEventHandle(event.value));
            UINT32 bufferFrames=0; check(client->GetBufferSize(&bufferFrames));
            ComPtr<IAudioRenderClient> render; check(client->GetService(IID_PPV_ARGS(&render)));
            check(client->Start());
            { std::lock_guard lock(p.mutex); p.deviceFrames=bufferFrames; p.initialized=true; p.accepting=true; } p.ready.notify_all();
            bool supplied=false;
            try {
                while(!stop.stop_requested()) {
                    if(WaitForSingleObject(event.value,50)!=WAIT_OBJECT_0) continue;
                    UINT32 padding=0; check(client->GetCurrentPadding(&padding));
                    const UINT32 available=bufferFrames-padding; if(!available) continue;
                    BYTE* bytes=nullptr; check(render->GetBuffer(available,&bytes));
                    auto* output=reinterpret_cast<std::int16_t*>(bytes);
                    {
                        std::lock_guard lock(p.mutex);
                        const auto count=std::min(p.queue.size(),static_cast<std::size_t>(available)*2);
                        for(std::size_t i=0;i<count;i++) { output[i]=static_cast<std::int16_t>(p.queue.front()*volume); p.queue.pop_front(); }
                        std::fill(output+count,output+static_cast<std::size_t>(available)*2,0);
                        p.rendered+=count/2;
                        if(supplied&&count<static_cast<std::size_t>(available)*2) p.underrun+=available-count/2;
                        supplied=supplied||count>0;
                    }
                    check(render->ReleaseBuffer(available,0));
                }
            } catch(...) { client->Stop(); throw; }
            check(client->Stop());
        } catch(const std::exception& e) { std::lock_guard lock(p.mutex); p.error=QString::fromUtf8(e.what()); }
        { std::lock_guard lock(p.mutex); p.initialized=true; p.accepting=false; } p.ready.notify_all();
        if(SUCCEEDED(initialized)) CoUninitialize();
    });
    std::unique_lock lock(p.mutex);
    if(!p.ready.wait_for(lock,std::chrono::seconds(5),[&]{return p.initialized;})) { lock.unlock(); stop(); throw std::runtime_error("Playback initialization timed out"); }
    if(!p.error.isEmpty()) throw std::runtime_error(p.error.toStdString());
}
bool AudioPlayback::submit(std::span<const std::int16_t> samples) {
    auto& p=*impl_; std::lock_guard lock(p.mutex);
    if(samples.size()%2) throw std::invalid_argument("Stereo playback requires complete interleaved frames");
    if(!p.accepting||!p.error.isEmpty()||p.queue.size()+samples.size()>Impl::capacity) { p.rejected+=samples.size()/2; return false; }
    p.queue.insert(p.queue.end(),samples.begin(),samples.end()); p.highWater=std::max(p.highWater,p.queue.size()); return true;
}
void AudioPlayback::stop() {
    auto& p=*impl_;
    { std::lock_guard lock(p.mutex); p.accepting=false; }
    if(p.worker.joinable()) { p.worker.request_stop(); p.worker.join(); }
    std::lock_guard lock(p.mutex); p.queue.clear();
}
QJsonObject AudioPlayback::report() const {
    const auto& p=*impl_; std::lock_guard lock(p.mutex);
    return {{"sample_rate",48000},{"channels",2},{"rendered_frames",static_cast<double>(p.rendered)},{"rejected_frames",static_cast<double>(p.rejected)},{"underrun_frames",static_cast<double>(p.underrun)},{"queue_high_water_ms",p.highWater/96.0},{"device_buffer_ms",p.deviceFrames/48.0},{"error",p.error}};
}
}
