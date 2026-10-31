#pragma once
#include <memory>
#include "api/frame_transformer_interface.h"

namespace goodsharing {
// Installed before a new remote track reaches asynchronous room setup. No
// callback and no queue: media cannot reach a decoder through this placeholder.
class ClosedFrameTransformer : public webrtc::FrameTransformerInterface {
public:
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface>) override {}
};
}
