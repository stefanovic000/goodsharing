#include <QGuiApplication>
#include <QWindow>
#include <QBackingStore>
#include <QPainter>
#include <QTimer>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QCursor>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <chrono>

class FixtureWindow final:public QWindow {
    QBackingStore backing{this};
    QTimer timer;
    QElapsedTimer clock;
    qint64 nextFrameUs=0;
    unsigned frame=0;
    static constexpr qint64 framePeriodUs=1000000/60;
public:
    FixtureWindow() {
        setTitle("GoodSharing controlled test source"); setFlags(Qt::Window|Qt::FramelessWindowHint); resize(960,540);
        timer.setTimerType(Qt::PreciseTimer);timer.setSingleShot(true);
        QObject::connect(&timer,&QTimer::timeout,[this] { if(isExposed())render();scheduleNext(); });
        clock.start();nextFrameUs=framePeriodUs;timer.start(17);
    }
    void scheduleNext(){
        const auto now=clock.nsecsElapsed()/1000;
        nextFrameUs+=framePeriodUs;
        if(nextFrameUs<=now)nextFrameUs=now+framePeriodUs;
        timer.start(static_cast<int>(std::max<qint64>(1,(nextFrameUs-now+500)/1000)));
    }
    void render() {
        backing.resize(size()); backing.beginPaint(QRegion(0,0,width(),height()));
        QPainter painter(backing.paintDevice()); painter.fillRect(0,0,width(),height(),QColor("#17202e"));
        painter.setPen(Qt::white); painter.setFont(QFont("Segoe UI",22));
        painter.drawText(32,110,"GoodSharing capture fixture");
        painter.drawText(32,155,QString("Frame %1").arg(frame));
        painter.setFont(QFont("Consolas",12)); painter.drawText(32,210,"Controlled video and quiet test tone. Closes automatically.");
        painter.fillRect(static_cast<int>(frame*7%static_cast<unsigned>(width())),270,70,90,QColor("#80c7ff"));
        for(int bit=0;bit<16;bit++) painter.fillRect(bit*40,0,40,40,(frame>>bit)&1?Qt::white:Qt::black);
        for(int bit=0;bit<16;bit++) painter.fillRect(bit*40,40,40,40,(frame>>bit)&1?Qt::black:Qt::white);
        painter.end(); backing.endPaint(); backing.flush(QRegion(0,0,width(),height())); frame++;
    }
    unsigned freeze(){timer.stop();return (frame?frame-1:0)&0xffff;}
    void resume(){if(timer.isActive())return;nextFrameUs=clock.nsecsElapsed()/1000+framePeriodUs;timer.start(17);}
};

int main(int argc,char** argv) {
    QGuiApplication app(argc,argv);
    const auto args=app.arguments();
    QTimer quitSignal;const int quitAt=args.indexOf("--quit-file");
    if(quitAt>=0&&quitAt+1<args.size()){
        quitSignal.setInterval(50);QObject::connect(&quitSignal,&QTimer::timeout,&app,[&]{if(QFile::exists(args[quitAt+1]))app.quit();});quitSignal.start();
    }
    auto value=[&](const QString& name,int fallback) { int i=args.indexOf(name); return i>=0&&i+1<args.size()?args[i+1].toInt():fallback; };
    const int frequency=value("--tone",440),rightFrequency=value("--right-tone",frequency),seconds=value("--seconds",30);
    FixtureWindow window;if(args.contains("--hide-cursor"))window.setCursor(QCursor(Qt::BlankCursor));if(!args.contains("--no-window")) window.show();
    const int freeze=value("--static-after-ms",0);if(freeze>0)QTimer::singleShot(freeze,&window,[&]{
        const auto id=window.freeze();const int at=args.indexOf("--frozen-file");
        const auto localUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(at>=0&&at+1<args.size()){QFile file(args[at+1]);if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(QJsonObject{{"last_pixel_id",static_cast<int>(id)},{"freeze_local_us",static_cast<double>(localUs)}}).toJson());}
    });
    // Owned minimize/restore triggers for the source-minimized A/V case.
    QTimer minimizeSignal;const int minimizeAt=args.indexOf("--minimize-file");
    if(minimizeAt>=0&&minimizeAt+1<args.size()){
        minimizeSignal.setInterval(50);QObject::connect(&minimizeSignal,&QTimer::timeout,&window,[&]{
            if(QFile::exists(args[minimizeAt+1])){window.showMinimized();minimizeSignal.stop();}
        });minimizeSignal.start();
    }
    QTimer restoreSignal;const int restoreAt=args.indexOf("--restore-file");
    if(restoreAt>=0&&restoreAt+1<args.size()){
        restoreSignal.setInterval(50);QObject::connect(&restoreSignal,&QTimer::timeout,&window,[&]{
            if(QFile::exists(args[restoreAt+1])){window.showNormal();window.raise();window.requestActivate();restoreSignal.stop();}
        });restoreSignal.start();
    }    QTimer resumeSignal;const int resumeAt=args.indexOf("--resume-file");
    if(resumeAt>=0&&resumeAt+1<args.size()){
        resumeSignal.setInterval(50);QObject::connect(&resumeSignal,&QTimer::timeout,&window,[&]{
            if(QFile::exists(args[resumeAt+1])){window.resume();resumeSignal.stop();}
        });resumeSignal.start();
    }
    QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(2); format.setSampleFormat(QAudioFormat::Int16);
    const auto device=QMediaDevices::defaultAudioOutput();
    if(device.isNull()||!device.isFormatSupported(format)) return 2;
    QByteArray samples(48000*4,Qt::Uninitialized);
    auto* pcm=reinterpret_cast<qint16*>(samples.data());
    for(int i=0;i<48000;i++) { pcm[2*i]=static_cast<qint16>(std::sin(i*frequency*2*3.141592653589793/48000)*1600);pcm[2*i+1]=static_cast<qint16>(std::sin(i*rightFrequency*2*3.141592653589793/48000)*1600); }
    QBuffer buffer(&samples); buffer.open(QIODevice::ReadOnly);
    QAudioSink sink(device,format);
    QObject::connect(&sink,&QAudioSink::stateChanged,[&](QAudio::State s) { if(s==QAudio::IdleState) { buffer.seek(0); sink.start(&buffer); } });
    sink.start(&buffer);
    QJsonObject ready{{"pid",static_cast<int>(app.applicationPid())},{"hwnd",QString::number(window.winId())},{"tone_hz",frequency}};
    const int readyArg=args.indexOf("--ready-file");
    if(readyArg>=0&&readyArg+1<args.size()) { QFile file(args[readyArg+1]); if(file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(ready).toJson(QJsonDocument::Compact)); }
    std::cout<<QJsonDocument(ready).toJson(QJsonDocument::Compact).constData()<<std::endl;
    QTimer::singleShot(seconds*1000,&app,&QCoreApplication::quit);
    return app.exec();
}

