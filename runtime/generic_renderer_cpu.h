#pragma once
// Attempt v24 (inefficiency audit, local-analysis/inefficiency-audit/report.md): the CPU-only parts
// of the generic renderer, moved out of generic_renderer.cpp so host tests can check them against
// the v23 code (local-analysis/inefficiency-audit/tests). Everything here is a pure function of its
// inputs, or an exact-match cache of one; nothing here reads guest memory or touches Vulkan.
// Thread confinement: the caches are members of the Renderer and used only under its mutex.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h> // v209: transformIndices
#endif

namespace recomp::gpu::generic::cpu {
// v210 A/B (diag-generic-v210-baseline): transformIndices without v209's NEON loops.
inline std::atomic<bool> scalarTransform{false};

inline std::uint32_t be32(const std::uint8_t* p){return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];}
inline std::uint64_t fnv(const void* data,std::size_t n,std::uint64_t h=0xCBF29CE484222325ull) {
    const auto* p=static_cast<const std::uint8_t*>(data);
    for(std::size_t i=0;i<n;++i){h^=p[i];h*=0x100000001B3ull;}
    return h;
}
template<class T> std::uint64_t fnvOf(const T& v,std::uint64_t h=0xCBF29CE484222325ull){return fnv(&v,sizeof v,h);}

// ---- v23 code, unchanged (moved from generic_renderer.cpp)

// Instruction slots of every fetch, in the order the exec clauses list them.
inline std::vector<unsigned> fetchSlots(const std::vector<std::uint8_t>& code) {
    const std::size_t count=code.size()/4;std::size_t end=count/3;std::vector<unsigned> fetches;
    for(std::size_t slot=0;slot<end;++slot) {
        const auto w0=be32(code.data()+12*slot),w1=be32(code.data()+12*slot+4),w2=be32(code.data()+12*slot+8);
        const std::uint64_t cf[2]{w0|(std::uint64_t{w1&0xFFFF}<<32),((w1>>16)|(std::uint64_t{w2}<<16))&0xFFFFFFFFFFFFull};
        for(const auto c:cf) {
            const unsigned opcode=unsigned(c>>44)&15,lo=unsigned(c);
            if(opcode==1||opcode==2||opcode==3||opcode==4||opcode==5||opcode==6||opcode==13||opcode==14) {
                const unsigned address=lo&0xFFF,n=(lo>>12)&7,sequence=(lo>>16)&0xFFF;
                if(address)end=std::min<std::size_t>(end,address);
                for(unsigned i=0;i<n;++i)if((sequence>>(2*i))&1)fetches.push_back(address+i);
            }
        }
    }
    return fetches;
}
// Same canonical form as tools/iter/generic_shader_pack.py: zero the bind-time-patchable fields
// of every vertex fetch (found through the exec clauses of the control-flow prefix).
inline std::uint64_t canonicalHash(const std::vector<std::uint8_t>& code) {
    const std::size_t count=code.size()/4;
    std::vector<std::uint32_t> w(count);
    for(std::size_t i=0;i<count;++i)w[i]=be32(code.data()+4*i);
    std::size_t end=count/3;std::vector<unsigned> fetches;
    for(std::size_t slot=0;slot<end;++slot) {
        const auto w0=w[3*slot],w1=w[3*slot+1],w2=w[3*slot+2];
        const std::uint64_t cf[2]{w0|(std::uint64_t{w1&0xFFFF}<<32),
            ((w1>>16)|(std::uint64_t{w2}<<16))&0xFFFFFFFFFFFFull};
        for(const auto c:cf) {
            const unsigned opcode=unsigned(c>>44)&15,lo=unsigned(c);
            if(opcode==1||opcode==2||opcode==3||opcode==4||opcode==5||opcode==6||opcode==13||opcode==14) {
                const unsigned address=lo&0xFFF,n=(lo>>12)&7,sequence=(lo>>16)&0xFFF;
                if(address)end=std::min<std::size_t>(end,address);
                for(unsigned i=0;i<n;++i)if((sequence>>(2*i))&1)fetches.push_back(address+i);
            }
        }
    }
    for(const auto f:fetches)if(3*std::size_t{f}+2<count && (w[3*f]&0x1F)==0) {
        w[3*f]&=~((0x3FFu<<20)|(0x7Fu<<12));w[3*f+1]=0;w[3*f+2]=0; // const/prefetch/dst register; format+swizzle; stride/offset
    }
    std::uint64_t h=0xCBF29CE484222325ull;
    for(const auto x:w){const std::uint8_t b[4]{std::uint8_t(x>>24),std::uint8_t(x>>16),std::uint8_t(x>>8),std::uint8_t(x)};h=fnv(b,4,h);}
    return h;
}

