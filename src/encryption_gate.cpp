#include "encryption_gate.h"
#include <QJsonArray>
#include <algorithm>
#include <stdexcept>
namespace goodsharing {
EncryptionGate::EncryptionGate(std::uint64_t generation):generation_(generation){if(!generation)throw std::invalid_argument("Share generation must be nonzero");}
bool EncryptionGate::configure(std::string owner,std::vector<std::string> sids) {
    std::lock_guard lock(mutex_);
    if(configured_)throw std::logic_error("Encryption gate cannot be reused");
    if(closed_)return false;
    if(owner.empty()||owner.size()>512||sids.size()<2||sids.size()>3)throw std::invalid_argument("Invalid encryption manifest");
    std::map<std::string,bool> manifest;
    for(auto& sid:sids)if(sid.empty()||sid.size()>128||!manifest.emplace(sid,false).second)throw std::invalid_argument("Missing/duplicate encryption track SID");
    owner_=std::move(owner);tracks_=std::move(manifest);configured_=true;return true;
}
bool EncryptionGate::observe(const std::string& owner,const std::string& sid,livekit::EncryptionState state) {
    std::lock_guard lock(mutex_);
    if(closed_||!configured_||owner!=owner_)return false;
    auto it=tracks_.find(sid);if(it==tracks_.end())return false;
    switch(state) {
    case livekit::EncryptionState::Ok:it->second=true;wasReady_=wasReady_||std::all_of(tracks_.begin(),tracks_.end(),[](const auto& entry){return entry.second;});break;
    case livekit::EncryptionState::New:case livekit::EncryptionState::KeyRatcheted:it->second=false;if(wasReady_){closed_=true;failed_=true;}break;
    default:it->second=false;closed_=true;failed_=true;break;
    }
    return true;
}
void EncryptionGate::close(bool failure){std::lock_guard lock(mutex_);closed_=true;failed_=failed_||failure;}
bool EncryptionGate::ready()const{std::lock_guard lock(mutex_);return configured_&&!closed_&&std::all_of(tracks_.begin(),tracks_.end(),[](const auto& entry){return entry.second;});}
bool EncryptionGate::closed()const{std::lock_guard lock(mutex_);return closed_;}
bool EncryptionGate::failed()const{std::lock_guard lock(mutex_);return failed_;}
QJsonObject EncryptionGate::report()const {
    std::lock_guard lock(mutex_);QJsonArray tracks;
    for(const auto& [sid,ok]:tracks_)tracks.append(QJsonObject{{"sid",QString::fromStdString(sid)},{"ready",ok}});
    return {{"generation",static_cast<double>(generation_)},{"configured",configured_},{"closed",closed_},{"failed",failed_},{"tracks",tracks}};
}
}
