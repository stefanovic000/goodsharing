#pragma once
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <atomic>
#include <span>
#include <stdexcept>
#include <vector>
#include <optional>

namespace goodsharing_test {
// Synthetic probe only. A deterministic 20 ms amplitude code identifies audio
// content after Opus decoding; it carries no user audio and adds no app buffer.
inline int timingAmplitude(std::uint64_t sampleFrame) {
    auto x=static_cast<std::uint32_t>(sampleFrame/960+1)*0x9e3779b9u;
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
    return x&1?6000:2000;
}
class AudioTimingTrace {
public:
    explicit AudioTimingTrace(QString folder):folder_(std::move(folder)){}
    bool enabled()const{return !folder_.isEmpty();}
    void setSourceSampleClock(){sourceSampleClock_=true;}
    // Each collection has one owner: the synthetic source or receive worker.
    // finish() is called only after both have joined.
    void source(std::uint64_t offset,std::uint64_t captureUs) {
        if(!enabled())return;
        if(source_.size()>=2400){overflow_=true;return;}
        source_.append(QJsonObject{{"sample_frame",static_cast<double>(offset)},{"capture_us",static_cast<double>(captureUs)}});
    }
    void received(std::span<const std::int16_t> pcm,int channels,int rate,std::uint64_t receivedUs,
                  std::optional<std::int64_t> captureMs={},std::optional<std::int64_t> sinkUs={},
                  std::optional<std::uint32_t> rtp={},std::optional<std::int64_t> senderNtp={},
                  std::optional<std::uint32_t> reports={},std::optional<std::uint32_t> epoch={}) {
        if(!enabled())return;
        if(channels!=2||rate!=48000||pcm.empty()||pcm.size()%2){invalid_=true;return;}
        if(pcm_.size()+pcm.size()>48000*2*24||received_.size()>=2400){overflow_=true;return;}
        QJsonObject entry{{"sample_frame",static_cast<double>(pcm_.size()/2)},
            {"frames",static_cast<int>(pcm.size()/2)},{"received_us",static_cast<double>(receivedUs)}};
        entry["media_capture_ms"]=captureMs?QJsonValue(static_cast<double>(*captureMs)):QJsonValue(QJsonValue::Null);
        entry["native_sink_us"]=sinkUs?QJsonValue(static_cast<double>(*sinkUs)):QJsonValue(QJsonValue::Null);
        entry["rtp_timestamp"]=rtp?QJsonValue(static_cast<double>(*rtp)):QJsonValue(QJsonValue::Null);
        entry["sender_ntp_us"]=senderNtp?QJsonValue(static_cast<double>(*senderNtp)):QJsonValue(QJsonValue::Null);
        entry["rtcp_reports"]=reports?QJsonValue(static_cast<double>(*reports)):QJsonValue(QJsonValue::Null);
        entry["sender_clock_epoch"]=epoch?QJsonValue(static_cast<double>(*epoch)):QJsonValue(QJsonValue::Null);
        received_.append(entry);
        pcm_.insert(pcm_.end(),pcm.begin(),pcm.end());
    }
    QJsonObject finish() {
        if(!enabled())return {{"enabled",false}};
        QJsonObject info{{"enabled",true},{"synthetic_only",true},{"channels",2},{"sample_rate",48000},
            {"format","s16le"},{"code","am20-v1"},{"overflow",overflow_.load()},{"invalid_format",invalid_},
            {"source",source_},{"received",received_},{"pcm_sample_frames",static_cast<double>(pcm_.size()/2)},
            {"pcm_file","received.s16le"}};
        if(sourceSampleClock_)info["source_timestamp_model"]="synthetic-48000hz-sample-grid";
        QFile pcm(folder_+"/received.s16le"),json(folder_+"/timing.json");
        const auto bytes=static_cast<qint64>(pcm_.size()*sizeof(std::int16_t));
        if(!pcm.open(QIODevice::WriteOnly)||pcm.write(reinterpret_cast<const char*>(pcm_.data()),bytes)!=bytes)
            throw std::runtime_error("Cannot save synthetic audio timing PCM");
        const auto data=QJsonDocument(info).toJson();
        if(!json.open(QIODevice::WriteOnly)||json.write(data)!=data.size())throw std::runtime_error("Cannot save synthetic audio timing map");
        return {{"enabled",true},{"folder",folder_},{"overflow",overflow_.load()},{"invalid_format",invalid_},
            {"source_blocks",source_.size()},{"received_blocks",received_.size()},{"pcm_sample_frames",static_cast<double>(pcm_.size()/2)}};
    }
private:
    QString folder_;
    QJsonArray source_,received_;
    std::vector<std::int16_t> pcm_;
    std::atomic_bool overflow_=false;
    bool invalid_=false;
    bool sourceSampleClock_=false;
};
}