// Xenos vertex fetch (XenosRecomp shader_code.h VertexFetchInstruction layout).
// v41: + the exponent adjust (dword 1 bits 24-29, signed) and the signed repeating-fraction mode
// (dword 1 bit 14: 0 clamps -2^(n-1) to -1, 1 is the "no zero" mode (2c+1)/(2^n-1)).
// v66: `feed` is the swizzle over the fetched components the renderer applies before the translated
// shader (declarationSwizzle below); 0x688 (xyzw) feeds the components as fetched.
struct VFetch {unsigned slot=0,stride=0,format=0,dstSwizzle=0;int offset=0,expAdjust=0;bool isSigned=false,isInteger=false,mini=false,noZero=false;
    unsigned feed=0x688;
    bool bgra=false; // v67: the feed is fed natively as B8G8R8A8_UNORM (decodeAttribute still applies the feed)
};
// ---- v66: vertex fetch destination swizzles. D3D patches every bound vfetch's destination swizzle
// from the vertex declaration (e.g. D3DCOLOR's zyxw; 16-bit pairs under 8-in-32 endian), while the
// XenosRecomp translation applies the container's swizzle to its input (r.k = input[container_k]).
// Selector k: 0-3 a fetched component, 4 zero, 5 one, 6/7 not written.
inline unsigned swizzleSelector(unsigned swizzle,unsigned k){return (swizzle>>(3*k))&7;}
// The input the translation needs so that it computes what the hardware computes (r.k = fetched[bound_k]):
// input[container_k] = select(fetched, bound_k). Components no container selector names stay as fetched.
// `representable` is false when two container selectors name one input with different bound selectors.
struct Declaration {unsigned feed=0x688;bool representable=true;unsigned used=0;}; // used: input components the container names
inline Declaration declarationSwizzle(unsigned container,unsigned bound) {
    unsigned sel[4]{0,1,2,3};bool set[4]{};Declaration d;
    for(unsigned k=0;k<4;++k) {
        const unsigned c=swizzleSelector(container,k),b=swizzleSelector(bound,k);
        if(c>3 || b>5)continue;
        if(set[c] && sel[c]!=b){d.representable=false;continue;}
        sel[c]=b;set[c]=true;d.used|=1u<<c;
    }
    d.feed=sel[0]|(sel[1]<<3)|(sel[2]<<6)|(sel[3]<<9);
    return d;
}
inline unsigned fetchComponents(unsigned format) {
    switch(format){case 33:case 36:return 1;case 25:case 31:case 34:case 37:return 2;case 16:case 17:case 57:return 3;default:return 4;}
}
// Whether feeding `feed` equals what the input assembler delivers (fetched components, then the
// (0, 0, 0, 1) defaults past the format's components).
inline bool feedIsIdentity(unsigned feed,unsigned format) {
    const unsigned n=fetchComponents(format);
    for(unsigned c=0;c<4;++c) {
        const unsigned s=swizzleSelector(feed,c);
        if(s==c)continue;
        if(c>=n && ((s==4 && c<3) || (s==5 && c==3)))continue;
        return false;
    }
    return true;
}
// v67: native equivalents of a feed on the components the translation reads (`used`).
//  - pair swap: 16-bit formats whose feed exchanges the 16-bit halves of each dword (x<->y, z<->w): the
//    complementary endian mode (endian ^ 3: 8-in-32 <-> 8-in-16, none <-> 16-in-32) of the same bytes.
//  - BGRA: an unsigned-normalized 8_8_8_8 fed zyxw (D3DCOLOR) is B8G8R8A8_UNORM.
// Components past the format's own count read the (0, 0, 0, 1) default either way.
inline bool feedIsPairSwap(unsigned feed,unsigned format,unsigned used) {
    if(format!=25 && format!=26 && format!=31 && format!=32)return false;
    const unsigned n=fetchComponents(format);
    for(unsigned c=0;c<4;++c) {
        if(!((used>>c)&1))continue;
        const unsigned s=swizzleSelector(feed,c);
        if(c<n){if(s!=(c^1))return false;continue;}
        if(!(s==c || (s==4 && c<3) || (s==5 && c==3)))return false;
    }
    return true;
}
inline bool feedIsBgra(unsigned feed,const VFetch& f,unsigned used) {
    if(f.format!=6 || f.isSigned || f.isInteger || f.expAdjust)return false;
    constexpr unsigned bgra[4]{2,1,0,3};
    for(unsigned c=0;c<4;++c)if(((used>>c)&1) && swizzleSelector(feed,c)!=bgra[c])return false;
    return true;
}
inline unsigned complementEndian(unsigned endian){return endian^3u;}
inline std::array<float,4> applyFeed(const std::array<float,4>& v,unsigned feed) {
    std::array<float,4> r{};
    for(unsigned c=0;c<4;++c){const unsigned s=swizzleSelector(feed,c);r[c]=s<4?v[s]:s==4?0.0f:1.0f;}
    return r;
}
inline VFetch decodeVFetch(const std::vector<std::uint8_t>& code,unsigned address) {
    VFetch f;const auto at=std::size_t{address}*12;
    if(at+12>code.size())return f;
    const auto w0=be32(code.data()+at),w1=be32(code.data()+at+4),w2=be32(code.data()+at+8);
    f.slot=((w0>>20)&31)*3+((w0>>25)&3);
    f.dstSwizzle=w1&0xFFF;f.isSigned=(w1>>12)&1;f.isInteger=(w1>>13)&1;f.format=(w1>>16)&63;f.mini=(w1>>30)&1;
    f.noZero=(w1>>14)&1;f.expAdjust=static_cast<std::int32_t>(w1<<2)>>26;
    f.stride=w2&0xFF;f.offset=static_cast<std::int32_t>(w2<<1)>>9; // bits 8..30, signed
    return f;
}

