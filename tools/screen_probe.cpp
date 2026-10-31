#include "screen_capture.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <windows.h>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    auto value=[&](const QString& name,QString fallback={}) { int i=args.indexOf(name); return i>=0&&i+1<args.size()?args[i+1]:fallback; };
    const bool monitor=args.contains("--monitor");
    HWND hwnd=nullptr;
    DWORD pid=0;
    if(monitor) {
        EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR value,HDC,LPRECT,LPARAM output)->BOOL {
            *reinterpret_cast<HMONITOR*>(output)=value;return FALSE;
        },reinterpret_cast<LPARAM>(&hwnd));
        if(!hwnd)return 2;
    } else {
        hwnd=reinterpret_cast<HWND>(value("--hwnd").toULongLong());
        GetWindowThreadProcessId(hwnd,&pid);
        if(!pid||pid!=value("--pid").toUInt()) return 2;
    }
    std::mutex mutex; QString error; int frames=0,width=0,height=0; std::set<unsigned> ids;
    goodsharing::ScreenCapture capture;
    capture.start(reinterpret_cast<std::uintptr_t>(hwnd),monitor,[&](goodsharing::ScreenFrame frame) {
        std::lock_guard lock(mutex); frames++; width=frame.image.width(); height=frame.image.height();
        unsigned id=0; if(width>=640&&height>20) { for(int b=0;b<16;b++) if(qRed(frame.image.pixel(b*40+20,20))>128) id|=1u<<b; ids.insert(id); }
    },[&](QString problem) { std::lock_guard lock(mutex); error=problem; });
    std::this_thread::sleep_for(std::chrono::seconds(5)); capture.stop();
    const QJsonObject report{{"frames",frames},{"width",width},{"height",height},{"distinct_frame_ids",static_cast<int>(ids.size())},{"error",error}};
    const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented); std::cout<<bytes.constData();
    QFile file(value("--report")); if(file.open(QIODevice::WriteOnly)) file.write(bytes);
    const bool pixels_valid=monitor?frames>200:ids.size()>200;
    return frames>200&&pixels_valid&&error.isEmpty()?0:1;
}
