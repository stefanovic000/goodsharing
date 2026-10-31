#include "room_controller.h"
#include "share_model.h"
#include <livekit/livekit.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QImage>
#include <QJsonArray>
#include <QThread>
#include <chrono>
#include <mutex>
#include <set>
#include <iostream>
using namespace goodsharing;
using Clock=std::chrono::steady_clock;
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QJsonObject report;const auto args=app.arguments();
    auto value=[&](QString name){const int i=args.indexOf(name);return i>=0&&i+1<args.size()?args[i+1]:QString{};};
    try {
        struct SDK{SDK(){livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();}}sdk;
        const bool wrong=args.contains("--wrong-key"),audio=!args.contains("--audio-off");const int height=value("--quality")=="1080"?1080:720;
        std::mutex mutex;int frames=0,markerErrors=0,metadataErrors=0;std::uint64_t audioFrames=0,firstFrameUs=0,lastFrameUs=0,motionTraceOmitted=0;std::set<unsigned> ids;std::vector<std::int16_t> pcm;std::vector<double> ages;QJsonArray markerDetails,motionTrace;
        const bool traceMotion=qEnvironmentVariableIntValue("GOODSHARING_TEST_MOTION_TRACE")!=0;
        auto key=QByteArray::fromHex(qgetenv("GOODSHARING_TEST_E2EE_KEY"));if(wrong&&!key.isEmpty())key[0]^=0xff;
        RoomController room;ShareModel model;std::shared_ptr<RemoteShare> watch;QString selectedOwner,selectedId;
        QObject::connect(&room,&RoomController::trackAvailable,[&](QString owner,QString sid,QString name,bool isAudio,bool encrypted,bool screen,quint32 w,quint32 h){model.announce({owner,sid,name,isAudio,encrypted,screen,w,h});});
        if(!room.join(qEnvironmentVariable("GOODSHARING_TEST_URL"),qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),key))throw std::runtime_error("Viewer join rejected");
        const auto deadline=Clock::now()+std::chrono::seconds(16);
        while(Clock::now()<deadline) {
            QCoreApplication::processEvents();
            if(!watch&&room.state()==RoomController::State::Ready&&model.rowCount()) {
                const auto index=model.index(0);const bool hasVideo=model.data(index,height==1080?ShareModel::Has1080Role:ShareModel::Has720Role).toBool();
                if(hasVideo&&(!audio||model.data(index,ShareModel::HasAudioRole).toBool())) {
                    selectedOwner=model.data(index,ShareModel::OwnerRole).toString();selectedId=model.data(index,ShareModel::ShareIdRole).toString();
                    RemoteShare::Callbacks callbacks;
                    callbacks.video=[&](livekit::VideoFrameEvent& event){
                        const auto identity=event.metadata&&event.metadata->user_data?decodeFrameIdentity(*event.metadata->user_data):std::nullopt;
                        std::lock_guard lock(mutex);if(!identity){metadataErrors++;return;}if(identity->priming)return;
                        const int width=height==1080?1920:1280;if(event.frame.width()!=width||event.frame.height()!=height){markerErrors++;return;}
                        QJsonArray invalidBits;unsigned id=0;for(int bit=0;bit<16;bit++){
                            const int x=(bit*40+20)*width/960,top=20*height/540,bottom=60*height/540;
                            const bool first=event.frame.data()[4*(top*width+x)]>128,second=event.frame.data()[4*(bottom*width+x)]>128;
                            if(first==second){markerErrors++;invalidBits.append(QJsonObject{{"bit",bit},{"top_red",event.frame.data()[4*(top*width+x)]},{"bottom_red",event.frame.data()[4*(bottom*width+x)]}});}if(first)id|=1u<<bit;
                        }
                        if(!invalidBits.isEmpty()&&markerDetails.size()<8){
                            QString path;const auto screenshot=value("--screenshot");if(!screenshot.isEmpty()){path=screenshot+".error-"+QString::number(markerDetails.size()+1)+".png";QImage(event.frame.data(),width,height,width*4,QImage::Format_RGBA8888).save(path);}
                            markerDetails.append(QJsonObject{{"received_index",frames+1},{"source_pixel_id",static_cast<int>(id)},{"transport_frame_id",static_cast<double>(identity->frameId)},{"capture_us",static_cast<double>(identity->captureTimestampUs)},{"invalid_bits",invalidBits},{"screenshot",path}});
                        }
                        ids.insert(id);frames++;
                        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
                        if(!firstFrameUs)firstFrameUs=now;lastFrameUs=now;
                        ages.push_back((static_cast<double>(now)-identity->captureTimestampUs)/1000.);
                        if(traceMotion){
                            if(motionTrace.size()<8192)motionTrace.append(QJsonObject{{"received_index",frames+1},{"source_frame_id",static_cast<int>(id)},
                                {"transport_frame_id",static_cast<double>(identity->frameId)},{"capture_us",static_cast<double>(identity->captureTimestampUs)},
                                {"received_us",static_cast<double>(now)}});
                            else motionTraceOmitted++;
                        }
                        if(frames==30&&!value("--screenshot").isEmpty())QImage(event.frame.data(),width,height,width*4,QImage::Format_RGBA8888).save(value("--screenshot"));
                    };
                    callbacks.audio=[&](livekit::AudioFrameEvent& event){std::lock_guard lock(mutex);audioFrames+=event.frame.data().size()/2;if(pcm.size()<48000*2*5)pcm.insert(pcm.end(),event.frame.data().begin(),event.frame.data().end());};
                    watch=room.watchShare(selectedOwner,selectedId,model.data(index,ShareModel::GenerationRole).toULongLong(),height==1080?ShareTrack::Video1080:ShareTrack::Video720,audio,std::move(callbacks));
                }
            }
            QThread::msleep(2);
        }
        if(watch)room.unwatchShare(selectedOwner,selectedId);
        room.leave();const auto until=Clock::now()+std::chrono::seconds(4);
        while(room.state()!=RoomController::State::Idle&&Clock::now()<until){QCoreApplication::processEvents();QThread::msleep(2);}
        QFile pcmFile(value("--pcm"));if(!value("--pcm").isEmpty()&&pcmFile.open(QIODevice::WriteOnly))pcmFile.write(reinterpret_cast<const char*>(pcm.data()),pcm.size()*2);
        std::sort(ages.begin(),ages.end());const auto p95=ages.empty()?0:ages[static_cast<std::size_t>((ages.size()-1)*.95)];
        const bool passed=watch&&room.state()==RoomController::State::Idle&&metadataErrors==0&&markerErrors==0&&(wrong?(frames==0&&audioFrames==0):(frames>100&&ids.size()>60&&(!audio||audioFrames>48000)&&p95>0&&p95<=150));
        report={{"passed",passed},{"wrong_key",wrong},{"quality",height},{"frames",frames},{"distinct_pixel_ids",static_cast<int>(ids.size())},{"marker_errors",markerErrors},{"metadata_errors",metadataErrors},{"audio_sample_frames",static_cast<double>(audioFrames)},{"room_idle",room.state()==RoomController::State::Idle},{"watch",watch?watch->report():QJsonObject{}}};
        report["capture_to_decoded_p95_ms"]=p95;report["inspected_fps"]=lastFrameUs>firstFrameUs?(frames-1)*1000000./(lastFrameUs-firstFrameUs):0;report["marker_error_details"]=markerDetails;
        if(traceMotion){report["motion_trace"]=motionTrace;report["motion_trace_capacity"]=8192;report["motion_trace_omitted"]=static_cast<double>(motionTraceOmitted);}
    }catch(const std::exception&){report["error"]="Capture viewer operation failed";}
    const auto data=QJsonDocument(report).toJson();QFile output(value("--report"));if(output.open(QIODevice::WriteOnly))output.write(data);std::cout<<data.constData();return report["passed"].toBool()?0:1;
}
