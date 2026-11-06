#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recomp::bootstrap {

struct GuestFileMetadata {
    // Creation/access/write/change FILETIMEs, allocation size, end of file.
    std::array<std::uint64_t, 6> values{};
    std::uint32_t attributes{0};
};

enum class GuestFileQueryState { Found, NotFound, Unavailable };
struct GuestFileQueryResult {
    GuestFileQueryState state{GuestFileQueryState::Unavailable};
    GuestFileMetadata metadata{};
};

// Only bounded absolute ASCII DOS paths. No host filesystem access, dot
// traversal, wildcard expansion, device-path inference or locale folding.
inline std::optional<std::string> normalizeGuestFilePath(std::string_view path) {
    if (path.empty() || path.size() > 1024) { return std::nullopt; }
    if (path.substr(0, 4) == "\\??\\") { path.remove_prefix(4); }
    const auto colon = path.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 32) { return std::nullopt; }
    std::string normalized;
    for (std::size_t i = 0; i < path.size(); ++i) {
        auto c = static_cast<unsigned char>(path[i]);
        if (c < 32 || c >= 127 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|'
                || (c == ':' && i != colon)) { return std::nullopt; }
        if (i < colon && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_')) { return std::nullopt; }
        if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; }
        normalized.push_back(c == '/' ? '\\' : static_cast<char>(c));
    }
    if (normalized.size() == colon + 1) { return normalized + '\\'; }
    if (normalized[colon + 1] != '\\') { return std::nullopt; }
    if (normalized.size() == colon + 2) { return normalized; }
    std::size_t start = colon + 2;
    while (start < normalized.size()) {
        const auto end = normalized.find('\\', start);
        const auto part = normalized.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') {
            return std::nullopt;
        }
        if (end == std::string::npos) { return normalized; }
        start = end + 1;
    }
    // Trailing separator is accepted for directory queries, without retaining it.
    normalized.pop_back();
    return normalized;
}

// Immutable metadata snapshot for ONE explicit mounted namespace. This does
// not open files or claim that file contents have been staged on Android.
class GuestFileMetadataCatalog final {
public:
    static constexpr std::size_t MaximumBytes = 32 * 1024 * 1024;
    static constexpr std::size_t MaximumRecords = 16384;
    bool load(const std::vector<std::uint8_t>& bytes) {
        records_.clear(); mount_.clear();
        if (bytes.size() < 16 || bytes.size() > MaximumBytes
                || std::string_view(reinterpret_cast<const char*>(bytes.data()), 8) != std::string_view("GFMETA1\0", 8)) {
            return false;
        }
        std::size_t cursor = 8;
        const auto integer = [&](std::size_t size, std::uint64_t& value) {
            if (size > bytes.size() - cursor) { return false; }
            value = 0;
            for (std::size_t i = 0; i < size; ++i) { value = (value << 8) | bytes[cursor++]; }
            return true;
        };
        std::uint64_t count = 0, length = 0;
        if (!integer(4, count) || !integer(4, length) || count == 0 || count > MaximumRecords
                || length < 2 || length > 33 || length > bytes.size() - cursor) { return false; }
        std::string mount(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
        const auto root = normalizeGuestFilePath(mount);
        if (!root || *root != mount + '\\' || mount.back() != ':') { return false; }
        std::map<std::string, GuestFileMetadata> records;
        for (std::size_t i = 0; i < count; ++i) {
            if (!integer(4, length) || length > 1024 || length > bytes.size() - cursor) { return false; }
            std::string path(reinterpret_cast<const char*>(bytes.data() + cursor), length);
            cursor += length;
            const auto normalized = normalizeGuestFilePath(path);
            if (!normalized || *normalized != path || path.substr(0, root->size()) != *root) { return false; }
            GuestFileMetadata metadata;
            for (auto& value : metadata.values) {
                if (!integer(8, value) || value > INT64_MAX) { return false; }
            }
            std::uint64_t attributes = 0;
            if (!integer(4, attributes) || (attributes != 0x11 && attributes != 0x01)
                    || metadata.values[4] < metadata.values[5]
                    || (attributes == 0x11 && (metadata.values[4] != 0 || metadata.values[5] != 0))) { return false; }
            metadata.attributes = static_cast<std::uint32_t>(attributes);
            if (!records.emplace(path, metadata).second) { return false; }
        }
        const auto foundRoot = records.find(*root);
        if (cursor != bytes.size() || foundRoot == records.end() || foundRoot->second.attributes != 0x11) { return false; }
        // Every nested entry must have an explicit directory parent. Missing
        // directories cannot accidentally make an incomplete manifest authoritative.
        for (const auto& record : records) {
            if (record.first == *root) { continue; }
            auto parent = record.first.substr(0, record.first.rfind('\\'));
            if (parent == mount) { parent = *root; }
            const auto found = records.find(parent);
            if (found == records.end() || found->second.attributes != 0x11) { return false; }
        }
        records_ = std::move(records); mount_ = *root;
        return true;
    }
    [[nodiscard]] GuestFileQueryResult query(std::string_view path) const {
        const auto normalized = normalizeGuestFilePath(path);
        if (mount_.empty() || !normalized || normalized->substr(0, mount_.size()) != mount_) { return {}; }
        const auto found = records_.find(*normalized);
        if (found == records_.end()) { return {GuestFileQueryState::NotFound, {}}; }
        return {GuestFileQueryState::Found, found->second};
    }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] std::uint32_t recordIndex(std::string_view path) const {
        const auto normalized = normalizeGuestFilePath(path);
        if (!normalized) { return 0; }
        const auto found = records_.find(*normalized);
        return found == records_.end() ? 0 : static_cast<std::uint32_t>(std::distance(records_.begin(), found) + 1);
    }
    [[nodiscard]] std::optional<std::string> pathAt(std::uint32_t index) const {
        if (index == 0 || index > records_.size()) { return std::nullopt; }
        auto found = records_.begin(); std::advance(found, index - 1);
        return found->first;
    }
private:
    std::map<std::string, GuestFileMetadata> records_;
    std::string mount_;
};
} // namespace recomp::bootstrap
