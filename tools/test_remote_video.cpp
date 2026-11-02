#include "remote_video.h"
#include <QCoreApplication>
#include <QThread>
#include <iostream>
#include <stdexcept>
using namespace goodsharing;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
livekit::VideoFrameEvent frame(unsigned id=1,bool priming=false,int width=1280) {
    livekit::VideoFrameEvent event;event.frame=livekit::VideoFrame::create(width,720,livekit::VideoBufferType::RGBA);
    for(std::size_t n=0;n<event.frame.dataSize();n+=4){event.frame.data()[n]=static_cast<unsigned char>(id);event.frame.data()[n+1]=200;event.frame.data()[n+3]=255;}
    event.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;event.metadata=livekit::VideoFrameMetadata{};
    event.metadata->user_data=encodeFrameIdentity({999999999999999ull,id,priming});return event;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const RemoteShare::Manifest manifest{"alice","0123456789abcdef0123456789abcdef",1,ShareTrack::Video720,"video",{}};
    auto permission=std::make_shared<std::atomic_bool>(true);
    auto watch=std::make_shared<RemoteShare>(manifest,RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},{}},permission);
    RemoteVideo video;video.bind(watch);QImage canvas(320,180,QImage::Format_RGB32);
    auto draw=[&]{canvas.fill(Qt::black);QPainter painter(&canvas);return video.draw(painter,canvas.rect());};
    auto first=frame();video.submit(first);require(!draw()&&canvas.pixelColor(100,100)==QColor(Qt::black),"Unverified pixels were drawn");
    watch->encryption("alice","video",livekit::EncryptionState::Ok);
    auto priming=frame(2,true);video.submit(priming);require(!draw(),"Synthetic priming was presented");
    auto invalid=frame(3,false,640);video.submit(invalid);require(!draw(),"Wrong-size frame was presented");
    auto missing=frame(4);missing.metadata.reset();video.submit(missing);require(!draw(),"Missing metadata was presented");
    for(unsigned id=10;id<30;id++){auto newest=frame(id);video.submit(newest);}
    require(draw()&&canvas.pixelColor(100,100).red()==29,"The handoff did not retain only the newest pixels");
    require(video.report()["pending_images"].toInt()==1&&video.report()["drawn_distinct_frames"].toInt()==1,"Presentation queue was unbounded or priming counted");
    QThread::msleep(165);video.changed();require(draw()&&video.report()["held_image_without_update"].toBool(),"An authorized static picture was discarded or marked live");
    video.submit(first);QThread::msleep(165);require(video.changed()&&draw()&&video.report()["expired_or_revoked_images"].toInt()>=1,"A late picture was neither painted nor counted as expired");
    video.submit(first);require(draw(),"Fresh frame did not recover from local expiry");
    watch->revoke();require(!draw()&&canvas.pixelColor(100,100)==QColor(Qt::black),"Queued pixels survived revocation at draw time");
    watch->encryption("alice","video",livekit::EncryptionState::Ok);video.submit(first);require(!draw(),"Revoked selection reopened");
    auto second=std::make_shared<RemoteShare>(manifest,RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},{}},permission);
    RemoteVideo interrupted;interrupted.bind(second);second->encryption("alice","video",livekit::EncryptionState::Ok);interrupted.submit(first);
    *permission=false;canvas.fill(Qt::black);{QPainter painter(&canvas);require(!interrupted.draw(painter,canvas.rect()),"Room invalidation left pixels admitted");}
    bool rebound=false;try{video.bind(second);}catch(const std::logic_error&){rebound=true;}require(rebound,"Old frame storage was rebound to a new watch");
    require(video.report()["invalid_frames"].toInt()==2,"Invalid input was not accounted");
    std::cout<<"Remote presentation blocks unverified, malformed, priming, stale, revoked and room-invalidated images; latest-frame handoff and independent sender clock passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
