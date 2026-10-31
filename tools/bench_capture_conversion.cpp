// Reproducible single-process comparison of captured-content conversion stages:
// the previous per-quality QImage scaling plus SDK BGRA->I420 round trip against
// the current libyuv pipeline. Timing and pixel evidence only; no media is
// published and no capture source is opened.
#include "capture_publisher_input.h"
#include <livekit/livekit.h>
#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace goodsharing;
namespace {
struct Stage {std::vector<double> us;};
std::uint64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
double valueAt(const std::vector<double>& sorted,double fraction){return sorted.empty()?0:sorted[std::min(sorted.size()-1,static_cast<std::size_t>(fraction*sorted.size()))];}
QJsonObject stats(Stage stage) {
    if(stage.us.empty())return QJsonObject{};
    std::sort(stage.us.begin(),stage.us.end());
    double sum=0;for(auto sample:stage.us)sum+=sample;
    return QJsonObject{{"mean_ms",sum/stage.us.size()/1000},{"p50_ms",valueAt(stage.us,.5)/1000},
        {"p95_ms",valueAt(stage.us,.95)/1000},{"maximum_ms",stage.us.back()/1000},{"samples",static_cast<int>(stage.us.size())}};
}
void fillRectangle(QImage& image,int left,int top,int width,int height,QRgb color) {
    for(int y=top;y<top+height;y++) {
        if(y<0||y>=image.height())continue;
        auto* row=reinterpret_cast<QRgb*>(image.scanLine(y));
        for(int x=left;x<left+width;x++)if(x>=0&&x<image.width())row[x]=color;
    }
}
// Same layout the owned fixture paints: sixteen 40x40 marker blocks, their
// inverse row, a moving square and high-frequency texture for realistic work.
// Windows Graphics Capture delivers premultiplied BGRA, which is the default
// model here; --format argb32 measures the straight-alpha composition path.
QImage makeSource(int width,int height,unsigned frame,QImage::Format format) {
    QImage image(width,height,format);
    image.fill(QColor("#17202e"));
    const double scale=static_cast<double>(width)/960;
    const int pitch=std::max(2,static_cast<int>(40*scale));
    for(int bit=0;bit<16;bit++) {
        const QRgb normal=((frame>>bit)&1)?qRgb(255,255,255):qRgb(0,0,0);
        const QRgb inverse=((frame>>bit)&1)?qRgb(0,0,0):qRgb(255,255,255);
        fillRectangle(image,bit*pitch,0,pitch,pitch,normal);
        fillRectangle(image,bit*pitch,pitch,pitch,pitch,inverse);
    }
    fillRectangle(image,static_cast<int>(frame*7%static_cast<unsigned>(width)),static_cast<int>(270*scale),
                  static_cast<int>(70*scale),static_cast<int>(90*scale),qRgb(128,199,255));
    for(int y=static_cast<int>(300*scale);y<std::min(height,static_cast<int>(540*scale));y++) {
        auto* row=reinterpret_cast<QRgb*>(image.scanLine(y));
        for(int x=0;x<width;x++){const unsigned noise=static_cast<unsigned>(x*2654435761u)^static_cast<unsigned>(y*40503u);row[x]=qRgb(noise&0xFF,(noise>>8)&0xFF,(noise>>16)&0xFF);}
    }
    return image;
}
// Previous production path: straight ARGB32 capture -> premultiplied ARGB32 ->
// copied BGRA -> SDK convert.
livekit::VideoFrame legacyPlanes(const QImage& image) {
    auto bounded=image.width()>1920||image.height()>1080?image.scaled(1920,1080,Qt::KeepAspectRatio,Qt::FastTransformation):image;
    auto straight=bounded.format()==QImage::Format_ARGB32?bounded:bounded.convertToFormat(QImage::Format_ARGB32);
    auto packed=straight.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(packed.width())*packed.height()*4);
    for(int y=0;y<packed.height();y++)std::memcpy(bytes.data()+static_cast<std::size_t>(y)*packed.width()*4,packed.constScanLine(y),static_cast<std::size_t>(packed.width())*4);
    return livekit::VideoFrame(packed.width(),packed.height(),livekit::VideoBufferType::BGRA,std::move(bytes)).convert(livekit::VideoBufferType::I420);
}
// Previous production canvas: three QImage plane scales, row copies, scalar
// UV interleave.
std::vector<std::uint8_t> legacyCanvas(const livekit::VideoFrame& planar,int width,int height) {
    const int sourceWidth=planar.width(),sourceHeight=planar.height();
    const double scale=std::min(static_cast<double>(width)/sourceWidth,static_cast<double>(height)/sourceHeight);
    const int fittedWidth=std::clamp(static_cast<int>(sourceWidth*scale)&~1,2,width);
    const int fittedHeight=std::clamp(static_cast<int>(sourceHeight*scale)&~1,2,height);
    const int left=((width-fittedWidth)/2)&~1,top=((height-fittedHeight)/2)&~1;
    const auto luma=static_cast<std::size_t>(width)*height;
    std::vector<std::uint8_t> output(luma*3/2,128);std::fill_n(output.begin(),luma,16);
    const auto planes=planar.planeInfos();
    auto scaled=[&](int plane,int w,int h){const auto& p=planes[plane];return QImage(reinterpret_cast<const uchar*>(p.data_ptr),plane?(sourceWidth+1)/2:sourceWidth,plane?(sourceHeight+1)/2:sourceHeight,p.stride,QImage::Format_Grayscale8).scaled(w,h,Qt::IgnoreAspectRatio,Qt::FastTransformation);};
    const auto y=scaled(0,fittedWidth,fittedHeight),u=scaled(1,fittedWidth/2,fittedHeight/2),v=scaled(2,fittedWidth/2,fittedHeight/2);
    for(int row=0;row<fittedHeight;row++)std::memcpy(output.data()+static_cast<std::size_t>(top+row)*width+left,y.constScanLine(row),fittedWidth);
    for(int row=0;row<fittedHeight/2;row++) {
        auto* uv=output.data()+luma+static_cast<std::size_t>(top/2+row)*width+left;
        const auto* up=u.constScanLine(row);const auto* vp=v.constScanLine(row);
        for(int x=0;x<fittedWidth/2;x++){uv[x*2]=up[x];uv[x*2+1]=vp[x];}
    }
    return output;
}
unsigned decodeMarker(const std::vector<std::uint8_t>& nv12,int width,int height,int& invalid) {
    unsigned id=0;invalid=0;
    const int sampleY=std::clamp(static_cast<int>(static_cast<double>(20)*height/540),0,height-1);
    const int inverseY=std::clamp(static_cast<int>(static_cast<double>(60)*height/540),0,height-1);
    for(int bit=0;bit<16;bit++) {
        const int x=std::clamp(static_cast<int>((bit*40+20)*static_cast<double>(width)/960),0,width-1);
        const bool normal=nv12[static_cast<std::size_t>(sampleY)*width+x]>128;
        const bool inverse=nv12[static_cast<std::size_t>(inverseY)*width+x]>128;
        if(normal==inverse)invalid++;
        if(normal)id|=1u<<bit;
    }
    return id;
}
double meanLumaDifference(const std::vector<std::uint8_t>& a,const std::vector<std::uint8_t>& b,std::size_t samples) {
    if(a.size()!=b.size()||!samples)return -1;
    double total=0;
    for(std::size_t i=0;i<samples;i++)total+=std::abs(static_cast<int>(a[i])-static_cast<int>(b[i]));
    return total/samples;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();
    auto value=[&](const QString& name,QString fallback) {int i=args.indexOf(name);return i>=0&&i+1<args.size()?args[i+1]:fallback;};
    const auto size=value("--source","960x540").split('x');
    const int width=size.value(0).toInt(),height=size.value(1).toInt();
    const int iterations=std::max(1,value("--iterations","60").toInt());
    const auto format=value("--format","premultiplied");
    if(format!="premultiplied"&&format!="argb32")throw std::invalid_argument("--format must be premultiplied or argb32");
    try {
        struct SDK{SDK(){livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();}}sdk;
        const auto frame=makeSource(width,height,0xA5C3u,format=="premultiplied"?QImage::Format_ARGB32_Premultiplied:QImage::Format_ARGB32);
        Stage legacySource,legacy1080,legacy720,currentSource,current1080,current720;
        for(int i=0;i<iterations+3;i++) {const bool measured=i>=3;
            const auto began=nowUs();auto legacy=legacyPlanes(frame);
            if(measured)legacySource.us.push_back(static_cast<double>(nowUs()-began));
            const auto first=nowUs();auto legacyCanvas1080=legacyCanvas(legacy,1920,1080);
            if(measured)legacy1080.us.push_back(static_cast<double>(nowUs()-first));
            const auto second=nowUs();auto legacyCanvas720=legacyCanvas(legacy,1280,720);
            if(measured)legacy720.us.push_back(static_cast<double>(nowUs()-second));
            PlaneImageI420 planes;
            const auto third=nowUs();capturePlanesI420(frame,planes);
            if(measured)currentSource.us.push_back(static_cast<double>(nowUs()-third));
            const auto fourth=nowUs();auto currentCanvas1080=captureCanvasNv12(planes,1920,1080);
            if(measured)current1080.us.push_back(static_cast<double>(nowUs()-fourth));
            const auto fifth=nowUs();auto currentCanvas720=captureCanvasNv12(planes,1280,720);
            if(measured)current720.us.push_back(static_cast<double>(nowUs()-fifth));
            if(i==iterations+2) {
                int legacyInvalid=0,currentInvalid=0;
                const unsigned legacyId=decodeMarker(legacyCanvas1080,1920,1080,legacyInvalid);
                const unsigned currentId=decodeMarker(currentCanvas1080,1920,1080,currentInvalid);
                const auto luma=static_cast<std::size_t>(1920)*1080;
                const QJsonObject report{
                    {"source",QJsonObject{{"width",width},{"height",height},{"format",format}}},{"iterations",iterations},
                    {"legacy",QJsonObject{{"source_i420",stats(legacySource)},{"canvas_1080",stats(legacy1080)},{"canvas_720",stats(legacy720)},
                        {"marker_id",static_cast<int>(legacyId)},{"marker_invalid_bits",legacyInvalid}}},
                    {"current",QJsonObject{{"source_i420",stats(currentSource)},{"canvas_1080",stats(current1080)},{"canvas_720",stats(current720)},
                        {"marker_id",static_cast<int>(currentId)},{"marker_invalid_bits",currentInvalid}}},
                    {"difference",QJsonObject{{"mean_abs_luma_1080",meanLumaDifference(legacyCanvas1080,currentCanvas1080,luma)},
                        {"mean_abs_luma_720",meanLumaDifference(legacyCanvas720,currentCanvas720,static_cast<std::size_t>(1280)*720)},
                        {"same_marker_id",legacyId==currentId}}}};
                const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);
                std::cout<<bytes.constData();
                const auto path=value("--report",{});
                if(!path.isEmpty()){QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())throw std::runtime_error("Could not write the report");}
                // Luma-only side-by-side panels make the filter change
                // inspectable: previous 720p canvas, current 720p canvas and the
                // 1080p canvas the downscale starts from.
                const auto dump=value("--dump",{});
                if(!dump.isEmpty()) {
                    const auto panel=[](const std::vector<std::uint8_t>& nv12,int panelWidth,int panelHeight) {
                        return QImage(nv12.data(),panelWidth,panelHeight,panelWidth,QImage::Format_Grayscale8).copy();
                    };
                    const QImage previous=panel(legacyCanvas720,1280,720),current=panel(currentCanvas720,1280,720),source=panel(currentCanvas1080,1920,1080);
                    QImage sheet(source.width()+previous.width()+current.width()+16,std::max({source.height(),previous.height(),current.height()}),QImage::Format_RGB32);
                    sheet.fill(QColor("#202020"));
                    int x=0;
                    for(const auto& image:{source,previous,current}) {
                        for(int row=0;row<image.height();row++) {
                            const auto* in=image.constScanLine(row);
                            auto* out=reinterpret_cast<QRgb*>(sheet.scanLine(row))+x;
                            for(int column=0;column<image.width();column++){const int value=in[column];out[column]=qRgb(value,value,value);}
                        }
                        x+=image.width()+8;
                    }
                    if(!sheet.save(dump))throw std::runtime_error("Could not write the comparison image");
                }
            }
        }
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
