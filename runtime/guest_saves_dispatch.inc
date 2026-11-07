// v142 (saves): a local signed-in profile for user 0 and persistent saved-game content.
// Included inside GuestImportRegistry::dispatchUnderRegistryLock after the address-space check: member scope, the
// registry lock is held (`lock` is its unique_lock), `importName` and `call` are the dispatch arguments. All state is
// in savesStateFor(this) (file scope in guest_import_registry.cpp), because the shared registry header fixes the
// members. files/private-trace/diag-no-local-profile restores v141 exactly (no user signed in; nothing here runs).
//
// Evidence: GTA V's boot alert "You have not signed in. You will need to sign in to save your progress" (every run
// since the prologue first played); its save path (XDK XContentCreate wrapper sub_8363A408 via sub_8363A6F8) uses
// XCONTENT flags 0x12 (CREATE_ALWAYS) and 3 (OPEN_EXISTING) with an XOVERLAPPED for a signed-in user (callers
// sub_831E6418/6608/6B98/6D20), and title/installed content keeps the existing synchronous user-0xFF path.
// Semantics follow the XAM/kernel contracts as Xenia implements them (reference only): XamUserGetXUID offline type,
// XamUserCheckPrivilege NOT_LOGGED_ON for local profiles, overlapped completion (result, length, extended error,
// event), XamShowDeviceSelectorUI completing with the first storage device, saved games always created by the user.
// Cheap gate first: every import passes here, and the hot ones (Ke*, Rtl*, Kf*, Mm*) are not handled below.
if (!importsFastSkipSaves && // imports-fast B (guest_import_registry.cpp, before this include)
    importName.size() > 8 && importName.substr(0, 7) == "__imp__" && (importName[7] == 'X' || importName[7] == 'N'))
