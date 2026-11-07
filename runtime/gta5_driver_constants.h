#pragma once
#include "gta5_inline_draw.h"
#include <atomic> // v123
#include <algorithm>
#include <bit>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h> // v213: State::setRun
#endif

namespace recomp::gpu {
// v213 A/B (diag-generic-v213-baseline, set by renderer_boundary_trace.cpp): State::setRun without the NEON runs.
inline std::atomic<bool> scalarConstantCommit{false};
// Effective float constants, in driver submission order. The ordinary device
// shadow is insufficient: 823BB448 loads shader literals directly from shader
// resources, without updating that shadow. Later ordinary uploads may overwrite
// them, so this is an ordered state cache, not an unconditional literal overlay.
//
// Attempt v43: each transaction validates every read first and then commits in place (v35 copied the
// whole 8.5 KB state into a candidate and back, twice per upload/literal load). A failed read, a bound
// violation or a short table still commits nothing. The *Viewed forms take in-place readers (a
// pointer to the guest bytes with the Reader's validation, nullptr where the Reader would refuse), so
// no bytes are copied into vectors; the Reader forms keep their contract for existing callers.
class Gta5DriverConstants {
public:
    bool upload(std::uint64_t mask,unsigned firstRegister,std::uint32_t source,
            const Gta5ResourceReader& heap) {
        Owned owned;
        return uploadCore(mask,firstRegister,source,[&](std::uint32_t a,std::uint32_t n){return owned.read(heap,a,n);});
    }
    template<class View>
    bool uploadViewed(std::uint64_t mask,unsigned firstRegister,std::uint32_t source,const View& heap) {
        return uploadCore(mask,firstRegister,source,heap);
    }
    // header is PS object+40 or VS object+872, exactly as passed to 823BB448.
    // Each 8-byte entry contains float4 index, DWORD count, data-relative offset.
    bool loadLiterals(std::uint32_t header,std::uint32_t dataBase,
            const Gta5ResourceReader& heap,const Gta5ResourceReader& data) {
        Owned owned;
        return literalsCore(header,dataBase,[&](std::uint32_t a,std::uint32_t n){return owned.read(heap,a,n);},
            [&](std::uint32_t a,std::uint32_t n){return owned.read(data,a,n);});
    }
    template<class View,class DataView>
    bool loadLiteralsViewed(std::uint32_t header,std::uint32_t dataBase,const View& heap,const DataView& data) {
        return literalsCore(header,dataBase,heap,data);
    }
    // v80: `words` big-endian words at `bytes` into constant words [firstWord, firstWord + words) (vertex
    // 0-1023, pixel 1024-2047, the ALU constant file's dword order): the data of a SET_SHADER_CONSTANTS
    // packet or of a LOAD_ALU_CONSTANT (gta5_inline_constants.h), in submission order like upload().
    // An empty or out-of-range load commits nothing.
    bool loadWords(unsigned firstWord,unsigned words,const std::uint8_t* bytes) {
        if(!bytes || !words || firstWord>=2048 || words>2048-firstWord)return false;
        state_.setRun(firstWord,words,bytes); // (v213)
        return true;
    }
    void apply(Gta5InlineDraw& draw)const {
        for(unsigned block=0;block<state_.present.size();++block) {
            auto mask=state_.present[block];
            while(mask) {
                const unsigned index=block*64+std::countr_zero(mask);mask&=mask-1;
                (index<1024?draw.vertexConstants[index]:draw.pixelConstants[index-1024])=state_.words(index);
            }
        }
    }
    // v43: one bank (0 vertex, 1 pixel) of a draw's constants: the device shadow's 1024 big-endian words
    // at `shadow` with every submitted constant applied over them, the values apply() gives a draw decoded
    // from that shadow. A bank whose every constant was submitted is returned in place (valid until the
    // next upload or literal load); otherwise it is assembled in `scratch`.
    const std::array<std::uint32_t,1024>& effectiveBank(unsigned bank,const std::uint8_t* shadow,
            std::array<std::uint32_t,1024>& scratch)const {
        const auto* present=state_.present.data()+bank*16;
        bool full=true;
        for(unsigned b=0;b<16;++b)full&=present[b]==~std::uint64_t{0};
        if(full)return state_.banks[bank];
        const auto& words=state_.banks[bank];
        for(unsigned b=0;b<16;++b) {
            const auto m=present[b];
            auto* out=scratch.data()+b*64;
            if(m==~std::uint64_t{0}){std::memcpy(out,words.data()+b*64,256);continue;}
            const auto* in=shadow+b*256;
            if(!m){for(unsigned i=0;i<64;++i)out[i]=word(in+i*4);continue;}
            for(unsigned i=0;i<64;++i)out[i]=(m>>i)&1?words[b*64+i]:word(in+i*4);
        }
        return scratch;
    }
    // v123: the content version of bank `bank` (0 vertex, 1 pixel) as effectiveBank returns it in place: nonzero, unique
    // across every Gta5DriverConstants of the process, and the same for two calls exactly when no word of the bank changed
    // value (and no constant became present) in between. Meaningful only while the bank is in place (every constant of
    // it submitted); an assembled bank depends on the device shadow, which the caller must treat as unversioned (0).
    std::uint64_t bankVersion(unsigned bank) {
        if(bank>1)return 0;
        if(state_.changed[bank]){state_.changed[bank]=false;state_.version[bank]=nextVersion();}
        return state_.version[bank];
    }
    // v276: bank `bank`'s dirty mask (see State::dirty), for DrawInput::vertexConstantsDirty / pixelConstantsDirty, and this
    // object's identity (DrawInput::*ConstantsSource): nonzero, unique in the process; a copy or move gets a new one, so two
    // objects never share an identity (their dirty masks describe different histories).
    std::array<std::uint64_t,16>* dirtyMask(unsigned bank){return bank<2?&state_.dirty[bank]:nullptr;}
    std::uint64_t sourceId()const noexcept{return source_.id;}
    // v43: the device shadow's words alone (no submitted constants).
    static void decodeBank(const std::uint8_t* shadow,std::array<std::uint32_t,1024>& out) {
        for(unsigned i=0;i<1024;++i)out[i]=word(shadow+i*4);
    }
    std::size_t size()const noexcept{return state_.count;}
private:
    static std::uint32_t word(const std::uint8_t* p) {
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
    }
    static std::uint64_t nextVersion() noexcept {static std::atomic<std::uint64_t> next{0};return next.fetch_add(1,std::memory_order_relaxed)+1;}
    // Keeps the Reader's copies alive for the transaction and serves them as pointers.
    struct Owned {
        std::vector<std::vector<std::uint8_t>> kept;
        const std::uint8_t* read(const Gta5ResourceReader& reader,std::uint32_t address,std::uint32_t bytes) {
            auto result=reader(address,bytes);
            if(!result || result->size()!=bytes)return nullptr;
            kept.push_back(std::move(*result));
            return kept.back().data(); // a moved vector keeps its buffer, so earlier pointers stay valid
        }
    };
    template<class Fetch>
    bool uploadCore(std::uint64_t mask,unsigned firstRegister,std::uint32_t source,const Fetch& fetch) {
        if(firstRegister<0x4000 || firstRegister>=0x4800)return false;
        // Attempt v32: the set blocks lie in one contiguous source; read the span from the first to
        // the last set block once (one registry lock instead of up to 64). If the whole span does not
        // validate, every block is read on its own exactly as before.
        const std::uint8_t* span=nullptr;
        unsigned spanFirst=0;
        if(mask) {
            spanFirst=static_cast<unsigned>(std::countl_zero(mask));
            const unsigned spanLast=63u-static_cast<unsigned>(std::countr_zero(mask));
            const auto start=std::uint64_t{source}+spanFirst*64,bytes=std::uint64_t{spanLast-spanFirst+1}*64;
            if(spanLast>spanFirst && start+bytes<=0x100000000ULL)
                span=fetch(static_cast<std::uint32_t>(start),static_cast<std::uint32_t>(bytes));
        }
        // Validate: every block's bounds and bytes; nothing is committed before all pass.
        std::array<const std::uint8_t*,64> blocks;
        // v264: only the set bits, most significant first (bit b = mask bit 63-b), the order the 64-bit scan visited them
        for(std::uint64_t m=mask;m;m&=~(std::uint64_t{1}<<(63-std::countl_zero(m)))) {
            const unsigned bit=static_cast<unsigned>(std::countl_zero(m));
            const auto first=firstRegister-0x4000+bit*16;
            const auto address=std::uint64_t{source}+bit*64;
            if(first+16>2048 || address+64>0x100000000ULL)return false;
            blocks[bit]=span?span+std::size_t{bit-spanFirst}*64:fetch(static_cast<std::uint32_t>(address),64);
            if(!blocks[bit])return false;
        }
        // Commit, in block order.
        for(std::uint64_t m=mask;m;m&=~(std::uint64_t{1}<<(63-std::countl_zero(m)))) {
            const unsigned bit=static_cast<unsigned>(std::countl_zero(m));
            state_.setRun(firstRegister-0x4000+bit*16,16,blocks[bit]); // (v213)
        }
        return true;
    }
    template<class Fetch,class DataFetch>
    bool literalsCore(std::uint32_t header,std::uint32_t dataBase,const Fetch& heap,const DataFetch& data) {
        const auto read=[&](std::uint64_t address)->std::optional<std::uint32_t> {
            if(address>0xFFFFFFFCULL)return std::nullopt;
            const auto* bytes=heap(static_cast<std::uint32_t>(address),4);
            return bytes?std::optional<std::uint32_t>{word(bytes)}:std::nullopt;
        };
        const auto relative=read(std::uint64_t{header}+20);
        if(!relative)return false;
        if(!*relative)return true;
        const auto table=std::uint64_t{header}+*relative;
        const auto size=read(table+16);
        if(!size || (*size&3) || *size>65536 || table+20+*size>0x100000000ULL)return false;
        if(!*size)return true;
        const auto* entries=heap(static_cast<std::uint32_t>(table+20),*size);
        if(!entries)return false;
        // Validate every entry and its data before committing any (entries decoded once, here).
        struct Entry {unsigned first,count;const std::uint8_t* bytes;};
        std::array<Entry,32> local{};std::vector<Entry> more;unsigned n=0;
        for(unsigned at=0;at<*size;at+=8) {
            const auto* entry=entries+at;
            const unsigned first=((unsigned(entry[0])<<8)|entry[1])*4;
            const unsigned count=(unsigned(entry[2])<<8)|entry[3];
            if(!count)break;
            // The terminating zero-count record is only four bytes: the
            // original returns before reading the optional data offset.
            if(*size-at<8)return false;
            const auto address=std::uint64_t{dataBase}+word(entry+4);
            if(first+count>2048 || address+count*4>0x100000000ULL)return false;
            const auto* bytes=data(static_cast<std::uint32_t>(address),count*4);
            if(!bytes)return false;
            if(n<local.size())local[n]=Entry{first,count,bytes};else more.push_back(Entry{first,count,bytes});
            ++n;
        }
        // Commit, in table order (a later entry overwrites an earlier overlapping one).
        for(unsigned k=0;k<n;++k) {
            const auto& e=k<local.size()?local[k]:more[k-local.size()];
            state_.setRun(e.first,e.count,e.bytes); // (v213)
        }
        return true;
    }
    // The original interface has exactly two1024-DWORD banks. Keep an explicit
    // presence mask: unsubmitted slots must retain the draw's existing values,
    // not become implicit zero constants. Transactions validate every read
    // before they commit, so a failed one changes nothing.
    struct State {
        std::array<std::array<std::uint32_t,1024>,2> banks{};
        std::array<std::uint64_t,32> present{};
        std::size_t count=0;
        std::array<bool,2> changed{true,true};std::array<std::uint64_t,2> version{}; // v123 (bankVersion)
        std::array<std::array<std::uint64_t,16>,2> dirty{}; // v276: words changed (value or presence) since the capture took them
        std::uint32_t words(unsigned index)const{return banks[index/1024][index%1024];}
        void set(unsigned index,std::uint32_t value) {
            const std::uint64_t bit=std::uint64_t{1}<<(index%64);
            if(!(present[index/64]&bit)){present[index/64]|=bit;++count;changed[index/1024]=true;dirty[index/1024][(index%1024)/64]|=bit;}
            auto& w=banks[index/1024][index%1024];
            if(w!=value){w=value;changed[index/1024]=true;dirty[index/1024][(index%1024)/64]|=bit;}
        }
        // v213: set(first+i, big-endian word i of `bytes`) for i < n, in order: the same words, presence bits, count and
        // changed flags (captureDriverConstants' commits were 2.3% of the render thread as a per-word loop, v212 house
        // profile). Runs of 16 words inside one bank are byte-swapped, compared and stored with NEON; the presence bits
        // of a run are set with at most two mask-word updates.
        void setRun(unsigned first,unsigned n,const std::uint8_t* bytes) {
            unsigned i=0;
#if defined(__aarch64__) && !defined(RECOMP_SCALAR_CONSTANT_COMMIT)
            while(!scalarConstantCommit.load(std::memory_order_relaxed) && i+16<=n) {
                const unsigned index=first+i;
                if(index/1024!=(index+15)/1024)break; // (a run across the bank boundary: the scalar loop)
                auto* w=banks[index/1024].data()+index%1024;
                uint32x4_t diff=vdupq_n_u32(0);
                for(unsigned q=0;q<4;++q) {
                    const uint32x4_t v=vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(bytes+(i+q*4)*4)));
                    diff=vorrq_u32(diff,veorq_u32(v,vld1q_u32(w+q*4)));
                    vst1q_u32(w+q*4,v);
                }
                bool changedRun=vmaxvq_u32(diff)!=0;
                for(unsigned at=index;at<index+16;) { // the run's bits in each mask word it touches
                    const unsigned lo=at%64,take=std::min(64u-lo,index+16-at);
                    const std::uint64_t bits=(take==64?~std::uint64_t{0}:((std::uint64_t{1}<<take)-1))<<lo;
                    const std::uint64_t fresh=bits&~present[at/64];
                    if(fresh){present[at/64]|=fresh;count+=static_cast<std::size_t>(std::popcount(fresh));changedRun=true;}
                    at+=take;
                }
                if(changedRun) { // (v276: the run's 16 words marked dirty, a superset of the changed ones)
                    changed[index/1024]=true;
                    const unsigned local=index%1024;auto& d=dirty[index/1024];
                    for(unsigned at=local;at<local+16;){const unsigned lo=at%64,take=std::min(64u-lo,local+16-at);
                        d[at/64]|=(take==64?~std::uint64_t{0}:((std::uint64_t{1}<<take)-1))<<lo;at+=take;}
                }
                i+=16;
            }
#endif
            for(;i<n;++i)set(first+i,(std::uint32_t{bytes[i*4]}<<24)|(std::uint32_t{bytes[i*4+1]}<<16)|(std::uint32_t{bytes[i*4+2]}<<8)|bytes[i*4+3]);
        }
    } state_;
    struct SourceId { // v276
        std::uint64_t id=nextVersion();
        SourceId()=default;SourceId(const SourceId&):id(nextVersion()){}
        SourceId& operator=(const SourceId&){id=nextVersion();return *this;}
    } source_;
};
}
