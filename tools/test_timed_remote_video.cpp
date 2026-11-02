#include "remote_video.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
using namespace goodsharing;
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
livekit::VideoFrameEvent frame(unsigned id,std::int64_t deadline){
    livekit::VideoFrameEvent e;e.frame=livekit::VideoFrame::create(1280,720,livekit::VideoBufferType::RGBA);
    for(std::size_t n=0;n<e.frame.dataSize();n+=4){e.frame.data()[n]=static_cast<unsigned char>(id);e.frame.data()[n+3]=255;}
    e.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;e.metadata=livekit::VideoFrameMetadata{};
    e.metadata->user_data=encodeFrameIdentity({999999999999999ull,id,false});e.sender_ntp_us=deadline;return e;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    std::int64_t now=1000000;std::uint64_t serial=1;unsigned paints=0;
    RemoteVideo video([&]{return now;});
    video.setTiming({[&](livekit::VideoFrameEvent& e)->std::optional<PresentationTicket>{return PresentationTicket{*e.sender_ntp_us,4000000000000000ll,serial};},
        [&](PresentationTicket t){return t.serial==serial;},[&](PresentationTicket){paints++;},true});
    auto watch=std::make_shared<RemoteShare>(RemoteShare::Manifest{"alice","0123456789abcdef0123456789abcdef",1,ShareTrack::Video720,"video",{}},RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},{}});
    video.bind(watch);watch->encryption("alice","video",livekit::EncryptionState::Ok);
    QImage canvas(320,180,QImage::Format_RGB32);auto draw=[&]{canvas.fill(Qt::black);QPainter painter(&canvas);return video.draw(painter,canvas.rect());};
    auto first=frame(11,1030000),second=frame(22,1046000);video.submit(first);video.submit(second);
    now=1020000;require(!draw(),"Video painted before its common deadline");
    now=1035000;require(draw()&&canvas.pixelColor(100,100).red()==11&&paints==0,"Future frame replaced the due picture or painting pretended to queue a frame");
    video.frameQueued();require(paints==1,"Queued frame did not notify the common timeline");
    require(draw()&&paints==1,"Held picture invented a new presentation event");
    now=1050000;require(draw()&&canvas.pixelColor(100,100).red()==22&&paints==1,"Second due picture was not selected");video.frameQueued();require(paints==2,"Second queued frame notification missing");
    for(unsigned n=30;n<39;n++){auto e=frame(n,1060000+(n-30)*16000);video.submit(e);}
    require(video.report()["scheduled_queue_high_water"].toInt()<=video.report()["scheduled_queue_capacity"].toInt()&&video.report()["pending_images"].toInt()<=video.report()["scheduled_queue_capacity"].toInt()+1,"Scheduled images exceeded fixed capacity");
    now=1160000;require(draw()&&canvas.pixelColor(100,100).red()==36,"Renderer did not choose the newest due picture");
    serial++;video.changed();require(!draw()&&canvas.pixelColor(100,100)==QColor(Qt::black),"Old-timeline pixels survived a clock reset");
    auto stale=frame(66,1000000);video.submit(stale);require(!draw(),"Late first paint passed the deadline gate");
    auto final=frame(77,1170000);video.submit(final);watch->revoke();now=1170000;require(!draw(),"Revoked scheduled image was painted");
    std::cout<<"Timed video holds future frames, chooses newest due picture, bounds storage and rejects stale/reset/revoked pixels\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
