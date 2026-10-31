#pragma once
// TEST ONLY: inspect raw SDK decoder/PLC output independently of app admission.
// This helper is included by diagnostic executables, never production transport.
#include "track_reader.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace goodsharing_test {
class DiagnosticTrackReaders {
public:
    DiagnosticTrackReaders():worker_([this]{run();}){}
    ~DiagnosticTrackReaders(){stop();}
    void video(std::shared_ptr<livekit::Track> track,goodsharing::TrackReader::VideoCallback callback) {
        attach({std::move(track),std::move(callback),{},2});
    }
    void audio(std::shared_ptr<livekit::Track> track,goodsharing::TrackReader::AudioCallback callback,unsigned channels) {
        attach({std::move(track),{},std::move(callback),channels});
    }
    void stop() {
        {std::lock_guard lock(mutex_);accepting_=false;pending_.clear();}
        wake_.notify_one();if(worker_.joinable())worker_.join();
    }
    std::string error()const {std::lock_guard lock(mutex_);return error_;}
private:
    struct Pending {std::shared_ptr<livekit::Track> track;goodsharing::TrackReader::VideoCallback video;goodsharing::TrackReader::AudioCallback audio;unsigned channels;};
    void attach(Pending next) {
        if(!next.track||next.track->sid().empty()||(!next.video&&!next.audio))throw std::invalid_argument("Invalid raw diagnostic track");
        std::lock_guard lock(mutex_);if(!accepting_)return;
        if(pending_.size()>=16)throw std::runtime_error("Raw diagnostic command bound exceeded");
        pending_.push_back(std::move(next));wake_.notify_one();
    }
    void run() {
        std::map<std::string,std::unique_ptr<goodsharing::TrackReader>> readers;
        try {
            for(;;) {
                Pending next;
                {std::unique_lock lock(mutex_);wake_.wait(lock,[this]{return !accepting_||!pending_.empty();});
                 if(!accepting_)break;next=std::move(pending_.front());pending_.pop_front();}
                const auto sid=next.track->sid();if(readers.contains(sid))continue;
                if(readers.size()>=8)throw std::runtime_error("Raw diagnostic reader bound exceeded");
                auto reader=std::make_unique<goodsharing::TrackReader>();
                if(next.video)reader->startVideo(next.track,std::move(next.video));
                else reader->startAudio(next.track,std::move(next.audio),next.channels);
                readers.emplace(sid,std::move(reader));
            }
        }catch(const std::exception& e){std::lock_guard lock(mutex_);error_=e.what();accepting_=false;pending_.clear();}
        for(auto& [sid,reader]:readers){reader->stop();const auto error=reader->error();if(!error.empty()){std::lock_guard lock(mutex_);if(error_.empty())error_=error;}}
    }
    mutable std::mutex mutex_;std::condition_variable wake_;std::deque<Pending> pending_;
    bool accepting_=true;std::string error_;std::jthread worker_;
};
}
