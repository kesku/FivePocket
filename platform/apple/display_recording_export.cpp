// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/display_recording_export.h"
#include <array>
#include <cmath>
#include <cstring>
#include <xlocale.h>

namespace recomp::apple::performance {
namespace {
bool identifier(const char* id) noexcept {
    if(!id)return false;
    const auto length=std::strlen(id);
    if(!length || length>64)return false;
    const auto alnum=[](char c) {return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9');};
    if(!alnum(id[0]))return false;
    for(std::size_t i=0;i<length;++i)if(!alnum(id[i]) && id[i]!='_' && id[i]!='-')return false;
    return true;
}
struct NumericLocale {
    locale_t value=newlocale(LC_NUMERIC_MASK,"C",nullptr);
    ~NumericLocale() {if(value)freelocale(value);}
};
}
bool exportDisplaySnapshot(FILE* file,const char* id,const DisplaySnapshot& s) noexcept {
    if(!file || !identifier(id))return false;
    NumericLocale locale;if(!locale.value)return false;
    // IDs are decimal strings to avoid downstream JavaScript >2^53 rounding.
    if(std::fprintf(file,"{\"schema\":\"apple-drawable-observation/2\",\"kind\":\"summary\",\"run_id\":\"%s\","
        "\"clock\":\"apple-drawable-host-seconds\",\"clock_joined\":false,"
        "\"physical_delivery_complete\":false,\"snapshot_advisory\":true,"
        "\"layer_epoch\":\"%llu\",\"closed\":%s,\"acquired\":%llu,\"completed\":%llu,"
        "\"rejected_registrations\":%llu,\"overflow\":%s,\"duplicate_callback\":%s,"
        "\"invalid_time\":%s,\"invalid_identity\":%s,\"registered_complete_at_snapshot\":%s}\n",
        id,(unsigned long long)s.layerEpoch,s.closed?"true":"false",(unsigned long long)s.acquired,
        (unsigned long long)s.completed,(unsigned long long)s.rejectedRegistrations,
        s.overflow?"true":"false",s.duplicateCallback?"true":"false",s.invalidTime?"true":"false",
        s.invalidIdentity?"true":"false",s.deliveryComplete()?"true":"false")<0)return false;
    for(const auto& row:s.samples) {
        const bool valid=std::isfinite(row.presentedHostSeconds) && row.presentedHostSeconds>0;
        if(std::fprintf(file,"{\"schema\":\"apple-drawable-observation/2\",\"kind\":\"sample\",\"run_id\":\"%s\","
            "\"ordinal\":%llu,\"drawable_id\":\"%llu\",\"presented_host_seconds\":",
            id,(unsigned long long)row.ordinal,(unsigned long long)row.drawableId)<0)return false;
        if(valid) {
            // JSON numbers must stay locale-independent even if the app's
            // process locale changes. This exporter runs away from callbacks;
            // an explicit C locale also supports the retained macOS13.0 floor.
            std::array<char,64> number{};
            const int length=snprintf_l(number.data(),number.size(),locale.value,"%.17g",row.presentedHostSeconds);
            if(length<0 || std::size_t(length)>=number.size() ||
               std::fwrite(number.data(),1,std::size_t(length),file)!=std::size_t(length))return false;
        }
        else if(std::fputs("null",file)==EOF)return false;
        // Metal documents zero as not presented or skipped, not a timestamp.
        // Preserve that observation separately from nonfinite/negative data.
        if(std::fprintf(file,",\"valid_time\":%s,\"reported_zero\":%s,\"frame_sequence_joined\":false}\n",
            valid?"true":"false",row.presentedHostSeconds==0?"true":"false")<0)return false;
    }
    return std::fflush(file)==0 && !std::ferror(file);
}
}
