#pragma once
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace recomp::gpu {
// A shader patch is not a bind. Call select only at an observed original
// shader-load boundary, after patch/link completion, using its exact range.
// Owned bytes preserve the selected version when command storage is reused.
class Gta5VertexShaderBinding {
public:
    enum class Source { CachedLoad, CommandCopy };
    struct Selection {
        std::uint32_t object=0,variant=0,address=0;
        Source source=Source::CachedLoad;
        std::vector<std::uint8_t> code;
    };
    void invalidate() { selected_.reset(); }
    const Selection* forObject(std::uint32_t object) const {
        return selected_ && selected_->object==object ? &*selected_ : nullptr;
    }
    template<class ReadBytes>
    bool select(std::uint32_t object,std::uint32_t variant,std::uint32_t address,
                std::uint32_t bytes,Source source,const ReadBytes& read) {
        invalidate();
        if(!object || (object&3) || variant>31 || !address || (address&3) || !bytes ||
           (bytes&3) || bytes>65536 || std::uint64_t{address}+bytes>0x100000000ULL)
            return false;
        auto code=read(address,bytes);
        if(!code || code->size()!=bytes)return false;
        selected_=Selection{object,variant,address,source,std::move(*code)};
        return true;
    }
private:
    std::optional<Selection> selected_;
};
}
