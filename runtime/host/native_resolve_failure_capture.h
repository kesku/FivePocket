#pragma once
#include "gta5_tile_resolve_capture.h"
#include <ostream>
#include <string>

namespace recomp::gpu {
enum class NativeResolveFailureStage { DisplayMissing, DisplayInvalid, ReturnedFalse, Exception };
inline const char* nativeResolveFailureStageName(NativeResolveFailureStage stage) {
    switch(stage) {
    case NativeResolveFailureStage::DisplayMissing:return "display-owner-missing";
    case NativeResolveFailureStage::DisplayInvalid:return "display-owner-invalid";
    case NativeResolveFailureStage::ReturnedFalse:return "resolver-returned-false";
    case NativeResolveFailureStage::Exception:return "resolver-exception";
    }
    return "unknown";
}
// Run INSIDE the existing exclusive guest gate. This preserves the current
// admission policy, including its display dependency; it does not fix a resolve.
// Capture must not suppress/replace an exception, admit work, or run on success.
template<class Operation,class Capture>
inline bool observeNativeResolveAttempt(bool displayFound,bool displayValid,
        Operation&& operation,Capture&& capture) {
    const auto failed=[&](NativeResolveFailureStage stage) noexcept {
        try {capture(stage);} catch(...) {} // Original rejection remains authoritative.
    };
    if(!displayFound){failed(NativeResolveFailureStage::DisplayMissing);return false;}
    if(!displayValid){failed(NativeResolveFailureStage::DisplayInvalid);return false;}
    bool completed=false;
    try {completed=operation();}
    catch(...) {failed(NativeResolveFailureStage::Exception);throw;}
    if(!completed)failed(NativeResolveFailureStage::ReturnedFalse);
    return completed;
}
struct NativeResolveFailureCapture {
    Gta5TileResolveCapture boundary;
    NativeResolveFailureStage stage=NativeResolveFailureStage::ReturnedFalse;
    bool displayFound=false,displayValid=false,ownsPresentation=false;
    bool fixed16ColorFound=false,fixed16ColorValid=false;
    std::uint64_t fixed16ColorSerial=0;
};
// Only bounded owned argument/state/header snapshots. No invented destination
// pixels, no GPU execution, no memory publication, and no completion authority.
// Missing ranges stay null. All individual payloads fit the 64KiB capture cap.
template<class Sink>
inline bool writeNativeResolveFailureCapture(const NativeResolveFailureCapture& c,
        std::ostream& out,const Sink& save) {
    const auto& b=c.boundary;bool ok=true,first=true;
    out<<"{\"version\":1,\"capture_only\":true,\"snapshot\":\"failure-under-exclusive-guest-gate\","
       <<"\"gpu_executed_by_capture\":false,\"guest_pixels_published_by_capture\":false,"
       <<"\"completion_granted_by_capture\":false,\"destination_storage_captured\":false,"
       <<"\"stage\":\""<<nativeResolveFailureStageName(c.stage)<<"\",\"abi\":\"82388A38-r3-r10-lr\","
       <<"\"stack\":"<<b.stack<<",\"f1_bits\":"<<b.f1<<",\"arguments\":[";
    for(unsigned i=0;i<b.arguments.size();++i){if(i)out<<',';out<<b.arguments[i];}
    out<<"],\"display_found\":"<<int(c.displayFound)<<",\"display_valid\":"<<int(c.displayValid)
       <<",\"owns_presentation\":"<<int(c.ownsPresentation)<<",\"fixed16_color_found\":"<<int(c.fixed16ColorFound)
       <<",\"fixed16_color_valid\":"<<int(c.fixed16ColorValid)<<",\"fixed16_color_serial\":"<<c.fixed16ColorSerial
       <<",\"files\":{";
    const auto file=[&](const std::string& key,const auto& bytes) {
        if(!first)out<<',';first=false;out<<'"'<<key<<"\":";
        if(bytes && bytes->size()<=65536) {
            const auto name="native-resolve-failure-"+key+".bin";
            out<<"{\"file\":\""<<name<<"\",\"bytes\":"<<bytes->size()<<'}';
            ok=save(name,*bytes)&&ok;
        }else {out<<"null";if(bytes)ok=false;}
    };
    file("state",b.state);file("destination-header",b.destinationHeader);
    file("allocator-tail",b.allocatorTail);file("allocator-header",b.allocatorHeader);
    file("allocator-fence",b.allocatorFence);file("resolve-globals",b.resolveGlobals);
    file("rectangle",b.pointed.rectangle);file("destination-point",b.pointed.point);
    file("clear-color",b.pointed.color);file("stack92",b.pointed.stack92);file("stack100",b.pointed.stack100);
    if(b.surfaces)for(unsigned i=0;i<5;++i)file("surface-"+std::to_string(i),b.surfaces->surfaces[i].header);
    out<<"}}\n";return bool(out)&&ok;
}
}
