#include "room_connection.h"
#include <stdexcept>
namespace goodsharing {
RoomConnection::RoomConnection(livekit::RoomDelegate* delegate){room_.setDelegate(delegate);}
RoomConnection::~RoomConnection(){stop();}
void RoomConnection::start(std::string url,std::string token,livekit::RoomOptions options) {
    if(state_!=State::Idle) throw std::logic_error("Create a fresh room connection for a new join");
    state_=State::Connecting;
    connector_=std::jthread([this,url=std::move(url),token=std::move(token),options=std::move(options)](std::stop_token stop) {
        try {
            const bool connected=room_.connect(url,token,options);
            if(stop.stop_requested()) {room_.setDelegate(nullptr);room_.disconnect();state_=State::Stopped;return;}
            state_=connected?State::Connected:State::Failed;
        } catch(const std::exception& e) { {std::lock_guard lock(mutex_);error_=e.what();} state_=State::Failed; }
    });
}
void RoomConnection::stop() noexcept {
    if(connector_.joinable()) {connector_.request_stop();connector_.join();}
    room_.setDelegate(nullptr);
    try {
        const bool connected=room_.connectionState()!=livekit::ConnectionState::Disconnected;
        if(!room_.disconnect()&&connected) {std::lock_guard lock(mutex_);error_="SDK reported an incomplete room disconnect";}
    } catch(const std::exception& e) {std::lock_guard lock(mutex_);error_=e.what();}
    state_=State::Stopped;
}
std::string RoomConnection::error() const {std::lock_guard lock(mutex_);return error_;}
}