// ---- v41: vertex attributes the input assembler cannot fetch as the Xenos does (moved from
// generic_renderer.cpp, where v35 had decodeAttribute for rect lists only)
// v41: attributes the input assembler cannot fetch as the Xenos does: no Vulkan layout, a 32-bit
// integer into a float input, an exponent adjust, or the signed "no zero" mode.
inline bool vertexNeedsConversion(const VFetch& f,bool uintInput) {
    if(!f.bgra && !feedIsIdentity(f.feed,f.format))return true; // v66 (v67: BGRA feeds natively)
    const bool normalizedInteger=f.format==6 || f.format==7 || f.format==16 || f.format==17 || f.format==25 || f.format==26 ||
                                 (f.format>=33 && f.format<=35);
    if(f.format==16 || f.format==17)return true;
    if(f.format>=33 && f.format<=35 && !uintInput)return true;
    return f.expAdjust!=0 || (normalizedInteger && f.isSigned && !f.isInteger && f.noZero);
}
// v176: moved above decodeAttribute, which reads it for unsigned-normalized 8-bit fields.
struct Unorm8Table {
    float v[256];
    Unorm8Table(){for(unsigned r=0;r<256;++r)v[r]=float(double(r)/255.0);} // decodeAttribute's packed(): float(double(raw)/double(mask))
};
inline const Unorm8Table& unorm8Table(){static const Unorm8Table t;return t;}
// CPU decode of one attribute to float4 (rect-list expansion and v41's converted attributes):
// component widths per format, integer or normalized (signed: clamp or "no zero"), then 2^exp.
inline std::array<float,4> decodeAttribute(const std::uint8_t* p,const VFetch& f) {
    std::array<float,4> v{0,0,0,1};
    const auto u32=[&](unsigned k){std::uint32_t x;std::memcpy(&x,p+4*k,4);return x;};
    // v176: the same values without libm. A Xenos half has no Inf/NaN (exponent 31 is a normal binade): e>0 is
    // (1+m/1024)*2^(e-15), exactly the float with exponent field e+112 and mantissa m<<13; e=0 is m*2^-24, exact
    // in float. (v175 computed both with std::ldexp; checked bit for bit over all 65536 halves on the host.)
    const auto half=[](std::uint16_t h){
        const std::uint32_t e=(h>>10)&31,m=h&1023;float r;
        if(e){const std::uint32_t bits=((e+112u)<<23)|(m<<13);std::memcpy(&r,&bits,4);}
        else r=float(m)*0x1p-24f;
        return (h&0x8000)?-r:r;};
    const auto packed=[&](std::uint64_t raw,unsigned bits)->float {
        const std::uint64_t mask=(std::uint64_t{1}<<bits)-1;raw&=mask;
        if(bits==8 && !f.isInteger && !f.isSigned)return unorm8Table().v[raw]; // v176: the table holds float(double(raw)/255.0)
        const double sext=(raw>>(bits-1))&1?double(raw)-double(mask)-1.0:double(raw);
        if(f.isInteger)return float(f.isSigned?sext:double(raw));
        if(!f.isSigned)return float(double(raw)/double(mask));
        if(f.noZero)return float((2.0*sext+1.0)/double(mask));
        return float(std::max(-1.0,sext/double(mask>>1)));
    };
    const auto fields=[&](std::uint32_t w,std::initializer_list<unsigned> widths){unsigned at=0,k=0;
        for(const unsigned b:widths){v[k++]=packed(w>>at,b);at+=b;}};
    switch(f.format) {
    case 36: case 37: case 38: case 57: {const unsigned n=f.format==36?1:f.format==37?2:f.format==57?3:4;
        for(unsigned k=0;k<n;++k){const auto x=u32(k);std::memcpy(&v[k],&x,4);}break;}
    case 31: case 32: {const unsigned n=f.format==31?2:4;
        for(unsigned k=0;k<n;++k){std::uint16_t h;std::memcpy(&h,p+2*k,2);v[k]=half(h);}break;}
    case 6: fields(u32(0),{8,8,8,8});break;
    case 7: fields(u32(0),{10,10,10,2});break;
    case 16: fields(u32(0),{11,11,10});break;
    case 17: fields(u32(0),{10,11,11});break;
    case 25: fields(u32(0),{16,16});break;
    case 26: fields(u32(0),{16,16});{const auto hi=u32(1);v[2]=packed(hi,16);v[3]=packed(hi>>16,16);}break;
    case 33: case 34: case 35: {const unsigned n=f.format==33?1:f.format==34?2:4;for(unsigned k=0;k<n;++k)v[k]=packed(u32(k),32);break;}
    default: break;
    }
    if(f.expAdjust) { // the fetched components only, not the (0, 0, 0, 1) defaults
        const unsigned n=f.format==36 || f.format==33?1:f.format==37 || f.format==31 || f.format==25 || f.format==34?2:
                         f.format==57 || f.format==16 || f.format==17?3:4;
        // v176: 2^expAdjust (a 6-bit signed exponent, always a normal float) built directly, as std::ldexp(1.0f, e) gave it.
        const std::uint32_t bits=std::uint32_t(127+f.expAdjust)<<23;float scale;std::memcpy(&scale,&bits,4);
        for(unsigned k=0;k<n;++k)v[k]*=scale;
    }
    return f.feed==0x688?v:applyFeed(v,f.feed); // v66
}

