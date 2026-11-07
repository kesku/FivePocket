#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace recomp::bootstrap {

// Explicit snapshot for one title on virtual read-only device 1. Not a license
// grant, account identity, signature verifier, or a successful content mount.
// XCONTENT_DATA layout: two BE words, 128 BE UTF-16 units, 42-byte name + 2 pad.
class GuestContentCatalog final {
public:
    static constexpr std::size_t RecordBytes = 0x134;
    static constexpr std::size_t MaximumRecords = 16;
    static constexpr std::size_t MaximumBytes = 16 + MaximumRecords * RecordBytes;
    using Record = std::array<std::uint8_t, RecordBytes>;

    [[nodiscard]] bool load(const std::vector<std::uint8_t>& bytes) {
        records_.clear(); title_ = 0;
        const std::array<std::uint8_t, 8> magic{'G','X','C','O','N','T','1',0};
        if (bytes.size() < 16 || bytes.size() > MaximumBytes || !std::equal(magic.begin(), magic.end(), bytes.begin())) { return false; }
        const auto word = [](const std::uint8_t* p) {
            return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
        };
        const auto title = word(bytes.data() + 8), count = word(bytes.data() + 12);
        if (title == 0 || count == 0 || count > MaximumRecords || bytes.size() != 16 + count * RecordBytes) { return false; }
        std::vector<Record> records;
        std::set<std::string> names;
        for (std::uint32_t i = 0; i < count; ++i) {
            Record record{};
            std::copy_n(bytes.data() + 16 + i * RecordBytes, RecordBytes, record.begin());
            if (word(record.data()) != 1 || word(record.data() + 4) != 2) { return false; }
            bool ended = false, surrogate = false;
            for (std::size_t at = 8; at < 264; at += 2) {
                const auto ch = (std::uint32_t{record[at]} << 8) | record[at + 1];
                if ((ended && ch != 0) || (surrogate && (ch < 0xDC00 || ch > 0xDFFF))
                        || (!surrogate && ch >= 0xDC00 && ch <= 0xDFFF)) { return false; }
                if (ch == 0) { ended = true; }
                surrogate = ch >= 0xD800 && ch <= 0xDBFF;
            }
            if (surrogate) { return false; }
            std::string name;
            ended = false;
            for (std::size_t at = 264; at < RecordBytes; ++at) {
                const auto ch = record[at];
                if (ch == 0) { ended = true; continue; }
                if (ended || at >= 304 || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) { return false; }
                name.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
            }
            if (name.empty() || !names.insert(name).second) { return false; }
            records.push_back(record);
        }
        records_ = std::move(records); title_ = title;
        return true;
    }
    [[nodiscard]] std::uint32_t title() const noexcept { return title_; }
    [[nodiscard]] const std::vector<Record>& records() const noexcept { return records_; }
private:
    std::uint32_t title_{0};
    std::vector<Record> records_;
};
} // namespace recomp::bootstrap
