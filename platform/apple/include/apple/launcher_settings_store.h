// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LauncherDisplayOptions.hpp"
#include <optional>

namespace recomp::apple::launcher {
enum class LoadStatus { Missing, Loaded, Invalid, Unavailable };
struct LoadedSettings {
    LoadStatus status;
    std::optional<StoredDisplay> value;
    int error=0;
};
enum class SaveStatus { Saved, Conflict, Invalid, Busy, Failed, CommittedUncertain };
struct SavedSettings {
    SaveStatus status;
    std::optional<StoredDisplay> value;
    int error=0;
};
#if defined(APPLE_LAUNCHER_STORE_TESTING)
enum class StoreFault { None, PartialWrite, FileSync, BeforeRename, DirectorySync };
using StoreReadHook=void(*)(int,void*);
#endif

// The host supplies an already opened, owned settings-directory descriptor.
// No game-root discovery, defaults, flag/asset/save writes or renderer publishing.
// This store writes only apple-launcher-display-v1.txt and its named lock/temp.
// Save compares the complete previously loaded snapshot under a nonblocking lock.
// A post-rename durability failure is NOT successful Save; its value must never
// be published as if it were a verified commit. Main integration is separate.
class LauncherSettingsStore {
public:
    explicit LauncherSettingsStore(int settingsDirectory) noexcept;
    ~LauncherSettingsStore();
    LauncherSettingsStore(const LauncherSettingsStore&)=delete;
    LauncherSettingsStore& operator=(const LauncherSettingsStore&)=delete;
    bool ready() const noexcept {return directory_>=0;}
    LoadedSettings load(
#if defined(APPLE_LAUNCHER_STORE_TESTING)
                        StoreReadHook hook=nullptr,void* context=nullptr
#endif
                        ) const;
    SavedSettings save(const std::optional<StoredDisplay>& expected,const DisplayChoices& choices
#if defined(APPLE_LAUNCHER_STORE_TESTING)
                       ,StoreFault fault=StoreFault::None
#endif
                       );
private:
    int directory_=-1;
};
}
