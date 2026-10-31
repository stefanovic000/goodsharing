#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
namespace goodsharing_test {
inline constexpr double pi=3.141592653589793;
inline QJsonObject spectrum(const std::vector<std::int16_t>& data,int channels) {
    QJsonArray result;
    for(int ch=0;ch<channels;ch++) {
        QJsonObject tones;
        for(int hz:{440,880}) {
            double re=0,im=0; std::size_t count=0;
            for(std::size_t i=static_cast<std::size_t>(ch);i<data.size();i+=channels,count++) { const auto phase=count*hz*2*pi/48000; re+=data[i]*std::cos(phase); im+=data[i]*std::sin(phase); }
            tones[QString::number(hz)]=count?2*std::hypot(re,im)/count:0;
        }
        result.append(tones);
    }
    const bool enough=result.size()==2&&data.size()>=48000*2;
    const double left=enough?result[0].toObject()["440"].toDouble():0;
    const double right=enough?result[1].toObject()["880"].toDouble():0;
    const double leakLeft=enough?result[0].toObject()["880"].toDouble():0;
    const double leakRight=enough?result[1].toObject()["440"].toDouble():0;
    // Overlapping 100 ms windows also catch a brief cleartext startup leak.
    double windowMax=0;
    if(channels>0) {
        const std::size_t width=4800*channels,step=2400*channels;
        for(std::size_t begin=0;begin+width<=data.size();begin+=step) {
            for(int ch=0;ch<channels;ch++)for(int hz:{440,880}) {
                double re=0,im=0;
                for(std::size_t n=0;n<4800;n++) {const double phase=n*hz*2*pi/48000;const auto value=data[begin+n*channels+ch];re+=value*std::cos(phase);im+=value*std::sin(phase);}
                windowMax=std::max(windowMax,2*std::hypot(re,im)/4800);
            }
        }
    }
    return {{"channels",channels},{"frames",static_cast<double>(channels?data.size()/channels:0)},{"tones",result},{"max_100ms_tone_amplitude",windowMax},{"left_crosstalk_ratio",left>0?leakLeft/left:1},{"right_crosstalk_ratio",right>0?leakRight/right:1},{"stereo_passed",enough&&left>100&&right>100&&leakLeft<left*.03&&leakRight<right*.03}};
}
}
