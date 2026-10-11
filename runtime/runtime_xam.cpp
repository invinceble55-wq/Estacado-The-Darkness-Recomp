#include "runtime_xam.h"

#include "runtime_achievement_store.h"
#include "runtime_filesystem.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>

namespace {
constexpr char kAchievementStoreSignature[] = "TDRAchievementStore";
constexpr uint32_t kAchievementStoreVersion = 1;

uint64_t CurrentFileTime() {
    using namespace std::chrono;
    const auto sinceUnixEpoch = system_clock::now().time_since_epoch();
    const auto intervals = duration_cast<duration<int64_t, std::ratio<1, 10'000'000>>>(
        sinceUnixEpoch);
    constexpr uint64_t kWindowsToUnixEpochIntervals = 116'444'736'000'000'000ull;
    return static_cast<uint64_t>(intervals.count()) + kWindowsToUnixEpochIntervals;
}

std::filesystem::path AchievementStorePath(uint32_t titleId) {
    if (!titleId) return {};
    std::wostringstream title;
    title << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << titleId;
    return GuestPortableContentDeviceRoot() / title.str() / L"Profile" /
           L"achievements.txt";
}

std::filesystem::path AchievementTomlPath(uint32_t titleId) {
    if (!titleId) return {};
    std::wostringstream title;
    title << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << titleId;
    return GuestPortableContentDeviceRoot() / L"achievements" / (title.str() + L".toml");
}

// Atomic replace through a temporary file beside the target.
bool WriteFileAtomically(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto parentStatus = std::filesystem::symlink_status(path.parent_path(), error);
    if (error || !std::filesystem::is_directory(parentStatus) ||
        std::filesystem::is_symlink(parentStatus))
        return false;
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << text;
        output.flush();
        if (!output) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
}

bool GuestXamState::ConfigureLanguage(uint32_t language) {
    // XLanguage: English=1, German=3, French=4, Spanish=5, Italian=6.
    // The extracted PAL/NTSC-U title does not contain the other Xbox language
    // packages, so accepting them would select content that cannot exist.
    if (language != 1 && language != 3 && language != 4 && language != 5 &&
        language != 6) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    language_ = language;
    return true;
}

uint32_t GuestXamState::Language() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return language_;
}

bool GuestXamState::ConfigureControllerProfile(uint32_t sensitivity,
                                               bool invertY) {
    if (sensitivity > 2) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    profileInt32Settings_[0x10040018u] = static_cast<int32_t>(sensitivity);
    profileInt32Settings_[0x10040002u] = invertY ? 1 : 0;
    return true;
}

uint64_t GuestXamState::ContextKey(uint32_t userIndex, uint32_t contextId) {
    return (uint64_t(userIndex) << 32) | contextId;
}

void GuestXamState::SetUserContext(uint32_t userIndex, uint32_t contextId, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    userContexts_[ContextKey(userIndex, contextId)] = value;
}

bool GuestXamState::GetUserContext(uint32_t userIndex, uint32_t contextId,
                                   uint32_t* value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = userContexts_.find(ContextKey(userIndex, contextId));
    if (entry == userContexts_.end()) return false;
    if (value) *value = entry->second;
    return true;
}

void GuestXamState::SetXmpVolumeBits(uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    xmpVolumeBits_ = value;
}

uint32_t GuestXamState::XmpVolumeBits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return xmpVolumeBits_;
}

void GuestXamState::SetXmpPlaybackClient(uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    xmpPlaybackClient_ = value;
}

uint32_t GuestXamState::XmpPlaybackClient() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return xmpPlaybackClient_;
}

bool GuestXamState::GetProfileInt32Setting(uint32_t settingId, int32_t* value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = profileInt32Settings_.find(settingId);
    if (entry == profileInt32Settings_.end()) return false;
    if (value) *value = entry->second;
    return true;
}

