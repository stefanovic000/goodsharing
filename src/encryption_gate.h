#pragma once
#include <livekit/room_event_types.h>
#include <QJsonObject>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace goodsharing {
// One immutable share generation per object. SDK callbacks may observe states;
// media owners may query/revoke it concurrently. No SDK calls under this lock.
class EncryptionGate {
public:
    explicit EncryptionGate(std::uint64_t generation);
    bool configure(std::string owner,std::vector<std::string> trackSids);
    bool observe(const std::string& owner,const std::string& sid,livekit::EncryptionState state);
    void close(bool failure=false);
    bool ready()const;
    bool closed()const;
    bool failed()const;
    std::uint64_t generation()const{return generation_;}
    QJsonObject report()const;
private:
    const std::uint64_t generation_;
    mutable std::mutex mutex_;
    std::string owner_;
    std::map<std::string,bool> tracks_;
    bool configured_=false,closed_=false,failed_=false,wasReady_=false;
};
}
