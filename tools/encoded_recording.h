#pragma once
#include "hardware_encoder.h"
#include "mf_marker_decoder.h"
#include <QDataStream>
#include <QFile>
#include <stdexcept>

// Only used with synthetic test frames. Keeps the exact access units so an
// independent decoder can inspect them after the live run without changing
// the encode/publish timing with a second synchronous decoder.
class EncodedRecording {
    struct Entry { goodsharing::EncodedFrame frame; unsigned marker=0; };
    QString path_;
    int width_=0,height_=0;
    std::vector<Entry> entries_;
    std::size_t totalBytes_=0;
public:
    EncodedRecording(const QString& path,int width,int height):path_(path),width_(width),height_(height) {
        entries_.reserve(18120);
    }
    void retain(goodsharing::EncodedFrame frame,unsigned marker) {
        totalBytes_+=frame.bytes.size();
        if(entries_.size()>=18120||totalBytes_>256*1024*1024) throw std::runtime_error("Synthetic recording memory limit reached");
        entries_.push_back({std::move(frame),marker});
    }
    void finish() {
        QFile file(path_); if(!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create encoded diagnostic recording");
        QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("GSVD0001",8); stream<<quint32(width_)<<quint32(height_);
        for(const auto& entry:entries_) {
            const auto& frame=entry.frame;
            stream<<qint64(frame.timestampUs)<<quint32(entry.marker)<<quint8(frame.keyframe)<<quint32(frame.bytes.size());
            stream.writeRawData(reinterpret_cast<const char*>(frame.bytes.data()),static_cast<qint64>(frame.bytes.size()));
        }
        if(stream.status()!=QDataStream::Ok||!file.flush()) throw std::runtime_error("Writing encoded diagnostic recording failed");
    }
};

inline QJsonObject verifyEncodedRecording(const QString& path) {
    QFile file(path); if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open encoded diagnostic recording");
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
    char magic[8]{}; stream.readRawData(magic,8);
    if(QByteArray(magic,8)!="GSVD0001") throw std::runtime_error("Invalid encoded recording header");
    quint32 width=0,height=0; stream>>width>>height;
    if(width<640||width>4096||height<40||height>4096) throw std::runtime_error("Invalid diagnostic recording dimensions");
    MfMarkerDecoder decoder(static_cast<int>(width),static_cast<int>(height));
    while(!stream.atEnd()) {
        qint64 timestamp=0; quint32 marker=0,length=0; quint8 key=0;
        stream>>timestamp>>marker>>key>>length;
        if(stream.status()!=QDataStream::Ok||!length||length>16*1024*1024) throw std::runtime_error("Invalid diagnostic access unit");
        goodsharing::EncodedFrame frame; frame.timestampUs=timestamp; frame.keyframe=key!=0; frame.bytes.resize(length);
        if(stream.readRawData(reinterpret_cast<char*>(frame.bytes.data()),length)!=length) throw std::runtime_error("Truncated diagnostic access unit");
        decoder.submit(frame,marker);
    }
    return decoder.report();
}
