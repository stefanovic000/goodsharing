#include "audio_endpoint_clock.h"
#include <iostream>
#include <stdexcept>
using namespace goodsharing;
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){try{
    EndpointClockReading r{48000,48000,100000000,100010000,48960,1000000};
    require(endpointDeadline(r)==1019000,"Device clock/QPC bridge deadline mismatch");
    r.position*=4;r.frequency*=4;require(endpointDeadline(r)==1019000,"Device clock units assumed to be frames");
    auto bad=r;bad.frequency=0;require(!endpointDeadline(bad),"Unknown frequency accepted");
    bad=r;bad.position=0;require(!endpointDeadline(bad),"Unstarted device clock accepted");
    bad=r;bad.nowQpc100ns=r.positionQpc100ns-1;require(!endpointDeadline(bad),"Future QPC accepted");
    bad=r;bad.nowQpc100ns=r.positionQpc100ns+200001;require(!endpointDeadline(bad),"Stale device observation accepted");
    bad=r;bad.submittedFrames=47000;require(!endpointDeadline(bad),"Underrun invented a new write position");
    bad=r;bad.position=156372;bad.frequency=192000;bad.positionQpc100ns=100000000;bad.nowQpc100ns=100000010;bad.submittedFrames=39093;
    require(endpointLeadUs(bad).value_or(-1)==0&&endpointDeadline(bad).has_value(),"One-microsecond clock rounding triggered an underrun");
    bad=r;bad.submittedFrames=50000;require(!endpointDeadline(bad)&&endpointLeadUs(bad).value_or(0)>30000,"More than 30 ms lead accepted or backpressure lost");
    bad=r;bad.localNowUs=INT64_MAX;require(!endpointDeadline(bad),"Overflowing local domain accepted");
    bad=r;bad.submittedFrames=UINT64_MAX;require(!endpointDeadline(bad),"Overflowing submission count accepted");
    require(contiguousEndpointDeadline(1014892,1017589,1002248)==1017589,"Observed 2697 us backward step overlapped committed device frames");
    require(contiguousEndpointDeadline(1010000,1015000,1000000)==1015000,"Bounded contiguous correction rejected");
    require(!contiguousEndpointDeadline(1010000,1015001,1000000),"Correction beyond 5 ms accepted");
    require(!contiguousEndpointDeadline(1029000,1031000,1000000),"Contiguous correction exceeded 30 ms lead");
    require(!contiguousEndpointDeadline(999999,1001000,1000000),"Expired device observation replaced with invented timing");
    require(contiguousEndpointDeadline(1010000,0,1000000)==1010000,"Fresh endpoint retained an old committed boundary");
    require(contiguousEndpointDeadline(1018000,1015000,1000000)==1018000,"Forward device clock observation was delayed");
    require(!contiguousEndpointDeadline(INT64_MAX,0,1000000)&&!contiguousEndpointDeadline(1010000,0,INT64_MAX),"Extreme contiguous timing overflow accepted");
    for(int n=0;n<100;n++)require(contiguousEndpointDeadline(1010000,1015000,1000000)==1015000,"Repeated correction accumulated clock delay");
    for(std::uint64_t second=1;second<600;second++){
        r={second*192000,192000,second*10000000,second*10000000+1000,second*48000+960,1000000+static_cast<std::int64_t>(second)*1000000};
        require(endpointDeadline(r)==r.localNowUs+19900,"Device clock accumulated unit conversion drift");
    }
    std::cout<<"Device frequency/QPC-to-steady bridge, bounded lead, unstarted/stale clocks, underrun and overflow rejection passed; mathematical model only\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
