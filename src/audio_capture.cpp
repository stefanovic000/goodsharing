#include "audio_capture.h"
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <chrono>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace goodsharing {
namespace {
QString formatError(HRESULT hr) { return QString("Windows audio error 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')); }
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error(formatError(hr).toStdString()); }
struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};
class Activation final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
public:
    Event completed;
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activated;
        HRESULT activationResult = E_FAIL;
        result = operation->GetActivateResult(&activationResult, &activated);
        if (SUCCEEDED(result)) result = activationResult;
        if (SUCCEEDED(result)) result = activated.As(&client);
        SetEvent(completed.handle);
        return S_OK;
    }
};
ComPtr<IAudioClient> activate(AudioMode mode, std::uint32_t pid) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = mode == AudioMode::Desktop ? GetCurrentProcessId() : pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = mode == AudioMode::Desktop ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT value{};
    value.vt = VT_BLOB;
    value.blob.cbSize = sizeof(params);
    value.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    auto completion = Microsoft::WRL::Make<Activation>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &value, completion.Get(), &operation));
    if (WaitForSingleObject(completion->completed.handle, 5000) != WAIT_OBJECT_0) throw std::runtime_error("Windows audio activation timed out");
    check(completion->result);
    return completion->client;
}
}
AudioCapture::~AudioCapture() { stop(); }
QString AudioCapture::probeProcessLoopback() {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        auto client = activate(AudioMode::Application, GetCurrentProcessId());
        client.Reset();
        if (SUCCEEDED(initialized)) CoUninitialize();
        return "supported";
    } catch (const std::exception& e) {
        if (SUCCEEDED(initialized)) CoUninitialize();
        return QString::fromUtf8(e.what());
    }
}
bool AudioCapture::start(AudioMode mode, std::uint32_t pid, Callback callback, Error error) {
    stop();
    if (mode == AudioMode::Off) return true;
    if (mode == AudioMode::Application && !pid) { error("Choose an application for audio capture."); return false; }
    running_ = true;
    worker_ = std::thread(&AudioCapture::run, this, mode, pid, std::move(callback), std::move(error));
    return true;
}
void AudioCapture::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}
void AudioCapture::run(AudioMode mode, std::uint32_t pid, Callback callback, Error error) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        auto client = activate(mode, pid);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = 192000;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &format, nullptr));
        Event ready;
        check(client->SetEventHandle(ready.handle));
        ComPtr<IAudioCaptureClient> capture;
        check(client->GetService(IID_PPV_ARGS(&capture)));
        check(client->Start());
        try {
            while (running_) {
                if (WaitForSingleObject(ready.handle, 50) != WAIT_OBJECT_0) continue;
                UINT32 packet = 0;
                check(capture->GetNextPacketSize(&packet));
                while (packet && running_) {
                    BYTE* bytes = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    UINT64 devicePosition = 0, qpc = 0;
                    check(capture->GetBuffer(&bytes, &frames, &flags, &devicePosition, &qpc));
                    AudioBlock block;
                    block.samples.resize(static_cast<std::size_t>(frames) * 2);
                    block.timestampUs = static_cast<std::int64_t>(qpc / 10);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && bytes) std::memcpy(block.samples.data(), bytes, block.samples.size() * sizeof(std::int16_t));
                    check(capture->ReleaseBuffer(frames));
                    callback(std::move(block));
                    check(capture->GetNextPacketSize(&packet));
                }
            }
        } catch (...) { client->Stop(); throw; }
        client->Stop();
    } catch (const std::exception& e) {
        running_ = false;
        error(QString::fromUtf8(e.what()));
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}
}
