#pragma once
#include "graphics_submission_diagnostic.h"
#include "xenos_indirect_inspection.h"
#include <cerrno>
#include <filesystem>
#include <sstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::trace {
constexpr std::size_t StoppedSubmissionMaxWords=256*1024;
constexpr std::size_t StoppedSubmissionMaxNodes=64;
// The private Android process has RLIMIT_FSIZE=65536 (including diagnostics).
// Split large owned nodes; do not raise the process limit or truncate a node.
constexpr std::size_t StoppedSubmissionChunkBytes=65536;
enum class StoppedSubmissionSave { Saved, InvalidGraph, Limit, IoFailure };

// Uses only owned post-join snapshots; never reads guest memory or executes PM4.
// The old small stdout capture and every execution/ownership limit are unchanged.
inline StoppedSubmissionSave validateStoppedSubmission(const gpu::Pm4Snapshot& root,
        const gpu::Pm4IndirectGraph& graph) {
    if(graph.nodes.size()>StoppedSubmissionMaxNodes || graph.totalWords>StoppedSubmissionMaxWords || graph.totalPackets>65536)
        return StoppedSubmissionSave::Limit;
    std::size_t words=0,packets=0,indirects=0;
    for(std::size_t id=0;id<=graph.nodes.size();++id) {
        const auto& snapshot=id?graph.nodes[id-1].snapshot:root;
        if(snapshot.words().size()>StoppedSubmissionMaxWords-words)return StoppedSubmissionSave::Limit;
        words+=snapshot.words().size();packets+=snapshot.packets().size();
        for(const auto& p:snapshot.packets())if(p.kind==gpu::Pm4Kind::Command && (p.opcode==0x3F || p.opcode==0x37)) {
            ++indirects;unsigned matches=0;
            for(const auto& n:graph.nodes)if(n.parent==id && n.parentOffsetWords==p.offsetWords)++matches;
            if(matches!=1 || p.payloadWords!=2 || p.predicated)return StoppedSubmissionSave::InvalidGraph;
        }
        if(!id)continue;
        const auto& node=graph.nodes[id-1];
        if(node.parent>=id || node.snapshot.words().empty() || node.physicalAddress&3 ||
           std::uint64_t{node.physicalAddress}+node.snapshot.words().size()*4>0x20000000 ||
           node.depth!=(node.parent?graph.nodes[node.parent-1].depth:0)+1 || node.depth>8)
            return StoppedSubmissionSave::InvalidGraph;
        const auto& parent=node.parent?graph.nodes[node.parent-1].snapshot:root;
        const auto p=std::find_if(parent.packets().begin(),parent.packets().end(),
            [&](const auto& packet){return packet.offsetWords==node.parentOffsetWords;});
        if(p==parent.packets().end() || p->kind!=gpu::Pm4Kind::Command ||
           (p->opcode!=0x3F && p->opcode!=0x37) || p->payloadWords!=2 || p->predicated ||
           parent.words()[p->offsetWords+1]!=node.physicalAddress ||
           parent.words()[p->offsetWords+2]!=node.snapshot.words().size())return StoppedSubmissionSave::InvalidGraph;
    }
    return words==graph.totalWords && packets==graph.totalPackets && indirects==graph.nodes.size()
        ?StoppedSubmissionSave::Saved:StoppedSubmissionSave::InvalidGraph;
}

