// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-vs-trim (Claude, 2026-10-16). Every translated vertex shader of the pack writes all 18 interpolators (TEXCOORD0-15 at
// Locations 0-15, COLOR0/1 at 16/17) whatever its pixel shader reads (e.g. the G-buffer pixel shader 9d109ce1 reads
// Locations 1 and 16; a depth-only pipeline has no fragment stage at all). A tile-based GPU (Apple) shades every vertex
// before rasterizing and writes ALL its outputs to memory, so the unread ones cost bandwidth on every vertex. These pure
// functions make, for one pipeline, the vertex module without the outputs its fragment stage does not read:
//   inputLocationMask(fragment words)       Locations of the fragment stage's non-builtin inputs (AllLocations when unsure)
//   trimOutputs(vertex words, keep mask)    the vertex module without the non-builtin outputs whose Location is not in the
//                                           mask: their OpVariable, entry-point interface entries, names, decorations, the
//                                           OpStores into them and access chains on them are removed. Empty result = keep the
//                                           module as it is (nothing to remove, or a use of an output other than a store /
//                                           access chain / name / decoration / interface entry was found).
// Unread outputs cannot change the picture: the values the fragment stage reads are written exactly as before.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace recomp::ios_vs_trim {
constexpr std::uint64_t AllLocations=~std::uint64_t{0};
namespace op {
constexpr std::uint32_t Name=5,MemberName=6,ExtInst=12,EntryPoint=15,Constant=43,SpecConstant=50,Function=54,Variable=59,Load=61,Store=62,
    AccessChain=65,InBoundsAccessChain=66,PtrAccessChain=67,Decorate=71,MemberDecorate=72,VectorShuffle=79,CompositeExtract=81,
    CompositeInsert=82,LoopMerge=246,SelectionMerge=247,BranchConditional=250,Switch=251,DecorateId=332,DecorateString=5632,
    MemberDecorateString=5633;
}
constexpr std::uint32_t StorageInput=1,StorageOutput=3,DecLocation=30,DecBuiltIn=11,ModelVertex=0,ModelFragment=4,Magic=0x07230203u;

