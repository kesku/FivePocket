// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace recomp::apple {
enum class MeasurementRequest { Disabled,Enabled,Invalid };
struct MeasurementLaunch {MeasurementRequest request=MeasurementRequest::Disabled;std::string id;};
inline MeasurementLaunch measurementLaunch(const std::vector<std::string_view>& arguments) {
    MeasurementLaunch result;
    constexpr std::string_view prefix="--measure-performance=";
    for(auto arg:arguments) {
        if(arg=="--measure-performance")return {MeasurementRequest::Invalid,{}};
        if(!arg.starts_with(prefix))continue;
        if(result.request!=MeasurementRequest::Disabled)return {MeasurementRequest::Invalid,{}};
        const auto id=arg.substr(prefix.size());
        const auto alnum=[](char c) {return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9');};
        if(id.empty() || id.size()>64 || !alnum(id.front()))return {MeasurementRequest::Invalid,{}};
        for(char c:id)if(!alnum(c) && c!='-' && c!='_')return {MeasurementRequest::Invalid,{}};
        result={MeasurementRequest::Enabled,std::string(id)};
    }
    return result;
}
}
