#include "frame_metadata_codec.h"
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

int main() {
    try {
        auto verify=[](goodsharing::FrameIdentity identity) {
            const auto wire=goodsharing::encodeFrameIdentity(identity);
            if(wire.size()!=22||goodsharing::decodeFrameIdentity(wire)!=identity) throw std::runtime_error("Frame identity was not preserved");
            for(const auto byte:wire) if(byte>=128||(byte^0xff)<128) throw std::runtime_error("Unsafe bytes in H.264 metadata envelope");
        };
        for(const auto timestamp:{0ULL,1ULL,0x000001fffffebe00ULL,std::numeric_limits<unsigned long long>::max()})
            for(const auto id:{0U,1U,65535U,std::numeric_limits<unsigned>::max()}) for(const bool priming:{false,true}) verify({timestamp,id,priming});
        std::mt19937_64 random(0x47533031);
        for(int i=0;i<100000;i++) verify({random(),static_cast<std::uint32_t>(random()),(random()&1)!=0});
        auto wire=goodsharing::encodeFrameIdentity({123,456});
        wire[3]='3'; if(goodsharing::decodeFrameIdentity(wire)) throw std::runtime_error("Unknown protocol version accepted");
        wire=goodsharing::encodeFrameIdentity({123,456}); wire.back()='!';
        if(goodsharing::decodeFrameIdentity(wire)) throw std::runtime_error("Malformed Base64 accepted");
        wire.pop_back(); if(goodsharing::decodeFrameIdentity(wire)) throw std::runtime_error("Truncated metadata accepted");
        std::cout<<"100032 round trips, H.264 byte-safety invariant, and malformed-envelope checks passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
