#include "capture_publisher_input.h"
#include <livekit/livekit.h>
#include <QCoreApplication>
#include <QImage>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace goodsharing;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
namespace {
void fillRectangle(QImage& image,int left,int top,int width,int height,QRgb color) {
    for(int y=top;y<top+height;y++) {
        if(y<0||y>=image.height())continue;
        auto* row=reinterpret_cast<QRgb*>(image.scanLine(y));
        for(int x=left;x<left+width;x++)if(x>=0&&x<image.width())row[x]=color;
    }
}
// Same marker layout the owned fixture paints: sixteen 40x40 blocks plus their
// inverse row, sampled at the block centres as the viewer does.
QImage markerSource(unsigned id) {
    QImage image(960,540,QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor("#17202e"));
    for(int bit=0;bit<16;bit++) {
        const QRgb normal=((id>>bit)&1)?qRgb(255,255,255):qRgb(0,0,0);
        const QRgb inverse=((id>>bit)&1)?qRgb(0,0,0):qRgb(255,255,255);
        fillRectangle(image,bit*40,0,40,40,normal);
        fillRectangle(image,bit*40,40,40,40,inverse);
    }
    return image;
}
unsigned decodeMarker(const std::vector<std::uint8_t>& nv12,int width,int height) {
    unsigned id=0;
    const int sampleY=static_cast<int>(20.0*height/540),inverseY=static_cast<int>(60.0*height/540);
    for(int bit=0;bit<16;bit++) {
        const int x=static_cast<int>((bit*40+20)*(static_cast<double>(width)/960));
        const bool normal=nv12[static_cast<std::size_t>(sampleY)*width+x]>128;
        const bool inverse=nv12[static_cast<std::size_t>(inverseY)*width+x]>128;
        require(normal!=inverse,"Marker block does not survive the canvas conversion");
        if(normal)id|=1u<<bit;
    }
    return id;
}
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    try {
        struct SDK{SDK(){livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();}}sdk;
        for(auto color:{Qt::red,Qt::blue,Qt::white}) {
            QImage source(80,40,QImage::Format_ARGB32);source.fill(color);
            const auto output=captureCanvasNv12(source,160,80);
            const auto y=output[40*160+80],u=output[160*80],v=output[160*80+1];
            require(output.size()==160*80*3/2,"NV12 dimensions incorrect");
            if(color==Qt::red)require(y>70&&y<95&&u<110&&v>220,"Red/blue channel or UV order incorrect");
            if(color==Qt::blue)require(y>30&&y<60&&u>220&&v<130,"Blue conversion incorrect");
            if(color==Qt::white)require(y>225&&u>120&&u<136&&v>120&&v<136,"White conversion incorrect");
        }
        for(const QSize size:{QSize(40,80),QSize(160,40),QSize(81,83)}) {
            QImage source(size,QImage::Format_ARGB32);source.fill(Qt::white);
            const auto output=captureCanvasNv12(source,160,90);
            require(output[45*160+80]>225,"Content not centered");
            if(size.width()<size.height())require(output[45*160+1]<25,"Portrait pillarbox missing");
            else require(output[160+80]<25,"Ultrawide letterbox missing");
        }
        QImage transparent(80,40,QImage::Format_ARGB32);transparent.fill(qRgba(255,0,0,0));
        require(captureCanvasNv12(transparent,160,80)[40*160+80]<25,"Transparent content was not composited over black");
        // Premultiplied capture surfaces need no extra composition; a fully
        // transparent premultiplied pixel is already black.
        QImage premultiplied(80,40,QImage::Format_ARGB32_Premultiplied);premultiplied.fill(qRgba(0,0,0,0));
        require(captureCanvasNv12(premultiplied,160,80)[40*160+80]<25,"Premultiplied capture was not passed through");
        bool rejected=false;try{captureCanvasNv12(QImage{},160,90);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"Empty source accepted");
        // One captured frame must feed both qualities with the same content.
        const unsigned id=0x5A3Cu;
        PlaneImageI420 planes;capturePlanesI420(markerSource(id),planes);
        require(planes.width==960&&planes.height==540,"Captured plane geometry incorrect");
        require(planes.planes[0].size()==960u*540u,"Luma plane size incorrect");
        require(planes.planes[1].size()==480u*270u&&planes.planes[2].size()==480u*270u,"Chroma plane size incorrect");
        const auto hd=captureCanvasNv12(planes,1920,1080);
        require(hd.size()==1920u*1080u*3/2,"1080p canvas size incorrect");
        require(decodeMarker(hd,1920,1080)==id,"1080p canvas changed the source marker");
        const auto sd=captureCanvasNv12(planes,1280,720);
        require(sd.size()==1280u*720u*3/2,"720p canvas size incorrect");
        require(decodeMarker(sd,1280,720)==id,"720p canvas changed the source marker");
        // A square source must be pillarboxed with neutral chroma outside the
        // fitted area, at both qualities.
        QImage square(480,480,QImage::Format_ARGB32);square.fill(Qt::white);
        PlaneImageI420 squarePlanes;capturePlanesI420(square,squarePlanes);
        for(const QSize canvas:{QSize(1920,1080),QSize(1280,720)}) {
            const auto output=captureCanvasNv12(squarePlanes,canvas.width(),canvas.height());
            const auto luma=static_cast<std::size_t>(canvas.width())*canvas.height();
            require(output[static_cast<std::size_t>(canvas.height()/2)*canvas.width()+1]<25,"Square pillarbox left edge is not black");
            require(output[static_cast<std::size_t>(canvas.height()/2)*canvas.width()+canvas.width()-1]<25,"Square pillarbox right edge is not black");
            require(output[luma+static_cast<std::size_t>(canvas.height()/4)*canvas.width()]==128,"Square pillarbox chroma is not neutral");
        }
        // Red content distinguishes neutral letterbox chroma from the content's
        // own chroma, which white cannot.
        QImage redSquare(480,480,QImage::Format_ARGB32);redSquare.fill(Qt::red);
        PlaneImageI420 redSquarePlanes;capturePlanesI420(redSquare,redSquarePlanes);
        const auto redSquareCanvas=captureCanvasNv12(redSquarePlanes,1920,1080);
        require(redSquareCanvas[1920u*1080u+270u*1920]==128,"Red pillarbox chroma is not neutral");
        require(redSquareCanvas[1920u*1080u+270u*1920+960]<110,"Red pillarbox hid the content chroma");
        // A source above the 1080p cap is scaled down before conversion; both
        // canvases must then be filled from the capped planes.
        QImage uhd(3840,2160,QImage::Format_ARGB32);uhd.fill(Qt::white);
        PlaneImageI420 uhdPlanes;capturePlanesI420(uhd,uhdPlanes);
        require(uhdPlanes.width==1920&&uhdPlanes.height==1080,"Above-cap source was not bounded to 1080p");
        for(const QSize canvas:{QSize(1920,1080),QSize(1280,720)}) {
            const auto output=captureCanvasNv12(uhdPlanes,canvas.width(),canvas.height());
            require(output.size()==static_cast<std::size_t>(canvas.width())*canvas.height()*3/2,"Above-cap canvas size incorrect");
            require(output[static_cast<std::size_t>(canvas.height()/2)*canvas.width()+canvas.width()/2]>225,"Above-cap content lost");
        }
        // A 21:9 4K source is bounded to 1920x810 and letterboxed inside 1080p.
        QImage ultrawide(5120,2160,QImage::Format_ARGB32);ultrawide.fill(Qt::white);
        PlaneImageI420 ultrawidePlanes;capturePlanesI420(ultrawide,ultrawidePlanes);
        require(ultrawidePlanes.width==1920&&ultrawidePlanes.height==810,"Ultrawide cap geometry incorrect");
        const auto letterboxed=captureCanvasNv12(ultrawidePlanes,1920,1080);
        require(letterboxed[0]<25&&letterboxed[1919]<25,"Ultrawide letterbox top row is not black");
        require(letterboxed[static_cast<std::size_t>(540)*1920+960]>225,"Ultrawide content is not centred");
        require(letterboxed[1920u*1080u]==128,"Ultrawide letterbox chroma is not neutral");
        QImage redUltrawide(5120,2160,QImage::Format_ARGB32);redUltrawide.fill(Qt::red);
        PlaneImageI420 redUltrawidePlanes;capturePlanesI420(redUltrawide,redUltrawidePlanes);
        const auto redLetterboxed=captureCanvasNv12(redUltrawidePlanes,1920,1080);
        require(redLetterboxed[1920u*1080u]==128,"Red ultrawide letterbox chroma is not neutral");
        require(redLetterboxed[1920u*1080u+270u*1920+960]<110,"Red ultrawide content chroma is missing");
        // Odd source dimensions keep their exact plane strides through both
        // qualities instead of being rounded away.
        QImage odd(961,541,QImage::Format_ARGB32);odd.fill(Qt::white);
        PlaneImageI420 oddPlanes;capturePlanesI420(odd,oddPlanes);
        require(oddPlanes.width==961&&oddPlanes.height==541,"Odd source geometry changed");
        require(oddPlanes.planes[1].size()==481u*271u,"Odd source chroma size incorrect");
        for(const QSize canvas:{QSize(1920,1080),QSize(1280,720)}) {
            const auto output=captureCanvasNv12(oddPlanes,canvas.width(),canvas.height());
            require(output.size()==static_cast<std::size_t>(canvas.width())*canvas.height()*3/2,"Odd source canvas size incorrect");
            require(output[static_cast<std::size_t>(canvas.height()/2)*canvas.width()+canvas.width()/2]>225,"Odd source content lost");
        }
        // A 2x2 source still produces a valid even-aligned canvas.
        QImage tiny(2,2,QImage::Format_ARGB32);tiny.fill(Qt::white);
        const auto tinyCanvas=captureCanvasNv12(tiny,160,90);
        require(tinyCanvas.size()==160u*90u*3/2&&tinyCanvas[45*160+80]>225,"2x2 source canvas incorrect");
        // Straight alpha above the cap must still composite over black after the
        // bounded downscale (cap and attenuation scratch both in use).
        QImage transparentUhd(3840,2160,QImage::Format_ARGB32);transparentUhd.fill(qRgba(255,0,0,0));
        PlaneImageI420 transparentPlanes;capturePlanesI420(transparentUhd,transparentPlanes);
        require(captureCanvasNv12(transparentPlanes,1920,1080)[540u*1920+960]<25,"Above-cap transparency was not composited over black");
        auto gate=std::make_shared<EncryptionGate>(1);auto input=std::make_shared<ShareInput>("0123456789abcdef0123456789abcdef",false,gate);
        CapturePublisherInput capture;rejected=false;
        try{capture.start({1,false,AudioMode::Off,0},input);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Capture started before encryption readiness");
        std::cout<<"Capture color/UV order, letterboxing, premultiplied passthrough, shared-plane quality canvases, above-cap, odd and tiny sources and pre-readiness rejection passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