// ---- v24: the per-draw vertex-fetch derivation of v23 draw(), as functions of their inputs

// (destination register, fetch) of every vertex fetch in execution order; a mini fetch takes the
// fetch constant and stride of the previous full fetch. Depends on the microcode bytes only.
using EffectiveFetches=std::vector<std::pair<unsigned,VFetch>>;
inline EffectiveFetches effectiveFetches(const std::vector<std::uint8_t>& code) {
    EffectiveFetches effective;
    VFetch last;bool haveFull=false;
    for(const auto slot:fetchSlots(code)) {
        if(std::size_t{slot}*12+12>code.size() || (be32(code.data()+12*slot)&0x1F)!=0)continue; // vertex fetch only
        auto f=decodeVFetch(code,slot);
        if(!f.mini){last=f;haveFull=true;}
        else if(haveFull){f.slot=last.slot;f.stride=last.stride;}
        effective.push_back({(be32(code.data()+12*slot)>>12)&0x3F,f});
    }
    return effective;
}
// The fetch of every pack element: the k-th effective fetch into the element's destination
// register is the element's k-th container fetch into it (D3D reorders the bound copy), else the
// fetch at the element's container address (counted: v23 noted each "vfetch-by-register").
struct ElementFetches {std::vector<VFetch> fetches;unsigned fallbacks=0;};
inline ElementFetches matchElements(const EffectiveFetches& effective,const std::vector<std::uint8_t>& code,
                                    unsigned elementCount,const std::uint8_t* elementDst,const std::uint32_t* elements) {
    ElementFetches r;r.fetches.resize(elementCount);
    for(unsigned i=0;i<elementCount;++i) {
        unsigned k=0;for(unsigned j=0;j<i;++j)k+=elementDst[j]==elementDst[i];
        bool found=false;
        for(const auto& [dst,f]:effective)if(dst==elementDst[i] && !k--){r.fetches[i]=f;found=true;break;}
        if(!found){r.fetches[i]=decodeVFetch(code,elements[i]&0xFFF);++r.fallbacks;}
    }
    return r;
}

// ---- v24: exact-match cache of everything derived from one microcode byte string

// Content hash for cache buckets only (never a pack key): four independent 64-bit lanes.
inline std::uint64_t contentHash(const std::uint8_t* p,std::size_t n) {
    constexpr std::uint64_t K0=0x9E3779B97F4A7C15ull,K1=0xC2B2AE3D27D4EB4Full,K2=0x165667B19E3779F9ull,K3=0xD6E8FEB86659FD93ull;
    std::uint64_t a=K0^n,b=K1,c=K2,d=K3;
    std::size_t i=0;
    for(;i+32<=n;i+=32) {
        std::uint64_t w[4];std::memcpy(w,p+i,32);
        a=(a^w[0])*K1;b=(b^w[1])*K2;c=(c^w[2])*K3;d=(d^w[3])*K0;
        a^=a>>29;b^=b>>31;c^=c>>27;d^=d>>33;
    }
    for(;i+8<=n;i+=8){std::uint64_t w;std::memcpy(&w,p+i,8);a=(a^w)*K1;a^=a>>29;}
    if(i<n){std::uint64_t w=0;std::memcpy(&w,p+i,n-i);b=(b^w)*K2;b^=b>>31;}
    std::uint64_t h=a^(b*K3)^(c*K0)^(d*K1);
    h^=h>>33;h*=0xFF51AFD7ED558CCDull;h^=h>>33;h*=0xC4CEB9FE1A85EC53ull;h^=h>>33;
    return h;
}

