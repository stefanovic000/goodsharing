#include "audio_playout.h"
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace goodsharing;
// Diagnostic of the current scheduler, not a test that approves its behavior.
// NetEq output cadence and RTP/source advancement can differ. No device, SDK,
// media permission or physical synchronization is exercised by this control.
void observe(std::uint32_t stride,bool comma,bool strict){
    constexpr std::int64_t sender=4000000000000000ll,local=1000000;
    AudioPlayout p;p.enableAudioAlignment();
    const auto clock=[](std::int64_t source,std::uint32_t rtp){return PlayoutClock{sender+source,rtp,1,2,true};};
    std::array<std::int16_t,960> pcm;pcm.fill(123);
    if(!p.video(clock(0,10000),local)||!p.acknowledgeReset(p.stats().resetSerial)||
       !p.audio(clock(0,10000),pcm,local)||!p.audio(clock(stride*1000000ll/48000,10000+stride),pcm,local+10000))
        throw std::runtime_error("Could not establish stride diagnostic");
    std::array<std::int16_t,960> output;
    const auto first=p.render(local+30000,local+30000,output);
    if(first.mediaFrames!=480)throw std::runtime_error("First decoded block did not render intact");
    p.mediaCommitted();
    const auto second=p.render(local+40000,local+40000,output);
    const auto stats=p.stats();
    if(strict&&(second.mediaFrames!=480||second.outputFrames!=480||stats.lateFrames||stats.silentFrames||
        !std::all_of(output.begin(),output.end(),[](auto v){return v==123;})))
        throw std::runtime_error("Variable source advancement inserted silence or trimmed decoded PCM");
    std::cout<<(comma?",":"")<<"{\"source_rtp_stride\":"<<stride<<",\"decoded_samples_per_block\":480,"
        "\"callback_spacing_us\":10000,\"first_media_frames\":"<<first.mediaFrames<<
        ",\"second_media_frames\":"<<second.mediaFrames<<",\"second_output_frames\":"<<second.outputFrames<<
        ",\"late_samples_trimmed\":"<<stats.lateFrames<<",\"synthetic_silent_samples\":"<<stats.silentFrames<<"}";
}
int main(int argc,char** argv){try{const bool strict=argc==2&&std::string_view(argv[1])=="--require-continuous";
    std::cout<<"{\"current_scheduler_audit\":true,\"physical_sync_measured\":false,\"cases\":[";
    observe(480,false,strict);observe(698,true,strict);observe(262,true,strict);std::cout<<"]}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
