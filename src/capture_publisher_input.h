#pragma once
#include "screen_capture.h"
#include "audio_capture.h"
#include "share_publisher.h"
#include <QImage>
#include <condition_variable>
#include <optional>

namespace goodsharing {
// Correctness-first SDR CPU conversion; hardware encoding is downstream.
// Aspect ratio is preserved in an even-aligned black canvas.
std::vector<std::uint8_t> captureCanvasNv12(const QImage& image,int width,int height);

// One captured frame converted once into tightly packed I420 planes, shared by
// both canvas conversions instead of being re-derived per quality.
struct PlaneImageI420 {
    int width=0,height=0;
    std::vector<std::uint8_t> planes[3];
    int stride(int plane) const{return plane?(width+1)/2:width;}
    int planeHeight(int plane) const{return plane?(height+1)/2:height;}
};
void capturePlanesI420(const QImage& image,PlaneImageI420& output);
std::vector<std::uint8_t> captureCanvasNv12(const PlaneImageI420& source,int width,int height);

// One capture session per encrypted publication generation. start/stop belong
// to the app owner; callbacks/conversion never run on the GUI/transport owner.
class CapturePublisherInput {
public:
    struct Config {std::uintptr_t handle=0;bool monitor=false;AudioMode audio=AudioMode::Off;std::uint32_t audioPid=0;int maximumFps=60;};
    using Preview=std::function<void(ScreenFrame)>;
    using Level=std::function<void(double)>;
    // Window sources only: minimized/restored transitions of the captured window.
    using SourceState=std::function<void(bool minimized)>;
    ~CapturePublisherInput();
    void start(Config config,std::shared_ptr<ShareInput> input,Preview preview={},Level level={},SourceState state={});
    void stop();
    QString error()const;
    QJsonObject report()const;
private:
    ScreenCapture screen_;
    AudioCapture audio_;
    std::shared_ptr<ShareInput> input_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::optional<ScreenFrame> latest_;
    std::jthread converter_;
    std::atomic_bool open_=false;
    QString error_;
    Level level_;
    std::int64_t clockOffsetUs_=0;
    PlaneImageI420 planes_;
    std::uint64_t captured_=0,replaced_=0,stale_=0,videoAccepted_=0,audioAccepted_=0,conversionUs_=0,maxConversionUs_=0;
    std::uint64_t sourceConversionUs_=0,canvas1080Us_=0,canvas720Us_=0,convertedFrames_=0;
    std::uint32_t sourceWidth_=0,sourceHeight_=0;
    bool sourceMinimized_=false;
    // Audio capture is a single producer; only inspect after stop/join.
    std::vector<std::int16_t> audioPending_;
    std::uint64_t audioPendingUs_=0,lastAudioEndUs_=0,audioDiscontinuities_=0;
    double audioSignalAbs_=0,audioDifferenceAbs_=0;
    void fail(QString message);
    void convert(std::stop_token stop);
    void receiveAudio(AudioBlock block);
};
}

