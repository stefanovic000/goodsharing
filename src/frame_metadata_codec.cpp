#include "frame_metadata_codec.h"
#include <QByteArray>
#include <QtEndian>

namespace goodsharing {
std::vector<std::uint8_t> encodeFrameIdentity(FrameIdentity identity) {
    QByteArray payload(13,Qt::Uninitialized);
    payload[0]=identity.priming?1:0;
    qToBigEndian<quint64>(identity.captureTimestampUs,payload.data()+1);
    qToBigEndian<quint32>(identity.frameId,payload.data()+9);
    const auto wire=QByteArray("GS02")+payload.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals);
    return {wire.begin(),wire.end()};
}
std::optional<FrameIdentity> decodeFrameIdentity(std::span<const std::uint8_t> bytes) {
    if(bytes.size()!=22) return std::nullopt;
    const QByteArray wire(reinterpret_cast<const char*>(bytes.data()),static_cast<qsizetype>(bytes.size()));
    if(!wire.startsWith("GS02")) return std::nullopt;
    const auto payload=QByteArray::fromBase64(wire.mid(4),QByteArray::Base64UrlEncoding|QByteArray::AbortOnBase64DecodingErrors);
    if(payload.size()!=13||(payload[0]!=0&&payload[0]!=1)||payload.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals)!=wire.mid(4)) return std::nullopt;
    return FrameIdentity{qFromBigEndian<quint64>(payload.constData()+1),qFromBigEndian<quint32>(payload.constData()+9),payload[0]==1};
}
}
