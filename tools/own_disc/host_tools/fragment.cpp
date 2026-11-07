// SPDX-License-Identifier: GPL-3.0-or-later
// Translate bounded instruction ranges from an owned executable for tail repair.
#include "recompiler.h"
#include <charconv>
#include <fstream>
#include <limits>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,"usage: xenon-fragment CONFIG OUTPUT ADDRESS...\n");
        return 2;
    }
    Recompiler recompiler;
    if (!recompiler.LoadConfig(argv[1])) return 1;
    recompiler.Analyse();
    std::ofstream output(argv[2],std::ios::binary);
    if (!output) return 1;
    for (int argument=3;argument<argc;++argument) {
        const auto address=std::stoul(argv[argument],nullptr,16);
        if (address>std::numeric_limits<std::uint32_t>::max() || address%4) return 2;
        const auto* data=static_cast<const std::uint32_t*>(recompiler.image.Find(address));
        if (!data) return 1;
        std::size_t length=0;
        for (;length<512;length+=4) {
            const auto* word=static_cast<const std::uint32_t*>(recompiler.image.Find(address+length));
            if (!word) return 1;
            const auto instruction=ByteSwap(*word);
            if (instruction==0x4E800020u || instruction==0x4E800420u) { length+=4; break; }
        }
        if (length==512) return 1;
        recompiler.image.symbols.emplace(fmt::format("sub_{:X}",address),address,length,Symbol_Function);
        recompiler.out.clear();
        if (!recompiler.Recompile(Function(address,length))) return 1;
        output<<recompiler.out;
    }
    return output.good()?0:1;
}
