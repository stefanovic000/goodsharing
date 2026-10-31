#include "room_connection.h"
#include <livekit/livekit.h>
#include <livekit/local_participant.h>
#include <livekit/local_audio_track.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <iostream>
#include <thread>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
using Clock=std::chrono::steady_clock;
QJsonObject resources() {
    DWORD handles=0;GetProcessHandleCount(GetCurrentProcess(),&handles);
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))throw std::runtime_error("Resource query failed");
    const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Thread query failed");
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);int threads=0;QJsonObject names;
    if(Thread32First(snapshot,&entry)){do{if(entry.th32OwnerProcessID==GetCurrentProcessId()) {
        threads++;QString name="<unnamed>";
        if(const HANDLE thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID)) {
            PWSTR description=nullptr;
            if(SUCCEEDED(GetThreadDescription(thread,&description))&&description){if(*description)name=QString::fromWCharArray(description);LocalFree(description);}
            CloseHandle(thread);
        }
        names[name]=names[name].toInt()+1;
    }}while(Thread32Next(snapshot,&entry));}
    CloseHandle(snapshot);
    return {{"handles",static_cast<int>(handles)},{"threads",threads},{"private_bytes",static_cast<double>(memory.PrivateUsage)},{"thread_names",names}};
}
livekit::RoomOptions options() {livekit::RoomOptions o;o.join_retries=0;o.connect_timeout=std::chrono::milliseconds(1000);return o;}
bool waitJoin(goodsharing::RoomConnection& connection) {
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    while(connection.state()==goodsharing::RoomConnection::State::Connecting&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return connection.state()==goodsharing::RoomConnection::State::Connected;
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QJsonObject report;QJsonArray cycles;
    try {
        struct Sdk{Sdk(){livekit::initialize(livekit::LogLevel::Warn);}~Sdk(){livekit::shutdown();}}sdk;
        const auto url=qEnvironmentVariable("GOODSHARING_TEST_URL").toStdString();
        const auto token=qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN").toStdString();
        const int count=qEnvironmentVariableIsEmpty("GOODSHARING_LIFECYCLE_CYCLES")?20:qEnvironmentVariableIntValue("GOODSHARING_LIFECYCLE_CYCLES");
        const int idle=qEnvironmentVariableIntValue("GOODSHARING_LIFECYCLE_IDLE_SECONDS");
        if(count<20||count>100||idle<0||idle>15)throw std::runtime_error("Invalid bounded lifecycle diagnostic");
        for(int i=0;i<count;i++) {
            QJsonObject cycle;
            {
            const auto start=Clock::now();
            goodsharing::RoomConnection connection;
            connection.start(url,token,options());
            if(!waitJoin(connection)) throw std::runtime_error("Lifecycle join failed or exceeded deadline");
            auto source=std::make_shared<livekit::AudioSource>(48000,2);
            auto track=livekit::LocalAudioTrack::createLocalAudioTrack("lifecycle-silence",source);
            livekit::TrackPublishOptions publish;publish.source=livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO;publish.dtx=false;
            connection.room().localParticipant().lock()->publishTrack(track,publish);
            source->captureFrame(livekit::AudioFrame::create(48000,2,480),100);
            // Cover deferred offers and closing while native offer work can run.
            std::this_thread::sleep_for(std::chrono::milliseconds((i%5)*50));
            const auto stopStart=Clock::now();
            connection.stop();connection.stop(); // Idempotent and safe after success.
            const auto stopMs=std::chrono::duration<double,std::milli>(Clock::now()-stopStart).count();
            cycle=QJsonObject{{"cycle",i},{"stop_ms",stopMs},{"total_ms",std::chrono::duration<double,std::milli>(Clock::now()-start).count()},{"error",QString::fromStdString(connection.error())},{"disconnected",connection.room().connectionState()==livekit::ConnectionState::Disconnected}};
            if(stopMs>2000||!connection.error().empty()) {
                // Preserve the failing cycle as well as successful cycles.
                cycles.append(cycle);
                throw std::runtime_error("Lifecycle shutdown deadline/error gate failed: "+std::to_string(stopMs)+" ms; "+connection.error());
            }
            }
            cycle["resources"]=resources();cycles.append(cycle);
        }
        // Rejected credentials must clean up listeners and partial room state.
        {
            goodsharing::RoomConnection rejected;
            rejected.start(url,"invalid-test-token",options());
            const bool joined=waitJoin(rejected);
            rejected.stop();
            report["invalid_token_rejected"]=!joined&&rejected.room().connectionState()==livekit::ConnectionState::Disconnected;
        }
        // RAII cleanup on an exception after publication.
        int abandonedCleanups=0;
        for(int i=0;i<3;i++) {
            try {
                goodsharing::RoomConnection abandoned;
                abandoned.start(url,token,options());
                if(!waitJoin(abandoned))throw std::runtime_error("Abandoned join failed");
                auto source=std::make_shared<livekit::AudioSource>(48000,2);
                auto track=livekit::LocalAudioTrack::createLocalAudioTrack("abandoned",source);
                abandoned.room().localParticipant().lock()->publishTrack(track,livekit::TrackPublishOptions{});
                throw std::logic_error("controlled-after-publish");
            } catch(const std::logic_error& e) {
                if(std::string(e.what())!="controlled-after-publish")throw;
                abandonedCleanups++;
            }
        }
        report["exception_cleanup_cycles"]=abandonedCleanups;
        const auto cancelledUrl=qEnvironmentVariable("GOODSHARING_CANCEL_URL");
        if(!cancelledUrl.isEmpty()) {
            goodsharing::RoomConnection cancelled;
            cancelled.start(cancelledUrl.toStdString(),token,options());
            const auto deadline=Clock::now()+std::chrono::seconds(3);
            const auto accepted=qEnvironmentVariable("GOODSHARING_CANCEL_ACCEPTED");
            while(!QFile::exists(accepted)&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if(!QFile::exists(accepted))throw std::runtime_error("Stall fixture did not receive the connection");
            const auto cancelStart=Clock::now();cancelled.stop();
            report["cancel_during_connect_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-cancelStart).count();
            report["cancelled_state_disconnected"]=cancelled.room().connectionState()==livekit::ConnectionState::Disconnected;
        }
        report["cycles"]=cycles;
        QJsonArray idleSamples;
        // Diagnostics only: never replace the existing immediate resource gate
        // with a later, lower measurement or extend a Stop deadline.
        for(int second=0;second<idle;second++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            idleSamples.append(QJsonObject{{"seconds",second+1},{"resources",resources()}});
        }
        report["idle_resources"]=idleSamples;
        report["final_resources"]=resources();
    } catch(const std::exception&e){report["error"]=QString::fromUtf8(e.what());report["cycles"]=cycles;}
    const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);std::cout<<data.constData();
    const auto args=app.arguments();const int index=args.indexOf("--report");if(index>=0&&index+1<args.size()){QFile f(args[index+1]);if(f.open(QIODevice::WriteOnly))f.write(data);}
    return report.contains("error")?1:0;
}
