#pragma once
#include "gta5_shader_resource.h"
#include <array>
#include <functional>
#include <vector>

namespace recomp::gpu {
using ShaderPatchReader = std::function<std::optional<std::vector<std::uint8_t>>(
    std::uint32_t, std::uint32_t)>;

// Diagnostic snapshots around the ORIGINAL 823C3A88 producer. These are not
// shader admission, instruction repair, a memory lease or a render command.
struct Gta5ShaderPatchCapture {
    // Original r3..r7: shader object, copied code, declaration, lookup, variant.
    std::array<std::uint32_t,5> arguments{};
    Gta5ShaderCodeRange sourceRange;
    std::vector<std::uint8_t> source, before, after;
    std::vector<std::uint8_t> sourceHeader, patchTable, fetchTemplates, declaration, lookup;
    bool returned = false, afterAvailable = false;
};

// Caller holds the exclusive executor gate for each capture. No guest writes.
// Exact-size copies only; optional context failure is represented by empty
// bytes, not invented metadata. Original/source and copied-code are required.
inline std::optional<Gta5ShaderPatchCapture> captureGta5ShaderPatchBefore(
        const std::array<std::uint32_t,5>& args,
        const ShaderPatchReader& heap, const ShaderPatchReader& data) noexcept {
    try {
        if (!heap || !data || !args[1] || (args[1]&3)) return std::nullopt;
        const auto exact = [](const ShaderPatchReader& read, std::uint64_t a, std::uint64_t n) {
            if (!a || !n || n>65536 || a+n>0x100000000ULL)
                return std::optional<std::vector<std::uint8_t>>{};
            auto bytes=read(static_cast<std::uint32_t>(a),static_cast<std::uint32_t>(n));
            if (bytes && bytes->size()!=n) bytes.reset();
            return bytes;
        };
        const auto word = [&](std::uint32_t a)->std::optional<std::uint32_t> {
            const auto b=exact(heap,a,4); if(!b)return std::nullopt;
            return (std::uint32_t{(*b)[0]}<<24)|(std::uint32_t{(*b)[1]}<<16)|
                (std::uint32_t{(*b)[2]}<<8)|(*b)[3];
        };
        const auto range=describeGta5ShaderCode(args[0],0,args[4],word);
        if (!range || std::uint64_t{args[1]}+range->byteCount>0x100000000ULL) return std::nullopt;
        auto source=exact(data,range->virtualAddress,range->byteCount);
        auto before=exact(data,args[1],range->byteCount);
        if (!source || !before) return std::nullopt;
        Gta5ShaderPatchCapture out;
        out.arguments=args; out.sourceRange=*range;
        out.source=std::move(*source); out.before=std::move(*before);
        const auto optional = [&](std::vector<std::uint8_t>& dst, std::uint64_t a, std::uint64_t n) {
            if(auto b=exact(heap,a,n))dst=std::move(*b);
        };
        // Header/table offsets follow original 823C3A88. Do not dereference
        // a table until its counts are bounded; preserve absent context.
        const auto relative=word(args[0]+896+8*args[4]);
        if(relative) {
            const auto header=std::uint64_t{args[0]}+*relative+872;
            optional(out.sourceHeader,header,36);
            if(out.sourceHeader.size()==36) {
                const auto hword=[&](unsigned at) {
                    const auto* p=out.sourceHeader.data()+at;
                    return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
                        (std::uint32_t{p[2]}<<8)|p[3];
                };
                const auto prefix=hword(24),count=hword(28);
                if(prefix<=16384 && count<=32)
                    optional(out.patchTable,header+36+std::uint64_t{prefix}*4,std::uint64_t{count}*4);
            }
        }
        optional(out.fetchTemplates,std::uint64_t{args[0]}+68+std::uint64_t{args[4]}*416,416);
        // Vertex declaration: original count at+24, twelve-byte records at+52.
        if(args[2] && std::uint64_t{args[2]}+28<=0x100000000ULL) {
            const auto count=word(args[2]+24);
            if(count && *count<=32)optional(out.declaration,args[2],52+std::uint64_t{*count}*12);
        }
        optional(out.lookup,args[3],96);
        return out;
    } catch (...) { return std::nullopt; }
}

inline bool captureGta5ShaderPatchAfter(Gta5ShaderPatchCapture& capture,
        const ShaderPatchReader& data) noexcept {
    capture.returned=true;
    if(capture.afterAvailable || !data || !capture.arguments[1] ||
       !capture.sourceRange.byteCount || capture.sourceRange.byteCount>65536 ||
       capture.before.size()!=capture.sourceRange.byteCount ||
       std::uint64_t{capture.arguments[1]}+capture.sourceRange.byteCount>0x100000000ULL)return false;
    try {
        auto b=data(capture.arguments[1],capture.sourceRange.byteCount);
        if(!b || b->size()!=capture.sourceRange.byteCount)return false;
        capture.after=std::move(*b);capture.afterAvailable=true;return true;
    } catch (...) { return false; }
}
}
