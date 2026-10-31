#pragma once
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace goodsharing {
struct EncodedFrame {
    std::vector<std::uint8_t> bytes;
    std::int64_t timestampUs=0;
    bool keyframe=false;
};

// Single-threaded encoder. COM/MF must be initialized by its owner.
// Only enumerates hardware MFTs; never substitutes a software encoder.
class HardwareEncoder {
public:
    using Output=std::function<void(EncodedFrame)>;
    HardwareEncoder(int width,int height,int bitrate,Output output,int fps=60);
    ~HardwareEncoder();
    HardwareEncoder(const HardwareEncoder&)=delete;
    HardwareEncoder& operator=(const HardwareEncoder&)=delete;
    bool submit(const std::vector<std::uint8_t>& nv12,std::int64_t timestampUs);
    void poll();
    void requestKeyframe();
    void setBitrate(std::uint64_t bitrate);
    QString name() const;
    int framesEncoded() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