struct CodeRecord {
    std::vector<std::uint8_t> bytes;  // the exact microcode this record describes
    std::uint64_t canonical=0;        // canonicalHash(bytes)
    bool haveEffective=false;
    std::uint32_t vkw4Memo=0; // vk-worker-4 (diag-vkw4-attrs): 1 + the renderer's attribute memo index (0: none); in haveEffective's padding
    EffectiveFetches effective;
    const void* matchedFor=nullptr;ElementFetches elements; // matchElements for that pack entry
};
// Records keyed by content hash and verified byte for byte. A second byte string with the same
// content hash is never cached (find returns nullptr and the caller computes from scratch).
class CodeCache {
public:
    CodeRecord* find(const std::vector<std::uint8_t>& code) {
        const auto h=contentHash(code.data(),code.size());
        auto [it,inserted]=records_.try_emplace(h);
        auto& r=it->second;
        if(inserted){r.bytes=code;r.canonical=canonicalHash(code);++misses;return &r;}
        if(r.bytes.size()==code.size() && std::equal(code.begin(),code.end(),r.bytes.begin())){++hits;return &r;}
        ++collisions;return nullptr;
    }
    std::uint64_t canonical(const std::vector<std::uint8_t>& code) {
        if(const auto* r=find(code))return r->canonical;
        return canonicalHash(code);
    }
    // Only between draws: clearing invalidates every CodeRecord pointer.
    void trim(std::size_t limit){if(records_.size()>limit){records_.clear();++trims;}}
    std::size_t size() const {return records_.size();}
    std::uint64_t hits=0,misses=0,collisions=0,trims=0;
private:
    std::unordered_map<std::uint64_t,CodeRecord> records_;
};
// The element fetches of `code` (which `record` describes, or nullptr) for one pack entry.
inline const ElementFetches& elementFetches(CodeRecord* record,const std::vector<std::uint8_t>& code,const void* entry,
        unsigned elementCount,const std::uint8_t* elementDst,const std::uint32_t* elements,ElementFetches& uncached) {
    if(!record) {
        uncached=matchElements(effectiveFetches(code),code,elementCount,elementDst,elements);
        return uncached;
    }
    if(!record->haveEffective){record->effective=effectiveFetches(code);record->haveEffective=true;}
    if(record->matchedFor!=entry) {
        record->elements=matchElements(record->effective,code,elementCount,elementDst,elements);
        record->matchedFor=entry;
    }
    return record->elements;
}

// ---- v24: vertex bytes straight into the upload ring

// v23: bytes = source[0,take); bytes.resize(n, 0); then every complete dword (k+3 < n) swapped per
// the fetch constant's endian mode (0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32); a trailing partial
// dword stays as is. This writes the same n bytes to dst in one pass (dst must not alias src).
inline void swapCopy(std::uint8_t* __restrict dst,const std::uint8_t* __restrict src,std::size_t take,std::size_t n,unsigned endian) {
    take=std::min(take,n);
    const std::size_t whole=take&~std::size_t{3};
    switch(endian) {
    case 1:
        for(std::size_t k=0;k<whole;k+=4){std::uint32_t v;std::memcpy(&v,src+k,4);v=((v>>8)&0x00FF00FFu)|((v<<8)&0xFF00FF00u);std::memcpy(dst+k,&v,4);}
        break;
    case 2:
        for(std::size_t k=0;k<whole;k+=4){std::uint32_t v;std::memcpy(&v,src+k,4);v=__builtin_bswap32(v);std::memcpy(dst+k,&v,4);}
        break;
    case 3:
        for(std::size_t k=0;k<whole;k+=4){std::uint32_t v;std::memcpy(&v,src+k,4);v=(v>>16)|(v<<16);std::memcpy(dst+k,&v,4);}
        break;
    default:
        if(whole)std::memcpy(dst,src,whole);
        break;
    }
    std::size_t at=whole;
    if(at<take) { // a dword straddling the end of the source: source bytes, then zeros
        std::uint8_t w[4]{};for(std::size_t k=0;k<4 && at+k<take;++k)w[k]=src[at+k];
        if(at+4<=n) {
            if(endian==2){std::swap(w[0],w[3]);std::swap(w[1],w[2]);}
            else if(endian==1){std::swap(w[0],w[1]);std::swap(w[2],w[3]);}
            else if(endian==3){std::swap(w[0],w[2]);std::swap(w[1],w[3]);}
            std::memcpy(dst+at,w,4);at+=4;
        } else {std::memcpy(dst+at,w,n-at);return;} // partial final dword: not swapped
    }
    if(at<n)std::memset(dst+at,0,n-at); // zero extension (a zero dword swaps to zero)
}

// ---- v78: map keys over 64-bit words (the v24-v71 keys ran FNV-1a one byte at a time: 256 dependent multiplies for
// a descriptor-set key, ~200 for a pipeline key). Two lanes, each step bijective in the lane and in the word, then a
// 64-bit finaliser; used only for the renderer's cache keys, which are compared by value alone like the FNV ones.
inline std::uint64_t wordsKey(const std::uint64_t* w,std::size_t n,std::uint64_t seed=0) {
    std::uint64_t a=seed^0x9E3779B97F4A7C15ull,b=(seed+n)*0xC2B2AE3D27D4EB4Full+0x165667B19E3779F9ull;
    std::size_t i=0;
    for(;i+2<=n;i+=2){a=(a^w[i])*0xFF51AFD7ED558CCDull;a^=a>>32;b=(b^w[i+1])*0xC4CEB9FE1A85EC53ull;b^=b>>29;}
    if(i<n){a=(a^w[i])*0xFF51AFD7ED558CCDull;a^=a>>32;}
    std::uint64_t h=a^(b*0x9E3779B97F4A7C15ull)^std::uint64_t(n);
    h^=h>>33;h*=0xFF51AFD7ED558CCDull;h^=h>>33;h*=0xC4CEB9FE1A85EC53ull;h^=h>>33;
    return h;
}
inline std::uint64_t word64(const void* p){std::uint64_t v=0;std::memcpy(&v,&p,sizeof p);return v;}

