// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::private_trace {
constexpr std::size_t MaximumArgumentBytes=65536;
constexpr std::size_t MaximumArguments=1024;

inline bool requestId(std::string_view id) noexcept {
    if(id.size()!=32)return false;
    for(char c:id)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
    return true;
}

inline bool canonicalPath(std::string_view path) noexcept {
    if(path.empty() || path.front()!='/' || path.back()=='/')return false;
    std::size_t at=1;
    while(at<path.size()){
        const auto end=path.find('/',at);
        const auto part=path.substr(at,end==path.npos?path.size()-at:end-at);
        if(part.empty() || part=="." || part=="..")return false;
        for(char c:part)if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
            (c>='0' && c<='9') || c=='_' || c=='-' || c=='.'))return false;
        if(end==path.npos)break;
        at=end+1;
    }
    return true;
}

// Syntax/location boundary only, NOT a hash/ownership or file-lifetime proof.
// Caller must authenticate the explicit local manifest and stage current owned
// read-only inputs. The existing game runner still validates semantic arguments.
// Output changes only on complete success, including when input aliases it.
inline bool parseArguments(const std::uint8_t* data,std::size_t bytes,
        std::string_view root,std::string_view id,std::vector<std::string>& output) noexcept {
    if(!data || !bytes || bytes>MaximumArgumentBytes || data[bytes-1]!=0 ||
        !canonicalPath(root) || root.size()>3000 || !requestId(id))return false;
    try {
        const std::string base(root);
        const std::string run=base+"/run-"+std::string(id)+"/";
        const std::string inputs=base+"/inputs/",modules=base+"/modules/";
        std::vector<std::string> candidate;
        std::size_t start=0;
        for(std::size_t at=0;at<bytes;++at){
            if(data[at]){
                if(data[at]<32 || data[at]>126 || at-start>=4096)return false;
                continue;
            }
            if(at==start || candidate.size()==MaximumArguments)return false;
            std::string value(reinterpret_cast<const char*>(data+start),at-start);
            if(value.front()=='/'){
                if(!canonicalPath(value) || (value.compare(0,run.size(),run)!=0 &&
                    value.compare(0,inputs.size(),inputs)!=0 && value.compare(0,modules.size(),modules)!=0))return false;
            } else if(value.find('/')!=value.npos || value.find('\\')!=value.npos)return false;
            candidate.push_back(std::move(value));start=at+1;
        }
        if(candidate.size()<11 || candidate[0]!=run+"host_guest_trace" ||
            candidate[1]!=run+"decoded.image.bin")return false;
        output.swap(candidate);return true;
    } catch(...) {return false;}
}
}
