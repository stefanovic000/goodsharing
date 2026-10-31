#pragma once
#include "remote_share.h"
#include <stdexcept>

namespace goodsharing {
// One persistent audio lease, an active video lease, and at most one candidate.
// The caller owns subscriptions/readers. All final output operations take the
// same audio -> active video -> candidate order. Never re-enter this selection
// or any RemoteShare from an admitted operation.
class RemoteMediaSelection {
public:
    void bind(const std::shared_ptr<RemoteShare>& audio,const std::shared_ptr<RemoteShare>& video){
        std::lock_guard lock(mutex_);
        if(bound_||closed_||!compatible(audio,video))throw std::invalid_argument("Split media requires matching audio/video leases from one room");
        audio_=audio;active_=video;bound_=true;
    }
    bool stage(const std::shared_ptr<RemoteShare>& video){
        std::lock_guard lock(mutex_);const auto audio=audio_.lock(),active=active_.lock();
        if(closed_||!bound_||!active||active->closed()||!candidate_.expired()||!compatible(audio,video)||
           active==video||active->manifest().quality==video->manifest().quality||
           active->manifest().videoSid==video->manifest().videoSid)return false;
        candidate_=video;return true;
    }
    bool cancel(const std::shared_ptr<RemoteShare>& video){
        std::lock_guard lock(mutex_);if(!video||candidate_.lock()!=video)return false;candidate_.reset();return true;
    }
    bool presentActive(const std::function<void()>& operation)const{
        if(closed_)return false;
        std::shared_lock lock(mutex_);return presentLocked(active_.lock(),operation);
    }
    bool presentVideo(const std::shared_ptr<RemoteShare>& video,const std::function<void()>& operation)const{
        if(closed_)return false;
        std::shared_lock lock(mutex_);return presentLocked(video,operation);
    }
    bool adopt(const std::shared_ptr<RemoteShare>& video,const std::function<bool()>& operation){
        std::lock_guard lock(mutex_);if(!video||candidate_.lock()!=video||!operation)return false;
        bool accepted=false;
        if(!presentLocked(video,[&]{accepted=operation();}))return false;
        if(accepted){active_=video;candidate_.reset();}return accepted;
    }
    std::shared_ptr<RemoteShare> selected()const{std::shared_lock lock(mutex_);return active_.lock();}
    std::shared_ptr<RemoteShare> candidate()const{std::shared_lock lock(mutex_);return candidate_.lock();}
    bool closed()const{
        std::shared_lock lock(mutex_);const auto audio=audio_.lock(),active=active_.lock();
        return closed_||!audio||!active||audio->closed()||active->closed();
    }
    void stop(){closed_=true;std::lock_guard lock(mutex_);candidate_.reset();}
private:
    mutable std::shared_mutex mutex_;
    std::weak_ptr<RemoteShare> audio_,active_,candidate_;
    bool bound_=false;
    std::atomic_bool closed_=false;
    static bool compatible(const std::shared_ptr<RemoteShare>& audio,const std::shared_ptr<RemoteShare>& video){
        if(!audio||!video||audio->closed()||video->closed()||!audio->sameRoom(*video))return false;
        const auto& a=audio->manifest();const auto& v=video->manifest();
        return a.quality==ShareTrack::Audio&&a.videoSid.isEmpty()&&!a.audioSid.isEmpty()&&
            (v.quality==ShareTrack::Video1080||v.quality==ShareTrack::Video720)&&!v.videoSid.isEmpty()&&v.audioSid.isEmpty()&&
            a.owner==v.owner&&a.id==v.id&&a.generation==v.generation&&a.audioSid!=v.videoSid;
    }
    bool presentLocked(const std::shared_ptr<RemoteShare>& video,const std::function<void()>& operation)const{
        const auto audio=audio_.lock(),active=active_.lock(),candidate=candidate_.lock();
        if(closed_||!operation||!audio||!active||!video||(video!=active&&video!=candidate))return false;
        bool allowed=false;
        audio->presentAudio([&]{active->presentVideo([&]{
            if(video==active){operation();allowed=true;}
            else video->presentVideo([&]{operation();allowed=true;});
        });});
        return allowed;
    }
};
}
