#pragma once
#include <string_view>
#include <cstdint>

namespace goodsharing {
// Only return application-owned labels. SDK exception text can contain remote
// data and must never be copied into a report.
inline const char* publicationFailureCode(std::string_view message) {
    if(message=="Audio capture callback timed out")return "audio_callback_timeout_ambiguous";
    if(message=="an RtcError occurred: InvalidState - failed to capture frame without buffering")return "audio_native_rejected";
    if(message=="Audio permission lost"||message=="Outgoing video permission revoked"||message=="Priming revoked")return "permission_lost";
    if(message=="Hardware encoder output exceeded its age budget")return "encoder_output_expired";
    if(message=="Hardware encoder backlog exceeded 16 admitted frames")return "encoder_backlog";
    if(message=="Encoder failed to produce a fresh recovery keyframe")return "encoder_recovery_exhausted";
    if(message=="Encoder output has no admitted frame identity")return "encoder_identity_missing";
    if(message=="Encoded video handoff rejected; restart from a fresh keyframe")return "video_sdk_rejected";
    if(message=="EncodedVideoSource: missing capture response")return "video_capture_response_missing";
    if(message=="EncodedVideoSource: missing feedback response")return "video_feedback_response_missing";
    if(message=="EncodedVideoSource: invalid FFI handle")return "video_handle_invalid";
    constexpr std::string_view steps[]{"Set encoder control","Receive encoded frame","Read encoder event","Hardware encoder event","Submit hardware frame"};
    for(auto step:steps)if(message.starts_with(step)&&message.substr(step.size()).starts_with(" failed: 0x"))return "hardware_encoder_error";
    return "unclassified_exception";
}
// The pinned unbuffered native source returns this error before OnData, so a
// confirmed expired rejection has no ambiguous completion and can be dropped.
// Timeout/unknown completion must still stop; never retry the same block.
inline bool expiredAudioRejected(std::string_view message,std::uint64_t capture,std::uint64_t now){
    return message=="an RtcError occurred: InvalidState - failed to capture frame without buffering"&&
        capture>0&&now>capture&&now-capture>100000;
}
}
