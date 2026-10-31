#pragma once
#include <atomic>
#include "api/crypto/frame_crypto_transformer.h"

namespace goodsharing {
// Dedicated app policy: encrypted tracks never accept server plaintext frames.
// Set the retained options after construction: the pinned upstream options copy
// constructor omits discard_frame_when_cryptor_not_ready.
class StrictKeyProvider : public webrtc::DefaultKeyProviderImpl {
public:
    explicit StrictKeyProvider(webrtc::KeyProviderOptions options)
        : webrtc::DefaultKeyProviderImpl(options) {
        this->options().discard_frame_when_cryptor_not_ready = true;
        this->options().uncrypted_magic_bytes.clear();
    }
    void SetSifTrailer(const std::vector<uint8_t>) override {}
};

class StrictFrameCryptor : public webrtc::FrameCryptorTransformer {
public:
    StrictFrameCryptor(webrtc::Thread* signaling, const std::string participant,
                      MediaType type, Algorithm algorithm,
                      webrtc::scoped_refptr<webrtc::KeyProvider> provider)
        : webrtc::FrameCryptorTransformer(signaling, participant, type, algorithm, provider) {
        webrtc::FrameCryptorTransformer::SetEnabled(true);
    }
    // Disabling is sticky revocation, never the upstream plaintext passthrough.
    // Already queued cryptor work still runs with encryption enabled.
    void SetEnabled(bool enabled) override {
        if (!enabled) revoked_.store(true, std::memory_order_release);
    }
    bool enabled() const override { return !revoked_.load(std::memory_order_acquire); }
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        if (!revoked_.load(std::memory_order_acquire))
            webrtc::FrameCryptorTransformer::Transform(std::move(frame));
    }
private:
    std::atomic<bool> revoked_{false};
};
}
