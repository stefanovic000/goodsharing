#include "share_receiver.h"
#include <QJsonArray>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace goodsharing {
struct ShareReceiver::Impl {
    struct Permission {std::string owner;std::atomic_bool allowed=false;bool wasReady=false;std::atomic_bool revoked=false;};
    enum class Action {Attach,Remove,Changed};
    struct Command {
        Action action=Action::Changed;
        std::string sid;
        std::shared_ptr<livekit::Track> track;
        TrackReader::VideoCallback video;
        TrackReader::AudioCallback audio;
        unsigned channels=2;
        std::shared_ptr<Permission> permission;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> commands;
    std::map<std::string,std::shared_ptr<Permission>> permissions;
    // Stream ownership stays on the control thread, never the SDK event thread.
    std::map<std::string,std::unique_ptr<TrackReader>> readers;
    std::map<std::string,Command> pending;
    bool accepting=true;
    std::string failure;
    QJsonArray completedReaders;
    std::jthread controller;
    Impl():controller([this]{run();}){}
    void revokeAll(){for(auto& [sid,p]:permissions){p->allowed=false;p->revoked=true;}}
    void fail(std::string message){std::lock_guard lock(mutex);if(failure.empty())failure=std::move(message);accepting=false;revokeAll();wake.notify_one();}
    std::shared_ptr<Permission> permission(const std::string& sid,const std::string& owner) {
        if(auto it=permissions.find(sid);it!=permissions.end())return it->second->owner==owner?it->second:nullptr;
        if(permissions.size()>=128){failure="Receive encryption-state limit exceeded";accepting=false;revokeAll();wake.notify_one();return {};}
        auto p=std::make_shared<Permission>();p->owner=owner;permissions[sid]=p;return p;
    }
    bool enqueueLocked(Command command) {
        if(!accepting)return false;
        if(commands.size()>=64){failure="Receive command queue overflow";accepting=false;revokeAll();wake.notify_one();return false;}
        commands.push_back(std::move(command));wake.notify_one();return true;
    }
    bool attach(Command command,const std::string& owner) {
        std::lock_guard lock(mutex);if(!accepting)return false;
        command.permission=permission(command.sid,owner);if(!command.permission||command.permission->revoked)return false;
        return enqueueLocked(std::move(command));
    }
    void removeReader(const std::string& sid) {
        pending.erase(sid);auto it=readers.find(sid);if(it==readers.end())return;
        it->second->stop();rememberReader(sid,*it->second);const auto error=it->second->error();if(!error.empty())fail(error);readers.erase(it);
    }
    void rememberReader(const std::string& sid,const TrackReader& reader) {
        auto report=reader.report();if(report.isEmpty())return;report["sid"]=QString::fromStdString(sid);
        std::lock_guard lock(mutex);if(completedReaders.size()<128)completedReaders.append(report);
    }
    void startReader(Command command) {
        if(command.permission->revoked)return;
        if(!command.permission->allowed){if(pending.size()>=16)throw std::runtime_error("Pending encrypted track limit exceeded");pending.insert_or_assign(command.sid,std::move(command));return;}
        if(readers.contains(command.sid))return;
        if(readers.size()>=8)throw std::runtime_error("Receive track limit exceeded");
        auto reader=std::make_unique<TrackReader>();const auto permit=command.permission;
        if(command.video)reader->startVideo(command.track,[permit,callback=std::move(command.video)](livekit::VideoFrameEvent& event){if(permit->allowed)callback(event);});
        else reader->startAudio(command.track,[permit,callback=std::move(command.audio)](livekit::AudioFrameEvent& event){if(permit->allowed)callback(event);},command.channels);
        readers.emplace(command.sid,std::move(reader));
    }
    void run() {
        for(;;) {
            Command command;bool have=false;
            {std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(10),[this]{return !accepting||!commands.empty();});
             if(!accepting)break;
             if(!commands.empty()){command=std::move(commands.front());commands.pop_front();have=true;}}
            try {
                if(!have){for(const auto& [sid,reader]:readers)if(const auto error=reader->error();!error.empty())fail(error);continue;}
                if(command.action==Action::Remove){removeReader(command.sid);continue;}
                if(command.action==Action::Changed){
                    if(command.permission->revoked){removeReader(command.sid);continue;}
                    auto it=pending.find(command.sid);if(it!=pending.end()&&command.permission->allowed){auto waiting=std::move(it->second);pending.erase(it);startReader(std::move(waiting));}
                } else startReader(std::move(command));
            }catch(const std::exception& e){fail(e.what());}
        }
        std::deque<Command> abandoned;{std::lock_guard lock(mutex);abandoned.swap(commands);}
        pending.clear();
        for(auto& [sid,reader]:readers){reader->stop();rememberReader(sid,*reader);const auto error=reader->error();if(!error.empty())fail(error);}
        readers.clear();
    }
};
ShareReceiver::ShareReceiver():impl_(std::make_unique<Impl>()){}
ShareReceiver::~ShareReceiver(){stop();}
bool ShareReceiver::video(std::shared_ptr<livekit::Track> track,TrackReader::VideoCallback callback,std::string owner) {
    if(!track||track->sid().empty()||!callback||owner.empty())throw std::invalid_argument("Missing verified video subscription");
    const auto sid=track->sid();return impl_->attach({Impl::Action::Attach,sid,std::move(track),std::move(callback),{},2,{}},owner);
}
bool ShareReceiver::audio(std::shared_ptr<livekit::Track> track,TrackReader::AudioCallback callback,unsigned channels,std::string owner) {
    if(!track||track->sid().empty()||!callback||owner.empty())throw std::invalid_argument("Missing verified audio subscription");
    const auto sid=track->sid();return impl_->attach({Impl::Action::Attach,sid,std::move(track),{},std::move(callback),channels,{}},owner);
}
bool ShareReceiver::encryption(std::string owner,std::string sid,livekit::EncryptionState state) {
    auto& p=*impl_;
    if(owner.empty()||owner.size()>512||sid.empty()||sid.size()>128)return false;
    std::lock_guard lock(p.mutex);if(!p.accepting)return false;auto permit=p.permission(sid,owner);if(!permit||permit->revoked)return false;
    if(state==livekit::EncryptionState::Ok){permit->allowed=true;permit->wasReady=true;}
    else {permit->allowed=false;if(permit->wasReady||(state!=livekit::EncryptionState::New&&state!=livekit::EncryptionState::KeyRatcheted))permit->revoked=true;}
    return p.enqueueLocked({Impl::Action::Changed,std::move(sid),{},{},{},2,std::move(permit)});
}
bool ShareReceiver::remove(std::string sid) {
    auto& p=*impl_;std::lock_guard lock(p.mutex);auto it=p.permissions.find(sid);if(it!=p.permissions.end()){it->second->allowed=false;it->second->revoked=true;}
    return p.enqueueLocked({Impl::Action::Remove,std::move(sid),{},{},{},2,{}});
}
bool ShareReceiver::canPresent(const std::string& owner,const std::string& sid)const {
    const auto& p=*impl_;std::lock_guard lock(p.mutex);const auto it=p.permissions.find(sid);
    return p.accepting&&p.failure.empty()&&it!=p.permissions.end()&&it->second->owner==owner&&it->second->allowed&&!it->second->revoked;
}
void ShareReceiver::stop() {
    auto& p=*impl_;{std::lock_guard lock(p.mutex);p.accepting=false;p.revokeAll();p.wake.notify_one();}
    if(p.controller.joinable())p.controller.join();
}
std::string ShareReceiver::error()const{std::lock_guard lock(impl_->mutex);return impl_->failure;}
QJsonObject ShareReceiver::report()const {
    const auto& p=*impl_;std::lock_guard lock(p.mutex);int verified=0,revoked=0;QJsonArray tracks;
    for(const auto& [sid,permission]:p.permissions){verified+=permission->wasReady?1:0;revoked+=permission->revoked?1:0;tracks.append(QJsonObject{{"sid",QString::fromStdString(sid)},{"owner",QString::fromStdString(permission->owner)},{"verified",permission->wasReady},{"revoked",permission->revoked.load()}});}
    QJsonArray liveReaders;for(const auto& [sid,reader]:p.readers)if(reader)liveReaders.append(QJsonObject{{"sid",QString::fromStdString(sid)},{"report",reader->report()}});
    return {{"verification_required",true},{"known_tracks",static_cast<int>(p.permissions.size())},{"verified_tracks",verified},{"revoked_tracks",revoked},{"tracks",tracks},{"completed_readers",p.completedReaders},{"live_readers",liveReaders}};
}
}