struct Inst {std::size_t at;std::uint32_t count,opcode;};
inline bool parse(const std::uint32_t* w,std::size_t n,std::vector<Inst>& out) {
    if(!w || n<5 || w[0]!=Magic)return false;
    for(std::size_t i=5;i<n;) {
        const std::uint32_t count=w[i]>>16,opcode=w[i]&0xFFFFu;
        if(!count || i+count>n)return false;
        out.push_back({i,count,opcode});i+=count;
    }
    return true;
}
// The execution model of the first entry point (-1: none / unparsable).
inline int executionModel(const std::uint32_t* w,std::size_t n) {
    std::vector<Inst> in;if(!parse(w,n,in))return -1;
    for(const auto& x:in)if(x.opcode==op::EntryPoint && x.count>=3)return int(w[x.at+1]);
    return -1;
}
// Variables of `storage` with their Location / BuiltIn decorations.
struct Var {std::uint32_t id;int location=-1;bool builtin=false;std::uint32_t builtinKind=~0u;};
inline bool variables(const std::uint32_t* w,const std::vector<Inst>& in,std::uint32_t storage,std::vector<Var>& vars) {
    std::unordered_set<std::uint32_t> ids;
    for(const auto& x:in)if(x.opcode==op::Variable && x.count>=4 && w[x.at+3]==storage){vars.push_back({w[x.at+2]});ids.insert(w[x.at+2]);}
    for(const auto& x:in)if(x.opcode==op::Decorate && x.count>=3 && ids.count(w[x.at+1])) {
        for(auto& v:vars)if(v.id==w[x.at+1]) {
            if(w[x.at+2]==DecLocation && x.count>=4)v.location=int(w[x.at+3]);
            if(w[x.at+2]==DecBuiltIn){v.builtin=true;if(x.count>=4)v.builtinKind=w[x.at+3];}
        }
    }
    return true;
}
inline std::uint64_t inputLocationMask(const std::uint32_t* w,std::size_t n) {
    std::vector<Inst> in;if(!parse(w,n,in))return AllLocations;
    std::vector<Var> vars;variables(w,in,StorageInput,vars);
    std::uint64_t mask=0;
    for(const auto& v:vars) {
        if(v.builtin)continue;
        if(v.location<0 || v.location>=64)return AllLocations; // (an input without a usable Location: keep every output)
        mask|=std::uint64_t{1}<<v.location;
    }
    return mask;
}
// Words of x that are literals rather than ids (by opcode), so a use scan does not read them as ids.
inline bool literalWord(const std::uint32_t* w,const Inst& x,std::uint32_t k) {
    switch(x.opcode) {
    case op::ExtInst: return k==4;                                   // type, result, set, INSTRUCTION, operands...
    case op::VectorShuffle: return k>=5;                             // type, result, v1, v2, components...
    case op::CompositeExtract: return k>=4;                          // type, result, composite, indices...
    case op::CompositeInsert: return k>=5;                           // type, result, object, composite, indices...
    case op::LoopMerge: return k>=3;                                 // merge, continue, control, parameters...
    case op::SelectionMerge: return k>=2;                            // merge, control
    case op::BranchConditional: return k>=4;                         // condition, true, false, weights...
    case op::Switch: return k>=3 && (k-3)%2==0;                      // selector, default, (literal, label)...
    case op::Load: return k>=4;                                      // type, result, pointer, memory operands...
    case op::Store: return k>=3;                                     // pointer, object, memory operands...
    case op::Variable: return k==3;                                  // type, result, STORAGE CLASS, initializer
    default: (void)w; return false;
    }
}
// dropClip (MEASUREMENT ONLY: wrong wherever the game uses user clip planes): also remove the ClipDistance builtin output.
inline std::vector<std::uint32_t> trimOutputs(const std::uint32_t* w,std::size_t n,std::uint64_t keep,bool dropClip=false) {
    std::vector<Inst> in;if(!parse(w,n,in))return {};
    std::vector<Var> outs;variables(w,in,StorageOutput,outs);
    std::unordered_set<std::uint32_t> drop; // removed variables and access chains on them
    for(const auto& v:outs) {
        if(v.builtin){if(dropClip && v.builtinKind==3u)drop.insert(v.id);continue;} // (3 = ClipDistance)
        if(v.location<0)continue;
        if(v.location<64 && (keep>>v.location)&1)continue;
        drop.insert(v.id);
    }
    if(drop.empty())return {};
    bool inFunctions=false;
    std::vector<char> remove(in.size(),0);
    for(std::size_t i=0;i<in.size();++i) { // access chains on dropped variables join the set (in order: a chain precedes its uses)
        const auto& x=in[i];
        if((x.opcode==op::AccessChain || x.opcode==op::InBoundsAccessChain || x.opcode==op::PtrAccessChain) && x.count>=4 && drop.count(w[x.at+3]))
            drop.insert(w[x.at+2]);
    }
    for(std::size_t i=0;i<in.size();++i) {
        const auto& x=in[i];
        if(x.opcode==op::Function)inFunctions=true;
        switch(x.opcode) {
        case op::Name: case op::Decorate: case op::MemberDecorate: case op::DecorateId: case op::DecorateString: case op::MemberDecorateString:
            if(x.count>=2 && drop.count(w[x.at+1]))remove[i]=1;
            continue;
        case op::MemberName:
            continue;
        case op::Variable:
            if(x.count>=3 && drop.count(w[x.at+2])){remove[i]=1;continue;}
            break;
        case op::Store:
            if(x.count>=3 && drop.count(w[x.at+1])) {
                if(drop.count(w[x.at+2]))return {};                  // (storing a dropped pointer itself: refuse)
                remove[i]=1;continue;
            }
            break;
        case op::AccessChain: case op::InBoundsAccessChain: case op::PtrAccessChain:
            if(x.count>=4 && drop.count(w[x.at+3])) {
                for(std::uint32_t k=4;k<x.count;++k)if(drop.count(w[x.at+k]))return {};
                remove[i]=1;continue;
            }
            break;
        case op::EntryPoint:
            continue; // (rewritten below)
        default: break;
        }
        if(!inFunctions && x.opcode!=op::Variable)continue; // (types / constants before the functions name no variable)
        for(std::uint32_t k=1;k<x.count;++k)
            if(!literalWord(w,x,k) && drop.count(w[x.at+k]))return {};  // any other use of a dropped output: keep the module
    }
    std::vector<std::uint32_t> out(w,w+5);out.reserve(n);
    for(std::size_t i=0;i<in.size();++i) {
        const auto& x=in[i];
        if(remove[i])continue;
        if(x.opcode==op::EntryPoint) {
            std::uint32_t k=3; // the name: a nul-terminated string from word 3
            while(k<x.count){const std::uint32_t s=w[x.at+k];++k;if(!(s&0xFF000000u) || !(s&0xFF0000u) || !(s&0xFF00u) || !(s&0xFFu))break;}
            std::vector<std::uint32_t> e(w+x.at,w+x.at+k);
            for(;k<x.count;++k)if(!drop.count(w[x.at+k]))e.push_back(w[x.at+k]);
            e[0]=(std::uint32_t(e.size())<<16)|op::EntryPoint;
            out.insert(out.end(),e.begin(),e.end());
            continue;
        }
        out.insert(out.end(),w+x.at,w+x.at+x.count);
    }
    return out;
}
}