// All output names are fixed/generated, relative to an exclusively-created
// request-local directory. Never overwrites an older capture or follows links.
// Manifest is written LAST; partial I/O leaves evidence but cannot claim success.
inline StoppedSubmissionSave saveStoppedSubmission(const std::filesystem::path& parent,
        const gpu::Pm4Snapshot& root,const gpu::Pm4IndirectGraph& graph,
        unsigned readWord,unsigned writeWord,const GraphicsSubmissionDiagnostic& diagnostic) {
    const auto valid=validateStoppedSubmission(root,graph);
    if(valid!=StoppedSubmissionSave::Saved)return valid;
    struct Fd { int value=-1; ~Fd(){if(value>=0)::close(value);} };
    Fd base{::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW)};
    if(base.value<0 || ::mkdirat(base.value,"stopped-submission",0700))return StoppedSubmissionSave::IoFailure;
    Fd directory{::openat(base.value,"stopped-submission",O_RDONLY|O_DIRECTORY|O_NOFOLLOW)};
    if(directory.value<0)return StoppedSubmissionSave::IoFailure;
    const auto writeAll=[](int fd,const char* data,std::size_t bytes) {
        while(bytes) {const auto n=::write(fd,data,bytes);if(n<0 && errno==EINTR)continue;
            if(n<=0)return false;data+=n;bytes-=static_cast<std::size_t>(n);}return true;
    };
    try {
        // This survives a later payload-write failure but is NOT a completion
        // manifest and does not certify that the graph was saved in full.
        std::ostringstream dispatch;diagnostic.json(dispatch);const auto dispatchText=dispatch.str();
        if(dispatchText.size()>32768)return StoppedSubmissionSave::Limit;
        {
            Fd file{::openat(directory.value,"dispatch.json",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600)};
            if(file.value<0 || !writeAll(file.value,dispatchText.data(),dispatchText.size()) || ::fsync(file.value))
                return StoppedSubmissionSave::IoFailure;
        }
        std::ostringstream metadata;
        metadata<<"{\"version\":2,\"writers_stopped\":true,\"executed\":false,\"guest_memory_changed\":false,"
            "\"ring_advanced\":false,\"read_word\":"<<readWord<<",\"write_word\":"<<writeWord
            <<",\"total_words\":"<<graph.totalWords<<",\"total_packets\":"<<graph.totalPackets<<",\"dispatch\":";
        diagnostic.json(metadata);metadata<<",\"nodes\":[";
        for(std::size_t id=0;id<=graph.nodes.size();++id) {
            const auto& snapshot=id?graph.nodes[id-1].snapshot:root;
            const auto name="node-"+std::to_string(id)+".bin";
            constexpr auto ChunkWords=StoppedSubmissionChunkBytes/4;
            const auto& words=snapshot.words();const auto parts=std::max(std::size_t{1},(words.size()+ChunkWords-1)/ChunkWords);
            for(std::size_t part=0;part<parts;++part) {
                const auto filename=parts==1?name:"node-"+std::to_string(id)+"-part-"+std::to_string(part)+".bin";
                Fd file{::openat(directory.value,filename.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600)};
                if(file.value<0)return StoppedSubmissionSave::IoFailure;
                std::array<char,4096> buffer{};std::size_t used=0;
                const auto end=std::min(words.size(),(part+1)*ChunkWords);
                for(auto at=part*ChunkWords;at<end;++at) {
                    const auto word=words[at];
                    for(unsigned b=0;b<4;++b)buffer[used++]=static_cast<char>(word>>(24-8*b));
                    if(used==buffer.size()){if(!writeAll(file.value,buffer.data(),used))return StoppedSubmissionSave::IoFailure;used=0;}
                }
                if(!writeAll(file.value,buffer.data(),used) || ::fsync(file.value))return StoppedSubmissionSave::IoFailure;
            }
            if(id)metadata<<',';
            metadata<<"{\"node\":"<<id<<",\"path\":\""<<name<<"\",\"word_count\":"<<snapshot.words().size()
                <<",\"packet_count\":"<<snapshot.packets().size()<<",\"chunk_bytes\":"<<StoppedSubmissionChunkBytes;
            if(id){const auto& n=graph.nodes[id-1];metadata<<",\"parent\":"<<n.parent<<",\"parent_offset\":"<<n.parentOffsetWords
                <<",\"physical_address\":"<<n.physicalAddress<<",\"depth\":"<<n.depth;}
            metadata<<'}';
        }
        metadata<<"]}\n";const auto text=metadata.str();
        if(text.size()>32768)return StoppedSubmissionSave::Limit;
        Fd manifest{::openat(directory.value,"manifest.json",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600)};
        if(manifest.value<0 || !writeAll(manifest.value,text.data(),text.size()) || ::fsync(manifest.value))
            return StoppedSubmissionSave::IoFailure;
        return StoppedSubmissionSave::Saved;
    } catch(...) {return StoppedSubmissionSave::IoFailure;}
}
}
