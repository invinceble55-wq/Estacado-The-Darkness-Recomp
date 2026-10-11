#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class GuestAchievementUnlockResult {
    Unlocked,
    AlreadyUnlocked,
    InvalidUser,
    UnknownAchievement,
    StorageFailure,
};

// Minimal retained XAM state for message contracts that have been reached by
// The Darkness. Unknown applications and messages remain hard failures.
class GuestXamState {
public:
    // Xbox XLanguage values dynamically reached by The Darkness. Only
    // languages backed by the user's extracted multi-language title content
    // are accepted; the verified compatibility default is English (1).
    bool ConfigureLanguage(uint32_t language);
    uint32_t Language() const;
    // Standard Xbox profile preferences consumed by The Darkness through
    // XamUserReadProfileSettings. Sensitivity values are the documented
    // medium=0, low=1, high=2 enum; inversion is the standard 0/1 setting.
    bool ConfigureControllerProfile(uint32_t sensitivity, bool invertY);
    void SetUserContext(uint32_t userIndex, uint32_t contextId, uint32_t value);
    bool GetUserContext(uint32_t userIndex, uint32_t contextId, uint32_t* value) const;
    void SetXmpVolumeBits(uint32_t value);
    uint32_t XmpVolumeBits() const;
    void SetXmpPlaybackClient(uint32_t value);
    uint32_t XmpPlaybackClient() const;
    bool GetProfileInt32Setting(uint32_t settingId, int32_t* value) const;
    bool ConfigureAchievementCatalog(uint32_t titleId,
                                     const std::vector<uint32_t>& achievementIds);
    GuestAchievementUnlockResult UnlockAchievement(uint32_t userIndex,
                                                    uint32_t achievementId,
                                                    uint64_t* unlockFileTime = nullptr);
    bool IsAchievementUnlocked(uint32_t achievementId,
                               uint64_t* unlockFileTime = nullptr) const;
    void ResetForTests();

private:
    static uint64_t ContextKey(uint32_t userIndex, uint32_t contextId);
    bool LoadAchievementStoreLocked();
    bool SaveAchievementStoreLocked() const;
    // #22: <content>/achievements/<TITLEID>.toml (ReXGlue's format) is the
    // store; the legacy Profile/achievements.txt is merged in and kept
    // written for older versions.
    void LoadAchievementTomlLocked();
    bool SaveAchievementTomlLocked() const;

    mutable std::mutex mutex_;
    uint32_t language_ = 1;
    std::unordered_map<uint64_t, uint32_t> userContexts_;
    std::unordered_map<uint32_t, int32_t> profileInt32Settings_ = {
        {0x10040002u, 0}, // Y-axis inversion
        {0x10040003u, 3}, // controller vibration
        {0x10040015u, 0}, // difficulty
        {0x10040018u, 0}, // control sensitivity
        {0x10040022u, 1}, // auto aim
        {0x10040024u, 0}, // movement control
        {0x1004000Cu, 0}, // voice muted (read by the voice engine)
        {0x1004000Du, 0}, // voice through speakers
        {0x1004000Eu, 0x64}, // voice volume
    };
    uint32_t xmpVolumeBits_ = 0x3F800000u;
    uint32_t xmpPlaybackClient_ = 1;
    uint32_t achievementTitleId_{};
    std::unordered_set<uint32_t> knownAchievementIds_;
    std::unordered_map<uint32_t, uint64_t> unlockedAchievements_;
};

GuestXamState& GetGuestXamState();