// ---- v78: CPU-converted vertex attributes without swapping the whole stream
// v41-v71 (generic_renderer.cpp draw()): per converted attribute, the whole stream was swapped (swapCopy of n bytes),
// then per vertex 16 bytes at v*stride+offset were copied (zeros past n), decoded with decodeAttribute and written as
// a float4 (uint4 for a uint input: max(0, x) truncated). The functions below produce the same 16 bytes per vertex.

// The 4 bytes at `at` (4-aligned) of what swapCopy(dst, src, take, n, endian) writes, zeros at and past n.
inline std::uint32_t swappedWord(const std::uint8_t* src,std::size_t take,std::size_t n,unsigned endian,std::size_t at) {
    take=std::min(take,n);
    const std::size_t whole=take&~std::size_t{3};
    std::uint8_t w[4]{};
    if(at+4<=whole) {
        std::memcpy(w,src+at,4);
    } else if(at==whole && at<take) { // the dword straddling the end of the source: source bytes, then zeros
        for(std::size_t k=0;k<4 && at+k<take;++k)w[k]=src[at+k];
        if(at+4>n) { // a partial final dword is copied unswapped and only up to n
            for(std::size_t k=n-at;k<4;++k)w[k]=0;
            std::uint32_t v;std::memcpy(&v,w,4);return v;
        }
    } else return 0;
    if(at+4>n)return 0; // (unreachable: at+4<=whole<=n)
    std::uint32_t v;std::memcpy(&v,w,4);
    switch(endian) {
    case 1: return ((v>>8)&0x00FF00FFu)|((v<<8)&0xFF00FF00u);
    case 2: return __builtin_bswap32(v);
    case 3: return (v>>16)|(v<<16);
    default: return v;
    }
}
// Fast decoders for the kinds the Thor converts (v69/v70 logs: k_8_8_8_8 unorm BLENDINDICES fed to a uint input,
// k_16_16 / k_16_16_16_16 signed integer, whose SSCALED Vulkan formats the Adreno cannot fetch). Each returns
// exactly decodeAttribute's value (checked on the host against it, bit for bit, including the feed).
enum class FastDecode : std::uint8_t {None,Unorm8x4,Sint16x2,Sint16x4};
inline FastDecode fastDecodeKind(const VFetch& f) {
    if(f.expAdjust)return FastDecode::None;
    if(f.format==6 && !f.isSigned && !f.isInteger)return FastDecode::Unorm8x4;
    if(f.format==25 && f.isSigned && f.isInteger)return FastDecode::Sint16x2;
    if(f.format==26 && f.isSigned && f.isInteger)return FastDecode::Sint16x4;
    return FastDecode::None;
}
// Words w0 (and w1 for 16_16_16_16) are the attribute's first dwords as swapCopy leaves them (little-endian fields).
inline std::array<float,4> fastDecode(FastDecode kind,std::uint32_t w0,std::uint32_t w1,unsigned feed) {
    std::array<float,4> v{0,0,0,1};
    switch(kind) {
    case FastDecode::Unorm8x4: {const auto& t=unorm8Table().v;v={t[w0&255],t[(w0>>8)&255],t[(w0>>16)&255],t[w0>>24]};break;}
    case FastDecode::Sint16x2: v[0]=float(std::int16_t(w0&0xFFFF));v[1]=float(std::int16_t(w0>>16));break;
    case FastDecode::Sint16x4: v[0]=float(std::int16_t(w0&0xFFFF));v[1]=float(std::int16_t(w0>>16));
                               v[2]=float(std::int16_t(w1&0xFFFF));v[3]=float(std::int16_t(w1>>16));break;
    default: break;
    }
    return feed==0x688?v:applyFeed(v,feed);
}
// The 16 bytes the converted stream holds for one vertex (uint inputs: max(0, x) truncated, as v41-v71 wrote them).
inline void storeConverted(std::uint8_t* out,const std::array<float,4>& value,bool uintInput) {
    if(uintInput){std::array<std::uint32_t,4> u{};for(unsigned k=0;k<4;++k)u[k]=std::uint32_t(std::max(0.0f,value[k]));std::memcpy(out,u.data(),16);}
    else std::memcpy(out,value.data(),16);
}
// One converted attribute for `count` vertices of a stream (source bytes src[0,take) of n, endian mode, stride,
// attribute byte offset) into out[16*count]. The fast kinds read only the attribute's dwords; any other kind (or a
// stride/offset that is not dword aligned) swaps the stream into `scratch` once and decodes as v41-v71 did.
inline void convertAttribute(std::uint8_t* __restrict out,unsigned count,const std::uint8_t* src,std::size_t take,std::size_t n,unsigned endian,
                             unsigned stride,unsigned offset,const VFetch& f,bool uintInput,std::vector<std::uint8_t>& scratch,bool& scratchValid) {
    const auto kind=fastDecodeKind(f);
    if(kind!=FastDecode::None && !(stride&3) && !(offset&3)) {
        const bool two=kind==FastDecode::Sint16x4;
        // Vertices whose dwords lie inside the whole swapped dwords of the source read them directly (one endian
        // swap each, the mode chosen outside the loop); the rest (the source's end, zero extension) through swappedWord.
        const std::size_t whole=std::min(take,n)&~std::size_t{3},span=two?8:4;
        unsigned direct=0;
        if(whole>=offset+span)direct=unsigned(std::min<std::size_t>(count,(whole-offset-span)/std::max(1u,stride)+1));
        const auto run=[&](auto swap) {
            for(unsigned v=0;v<direct;++v) {
                const std::uint8_t* p=src+std::size_t{v}*stride+offset;
                std::uint32_t w0,w1=0;std::memcpy(&w0,p,4);w0=swap(w0);if(two){std::memcpy(&w1,p+4,4);w1=swap(w1);}
                storeConverted(out+16*std::size_t{v},fastDecode(kind,w0,w1,f.feed),uintInput);
            }
        };
        switch(endian) {
        case 1: run([](std::uint32_t v){return ((v>>8)&0x00FF00FFu)|((v<<8)&0xFF00FF00u);});break;
        case 2: run([](std::uint32_t v){return __builtin_bswap32(v);});break;
        case 3: run([](std::uint32_t v){return (v>>16)|(v<<16);});break;
        default: run([](std::uint32_t v){return v;});break;
        }
        for(unsigned v=direct;v<count;++v) {
            const std::size_t at=std::size_t{v}*stride+offset;
            const auto w0=swappedWord(src,take,n,endian,at),w1=two?swappedWord(src,take,n,endian,at+4):0u;
            storeConverted(out+16*std::size_t{v},fastDecode(kind,w0,w1,f.feed),uintInput);
        }
        return;
    }
    if(!scratchValid){scratch.resize(n);swapCopy(scratch.data(),src,take,n,endian);scratchValid=true;}
    for(unsigned v=0;v<count;++v) {
        std::array<std::uint8_t,16> tmp{};const std::size_t off=std::size_t{v}*stride+offset;
        if(off<scratch.size())std::memcpy(tmp.data(),scratch.data()+off,std::min<std::size_t>(16,scratch.size()-off));
        storeConverted(out+16*std::size_t{v},decodeAttribute(tmp.data(),f),uintInput);
    }
}

