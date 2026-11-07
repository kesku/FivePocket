// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
namespace recomp::apple::linear_upload {
inline int policy(const char* p) noexcept {if(!p||!std::strcmp(p,"0"))return 0;if(!std::strcmp(p,"1"))return 1;return -1;}
inline bool fits(std::uint64_t width,std::uint64_t height,std::uint64_t offset,std::uint64_t pitch,std::uint64_t size,std::uint64_t allocation) noexcept {
 if(!width||!height||width>std::numeric_limits<std::uint64_t>::max()/4)return false;
 const auto row=width*4;if(pitch<row||offset>allocation||row>size||row>allocation-offset)return false;
 const auto available=allocation-offset;
 return height-1 <= (size-row)/pitch && height-1 <= (available-row)/pitch;
}
inline bool copyRows(void* mapped,std::uint64_t allocation,std::uint64_t offset,std::uint64_t pitch,std::uint64_t size,std::uint64_t width,std::uint64_t height,const std::uint8_t* source,std::size_t bytes) noexcept {
 if(!mapped||!source||!fits(width,height,offset,pitch,size,allocation)||width>std::numeric_limits<std::size_t>::max()/4)return false;
 const std::size_t row=std::size_t(width)*4;if(height>std::numeric_limits<std::size_t>::max()/row||bytes!=row*height)return false;
 const auto dst= reinterpret_cast<std::uintptr_t>(mapped),src=reinterpret_cast<std::uintptr_t>(source);
 if(allocation>std::numeric_limits<std::uintptr_t>::max()-dst||bytes>std::numeric_limits<std::uintptr_t>::max()-src)return false;
 if(src<dst+allocation&&dst<src+bytes)return false;
 for(std::size_t y=0;y<height;++y)std::memcpy(static_cast<std::uint8_t*>(mapped)+offset+y*pitch,source+y*row,row);
 return true;
}
}
