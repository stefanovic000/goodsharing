#pragma once
#include <QImage>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace goodsharing {
struct ScreenFrame { QImage image; std::int64_t timestampUs=0; };
class ScreenCapture {
public:
    using Output=std::function<void(ScreenFrame)>;
    using Error=std::function<void(QString)>;
    // Window sources only. Reports minimize/restore transitions so a share can
    // tell viewers to wait instead of presenting a frozen picture, and resumes
    // from the same session when the window is restored.
    using SourceState=std::function<void(bool minimized)>;
    ~ScreenCapture();
    void start(std::uintptr_t nativeHandle,bool monitor,Output output,Error error,unsigned maximumFps=0,SourceState state={});
    void stop();
private:
    void run(std::uintptr_t nativeHandle,bool monitor,Output output,Error error,unsigned maximumFps,SourceState state);
    std::atomic_bool running_{false};
    std::thread worker_;
};
}
