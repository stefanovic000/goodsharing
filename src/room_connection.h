#pragma once
#include <livekit/room.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace goodsharing {
// Owner-thread start/stop. Delegate storage must outlive this object. Cancellation
// joins the connector before disconnecting; never call disconnect concurrently
// with the SDK's blocking connect, which can otherwise publish state afterwards.
class RoomConnection {
public:
    enum class State { Idle,Connecting,Connected,Failed,Stopped };
    explicit RoomConnection(livekit::RoomDelegate* delegate=nullptr);
    ~RoomConnection();
    void start(std::string url,std::string token,livekit::RoomOptions options);
    void stop() noexcept;
    State state() const {return state_.load();}
    livekit::Room& room() {return room_;}
    std::string error() const;
private:
    livekit::Room room_;
    std::jthread connector_;
    std::atomic<State> state_{State::Idle};
    mutable std::mutex mutex_;
    std::string error_;
};
}