// ---- v24: index processing of v23 draw() in one pass

// Non-quad primitives: v23 built indices (restart markers -> 0xFFFFFFFF, else clamp(raw+base) into
// [min, max(min,max)]; or a verbatim copy for inline draws), replaced every marker by minIndex
// when primitive restart is off, then took lo/hi over the non-markers. Same output, one pass.
// Returns false when every index is a marker (v23 "empty-indices").
struct IndexParameters {
    bool final=false,restartEnabled=false,restart=false;
    std::uint32_t restartIndex=0,baseVertex=0,minIndex=0,maxIndex=0;
};
inline bool transformIndices(const std::uint32_t* __restrict in,std::size_t count,const IndexParameters& p,
                             std::uint32_t* __restrict out,std::uint32_t& lo,std::uint32_t& hi) {
    std::uint32_t l=0xFFFFFFFFu,h=0;
    const std::uint32_t marker=p.restart?0xFFFFFFFFu:p.minIndex,top=std::max(p.minIndex,p.maxIndex);
    std::size_t i=0;
#if defined(__aarch64__) && !defined(RECOMP_SCALAR_TRANSFORM_INDICES)
    if(!scalarTransform.load(std::memory_order_relaxed)) // v210 A/B
    // v209: NEON, 4 indices per step, the same per-index function as the scalar loops below (5.2% of the worker's
    // samples while driving, v207). An excluded value is 0xFFFFFFFF, so min(l, v) leaves l unchanged for it exactly as
    // min(l, 0xFFFFFFFF) does, and the max takes v with the excluded lanes cleared (max(h, 0)); clamp(x, min, top) is
    // min(max(x, min), top) because min <= top. The scalar loops finish the last 0-3 indices.
    {
        const uint32x4_t all=vdupq_n_u32(0xFFFFFFFFu),mk=vdupq_n_u32(marker);
        uint32x4_t lv=all,hv=vdupq_n_u32(0);
        if(p.final) {
            for(;i+4<=count;i+=4) {
                const uint32x4_t raw=vld1q_u32(in+i);
                const uint32x4_t v=vbslq_u32(vceqq_u32(raw,all),mk,raw);
                vst1q_u32(out+i,v);
                lv=vminq_u32(lv,v);hv=vmaxq_u32(hv,vbicq_u32(v,vceqq_u32(v,all)));
            }
        } else {
            const uint32x4_t base=vdupq_n_u32(p.baseVertex),low24=vdupq_n_u32(0xFFFFFFu),mn=vdupq_n_u32(p.minIndex),tp=vdupq_n_u32(top),
                ri=vdupq_n_u32(p.restartIndex),re=vdupq_n_u32(p.restartEnabled?0xFFFFFFFFu:0u);
            for(;i+4<=count;i+=4) {
                const uint32x4_t raw=vld1q_u32(in+i);
                const uint32x4_t c=vminq_u32(vmaxq_u32(vandq_u32(vaddq_u32(raw,base),low24),mn),tp);
                const uint32x4_t v=vbslq_u32(vandq_u32(vceqq_u32(raw,ri),re),mk,c);
                vst1q_u32(out+i,v);
                lv=vminq_u32(lv,v);hv=vmaxq_u32(hv,vbicq_u32(v,vceqq_u32(v,all)));
            }
        }
        l=vminvq_u32(lv);h=vmaxvq_u32(hv);
    }
#endif
    if(p.final) {
        for(;i<count;++i) {
            std::uint32_t v=in[i];
            const bool m=v==0xFFFFFFFFu;
            v=m?marker:v;out[i]=v;
            const bool excluded=v==0xFFFFFFFFu;
            l=std::min(l,excluded?0xFFFFFFFFu:v);h=std::max(h,excluded?0u:v);
        }
    } else {
        for(;i<count;++i) {
            const std::uint32_t raw=in[i];
            const bool m=p.restartEnabled && raw==p.restartIndex;
            const std::uint32_t v=m?marker:std::clamp((raw+p.baseVertex)&0xFFFFFFu,p.minIndex,top);
            out[i]=v;
            const bool excluded=v==0xFFFFFFFFu;
            l=std::min(l,excluded?0xFFFFFFFFu:v);h=std::max(h,excluded?0u:v);
        }
    }
    lo=l;hi=h;
    return l!=0xFFFFFFFFu;
}
// Ring form of v23: markers stay 0xFFFFFFFF, other indices become relative to lo.
inline void rebaseIndices(const std::uint32_t* __restrict in,std::size_t count,std::uint32_t lo,std::uint8_t* __restrict dst) {
    for(std::size_t i=0;i<count;++i){const std::uint32_t v=in[i]==0xFFFFFFFFu?0xFFFFFFFFu:in[i]-lo;std::memcpy(dst+4*i,&v,4);}
}