bool GuestXamState::ConfigureAchievementCatalog(
    uint32_t titleId, const std::vector<uint32_t>& achievementIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    achievementTitleId_ = titleId;
    knownAchievementIds_.clear();
    unlockedAchievements_.clear();
    if (!titleId || achievementIds.empty()) {
        std::cout << "ACHIEVEMENT_CATALOG_REJECT title=0x" << std::hex << titleId
                  << std::dec << " entries=" << achievementIds.size()
                  << " reason=missing_catalog\n";
        return false;
    }
    for (uint32_t id : achievementIds) {
        // Achievement IDs are title-defined keys. The Darkness's authoritative
        // XDBF catalog starts at ID 0, so zero is not a sentinel here.
        if (!knownAchievementIds_.insert(id).second) {
            std::cout << "ACHIEVEMENT_CATALOG_REJECT title=0x" << std::hex
                      << titleId << " id=0x" << id << std::dec
                      << " reason=invalid_or_duplicate_id\n";
            return false;
        }
    }
    if (!LoadAchievementStoreLocked()) return false;
    LoadAchievementTomlLocked();
    return true;
}

void GuestXamState::LoadAchievementTomlLocked() {
    namespace store = darkness::achievement_store;
    const std::filesystem::path path = AchievementTomlPath(achievementTitleId_);
    if (path.empty()) return;
    const size_t legacy = unlockedAchievements_.size();
    store::Unlocks unlocks;
    bool present = false;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (!error && std::filesystem::exists(status)) {
        present = true;
        std::string text;
        if (std::filesystem::is_regular_file(status)) {
            std::ifstream input(path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        if (!std::filesystem::is_regular_file(status) || !store::ParseToml(text, unlocks)) {
            // Kept aside, never overwritten: the next save starts a new file
            // from the unlocks that could be read.
            const std::filesystem::path aside = path.wstring() + L".unreadable";
            std::filesystem::rename(path, aside, error);
            std::wcout << L"ACHIEVEMENT_STORE_TOML unreadable=1 moved=" << aside.wstring()
                       << L" ok=" << (error ? 0 : 1) << L'\n';
            present = false;
        }
    }
    const uint64_t now = CurrentFileTime();
    size_t added = 0;
    for (const auto& [id, filetime] : unlocks) {
        if (!knownAchievementIds_.count(id)) continue;  // not this title's
        if (unlockedAchievements_.emplace(id, filetime ? filetime : now).second) ++added;
    }
    // Migration: unlocks only the legacy file had go into the TOML store.
    bool migrate = !present && legacy;
    for (const auto& [id, filetime] : unlockedAchievements_) {
        if (!unlocks.count(id)) migrate = true;
    }
    if (migrate && !SaveAchievementTomlLocked()) {
        std::cout << "ACHIEVEMENT_STORE_TOML migrate_failed=1\n";
    }
    if (added && !SaveAchievementStoreLocked()) {
        std::cout << "ACHIEVEMENT_STORE_LEGACY_SAVE failed=1\n";
    }
    std::wcout << L"ACHIEVEMENT_STORE_TOML path=" << path.wstring()
               << L" present=" << (present ? 1 : 0) << L" legacy=" << legacy
               << L" toml_only=" << added << L" total=" << unlockedAchievements_.size()
               << L" migrated=" << (migrate ? 1 : 0) << L'\n';
}

bool GuestXamState::SaveAchievementTomlLocked() const {
    const std::filesystem::path path = AchievementTomlPath(achievementTitleId_);
    if (path.empty()) return false;
    darkness::achievement_store::Unlocks unlocks(unlockedAchievements_.begin(),
                                                 unlockedAchievements_.end());
    return WriteFileAtomically(path, darkness::achievement_store::FormatToml(unlocks));
}

GuestAchievementUnlockResult GuestXamState::UnlockAchievement(
    uint32_t userIndex, uint32_t achievementId, uint64_t* unlockFileTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (userIndex != 0) return GuestAchievementUnlockResult::InvalidUser;
    if (!achievementTitleId_ || !knownAchievementIds_.count(achievementId))
        return GuestAchievementUnlockResult::UnknownAchievement;
    const auto existing = unlockedAchievements_.find(achievementId);
    if (existing != unlockedAchievements_.end()) {
        if (unlockFileTime) *unlockFileTime = existing->second;
        return GuestAchievementUnlockResult::AlreadyUnlocked;
    }
    const uint64_t fileTime = CurrentFileTime();
    unlockedAchievements_.emplace(achievementId, fileTime);
    if (!SaveAchievementTomlLocked()) {
        unlockedAchievements_.erase(achievementId);
        return GuestAchievementUnlockResult::StorageFailure;
    }
    // Older versions read only the legacy file (best effort).
    if (!SaveAchievementStoreLocked())
        std::cout << "ACHIEVEMENT_STORE_LEGACY_SAVE failed=1\n";
    if (unlockFileTime) *unlockFileTime = fileTime;
    return GuestAchievementUnlockResult::Unlocked;
}

bool GuestXamState::IsAchievementUnlocked(uint32_t achievementId,
                                          uint64_t* unlockFileTime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = unlockedAchievements_.find(achievementId);
    if (entry == unlockedAchievements_.end()) return false;
    if (unlockFileTime) *unlockFileTime = entry->second;
    return true;
}

bool GuestXamState::LoadAchievementStoreLocked() {
    const std::filesystem::path path = AchievementStorePath(achievementTitleId_);
    if (path.empty()) return false;
    // A packaged title can already have portable save content while its
    // Profile subdirectory has never been created. Windows reports that case
    // as ERROR_PATH_NOT_FOUND, not generic no_such_file_or_directory. Both
    // missing-file forms are a valid empty achievement store. Existing
    // reparse points and non-files remain invalid so the store cannot escape
    // the portable content root.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        std::wcout << L"ACHIEVEMENT_STORE_LOAD path=" << path.wstring()
                   << L" state=missing win_error=" << error << L'\n';
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    std::wcout << L"ACHIEVEMENT_STORE_LOAD path=" << path.wstring()
               << L" state=existing attributes=0x" << std::hex << attributes
               << std::dec << L'\n';
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;

    std::ifstream input(path);
    std::string signature;
    uint32_t version{};
    uint32_t titleId{};
    if (!(input >> signature >> version >> std::hex >> titleId) ||
        signature != kAchievementStoreSignature ||
        version != kAchievementStoreVersion || titleId != achievementTitleId_)
        return false;

    uint32_t achievementId{};
    uint64_t fileTime{};
    while (input >> std::hex >> achievementId >> fileTime) {
        if (!fileTime || !knownAchievementIds_.count(achievementId) ||
            !unlockedAchievements_.emplace(achievementId, fileTime).second)
            return false;
    }
    return input.eof();
}

bool GuestXamState::SaveAchievementStoreLocked() const {
    const std::filesystem::path path = AchievementStorePath(achievementTitleId_);
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto parentStatus = std::filesystem::symlink_status(path.parent_path(), error);
    if (error || !std::filesystem::is_directory(parentStatus) ||
        std::filesystem::is_symlink(parentStatus))
        return false;

    std::vector<std::pair<uint32_t, uint64_t>> ordered(unlockedAchievements_.begin(),
                                                       unlockedAchievements_.end());
    std::sort(ordered.begin(), ordered.end());
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << kAchievementStoreSignature << ' ' << kAchievementStoreVersion << ' '
               << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
               << achievementTitleId_ << '\n';
        for (const auto& [id, fileTime] : ordered) {
            output << std::setw(8) << id << ' ' << std::setw(16) << fileTime << '\n';
        }
        output.flush();
        if (!output) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

void GuestXamState::ResetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    language_ = 1;
    userContexts_.clear();
    profileInt32Settings_ = {
        {0x10040002u, 0}, {0x10040003u, 3}, {0x10040015u, 0},
        {0x10040018u, 0}, {0x10040022u, 1}, {0x10040024u, 0},
        {0x1004000Cu, 0}, {0x1004000Du, 0}, {0x1004000Eu, 0x64},
    };
    xmpVolumeBits_ = 0x3F800000u;
    xmpPlaybackClient_ = 1;
    achievementTitleId_ = 0;
    knownAchievementIds_.clear();
    unlockedAchievements_.clear();
}

GuestXamState& GetGuestXamState() {
    static GuestXamState state;
    return state;
}
