#pragma once
#include "guest_file_metadata.h"

namespace recomp::bootstrap {
// Guest DOS namespace only. No host filesystem calls, device inference or
// target deletion. Open file handles retain their existing bound source.
class GuestSymbolicLinks {
public:
    static constexpr std::size_t Limit=32;
    static std::optional<std::string> rootName(std::string_view input) {
        if (input.substr(0,4)=="\\??\\") input.remove_prefix(4);
        if (input.empty() || input.back()!=':' || input.find(':')!=input.size()-1) return std::nullopt;
        const auto path=normalizeGuestFilePath(input);
        if (!path) return std::nullopt;
        return path->substr(0,path->size()-1);
    }
    bool create(std::string_view name,std::string_view target) {
        const auto root=rootName(name), destination=normalizeGuestFilePath(target);
        if (!root || !destination || links_.size()>=Limit || links_.count(*root)) return false;
        // Validate a proposed graph independently, preserving existing links on
        // cycles, expansion limits or allocation failure.
        auto candidate=*this;
        candidate.links_.emplace(*root,*destination);
        for (const auto& item : candidate.links_) if (!candidate.resolve(item.first+"\\")) return false;
        links_.swap(candidate.links_); return true;
    }
    bool erase(std::string_view name) {
        const auto root=rootName(name);
        return root && links_.erase(*root)!=0;
    }
    bool contains(std::string_view name) const {
        const auto root=rootName(name); return root && links_.count(*root);
    }
    std::size_t size() const noexcept { return links_.size(); }
    void clear() noexcept { links_.clear(); }
    std::optional<std::string> resolve(std::string_view path) const {
        auto current=normalizeGuestFilePath(path);
        if (!current) return std::nullopt;
        for (std::size_t hops=0;hops<=links_.size();++hops) {
            const auto colon=current->find(':');
            const auto link=links_.find(current->substr(0,colon+1));
            if (link==links_.end()) return current;
            auto replacement=link->second;
            const auto suffix=current->substr(colon+2);
            if (!suffix.empty()) {
                if (replacement.back()!='\\') replacement+='\\';
                replacement+=suffix;
            }
            current=normalizeGuestFilePath(replacement);
            if (!current) return std::nullopt;
        }
        return std::nullopt;
    }
private:
    std::map<std::string,std::string> links_;
};
}
