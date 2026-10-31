#include "audio_capture.h"
#include <QCoreApplication>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <chrono>
#include <thread>
#include <mutex>
#include <cmath>
#include <iostream>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    auto value=[&](const QString& name,QString fallback={}) { int i=args.indexOf(name); return i>=0&&i+1<args.size()?args[i+1]:fallback; };
    const auto mode=value("--mode","application"); const auto pid=value("--pid","0").toUInt();
    const int seconds=value("--seconds","5").toInt(); if(seconds<1||seconds>30) return 2;
    QProcess ownTone;
    if(mode=="desktop") {
        ownTone.start(app.applicationDirPath()+"/goodsharing_fixture.exe",{"--tone","660","--seconds",QString::number(seconds+5),"--no-window"});
        if(!ownTone.waitForStarted(5000)||!ownTone.waitForReadyRead(5000)) return 3;
    }
    std::mutex mutex; std::vector<std::int16_t> samples; QString error;
    goodsharing::AudioCapture capture;
    capture.start(mode=="desktop"?goodsharing::AudioMode::Desktop:goodsharing::AudioMode::Application,pid,[&](goodsharing::AudioBlock block) { std::lock_guard lock(mutex); for(std::size_t i=0;i<block.samples.size();i+=2) samples.push_back(block.samples[i]); },[&](QString problem) { std::lock_guard lock(mutex); error=problem; });
    std::this_thread::sleep_for(std::chrono::seconds(seconds)); capture.stop();
    if(ownTone.state()!=QProcess::NotRunning) { ownTone.terminate(); if(!ownTone.waitForFinished(3000)) { ownTone.kill(); ownTone.waitForFinished(3000); } }
    QJsonObject spectrum;
    for(const int hz:{440,660,880}) {
        double real=0,imag=0;
        for(std::size_t i=0;i<samples.size();i++) { const double phase=i*hz*2*3.141592653589793/48000; real+=samples[i]*std::cos(phase); imag+=samples[i]*std::sin(phase); }
        spectrum[QString::number(hz)]=samples.empty()?0:2*std::hypot(real,imag)/samples.size();
    }
    QJsonObject report{{"mode",mode},{"sample_rate",48000},{"channels",2},{"captured_frames",static_cast<int>(samples.size())},{"tone_amplitude",spectrum},{"error",error}};
    const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented); std::cout<<bytes.constData();
    const auto path=value("--report"); if(!path.isEmpty()) { QFile file(path); if(file.open(QIODevice::WriteOnly)) file.write(bytes); }
    return error.isEmpty()&&!samples.empty()?0:1;
}
