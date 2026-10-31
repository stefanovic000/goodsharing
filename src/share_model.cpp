#include "share_model.h"
#include <QRegularExpression>
#include <QThread>
#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace goodsharing {
namespace {
bool validId(const QString& id){static const QRegularExpression pattern("^[0-9a-f]{32}$");return pattern.match(id).hasMatch();}
}
QString shareTrackName(const ShareIdentity& identity) {
    if(!validId(identity.id)||!identity.generation)throw std::invalid_argument("Invalid share identity");
    const auto kind=identity.track==ShareTrack::Video1080?"1080":identity.track==ShareTrack::Video720?"720":"audio";
    return "gs1."+identity.id+"."+QString::number(identity.generation,16).rightJustified(16,'0')+"."+kind;
}
std::optional<ShareIdentity> parseShareTrackName(const QString& name) {
    if(name.size()>64)return std::nullopt;
    const auto fields=name.split('.');
    if(fields.size()!=4||fields[0]!="gs1"||!validId(fields[1])||fields[2].size()!=16)return std::nullopt;
    bool ok=false;const auto generation=fields[2].toULongLong(&ok,16);
    if(!ok||!generation||QString::number(generation,16).rightJustified(16,'0')!=fields[2])return std::nullopt;
    ShareTrack track;
    if(fields[3]=="1080")track=ShareTrack::Video1080;
    else if(fields[3]=="720")track=ShareTrack::Video720;
    else if(fields[3]=="audio")track=ShareTrack::Audio;
    else return std::nullopt;
    return ShareIdentity{fields[1],generation,track};
}
ShareModel::ShareModel(QObject* parent):QAbstractListModel(parent){}
void ShareModel::ownerThread()const{if(QThread::currentThread()!=thread())throw std::logic_error("Share model accessed outside its Qt owner thread");}
int ShareModel::rowCount(const QModelIndex& parent)const{return parent.isValid()?0:static_cast<int>(entries_.size());}
QVariant ShareModel::data(const QModelIndex& index,int role)const {
    if(!index.isValid()||index.row()<0||index.row()>=rowCount())return {};
    const auto& e=entries_[index.row()];
    switch(role) {
    case OwnerRole:return e.owner;case ShareIdRole:return e.id;case GenerationRole:return QVariant::fromValue(e.generation);
    case Has1080Role:return !e.video1080.isEmpty();case Has720Role:return !e.video720.isEmpty();case HasAudioRole:return !e.audio.isEmpty();
    default:return {};
    }
}
QHash<int,QByteArray> ShareModel::roleNames()const{return {{OwnerRole,"owner"},{ShareIdRole,"shareId"},{GenerationRole,"generation"},{Has1080Role,"has1080"},{Has720Role,"has720"},{HasAudioRole,"hasAudio"}};}
std::optional<ShareIdentity> validateShareAnnouncement(const ShareModel::Announcement& a) {
    const auto identity=parseShareTrackName(a.name);
    if(!identity||a.owner.isEmpty()||a.owner.size()>128||a.sid.isEmpty()||a.sid.size()>128||!a.encrypted||!a.screenSource)return {};
    const auto track=identity->track;
    if(a.audio!=(track==ShareTrack::Audio))return {};
    if(!a.audio&&(a.width!=(track==ShareTrack::Video1080?1920u:1280u)||a.height!=(track==ShareTrack::Video1080?1080u:720u)))return {};
    return identity;
}
bool ShareModel::announce(const Announcement& a) {
    ownerThread();const auto identity=validateShareAnnouncement(a);if(!identity)return false;
    const auto track=identity->track;
    auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.owner==a.owner&&e.id==identity->id;});
    if(it!=entries_.end()&&(identity->generation<it->generation||(identity->generation==it->generation&&it->retired)))return false;
    // A publication SID belongs to exactly one owner/share/generation/quality.
    for(const auto& e:entries_)if(e.video1080==a.sid||e.video720==a.sid||e.audio==a.sid) {
        return e.owner==a.owner&&e.id==identity->id&&e.generation==identity->generation&&
            (track==ShareTrack::Video1080?e.video1080:track==ShareTrack::Video720?e.video720:e.audio)==a.sid;
    }
    if(it==entries_.end()) {
        if(entries_.size()>=64)return false;
        const int row=rowCount();beginInsertRows({},row,row);entries_.push_back({a.owner,identity->id,identity->generation});endInsertRows();it=std::prev(entries_.end());
    }
    if(identity->generation>it->generation){it->generation=identity->generation;it->retired=false;it->video1080.clear();it->video720.clear();it->audio.clear();}
    auto& slot=track==ShareTrack::Video1080?it->video1080:track==ShareTrack::Video720?it->video720:it->audio;
    if(!slot.isEmpty()&&slot!=a.sid)return false; // Conflicting duplicate quality.
    slot=a.sid;const auto row=static_cast<int>(it-entries_.begin());Q_EMIT dataChanged(index(row),index(row));return true;
}
bool ShareModel::remove(const QString& sid) {
    ownerThread();if(sid.isEmpty())return false;
    for(auto& e:entries_)for(auto* slot:{&e.video1080,&e.video720,&e.audio})if(*slot==sid) {
        slot->clear();const auto row=static_cast<int>(&e-entries_.data());Q_EMIT dataChanged(index(row),index(row));return true;
    }
    return false;
}
bool ShareModel::retire(const QString& owner,const QString& id,quint64 generation) {
    ownerThread();
    for(auto& e:entries_)if(e.owner==owner&&e.id==id&&e.generation==generation) {
        e.retired=true;e.video1080.clear();e.video720.clear();e.audio.clear();const auto row=static_cast<int>(&e-entries_.data());Q_EMIT dataChanged(index(row),index(row));return true;
    }
    return false;
}
void ShareModel::removeOwner(const QString& owner) {
    ownerThread();for(int row=rowCount()-1;row>=0;--row)if(entries_[row].owner==owner){beginRemoveRows({},row,row);entries_.erase(entries_.begin()+row);endRemoveRows();}
}
void ShareModel::clear(){ownerThread();beginResetModel();entries_.clear();endResetModel();}
QString ShareModel::sid(const QString& owner,const QString& id,ShareTrack track)const {
    ownerThread();for(const auto& e:entries_)if(e.owner==owner&&e.id==id)return track==ShareTrack::Video1080?e.video1080:track==ShareTrack::Video720?e.video720:e.audio;
    return {};
}
}
