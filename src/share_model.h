#pragma once
#include <QAbstractListModel>
#include <QString>
#include <optional>
#include <vector>

namespace goodsharing {
enum class ShareTrack { Video1080,Video720,Audio };
struct ShareIdentity {
    QString id; // 32 lowercase hex digits, generated randomly at Share start.
    quint64 generation=0;
    ShareTrack track=ShareTrack::Video1080;
    bool operator==(const ShareIdentity&)const=default;
};
QString shareTrackName(const ShareIdentity& identity);
std::optional<ShareIdentity> parseShareTrackName(const QString& name);

// Call only on the Qt owner thread. The owner comes from the authenticated SDK
// participant, never from the track name. Retired generations remain tombstones
// until that participant leaves; delayed publish/unpublish cannot revive them.
class ShareModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {OwnerRole=Qt::UserRole+1,ShareIdRole,GenerationRole,Has1080Role,Has720Role,HasAudioRole};
    struct Announcement {
        QString owner,sid,name;
        bool audio=false,encrypted=false,screenSource=false;
        unsigned width=0,height=0;
    };
    explicit ShareModel(QObject* parent=nullptr);
    int rowCount(const QModelIndex& parent={})const override;
    QVariant data(const QModelIndex& index,int role)const override;
    QHash<int,QByteArray> roleNames()const override;
    bool announce(const Announcement& track);
    bool remove(const QString& sid);
    bool retire(const QString& owner,const QString& shareId,quint64 generation);
    void removeOwner(const QString& owner);
    void clear();
    QString sid(const QString& owner,const QString& shareId,ShareTrack track)const;
private:
    struct Entry {QString owner,id;quint64 generation=0;bool retired=false;QString video1080,video720,audio;};
    std::vector<Entry> entries_;
    void ownerThread()const;
};
// Pure validation shared by the UI model and transport-side catalog.
std::optional<ShareIdentity> validateShareAnnouncement(const ShareModel::Announcement& announcement);
}
