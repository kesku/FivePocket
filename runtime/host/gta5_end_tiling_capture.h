#pragma once
#include "gta5_recording_chain_capture.h"
#include <ostream>

namespace recomp::gpu {
// Exact entry snapshot, never an EndTiling implementation or completion token.
// Unavailable dependencies remain explicit, not replaced with initial-image data.
struct Gta5EndTilingCapture {
    static constexpr unsigned DeviceBytes=21944;
    static constexpr std::array<unsigned,8> DescriptorOffsets{13760,13616,13736,13640,13664,13688,13712,13784};
    std::uint32_t stack=0,pendingAddress=0;
    std::uint64_t f1=0;
    std::array<std::uint32_t,9> arguments{};
    std::optional<std::vector<std::uint8_t>> state,color,fence,globals,allocatorHeader,pendingCommands;
    // Original8232E278/8232DFA8/8232DEF8 direct scheduling dependencies. Keep
    // optional holes distinct from captured zero and do not copy heap slack.
    std::optional<std::vector<std::uint8_t>> schedulerFlags,submissionCallback,ringRead,ring,videoSlots;
    std::optional<bool> liveVideoRootMatches;
    unsigned ringAddress=0;
    std::array<Gta5RecordingChainCapture,8> chains;
};
inline Gta5EndTilingCapture captureGta5EndTilingBoundary(std::uint32_t stack,
        const std::array<std::uint32_t,9>& arguments,std::uint64_t f1,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics,
        const Gta5ResourceReader& ownedLinks,const Gta5ResourceReader& argumentReader,
        const Gta5ResourceReader& title) {
    Gta5EndTilingCapture c;c.stack=stack;c.arguments=arguments;c.f1=f1;
    const auto read=[](const Gta5ResourceReader& reader,unsigned address,unsigned bytes)
            ->std::optional<std::vector<std::uint8_t>> {
        if(!reader || !address || address%4 || !bytes || std::uint64_t{address}+bytes>0x100000000ULL)return std::nullopt;
        auto b=reader(address,bytes);return b && b->size()==bytes?std::move(b):std::nullopt;
    };
    c.state=read(heap,arguments[0],c.DeviceBytes);
    c.color=read(argumentReader,arguments[4],16); // original82476080 r7
    c.globals=read(title,0x83CD2AA4,32); // resolve globals plus8235E218 debug flag
    c.videoSlots=read(title,0x82000868,8);
    if(!c.state)return c;
    const auto word=[&](unsigned at){const auto& b=*c.state;return
        (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];};
    c.fence=read(graphics,word(11024),8);
    const auto offsetRead=[&](const auto& reader,unsigned address,unsigned offset,unsigned bytes) {
        return address && offset<=UINT32_MAX-address?read(reader,address+offset,bytes):std::nullopt;
    };
    c.schedulerFlags=offsetRead(heap,arguments[0],24392,4);
    c.submissionCallback=offsetRead(heap,arguments[0],23768,4);
    c.ringRead=offsetRead(graphics,word(11024),60,4);
    const auto mask=word(15304);
    if(mask && mask<=0x3FFF && !(mask&(mask+1))) {
        c.ringAddress=word(15300);c.ring=read(graphics,c.ringAddress,(mask+1)*4);
    }
    c.allocatorHeader=read(heap,word(13920),176);
    for(unsigned i=0;i<c.chains.size();++i) {
        std::array<unsigned,6> descriptor{};
        for(unsigned w=0;w<6;++w)descriptor[w]=word(c.DescriptorOffsets[i]+4*w);
        c.chains[i]=captureGta5RecordingChain(arguments[0],descriptor,ownedLinks);
    }
    // Original8232E5C8 flushes [device[15332],device[48]+4). This pending
    // interval has not yet been converted into an indirect descriptor.
    const auto first=word(15332),last=word(48),limit=word(56);
    if(first && !(first%4) && !(last%4) && last<=UINT32_MAX-4 && first<=last+4 &&
       last<=limit && std::uint64_t{last}+4-first<=4*1024*1024) {
        c.pendingAddress=first;
        if(first==last+4)c.pendingCommands=std::vector<std::uint8_t>{};
        else c.pendingCommands=read(graphics,first,last+4-first);
    }
    return c;
}
template<class Save>
bool writeGta5EndTilingCapture(const Gta5EndTilingCapture& c,std::ostream& meta,const Save& save) {
    bool ok=true;
    meta<<"{\"version\":1,\"abi\":\"82476080-r3-r10-lr\",\"capture_only\":true,"
        "\"original_executed\":false,\"gpu_executed\":false,\"completion_granted\":false,"
        "\"guest_pixels_published\":false,\"stack\":"<<c.stack<<",\"f1_bits\":"<<c.f1<<",\"arguments\":[";
    for(unsigned i=0;i<c.arguments.size();++i){if(i)meta<<',';meta<<c.arguments[i];}
    meta<<"],\"pending_address\":"<<c.pendingAddress<<",\"ring_address\":"<<c.ringAddress
        <<",\"live_video_root_matches\":";
    if(c.liveVideoRootMatches)meta<<(*c.liveVideoRootMatches?"true":"false");else meta<<"null";
    meta<<",\"files\":{";
    bool first=true;
    const auto file=[&](const char* key,const auto& bytes){
        if(!first)meta<<',';first=false;meta<<'"'<<key<<"\":";
        if(bytes){const auto name=std::string("tile-end-")+key+".bin";ok=save(name,*bytes)&&ok;meta<<'"'<<name<<'"';}
        else meta<<"null";
    };
    file("state",c.state);file("color",c.color);file("fence",c.fence);file("globals",c.globals);
    file("allocator-header",c.allocatorHeader);file("pending-commands",c.pendingCommands);
    file("scheduler-flags",c.schedulerFlags);file("submission-callback",c.submissionCallback);
    file("ring-read",c.ringRead);file("ring",c.ring);file("video-slots",c.videoSlots);
    meta<<"},\"chains\":[";
    constexpr const char* statuses[]{"empty","captured","unavailable","malformed","limit"};
    for(unsigned i=0;i<c.chains.size();++i){
        if(i)meta<<',';const auto& chain=c.chains[i];
        meta<<"{\"offset\":"<<c.DescriptorOffsets[i]<<",\"status\":\""<<statuses[unsigned(chain.status)]<<"\",\"descriptor\":[";
        for(unsigned w=0;w<6;++w){if(w)meta<<',';meta<<chain.descriptor[w];}
        meta<<"],\"blocks\":[";
        for(unsigned j=0;j<chain.blocks.size();++j){
            if(j)meta<<',';const auto& b=chain.blocks[j];
            const auto name="tile-end-chain-"+std::to_string(i)+"-block-"+std::to_string(j)+".bin";
            ok=save(name,b.bytes)&&ok;
            meta<<"{\"address\":"<<b.address<<",\"used_bytes\":"<<b.usedBytes<<",\"tail\":"<<(b.tail?"true":"false")
                <<",\"file\":\""<<name<<"\"}";
        }
        meta<<"]}";
    }
    meta<<"]}\n";return bool(meta)&&ok;
}
}