// ---- v24: RGBA8 readback with an opaque alpha channel

// v23: rgba(bytes) zero-filled, memcpy from the readback, then rgba[i]=255 for every 4th byte.
inline std::vector<std::uint8_t> opaqueRgba(const std::uint8_t* src,std::size_t bytes) {
    std::vector<std::uint8_t> rgba(src,src+bytes); // one copy, no zero fill
    std::uint8_t* p=rgba.data();const std::size_t whole=bytes&~std::size_t{3};
    for(std::size_t i=0;i<whole;i+=4){std::uint32_t v;std::memcpy(&v,p+i,4);v|=0xFF000000u;std::memcpy(p+i,&v,4);} // byte 3 of each little-endian texel
    return rgba;
}

// v225: opaqueRgba's bytes into `out` (resized to `bytes`; a recycled buffer of the same size is not reallocated, so no
// fresh 3.7 MB allocation and page faults per frame), one pass: 16 bytes per NEON step with alpha (byte 3 of each texel)
// ORed to 255, the scalar loop for the tail. Identical output to opaqueRgba (tests/opaque_rgba_test.cpp).
inline void opaqueRgbaInto(std::vector<std::uint8_t>& out,const std::uint8_t* src,std::size_t bytes) {
    out.resize(bytes);
    std::uint8_t* p=out.data();std::size_t i=0;const std::size_t whole=bytes&~std::size_t{3};
#if defined(__aarch64__)
    const uint32x4_t alpha=vdupq_n_u32(0xFF000000u);
    for(;i+16<=whole;i+=16)vst1q_u8(p+i,vreinterpretq_u8_u32(vorrq_u32(vreinterpretq_u32_u8(vld1q_u8(src+i)),alpha)));
#endif
    for(;i<whole;i+=4){std::uint32_t v;std::memcpy(&v,src+i,4);v|=0xFF000000u;std::memcpy(p+i,&v,4);}
    if(bytes>whole)std::memcpy(p+whole,src+whole,bytes-whole);
}

// ---- v24: texture fetch keys

struct FetchKey {
    std::array<std::uint32_t,6> w{};
    bool operator==(const FetchKey& o) const {return w==o.w;}
};
struct FetchKeyHash {
    std::size_t operator()(const FetchKey& k) const {
        std::uint64_t a,b,c;std::memcpy(&a,&k.w[0],8);std::memcpy(&b,&k.w[2],8);std::memcpy(&c,&k.w[4],8);
        std::uint64_t h=(a*0x9E3779B97F4A7C15ull)^(b*0xC2B2AE3D27D4EB4Full)^(c*0x165667B19E3779F9ull);
        h^=h>>31;h*=0xD6E8FEB86659FD93ull;h^=h>>32;
        return static_cast<std::size_t>(h);
    }
};
}
