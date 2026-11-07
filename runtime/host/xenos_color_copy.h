#pragma once
#include "xenos_edram_alias_clear.h"
#include "xenos_tiled32_texture.h"
#include <limits>

// Full-origin, single-sample,32bpp reference color copy only. Not a PM4 executor,
// allocation owner, source ROP/format12 precision model or guest memory writer.
namespace recomp::gpu {
struct ColorCopyPlan {
    AliasView source{};
    std::uint32_t destinationBase=0,capacity=0,touchedSpan=0,pixelBytes=0;
    bool swapRedBlue=false;
};
inline bool colorCopyOffset(std::uint32_t width,std::uint32_t height,std::uint32_t x,
        std::uint32_t y,std::uint32_t& out){
    if(!width || width>4096 || (width&31) || !height || height>4096 || x>=width || y>=height)return false;
    std::uint32_t inner=0;tiled32TexelOffset(x&31,y&31,inner);
    out=((y>>5)*(width>>5)+(x>>5))*4096+inner;return true;
}
inline bool planColorCopy(const AliasView& source,std::uint32_t control,std::uint32_t destinationBase,
        std::uint32_t destinationPitch,std::uint32_t destinationInfo,ColorCopyPlan& out){
    AliasFootprint footprint;
    if(source.depth || !fullAliasFootprint(source,footprint) || footprint.samples!=1 ||
        source.width>4096 || source.height>4096 || (source.width&31) || control!=0x100000 ||
        (destinationBase&4095) || destinationPitch!=(source.width|(source.height<<16)) ||
        (destinationInfo&~0x1000000u)!=0x300)return false;
    const std::uint64_t capacity=std::uint64_t{source.width}*((source.height+31)&~31u)*4;
    if(capacity>16u*1024*1024 || std::uint64_t{destinationBase}+capacity>0x20000000)return false;
    std::uint32_t largest=0;
    // Last macro tile has a potentially partial Y bank. Do not claim padding
    // after its last written pixel is owned or written.
    for(unsigned y=(source.height-1)&~31u;y<source.height;++y)
        for(unsigned x=source.width-32;x<source.width;++x){
            std::uint32_t at=0;if(!colorCopyOffset(source.width,source.height,x,y,at))return false;
            if(at>largest)largest=at;
        }
    out={source,destinationBase,static_cast<std::uint32_t>(capacity),largest+4,
        source.width*source.height*4,(destinationInfo&0x1000000u)!=0};return true;
}
inline std::uint32_t color7e3ToUnorm8(std::uint32_t code){
    code&=1023;
    if(code>=384)return 255; // All RGB values >=1 saturate.
    const unsigned e=code>>7,m=code&127;
    const unsigned numerator=e?128+m:m,denominator=e==2?256:512;
    return (numerator*255+denominator/2)/denominator;
}
inline std::uint32_t copyColorWord(std::uint32_t source,bool swap){
    unsigned r=color7e3ToUnorm8(source),g=color7e3ToUnorm8(source>>10),b=color7e3ToUnorm8(source>>20);
    if(swap){const auto tmp=r;r=b;b=tmp;}
    return r|(g<<8)|(b<<16)|((source>>30)*85u<<24);
}
inline bool copyColorToOwnedBuffer(const AliasView& source,std::uint32_t control,std::uint32_t destinationBase,
        std::uint32_t destinationPitch,std::uint32_t destinationInfo,const std::uint8_t* edram,std::size_t edramBytes,
        std::uint8_t* destination,std::size_t destinationBytes){
    ColorCopyPlan plan;
    if(!edram || !destination || edramBytes!=AliasEdramBytes ||
        !planColorCopy(source,control,destinationBase,destinationPitch,destinationInfo,plan) ||
        destinationBytes!=plan.capacity)return false;
    const auto a=reinterpret_cast<std::uintptr_t>(edram),b=reinterpret_cast<std::uintptr_t>(destination);
    const auto max=std::numeric_limits<std::uintptr_t>::max();
    if(a>max-edramBytes || b>max-destinationBytes || (a<b+destinationBytes && b<a+edramBytes))return false;
    for(unsigned y=0;y<source.height;++y)for(unsigned x=0;x<source.width;++x){
        std::uint32_t from=0,to=0;aliasSampleOffset(source,x,y,0,from);colorCopyOffset(source.width,source.height,x,y,to);
        std::uint32_t word=0;for(unsigned c=0;c<4;++c)word|=std::uint32_t{edram[from+c]}<<(8*c);
        word=copyColorWord(word,plan.swapRedBlue);
        for(unsigned c=0;c<4;++c)destination[to+c]=static_cast<std::uint8_t>(word>>(8*c));
    }
    return true;
}
}
