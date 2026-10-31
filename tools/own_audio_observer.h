#pragma once
#include "audio_capture.h"
#include "audio_playback.h"
#include "audio_test_spectrum.h"
#include <windows.h>
#include <array>
#include <QJsonArray>
#include <mutex>

namespace goodsharing_test {
// Test-only own-process recording: bounded last-three-second ring, aggregate
// whole-run peak and one-second tail. No microphone or unrelated process.
class OwnAudioObserver {
public:
    OwnAudioObserver(){
        observerStart_=qpcUs();
        silence_.start(0); // Never receives content; keeps post-stop capture live.
        if(!capture_.start(goodsharing::AudioMode::Application,GetCurrentProcessId(),[this](goodsharing::AudioBlock block){
            std::lock_guard lock(mutex_);
            for(auto s:block.samples)wholePeak_=std::max(wholePeak_,std::abs(static_cast<int>(s)));
            for(std::size_t frame=0;frame<block.samples.size()/2;frame++){
                const auto at=block.timestampUs+static_cast<std::int64_t>(frame)*1000000/48000;
                const bool quiet=std::abs(static_cast<int>(block.samples[frame*2]))<40&&std::abs(static_cast<int>(block.samples[frame*2+1]))<40;
                if((!cut_||at<cut_)&&(toneStart_||!quiet)){
                    if(!toneStart_)toneStart_=at;
                    toneFrames_++;if(quiet){toneQuiet_++;toneRun_++;toneLongest_=std::max(toneLongest_,toneRun_);}else toneRun_=0;
                }
                for(auto& interval:intervals_)if(interval.start&&at>=interval.start&&(!interval.end||at<interval.end)){
                    interval.frames++;if(quiet){interval.quiet++;interval.run++;interval.longest=std::max(interval.longest,interval.run);}else interval.run=0;
                }
            }
            if(cut_&&block.timestampUs>=cut_){
                if(block.timestampUs>cut_+80000){tailFrames_+=block.samples.size()/2;for(auto s:block.samples)tailPeak_=std::max(tailPeak_,std::abs(static_cast<int>(s)));}
                return;
            }
            for(auto s:block.samples){ring_[cursor_++%ring_.size()]=s;count_=std::min(count_+1,ring_.size());}
        },[this](QString error){std::lock_guard lock(mutex_);error_=error;}))throw std::runtime_error("Own-process audio observer could not start");
    }
    ~OwnAudioObserver(){stop();}
    int beginInterval(){std::lock_guard lock(mutex_);for(std::size_t n=0;n<intervals_.size();n++)if(!intervals_[n].start){intervals_[n].start=qpcUs();return static_cast<int>(n)+1;}return 0;}
    void endInterval(int id){std::lock_guard lock(mutex_);if(id>0&&id<=static_cast<int>(intervals_.size())&&!intervals_[id-1].end)intervals_[id-1].end=qpcUs();}
    void cut(){LARGE_INTEGER q{},f{};QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);
        std::lock_guard lock(mutex_);cut_=static_cast<std::int64_t>(static_cast<long double>(q.QuadPart)*1000000/f.QuadPart);for(auto& interval:intervals_)if(interval.start&&!interval.end)interval.end=cut_;}
    void stop(){capture_.stop();silence_.stop();}
    QJsonObject report(){
        std::lock_guard lock(mutex_);std::vector<std::int16_t> recent;recent.reserve(count_);
        const auto first=(cursor_+ring_.size()-count_)%ring_.size();for(std::size_t n=0;n<count_;n++)recent.push_back(ring_[(first+n)%ring_.size()]);
        QJsonArray intervals;for(const auto& interval:intervals_)if(interval.start){const auto elapsed=interval.end-interval.start;
            intervals.append(QJsonObject{{"closed",interval.end>0},{"duration_ms",elapsed/1000.0},{"frames",static_cast<double>(interval.frames)},
                {"observed_frame_coverage",elapsed>0?interval.frames*1000000.0/48000/elapsed:0},{"quiet_frame_fraction",interval.frames?static_cast<double>(interval.quiet)/interval.frames:1},
                {"longest_quiet_ms",interval.longest/48.0}});}
        const auto toneDuration=cut_>toneStart_&&toneStart_?cut_-toneStart_:0;
        const QJsonObject continuous{{"started",toneStart_>0},{"duration_ms",toneDuration/1000.0},{"frames",static_cast<double>(toneFrames_)},
            {"first_tone_startup_ms",toneStart_?(toneStart_-observerStart_)/1000.0:0},
            {"observed_frame_coverage",toneDuration?toneFrames_*1000000.0/48000/toneDuration:0},
            {"quiet_frame_fraction",toneFrames_?static_cast<double>(toneQuiet_)/toneFrames_:1},{"longest_quiet_ms",toneLongest_/48.0}};
        return {{"own_process_only",true},{"recent_audio",spectrum(recent,2)},{"whole_run_peak",wholePeak_},{"intervals",intervals},{"continuous_tone_observation",continuous},
            {"post_stop_frames",static_cast<double>(tailFrames_)},{"post_stop_peak",tailPeak_},{"error",error_},
            {"legacy_player_supplies_only_silence",true},{"physical_speaker_display_sync_measured",false}};
    }
private:
    struct Interval{std::int64_t start=0,end=0;std::uint64_t frames=0,quiet=0,run=0,longest=0;};
    std::array<Interval,4> intervals_{};
    std::int64_t observerStart_=0,toneStart_=0;std::uint64_t toneFrames_=0,toneQuiet_=0,toneRun_=0,toneLongest_=0;
    static std::int64_t qpcUs(){LARGE_INTEGER q{},f{};QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return static_cast<std::int64_t>(static_cast<long double>(q.QuadPart)*1000000/f.QuadPart);}
    mutable std::mutex mutex_;
    std::array<std::int16_t,48000*2*3> ring_{};
    std::size_t cursor_=0,count_=0;
    std::uint64_t tailFrames_=0;
    int wholePeak_=0,tailPeak_=0;std::int64_t cut_=0;QString error_;
    goodsharing::AudioPlayback silence_;
    goodsharing::AudioCapture capture_;
};
}