if (auto* sv = savesStateFor(this, nativeXamAddress_ != 0 && signedOutUsersBound_,
                             installedContent_.title())) {
    constexpr std::uint32_t IoPending = 997, ErrorSuccess = 0, ErrorNotLoggedOn = 1245,
            ErrorInvalidParameter = 87, ErrorInsufficientBuffer = 122, ErrorNoMoreFiles = 18, ErrorAlreadyExists = 183;
    constexpr std::uint32_t XNoSuchUser = 0x80070525U, XInvalidArg = 0x80070057U;
    constexpr std::uint32_t StatusEndOfFile = 0xC0000011U, StatusNameNotFound = 0xC0000034U,
            StatusAccessDenied = 0xC0000022U, StatusBufferTooSmall = 0xC0000023U;
    const auto writable = [&](std::uint32_t at, std::uint64_t n, std::uint32_t align = 4) {
        return validGuestDataBuffer(at, n, true, align) && !hasActiveCriticalSectionInRange(at, n);
    };
    const auto disjoint = [](std::uint64_t a, std::uint64_t an, std::uint64_t b, std::uint64_t bn) {
        return !(a < b + bn && b < a + an);
    };
    const auto write64 = [&](std::uint32_t at, std::uint64_t v) {
        (void)writeU32(at, std::uint32_t(v >> 32)); (void)writeU32(at + 4, std::uint32_t(v));
    };
    // Queue a notification for every listener whose area matches (postNotification's rule, under this lock).
    const auto post = [&](std::uint32_t id, std::uint32_t parameter) {
        const auto bit = std::uint64_t{1} << ((id >> 25) & 63);
        for (auto& [handle, listener] : notificationListeners_) {
            if ((listener.mask & bit) == 0 || ((id >> 16) & 511) > listener.maximumVersion
                    || listener.size == MaximumQueuedNotifications) { continue; }
            listener.queue[listener.size++] = {id, parameter};
            if (auto h = kernelHandles_.find(handle); h != kernelHandles_.end()) { h->second.signalCount = 1; }
            wakeHandleWaiters(this, handle);
        }
    };
    // XOVERLAPPED: [0] result, [1] length, [2] context, [3] event, [4] routine, [5] routine context, [6] ext. error.
    // Completion is immediate (the operation already ran); the caller gets ERROR_IO_PENDING as from real XAM.
    const auto complete = [&](std::uint32_t overlapped, std::uint32_t result, std::uint32_t length,
                              std::uint32_t extended) -> bool {
        if (!writable(overlapped, 28)) { return false; }
        std::uint32_t routine = 0, event = 0; (void)readU32(overlapped + 16, routine); (void)readU32(overlapped + 12, event);
        if (routine != 0) { return false; } // APC completion routines are not modeled.
        (void)writeU32(overlapped, result); (void)writeU32(overlapped + 4, length); (void)writeU32(overlapped + 24, extended);
        if (event != 0) {
            const auto found = kernelHandles_.find(event);
            if (found == kernelHandles_.end() || (found->second.kind != KernelObjectKind::NotificationEvent
                    && found->second.kind != KernelObjectKind::SynchronizationEvent)) { return false; }
            found->second.signalCount = 1; wakeHandleWaiters(this, event);
        }
        return true;
    };
    const auto hresult = [](std::uint32_t win32) { return win32 == 0 ? 0U : (0x80070000U | (win32 & 0xFFFF)); };
    // Result delivery: synchronous (returnValue) or through the overlapped (IO_PENDING).
    const auto deliver = [&](std::string_view name, std::uint32_t overlapped, std::uint32_t result, std::uint32_t length,
                             const char* what) {
        if (overlapped == 0) { call.returnValue = result; return handled(name, what); }
        if (!complete(overlapped, result, length, hresult(result))) {
            return memoryFault(name, "Overlapped completion is not a bounded XOVERLAPPED with an owned event");
        }
        call.returnValue = IoPending; return handled(name, what);
    };
    const auto stackArgument = [&](unsigned index, std::uint32_t& value) { return call.readArgument32(*addressSpace_, index, value); };
    const bool user0 = call.argument32(0) == 0;

    // ---- Sign-in and identity (user 0 is a local, not Live, profile) -------------------------------------------
    if (importName == "__imp__XamUserGetSigninState" && user0) {
        call.returnValue = 1; // eXUserSigninState_SignedInLocally
        return handled(importName, "User 0 is signed in to the local profile");
    }
    if (importName == "__imp__XamUserGetXUID" && user0) {
        const auto flags = call.argument32(1), output = call.argument32(2);
        if (!output) { call.returnValue = XInvalidArg; return handled(importName, "Null XUID output"); }
        if (!writable(output, 8, 8)) { return memoryFault(importName, "XUID output is not bounded writable storage"); }
        write64(output, 0);
        if (flags & 1) { write64(output, sv->identity.xuid); call.returnValue = 0; return handled(importName, "Offline XUID of the local profile"); }
        call.returnValue = XNoSuchUser; // no online/Live XUID for a local profile
        return handled(importName, "Local profile has no online XUID");
    }
    if (importName == "__imp__XamUserGetSigninInfo" && user0) {
        const auto output = call.argument32(2);
        if (!output) { call.returnValue = XInvalidArg; return handled(importName, "Null sign-in info output"); }
        if (!writable(output, 40, 4)) { return memoryFault(importName, "Sign-in info output is not bounded writable storage"); }
        const auto info = profile::encodeSigninInfo(sv->identity, 1, call.argument32(1));
        std::memcpy(addressSpace_->translate(output), info.data(), info.size());
        call.returnValue = 0;
        return handled(importName, "Sign-in info of the local profile");
    }
    if (importName == "__imp__XamUserGetName" && user0) {
        const auto buffer = call.argument32(1), size = call.argument32(2);
        if (!buffer || !size) { call.returnValue = ErrorInvalidParameter; return handled(importName, "Null or empty name buffer"); }
        if (!writable(buffer, std::min<std::uint32_t>(size, 16), 1)) { return memoryFault(importName, "Name buffer is not bounded writable storage"); }
        const auto name = profile::encodeName(sv->identity, std::min<std::uint32_t>(size, 16));
        std::memcpy(addressSpace_->translate(buffer), name.data(), std::min<std::size_t>(name.size(), std::min<std::uint32_t>(size, 16)));
        call.returnValue = 0;
        return handled(importName, "Gamertag of the local profile");
    }
    if (importName == "__imp__XamUserCheckPrivilege" && (user0 || call.argument32(0) == 0xFF)) {
        const auto output = call.argument32(2);
        if (output && !writable(output, 4)) { return memoryFault(importName, "Privilege output is not bounded writable storage"); }
        if (output) { (void)writeU32(output, 0); }
        call.returnValue = ErrorNotLoggedOn; // local profile: no Live privileges
        return handled(importName, "Local profile is not signed in to Live; no privilege granted");
    }
    if (importName == "__imp__XamNotifyCreateListener") {
        auto result = dispatchXamNotifyCreateListener(call);
        const auto handle = static_cast<std::uint32_t>(call.returnValue);
        if (const auto listener = notificationListeners_.find(handle); handle && listener != notificationListeners_.end()
                && (listener->second.mask & 1) && listener->second.size < MaximumQueuedNotifications) {
            // As the system tells a new system-area listener of the signed-in user (XN_SYS_SIGNINCHANGED, user bits).
            listener->second.queue[listener->second.size++] = {0x0000000A, 1};
            kernelHandles_[handle].signalCount = 1; wakeHandleWaiters(this, handle);
        }
        return result;
    }
    // ---- Profile settings -------------------------------------------------------------------------------------
    if (importName == "__imp__XamUserReadProfileSettings" && call.argument32(1) == 0) {
        const auto xuidCount = call.argument32(2), xuids = call.argument32(3), count = call.argument32(4);
        const auto ids = call.argument32(5), sizePointer = call.argument32(6), buffer = call.argument32(7);
        std::uint32_t overlapped = 0;
        if (!stackArgument(8, overlapped)) { return memoryFault(importName, "Profile completion argument requires a bounded stack slot"); }
        if (xuidCount > 1) { return rejected(importName, "Multi-XUID profile lookup is not modeled"); }
        if (xuidCount == 1) {
            std::uint32_t hi = 0, lo = 0;
            if (!validGuestDataBuffer(xuids, 8, false, 4) || !readU32(xuids, hi) || !readU32(xuids + 4, lo)) {
                return memoryFault(importName, "Profile XUID list is not bounded guest data");
            }
            if (((std::uint64_t{hi} << 32) | lo) != sv->identity.xuid) { return rejected(importName, "Profile lookup for another XUID"); }
        }
        if (!count || count > 32 || !sizePointer || !ids) {
            return deliver(importName, overlapped, ErrorInvalidParameter, 0, "Invalid profile setting request");
        }
        if (!validGuestDataBuffer(ids, count * 4, false, 4) || !writable(sizePointer, 4)) {
            return memoryFault(importName, "Profile setting IDs or size output are not bounded guest data");
        }
        std::vector<std::uint32_t> list(count);
        for (std::uint32_t i = 0; i < count; ++i) { (void)readU32(ids + 4 * i, list[i]); }
        std::uint32_t size = 0; (void)readU32(sizePointer, size);
        const auto titleId = call.argument32(0) ? call.argument32(0) : sv->titleId;
        const auto& settings = sv->settingsFor(titleId);
        if (!settings.ok()) { return rejected(importName, "Local profile settings storage is unavailable"); }
        const auto encoded = profile::encodeRead(buffer, buffer ? size : 0, 0, list, settings);
        if (encoded.storageFault) { return rejected(importName, "Local profile settings storage fault"); }
        for (const auto id : encoded.skippedIds) { sv->noteSkippedSetting(id); }
        if (encoded.status == ErrorInsufficientBuffer) {
            if (encoded.writeNeededSize) { (void)writeU32(sizePointer, encoded.needed); }
            return deliver(importName, overlapped, ErrorInsufficientBuffer, 0, "Reported the profile buffer requirement");
        }
        if (encoded.status != 0) { return deliver(importName, overlapped, encoded.status, 0, "Profile read refused its arguments"); }
        if (!writable(buffer, encoded.bytes.size(), 4)) { return memoryFault(importName, "Profile buffer is not bounded writable storage"); }
        std::memcpy(addressSpace_->translate(buffer), encoded.bytes.data(), encoded.bytes.size());
        return deliver(importName, overlapped, ErrorSuccess, 0, "Read local profile settings (defaults and stored title settings)");
    }
    if (importName == "__imp__XamUserWriteProfileSettings" && call.argument32(1) == 0) {
        const auto count = call.argument32(2), settings = call.argument32(3), overlapped = call.argument32(4);
        if (!count || !settings || count > 32) { return deliver(importName, overlapped, ErrorInvalidParameter, 0, "Invalid profile write"); }
        const auto decoded = profile::decodeWrite(
                [&](std::uint32_t at, std::uint32_t n, std::uint8_t* out) {
                    if (n && !validGuestDataBuffer(at, n, false, 1)) { return false; }
                    if (n) { std::memcpy(out, addressSpace_->translate(at), n); } return true; },
                count, settings);
        if (decoded.memoryFault) { return memoryFault(importName, "Profile setting records are not bounded guest data"); }
        if (decoded.status != 0) { return deliver(importName, overlapped, decoded.status, 0, "Profile write refused its records"); }
        const auto titleId = call.argument32(0) ? call.argument32(0) : sv->titleId;
        auto& store = sv->settingsFor(titleId);
        if (!store.ok()) { return rejected(importName, "Local profile settings storage is unavailable"); }
        if (const auto e = store.writeAll(decoded.settings); e != 0) {
            if (e == 29 || e == 1392) { return rejected(importName, "Local profile settings storage fault"); }
            return deliver(importName, overlapped, e, 0, "Profile setting refused by the profile rules");
        }
        post(0x0000000E, 1); // XN_SYS_PROFILESETTINGCHANGED for user 0
        return deliver(importName, overlapped, ErrorSuccess, 0, "Stored local profile settings");
    }
    // ---- XGI (app FB): presence context/property and achievements for the local profile -----------------------
    if (importName == "__imp__XMsgStartIORequest" && call.argument32(0) == 0xFB
            && (call.argument32(1) == 0x000B0006 || call.argument32(1) == 0x000B0007 || call.argument32(1) == 0x000B0008)) {
        const auto message = call.argument32(1), overlapped = call.argument32(2), buffer = call.argument32(3), length = call.argument32(4);
        const std::uint32_t expected = message == 0x000B0006 ? 0x18 : message == 0x000B0007 ? 0x20 : 8;
        if ((length != 0 && length != expected) || !validGuestDataBuffer(buffer, expected, false, 4)) {
            return memoryFault(importName, "XGI request is not a bounded record of its message's size");
        }
        if (message == 0x000B0008) {
            std::uint32_t countAchieved = 0, list = 0; (void)readU32(buffer, countAchieved); (void)readU32(buffer + 4, list);
            if (countAchieved > 64 || (countAchieved && !validGuestDataBuffer(list, countAchieved * 12, false, 4))) {
                return memoryFault(importName, "Achievement list is not bounded guest data");
            }
            for (std::uint32_t i = 0; i < countAchieved; ++i) {
                std::uint32_t userIndex = 0, id = 0; (void)readU32(list + 12 * i, userIndex); (void)readU32(list + 12 * i + 4, id);
                sv->recordAchievement(id);
            }
        }
        return deliver(importName, overlapped, 0, 0, message == 0x000B0008 ? "Recorded local-profile achievements"
                       : "Accepted local-profile presence context/property (no Live presence)");
    }
    if (importName == "__imp__XMsgCancelIORequest") {
        // v145: GTA cancels an outstanding XMsg request 34 s into a signed-in boot. Every request this registry
        // starts completes before it returns (the XOVERLAPPED is final and its event set), so there is nothing left to
        // cancel: as XAM does for an already-completed request, return 0 (Xenia xam_msg.cc waits on the event when
        // asked to; it is already signalled). The overlapped is left unchanged.
        const auto overlapped = call.argument32(0);
        if (!validGuestDataBuffer(overlapped, 28, false, 4)) { return memoryFault(importName, "Cancelled XOVERLAPPED is not bounded"); }
        std::uint32_t result = 0; (void)readU32(overlapped, result);
        if (result == IoPending) { return rejected(importName, "Cancel of a still-pending request (none is modeled)"); }
        call.returnValue = 0;
        return handled(importName, "Request had already completed; nothing to cancel");
    }
    // v146: voice chat for the signed-in user. The Thor has no Xbox voice headset: XamVoiceCreate zeroes its output
    // and fails with ERROR_ACCESS_DENIED, a headset is never present, packets are refused (Xenia xam_voice.cc).
    if (importName == "__imp__XamVoiceCreate") {
        const auto output = call.argument32(2);
        if (output && !writable(output, 4)) { return memoryFault(importName, "Voice object output is not bounded"); }
        if (output) { (void)writeU32(output, 0); }
        call.returnValue = 5;
        return handled(importName, "No voice headset: no voice object");
    }
    if (importName == "__imp__XamVoiceClose") { call.returnValue = 0; return handled(importName, "No voice object to close"); }
    if (importName == "__imp__XamVoiceHeadsetPresent") { call.returnValue = 0; return handled(importName, "No voice headset present"); }
    if (importName == "__imp__XamVoiceSubmitPacket") { call.returnValue = 0x800700AAU; return handled(importName, "No voice device accepts packets"); }
    // ---- Storage device ---------------------------------------------------------------------------------------
    if (importName == "__imp__XamShowDeviceSelectorUI") {
        const auto user = call.argument32(0), flags = call.argument32(2), output = call.argument32(4), overlapped = call.argument32(5);
        if (!overlapped) { call.returnValue = ErrorInvalidParameter; return handled(importName, "Device selector requires an XOVERLAPPED"); }
        if ((user >= 4 && user != 0xFF) || (flags & 0x83F00008U) != 0 || !output) {
            return deliver(importName, overlapped, ErrorInvalidParameter, 0, "Invalid device selector arguments");
        }
        if (!writable(output, 4)) { return memoryFault(importName, "Device id output is not bounded writable storage"); }
        post(0x00000009, 1); // XN_SYS_UI shown ...
        (void)writeU32(output, 1);
        post(0x00000009, 0); // ... and dismissed: the single storage device is chosen without a dialog.
        return deliver(importName, overlapped, ErrorSuccess, 0, "Selected the only storage device (local save storage)");
    }
    if (importName == "__imp__XamContentGetDeviceData" && call.argument32(0) == 1) {
        const auto output = call.argument32(1);
        if (!writable(output, 80, 8)) { return memoryFault(importName, "Content device output is not a bounded 80-byte structure"); }
        std::memset(addressSpace_->translate(output), 0, 80);
        (void)writeU32(output, 1); (void)writeU32(output + 4, 1);
        write64(output + 8, sv->store->totalBytes()); write64(output + 16, sv->store->freeBytes());
        constexpr std::string_view Label{"Local save storage"};
        for (std::size_t i = 0; i < Label.size(); ++i) { addressSpace_->translate(output)[25 + i * 2] = static_cast<std::uint8_t>(Label[i]); }
        call.returnValue = 0;
        return handled(importName, "Reported local save storage capacity and free space");
    }
    // ---- Saved-game content -----------------------------------------------------------------------------------
    const auto saveKey = [&](std::uint32_t record) -> std::optional<saves::ContentKey> {
        if (!validGuestDataBuffer(record, GuestContentCatalog::RecordBytes, false, 4)) { return std::nullopt; }
        return saves::keyFromRecord(sv->identity.xuid, addressSpace_->translate(record));
    };
    const auto isSaveRecord = [&](std::uint32_t record) {
        std::uint32_t device = 0, type = 0;
        return validGuestDataBuffer(record, 8, false, 4) && readU32(record, device) && readU32(record + 4, type) && type == 1 && device == 1;
    };
    if (importName == "__imp__XamContentCreateEx" && user0 && !isSaveRecord(call.argument32(2))) {
        std::uint32_t device = 0, type = 0; (void)readU32(call.argument32(2), device); (void)readU32(call.argument32(2) + 4, type);
        std::string name; if (validGuestDataBuffer(call.argument32(2) + 264, 42, false, 1)) {
            for (unsigned i = 0; i < 42; ++i) { const char c = char(addressSpace_->translate(call.argument32(2) + 264)[i]); if (!c) { break; } name.push_back(c); } }
        __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_CONTENT_CREATE_OTHER user=0 device=%u type=%u name=%s flags=%08x",
                            device, type, name.c_str(), call.argument32(3));
        if (type == 2) {
            // v148: the signed-in user mounts the console's installed marketplace content (v147 boot, 66 s): the same
            // packages the no-profile (0xFF) path serves.
            call.arguments[0] = 0xFF;
            auto result = dispatchContentMount(call, false);
            call.arguments[0] = 0;
            return result;
        }
    }
    if (importName == "__imp__XamContentCreateEx" && user0 && isSaveRecord(call.argument32(2))) {
        const auto rootPointer = call.argument32(1), record = call.argument32(2), flags = call.argument32(3);
        const auto dispositionOut = call.argument32(4), licenseOut = call.argument32(5);
        std::uint32_t overlapped = 0;
        if (!stackArgument(8, overlapped)) { return memoryFault(importName, "Content overlapped argument requires a bounded stack slot"); }
        std::string root;
        if (!readAnsiString(rootPointer, 33, root)) { return memoryFault(importName, "Content root is not a bounded string"); }
        const auto normalized = normalizeGuestFilePath(root + ":\\");
        if (!normalized || normalized->size() != root.size() + 2 || root.find_first_of(":/\\") != root.npos) {
            return rejected(importName, "Save content root must be a simple DOS mount name");
        }
        const auto rootName = normalized->substr(0, normalized->size() - 1);
        const auto disposition = flags & 0xF;
        if (disposition < 1 || disposition > 5 || (flags & ~0x3FF0U & ~0xFU) != 0) {
            return rejected(importName, "Unknown saved-game content flags");
        }
        if ((dispositionOut && !writable(dispositionOut, 4)) || (licenseOut && !writable(licenseOut, 4))) {
            return memoryFault(importName, "Content disposition/license outputs are not bounded writable storage");
        }
        const auto key = saveKey(record);
        if (!key) { return deliver(importName, overlapped, ErrorInvalidParameter, 0, "Saved-game content name is invalid"); }
        if (sv->mounts.count(rootName) || contentMounts_.count(rootName) || symbolicLinks_.contains(rootName)) {
            return deliver(importName, overlapped, ErrorAlreadyExists, 0, "Content root is already mounted");
        }
        const auto opened = sv->store->openContent(*key, disposition, addressSpace_->translate(record), rootName);
        // v166: every save-content open is logged, so a boot that loads the autosave shows it directly.
        __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_CONTENT_OPEN root=%s disposition=%u error=%u created=%u",
                            rootName.c_str(), disposition, opened.error, unsigned(opened.error == 0 && opened.created));
        if (opened.error != 0) {
            if (dispositionOut) { (void)writeU32(dispositionOut, 0); }
            return deliver(importName, overlapped, opened.error, 0, "Saved-game content open failed as requested (absent/existing/storage)");
        }
        sv->mounts.emplace(rootName, *key);
        if (dispositionOut) { (void)writeU32(dispositionOut, opened.created ? 1 : 2); }
        if (licenseOut) { (void)writeU32(licenseOut, 0); }
        ++sv->contentOpens;
        return deliver(importName, overlapped, ErrorSuccess, 0, opened.created ? "Created saved-game content in local storage"
                                                                              : "Opened saved-game content from local storage");
    }
    if (importName == "__imp__XamContentClose") {
        std::string root;
        if (readAnsiString(call.argument32(0), 33, root)) {
            const auto normalized = normalizeGuestFilePath(root + ":\\");
            const auto rootName = normalized ? normalized->substr(0, normalized->size() - 1) : std::string{};
            if (const auto mount = sv->mounts.find(rootName); mount != sv->mounts.end()) {
                for (auto it = sv->files.begin(); it != sv->files.end();) {
                    if (it->second.root == rootName) {
                        __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_FILE_CLOSE path=%s%s read=%llu written=%llu (content close)",
                                            it->second.root.c_str(), it->second.path.c_str(), (unsigned long long)it->second.bytesRead,
                                            (unsigned long long)it->second.bytesWritten);
                        sv->closeFile(it->second); ::recomp::imports_fast::eraseKey(kernelHandles_, it->first, ::recomp::imports_fast::handleGenerations()); it = sv->files.erase(it);
                    }
                    else { ++it; }
                }
                (void)sv->store->closeContent(mount->second, mount->first); sv->mounts.erase(mount); // save-replace-safety: a replaced save is committed here
                return deliver(importName, call.argument32(1), ErrorSuccess, 0, "Closed saved-game content (files flushed and closed)");
            }
        }
    }
    if (importName == "__imp__XamUserCreateAchievementEnumerator") {
        // v143: GTA creates this 7 s into boot once a user is signed in (title 0, user 0, flags ~0, 80 items). The
        // buffer size follows XAM (36-byte X_ACHIEVEMENT_DETAILS + 464 string bytes when flags & 7, per item); the
        // enumeration itself reports no achievement definitions (the title's XDBF achievement table is not parsed):
        // XamEnumerate returns ERROR_NO_MORE_FILES. Awarded achievements are still recorded (XGI B0008).
        const auto user = call.argument32(1), flags = call.argument32(3), count = call.argument32(5);
        const auto sizeOut = call.argument32(6), handleOut = call.argument32(7);
        if (!count || !sizeOut || !handleOut || user >= 4) { call.returnValue = ErrorInvalidParameter; return handled(importName, "Invalid achievement enumerator arguments"); }
        if (user != 0) { call.returnValue = ErrorInvalidParameter; return handled(importName, "No profile in that user slot"); }
        if (!writable(sizeOut, 4) || !writable(handleOut, 4) || sizeOut == handleOut || count > 4096) {
            return memoryFault(importName, "Achievement enumerator outputs are not bounded");
        }
        const std::uint32_t itemSize = 36 + ((flags & 7) ? 464 : 0);
        std::uint32_t handle = 0;
        try {
            handle = createKernelHandle({KernelObjectKind::ContentEnumerator, 0, 0});
            if (handle) { SaveEnumerator e; e.capacity = count; e.itemSize = itemSize; sv->enums[handle] = std::move(e); }
        } catch (const std::bad_alloc&) { if (handle) { ::recomp::imports_fast::eraseKey(kernelHandles_, handle, ::recomp::imports_fast::handleGenerations()); } handle = 0; }
        if (!handle) { call.returnValue = 8; return handled(importName, "Enumerator allocation failed"); }
        (void)writeU32(sizeOut, itemSize * count); (void)writeU32(handleOut, handle);
        __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_ACHIEVEMENT_ENUMERATOR title=%08x count=%u flags=%08x (no definitions modeled)",
                            call.argument32(0), count, flags);
        call.returnValue = 0;
        return handled(importName, "Created an achievement enumerator (no achievement definitions modeled)");
    }
    if (importName == "__imp__XamContentCreateEnumerator" && user0 && call.argument32(2) == 2) {
        // v144: with user 0 signed in, GTA enumerates marketplace content (type 2) as that user (v143 boot, 7 s):
        // a user's marketplace enumeration includes the console's shared installed content, i.e. exactly the
        // installed snapshot the title-context (0xFE) path already serves. Same handler, same rows.
        call.arguments[0] = 0xFE;
        auto result = dispatchContentEnumeration(call, true);
        call.arguments[0] = 0;
        return result;
    }
    if (importName == "__imp__XamContentCreateEnumerator" && user0 && call.argument32(2) == 1) {
        const auto device = call.argument32(1), capacity = call.argument32(4), sizeOut = call.argument32(5), handleOut = call.argument32(6);
        // v147: GTA asks for 85 items (flags 0x1000) after the display calibration; XAM sizes the buffer from the count.
        if ((device != 0 && device != 1) || capacity == 0 || capacity > 1024) { return rejected(importName, "Unsupported saved-game enumeration"); }
        if (!writable(sizeOut, 4) || !writable(handleOut, 4) || sizeOut == handleOut) {
            return memoryFault(importName, "Enumerator outputs must be disjoint bounded external words");
        }
        std::uint32_t handle = 0;
        try {
            handle = createKernelHandle({KernelObjectKind::ContentEnumerator, 0, 0});
            if (handle) { SaveEnumerator e; e.records = sv->store->enumerate(sv->identity.xuid, 1, 1); e.capacity = capacity; sv->enums[handle] = std::move(e); }
        } catch (const std::bad_alloc&) { if (handle) { ::recomp::imports_fast::eraseKey(kernelHandles_, handle, ::recomp::imports_fast::handleGenerations()); } handle = 0; }
        if (!handle) { call.returnValue = 8; return handled(importName, "Enumerator allocation failed"); }
        (void)writeU32(sizeOut, capacity * GuestContentCatalog::RecordBytes); (void)writeU32(handleOut, handle);
        call.returnValue = 0;
        return handled(importName, "Created an enumerator over local saved games");
    }
    if (importName == "__imp__XamEnumerate") {
        if (const auto found = sv->enums.find(call.argument32(0)); found != sv->enums.end()) {
            const auto buffer = call.argument32(2), length = call.argument32(3), itemsOut = call.argument32(4), overlapped = call.argument32(5);
            auto& e = found->second;
            if (itemsOut && !writable(itemsOut, 4)) { return memoryFault(importName, "Enumerator count output is not bounded"); }
            if (length < e.capacity * e.itemSize || !writable(buffer, length)) {
                if (itemsOut) { (void)writeU32(itemsOut, 0); }
                return deliver(importName, overlapped, ErrorInsufficientBuffer, 0, "Enumerator buffer is smaller than its capacity");
            }
            const auto n = std::min<std::size_t>(e.capacity, e.records.size() - e.cursor);
            for (std::size_t i = 0; i < n; ++i) {
                std::memcpy(addressSpace_->translate(buffer + std::uint32_t(i * GuestContentCatalog::RecordBytes)),
                            e.records[e.cursor + i].data(), GuestContentCatalog::RecordBytes);
            }
            e.cursor += n;
            if (itemsOut) { (void)writeU32(itemsOut, std::uint32_t(n)); }
            return deliver(importName, overlapped, n ? ErrorSuccess : ErrorNoMoreFiles, std::uint32_t(n), "Enumerated local saved games");
        }
    }
    if ((importName == "__imp__XamContentGetCreator" || importName == "__imp__XamContentSetThumbnail"
            || importName == "__imp__XamContentDelete") && user0 && isSaveRecord(call.argument32(1))) {
        const auto key = saveKey(call.argument32(1));
        if (!key) { return rejected(importName, "Saved-game content name is invalid"); }
        if (importName == "__imp__XamContentGetCreator") {
            const auto isCreator = call.argument32(2), xuidOut = call.argument32(3), overlapped = call.argument32(4);
            if (!isCreator) { call.returnValue = ErrorInvalidParameter; return handled(importName, "Null creator output"); }
            if (!writable(isCreator, 4) || (xuidOut && !writable(xuidOut, 8, 8))) { return memoryFault(importName, "Creator outputs are not bounded"); }
            if (!sv->store->exists(*key)) { return deliver(importName, overlapped, 3, 0, "Saved game does not exist"); } // ERROR_PATH_NOT_FOUND
            (void)writeU32(isCreator, 1); if (xuidOut) { write64(xuidOut, sv->store->creator(*key).value_or(sv->identity.xuid)); }
            return deliver(importName, overlapped, ErrorSuccess, 0, "The local profile created this saved game");
        }
        if (importName == "__imp__XamContentSetThumbnail") {
            const auto image = call.argument32(2), size = call.argument32(3), overlapped = call.argument32(4);
            if (size > 65536 || (size && !validGuestDataBuffer(image, size, false, 1))) { return memoryFault(importName, "Thumbnail is not bounded guest data"); }
            return deliver(importName, overlapped, sv->store->setThumbnail(*key, size ? addressSpace_->translate(image) : nullptr, size), 0,
                           "Stored the saved-game thumbnail");
        }
        for (const auto& [mounted, mountedKey] : sv->mounts) {
            if (saves::sameContent(mountedKey, *key)) { return deliver(importName, call.argument32(2), 5, 0, "Saved game is mounted; not deleted"); }
        }
        return deliver(importName, call.argument32(2), sv->store->remove(*key), 0, "Deleted the saved game from local storage");
    }
    // ---- Files inside mounted saved-game content ----------------------------------------------------------------
    // Object attributes -> resolved lower-case path; the save mount it names (or nullptr).
    const auto savePath = [&](std::uint32_t object, std::string& path) -> const saves::ContentKey* {
        std::uint32_t root = 0, descriptor = 0, pointer = 0; std::uint16_t length = 0, maximum = 0;
        if (!validGuestDataBuffer(object, 12, false, 4) || !readU32(object, root) || !readU32(object + 4, descriptor)
                || (root != 0 && root != 0xFFFFFFFDU) || !validGuestDataBuffer(descriptor, 8, false, 4) || !readU16(descriptor, length)
                || !readU16(descriptor + 2, maximum) || !readU32(descriptor + 4, pointer) || !length || length > 1024
                || length > maximum || !validGuestDataBuffer(pointer, length, false, 1)) { return nullptr; }
        const auto resolved = symbolicLinks_.resolve(std::string(reinterpret_cast<const char*>(addressSpace_->translate(pointer)), length));
        if (!resolved) { return nullptr; }
        const auto mount = sv->mounts.find(resolved->substr(0, resolved->find(':') + 1));
        if (mount == sv->mounts.end()) { return nullptr; }
        path = *resolved; return &mount->second;
    };
    const auto relativeOf = [](const std::string& path) {
        auto rel = path.substr(path.find(':') + 1); while (!rel.empty() && rel.front() == '\\') { rel.erase(rel.begin()); } return rel;
    };
    if (importName == "__imp__NtCreateFile" || importName == "__imp__NtOpenFile") {
        const bool open = importName == "__imp__NtOpenFile";
        std::string path;
        if (const auto* key = savePath(call.argument32(2), path)) {
            const auto handleOut = call.argument32(0), access = call.argument32(1), iosb = call.argument32(3);
            std::uint32_t options = 0, disposition = 1;
            if (open) { options = call.argument32(5); }
            else { disposition = call.argument32(7); if (!stackArgument(8, options)) { return memoryFault(importName, "File create options argument is not bounded"); } }
            if (!writable(handleOut, 4) || !writable(iosb, 8) || !disjoint(handleOut, 4, iosb, 8)) {
                return memoryFault(importName, "File open outputs are not bounded disjoint storage");
            }
            // v165: GTA's first autosave creates its save file for overlapped I/O (no FILE_SYNCHRONOUS_IO_* bit; access
            // 0x40100080, share 7, FILE_OVERWRITE_IF; v164 stop). Every transfer below completes before it returns
            // (IOSB written, event signalled, STATUS_SUCCESS), which NT permits on an asynchronous handle, so
            // asynchronous and alertable handles behave like the synchronous one. APC completion stays refused.
            if ((options & 0x30) == 0x30) { return rejected(importName, "Both synchronous I/O modes requested"); }
            const bool write = (access & (0x40000000U | 0x10000000U | 0x2U | 0x4U | 0x10000U | 0x100U | 0x10U)) != 0;
            const auto kind = (options & 1) ? saves::FileKind::Directory : (options & 0x40) ? saves::FileKind::NonDirectory : saves::FileKind::Any;
            const auto result = sv->store->openFile(*key, relativeOf(path), disposition, write, kind);
            __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_FILE_OPEN path=%s disposition=%u write=%u status=%08x size=%llu",
                                path.c_str(), disposition, unsigned(write), result.status,
                                (unsigned long long)(result.status == StatusSuccess && !result.directory ? sv->store->size(result.fd).value_or(0) : 0));
            (void)writeU32(iosb, result.status); (void)writeU32(iosb + 4, result.information);
            if (result.status != StatusSuccess) { call.returnValue = result.status; return handled(importName, "Saved-game file open failed as NT reports it"); }
            std::uint32_t token = 0;
            try { token = createKernelHandle({KernelObjectKind::File, 1, 1}); } catch (const std::bad_alloc&) { token = 0; }
            if (!token) { sv->closeFd(result.fd); call.returnValue = StatusNoMemory; return handled(importName, "File handle storage is exhausted"); }
            SaveFile file; file.fd = result.fd; file.directory = result.directory; file.write = write; file.key = *key;
            file.path = relativeOf(path); file.root = path.substr(0, path.find(':') + 1);
            sv->files.emplace(token, std::move(file));
            (void)writeU32(handleOut, token);
            call.returnValue = StatusSuccess;
            return handled(importName, "Opened a saved-game file in local storage");
        }
    }
    if (importName == "__imp__NtQueryFullAttributesFile") {
        std::string path;
        if (const auto* key = savePath(call.argument32(0), path)) {
            const auto output = call.argument32(1);
            if (!writable(output, 56, 8)) { return memoryFault(importName, "Attribute output is not bounded"); }
            const auto info = sv->store->stat(*key, relativeOf(path));
            if (!info) { call.returnValue = StatusNameNotFound; return handled(importName, "Saved-game file is absent"); }
            std::memset(addressSpace_->translate(output), 0, 56);
            write64(output, info->creationTime); write64(output + 8, info->lastAccessTime);
            write64(output + 16, info->lastWriteTime); write64(output + 24, info->changeTime);
            write64(output + 32, info->directory ? 0 : (info->size + 4095) & ~std::uint64_t{4095}); write64(output + 40, info->directory ? 0 : info->size);
            (void)writeU32(output + 48, info->directory ? 0x10 : 0x80);
            call.returnValue = StatusSuccess;
            return handled(importName, "Attributes of a saved-game file");
        }
    }
    const bool fileOp = importName == "__imp__NtClose" || importName == "__imp__NtReadFile" || importName == "__imp__NtWriteFile"
            || importName == "__imp__NtFlushBuffersFile" || importName == "__imp__NtSetInformationFile"
            || importName == "__imp__NtQueryInformationFile" || importName == "__imp__NtQueryVolumeInformationFile";
    if (const auto open = fileOp ? sv->files.find(call.argument32(0)) : sv->files.end(); open != sv->files.end()) {
        auto& file = open->second;
        if (importName == "__imp__NtClose") {
            __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_FILE_CLOSE path=%s%s read=%llu written=%llu",
                                file.root.c_str(), file.path.c_str(), (unsigned long long)file.bytesRead, (unsigned long long)file.bytesWritten);
            sv->closeFile(file); sv->files.erase(open); ::recomp::imports_fast::eraseKey(kernelHandles_, call.argument32(0), ::recomp::imports_fast::handleGenerations());
            call.returnValue = StatusSuccess; return handled(importName, "Closed a saved-game file");
        }
        if (importName == "__imp__NtReadFile" || importName == "__imp__NtWriteFile") {
            const bool isWrite = importName == "__imp__NtWriteFile";
            const auto event = call.argument32(1), apc = call.argument32(2), iosb = call.argument32(4);
            const auto buffer = call.argument32(5), length = call.argument32(6), offsetPointer = call.argument32(7);
            if (apc != 0) { return rejected(importName, "APC file completion is not modeled"); }
            if (!writable(iosb, 8) || (length && !(isWrite ? validGuestDataBuffer(buffer, length, false, 1) : writable(buffer, length, 1)))) {
                return memoryFault(importName, "File transfer buffers are not bounded");
            }
            if (file.directory) { call.returnValue = 0xC00000BAU; return handled(importName, "Transfer on a directory handle"); }
            if (isWrite && !file.write) { (void)writeU32(iosb, StatusAccessDenied); (void)writeU32(iosb + 4, 0); call.returnValue = StatusAccessDenied; return handled(importName, "Write on a read-only handle"); }
            std::uint64_t offset = file.position;
            if (offsetPointer) {
                std::uint32_t hi = 0, lo = 0;
                if (!validGuestDataBuffer(offsetPointer, 8, false, 4) || !readU32(offsetPointer, hi) || !readU32(offsetPointer + 4, lo)) {
                    return memoryFault(importName, "File offset is not bounded");
                }
                const auto value = (std::uint64_t{hi} << 32) | lo;
                if (value != 0xFFFFFFFFFFFFFFFEULL) { offset = value; }
            }
            const auto io = isWrite ? sv->store->write(file.fd, offset, addressSpace_->translate(buffer), length)
                                    : sv->store->read(file.fd, offset, addressSpace_->translate(buffer), length);
            auto moved = io.bytes;
            std::uint32_t status = io.status;
            if (status == StatusSuccess && !isWrite && moved == 0 && length != 0) { status = StatusEndOfFile; }
            file.position = offset + std::uint64_t(moved);
            if (isWrite) { sv->bytesWritten += std::uint64_t(moved); ++sv->writes; file.bytesWritten += std::uint64_t(moved); }
            else { file.bytesRead += std::uint64_t(moved); }
            (void)writeU32(iosb, status); (void)writeU32(iosb + 4, std::uint32_t(moved));
            if (event) {
                const auto found = kernelHandles_.find(event);
                if (found != kernelHandles_.end() && (found->second.kind == KernelObjectKind::NotificationEvent
                        || found->second.kind == KernelObjectKind::SynchronizationEvent)) { found->second.signalCount = 1; wakeHandleWaiters(this, event); }
            }
            call.returnValue = status;
            return handled(importName, isWrite ? "Wrote a saved-game file" : "Read a saved-game file");
        }
        if (importName == "__imp__NtFlushBuffersFile") {
            const auto iosb = call.argument32(1);
            if (!writable(iosb, 8)) { return memoryFault(importName, "Flush IO_STATUS_BLOCK is not bounded"); }
            const auto flushed = file.directory ? StatusSuccess : sv->store->flush(file.fd);
            (void)writeU32(iosb, flushed); (void)writeU32(iosb + 4, 0);
            call.returnValue = flushed; return handled(importName, "Flushed a saved-game file to local storage");
        }
        if (importName == "__imp__NtSetInformationFile" || importName == "__imp__NtQueryInformationFile"
                || importName == "__imp__NtQueryVolumeInformationFile") {
            const auto iosb = call.argument32(1), data = call.argument32(2), length = call.argument32(3), type = call.argument32(4);
            if (!writable(iosb, 8)) { return memoryFault(importName, "File information IO_STATUS_BLOCK is not bounded"); }
            const auto status = [&](std::uint32_t s, std::uint32_t n, const char* what) {
                (void)writeU32(iosb, s); (void)writeU32(iosb + 4, n); call.returnValue = s; return handled(importName, what);
            };
            const std::uint64_t size = file.directory ? 0 : sv->store->size(file.fd).value_or(0);
            if (importName == "__imp__NtSetInformationFile") {
                if (!validGuestDataBuffer(data, length, false, 4)) { return memoryFault(importName, "File information input is not bounded"); }
                std::uint32_t hi = 0, lo = 0; (void)readU32(data, hi); (void)readU32(data + 4, lo);
                const auto value = (std::uint64_t{hi} << 32) | lo;
                if (type == 14 && length >= 8) { file.position = value; return status(StatusSuccess, 0, "Set a saved-game file position"); }
                if ((type == 20 || type == 19) && length >= 8) { // end of file / allocation
                    if (!file.write) { return status(StatusAccessDenied, 0, "Size change on a read-only handle"); }
                    if (type == 19 && value >= size) { return status(StatusSuccess, 0, "Allocation reservation (no change)"); }
                    return status(sv->store->setSize(file.fd, value), 0, "Set a saved-game file size");
                }
                if (type == 13 && length >= 1) { file.deleteOnClose = addressSpace_->translate(data)[0] != 0; return status(StatusSuccess, 0, "Set delete-on-close"); }
                if (type == 4) { return status(StatusSuccess, 0, "File times accepted (local storage keeps its own)"); }
                return rejected(importName, "Saved-game file information class is not modeled");
            }
            if (!writable(data, length, 4)) { return memoryFault(importName, "File information output is not bounded"); }
            if (importName == "__imp__NtQueryInformationFile") {
                if (type == 14) { if (length < 8) { return status(StatusBufferTooSmall, 0, "Position buffer too small"); } write64(data, file.position); return status(StatusSuccess, 8, "Saved-game file position"); }
                if (type == 5) {
                    if (length < 24) { return status(StatusBufferTooSmall, 0, "Standard info buffer too small"); }
                    std::memset(addressSpace_->translate(data), 0, 24); write64(data, (size + 4095) & ~std::uint64_t{4095}); write64(data + 8, size);
                    (void)writeU32(data + 16, 1); addressSpace_->translate(data)[21] = file.directory ? 1 : 0;
                    return status(StatusSuccess, 24, "Saved-game file standard info");
                }
                if (type == 34) {
                    if (length < 56) { return status(StatusBufferTooSmall, 0, "Network-open info buffer too small"); }
                    const auto info = sv->store->stat(file.key, file.path);
                    std::memset(addressSpace_->translate(data), 0, 56);
                    if (info) { write64(data, info->creationTime); write64(data + 8, info->lastAccessTime);
                                write64(data + 16, info->lastWriteTime); write64(data + 24, info->changeTime); }
                    write64(data + 32, (size + 4095) & ~std::uint64_t{4095}); write64(data + 40, size);
                    (void)writeU32(data + 48, file.directory ? 0x10 : 0x80);
                    return status(StatusSuccess, 56, "Saved-game file network-open info");
                }
                if (type == 17) { (void)writeU32(data, 0); return status(StatusSuccess, 4, "Byte alignment"); }
                return rejected(importName, "Saved-game file query class is not modeled");
            }
            // Volume information of the save storage.
            const auto total = sv->store->totalBytes() / 4096, available = sv->store->freeBytes() / 4096;
            if (type == 3 && length >= 24) { write64(data, total); write64(data + 8, available); (void)writeU32(data + 16, 1); (void)writeU32(data + 20, 4096); return status(StatusSuccess, 24, "Save storage size"); }
            if (type == 7 && length >= 32) { write64(data, total); write64(data + 8, available); write64(data + 16, available); (void)writeU32(data + 24, 1); (void)writeU32(data + 28, 4096); return status(StatusSuccess, 32, "Save storage full size"); }
            return rejected(importName, "Saved-game volume information class is not modeled");
        }
    }
    if (importName == "__imp__NtClose") {
        if (const auto e = sv->enums.find(call.argument32(0)); e != sv->enums.end()) {
            sv->enums.erase(e); ::recomp::imports_fast::eraseKey(kernelHandles_, call.argument32(0), ::recomp::imports_fast::handleGenerations());
            call.returnValue = StatusSuccess; return handled(importName, "Closed a saved-game enumerator");
        }
    }
}
