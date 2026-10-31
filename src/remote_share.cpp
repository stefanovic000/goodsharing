#include "remote_share.h"
#include <algorithm>
#include <stdexcept>

namespace goodsharing {
namespace {std::size_t slot(ShareTrack t){return t==ShareTrack::Video1080?0:t==ShareTrack::Video720?1:2;}}
void RemoteShareCatalog::tombstone(const QString& sid) {
    if(sid.isEmpty())return;tracks_.erase(sid);
    // A long session can exhaust the SID history; drop it instead of poisoning
    // every later watch ("share changed" with matching generations).
    if(retired_.size()>=512&&!retired_.contains(sid))retired_.clear();
    if(retired_.size()>=512){error_="Remote track history limit exceeded; rejoin the room";return;}
    retired_.insert(sid);
}
RemoteShareCatalog::Change RemoteShareCatalog::announce(const ShareModel::Announcement& a) {
    Change change;const auto identity=validateShareAnnouncement(a);
    if(!error_.isEmpty()||!identity||retired_.contains(a.sid))return change;
    const Key key{a.owner,identity->id};auto it=entries_.find(key);
    if(it!=entries_.end()&&(identity->generation<it->second.generation||(identity->generation==it->second.generation&&it->second.retired)))return change;
    if(const auto existing=tracks_.find(a.sid);existing!=tracks_.end()) {
        change.accepted=existing->second.announcement.owner==a.owner&&existing->second.identity==*identity;return change;
    }
    if(it==entries_.end()) {
        if(entries_.size()>=64){
            for(auto it=entries_.begin();it!=entries_.end();){if(it->second.retired)it=entries_.erase(it);else ++it;}
        }
        if(entries_.size()>=64){error_="Remote share history limit exceeded; rejoin the room";return change;}
        it=entries_.emplace(key,Entry{identity->generation}).first;
    }
    auto& entry=it->second;
    if(identity->generation>entry.generation) {
        for(const auto& sid:entry.sids)if(!sid.isEmpty()){change.retired.push_back(sid);tombstone(sid);}
        entry={identity->generation};
    }
    if(!error_.isEmpty())return change;
    auto& target=entry.sids[slot(identity->track)];if(!target.isEmpty())return change;
    if(tracks_.size()>=192){error_="Remote track limit exceeded";return change;}
    target=a.sid;tracks_.emplace(a.sid,Record{a,*identity});change.accepted=true;change.changed=true;return change;
}
std::optional<RemoteShareCatalog::Record> RemoteShareCatalog::find(const QString& sid)const {
    if(!error_.isEmpty())return {};const auto it=tracks_.find(sid);return it==tracks_.end()?std::nullopt:std::optional{it->second};
}
bool RemoteShareCatalog::hasVideo(const QString& owner,const QString& id,int quality)const {
    const auto it=entries_.find({owner,id});
    if(it==entries_.end()||it->second.retired)return false;
    return !it->second.sids[quality==1080?0:1].isEmpty();
}
std::optional<quint64> RemoteShareCatalog::generation(const QString& owner,const QString& id)const {
    const auto it=entries_.find({owner,id});
    if(it==entries_.end()||it->second.retired)return std::nullopt;
    return it->second.generation;
}
std::optional<RemoteShareCatalog::Manifest> RemoteShareCatalog::manifest(const QString& owner,const QString& id,quint64 generation,ShareTrack quality,bool audio)const {
    manifestError_.clear();
    if(!error_.isEmpty()){manifestError_="catalog error: "+error_;return {};}
    if(quality!=ShareTrack::Video1080&&quality!=ShareTrack::Video720&&quality!=ShareTrack::Audio){manifestError_="invalid quality";return {};}
    const auto it=entries_.find({owner,id});
    if(it==entries_.end()){manifestError_="no entry";return {};}
    if(it->second.retired){manifestError_="entry retired";return {};}
    if(it->second.generation!=generation){manifestError_=QString("generation mismatch %1!=%2").arg(it->second.generation).arg(generation);return {};}
    const auto& entry=it->second;
    if(quality==ShareTrack::Audio){if(entry.sids[2].isEmpty()){manifestError_="no audio sid";return {};}return Manifest{owner,id,generation,quality,{},entry.sids[2]};}
    const auto& video=entry.sids[slot(quality)];
    if(video.isEmpty()){manifestError_=QString("no sid for slot %1").arg(slot(quality));return {};}
    if(audio&&entry.sids[2].isEmpty()){manifestError_="no audio sid";return {};}
    return Manifest{owner,id,generation,quality,video,audio?entry.sids[2]:QString{}};
}
bool RemoteShareCatalog::remove(const QString& sid) {
    const auto it=tracks_.find(sid);if(it==tracks_.end())return false;
    auto& entry=entries_.at({it->second.announcement.owner,it->second.identity.id});
    entry.sids[slot(it->second.identity.track)].clear();tombstone(sid);
    if(std::all_of(entry.sids.begin(),entry.sids.end(),[](const QString& s){return s.isEmpty();}))entry.retired=true;
    return true;
}
std::vector<QString> RemoteShareCatalog::removeOwner(const QString& owner) {
    std::vector<QString> removed;
    for(auto& [key,entry]:entries_)if(key.first==owner){for(const auto& sid:entry.sids)if(!sid.isEmpty()){removed.push_back(sid);tombstone(sid);}entry.sids={};entry.retired=true;}
    return removed;
}

RemoteShare::RemoteShare(Manifest manifest,Callbacks callbacks,std::shared_ptr<std::atomic_bool> roomAdmission):manifest_(std::move(manifest)),owner_(manifest_.owner.toStdString()),videoSid_(manifest_.videoSid.toStdString()),audioSid_(manifest_.audioSid.toStdString()),callbacks_(std::move(callbacks)),roomAdmission_(roomAdmission?std::move(roomAdmission):std::make_shared<std::atomic_bool>(true)) {
    const bool audioOnly=manifest_.quality==ShareTrack::Audio;
    if(manifest_.owner.isEmpty()||manifest_.owner.size()>128||manifest_.videoSid.size()>128||manifest_.audioSid.size()>128||manifest_.videoSid==manifest_.audioSid||
       (audioOnly?(!manifest_.videoSid.isEmpty()||manifest_.audioSid.isEmpty()||!callbacks_.audio):
       (manifest_.videoSid.isEmpty()||(manifest_.quality!=ShareTrack::Video1080&&manifest_.quality!=ShareTrack::Video720)||!callbacks_.video||(!manifest_.audioSid.isEmpty()&&!callbacks_.audio))))throw std::invalid_argument("Invalid remote share selection");
    shareTrackName({manifest_.id,manifest_.generation,manifest_.quality});
}
RemoteShare::~RemoteShare(){stop();}
bool RemoteShare::accepts(const QString& owner,const QString& sid)const {
    return !closed()&&owner==manifest_.owner&&!sid.isEmpty()&&(sid==manifest_.videoSid||sid==manifest_.audioSid);
}
bool RemoteShare::subscribed(const QString& owner,const std::shared_ptr<livekit::Track>& track) {
    if(!track||!accepts(owner,QString::fromStdString(track->sid())))return false;
    const bool video=QString::fromStdString(track->sid())==manifest_.videoSid;const auto index=video?0:1;
    if(track->kind()!=(video?livekit::TrackKind::KIND_VIDEO:livekit::TrackKind::KIND_AUDIO))return false;
    std::lock_guard lock(mutex_);if(closed())return false;
    if(tracks_[index])return tracks_[index]==track;tracks_[index]=track;
    const bool accepted=video?receiver_.video(track,[this](livekit::VideoFrameEvent& frame){if(!closed())callbacks_.video(frame);},owner.toStdString()):receiver_.audio(track,[this](livekit::AudioFrameEvent& frame){if(!closed())callbacks_.audio(frame);},2,owner.toStdString());
    if(!accepted)open_=false;return accepted;
}
bool RemoteShare::encryption(const QString& owner,const QString& sid,livekit::EncryptionState state) {
    if(!accepts(owner,sid))return false;
    std::lock_guard lock(mutex_);if(closed())return false;
    const auto index=sid==manifest_.videoSid?0:1;
    if(state==livekit::EncryptionState::Ok)verified_[index]=true;
    else if(verified_[index]||(state!=livekit::EncryptionState::New&&state!=livekit::EncryptionState::KeyRatcheted))open_=false;
    const bool accepted=receiver_.encryption(owner.toStdString(),sid.toStdString(),state);
    if(!accepted)open_=false;
    return accepted;
}
void RemoteShare::unsubscribed(const std::shared_ptr<livekit::Track>& track) {
    if(!track)return;std::lock_guard lock(mutex_);
    // A late unsubscribe of an older SDK Track must not kill its replacement.
    if(track==tracks_[0]||track==tracks_[1])open_=false;
}
bool RemoteShare::presentVideo(const std::function<void()>& operation)const{return present(operation,false);}
bool RemoteShare::presentAudio(const std::function<void()>& operation)const{return present(operation,true);}
bool RemoteShare::present(const std::function<void()>& operation,bool audio)const {
    if(!operation||closed())return false;
    std::shared_lock lock(mutex_);
    if(!operation||closed()||(audio?audioSid_.empty():videoSid_.empty())||
       (!videoSid_.empty()&&(!verified_[0]||!receiver_.canPresent(owner_,videoSid_)))||
       (!audioSid_.empty()&&(!verified_[1]||!receiver_.canPresent(owner_,audioSid_))))return false;
    operation();return true;
}
void RemoteShare::revoke(){open_=false;std::lock_guard lock(mutex_);}
void RemoteShare::stop(){revoke();receiver_.stop();std::lock_guard lock(mutex_);tracks_={};}
void RemoteShare::finish(){stop();stopped_=true;}
std::vector<std::future<std::vector<livekit::RtcStats>>> RemoteShare::requestStatistics()const{
    std::array<std::shared_ptr<livekit::Track>,2> tracks;
    {std::shared_lock lock(mutex_);if(closed())return {};tracks=tracks_;}
    std::vector<std::future<std::vector<livekit::RtcStats>>> requests;
    for(const auto& track:tracks)if(track)requests.push_back(track->getStats());
    return requests;
}
QJsonObject RemoteShare::report()const {
    std::shared_lock lock(mutex_);
    return {{"owner",manifest_.owner},{"share_id",manifest_.id},{"generation",static_cast<double>(manifest_.generation)},
        {"quality",manifest_.quality==ShareTrack::Audio?0:manifest_.quality==ShareTrack::Video1080?1080:720},{"video_sid",manifest_.videoSid},{"audio_sid",manifest_.audioSid},
        {"closed",closed()},{"stopped",stopped()},{"ready",!closed()&&(videoSid_.empty()||verified_[0])&&(audioSid_.empty()||verified_[1])},
        {"reader_error",QString::fromStdString(receiver_.error())},{"receiver",receiver_.report()}};
}
}
