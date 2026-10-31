#pragma once
#include "hardware_encoder.h"
#include <QJsonObject>
#include <memory>

// Diagnostic decoder before RTP/E2EE. It checks the same unmodified marker
// pixels independently of LiveKit's decoder and transport.
class MfMarkerDecoder {
public:
    MfMarkerDecoder(int width,int height);
    ~MfMarkerDecoder();
    void submit(const goodsharing::EncodedFrame& frame,unsigned marker);
    QJsonObject report();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
