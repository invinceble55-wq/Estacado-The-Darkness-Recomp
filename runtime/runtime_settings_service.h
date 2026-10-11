#pragma once

#include "pc_settings_ui.h"

#include <filesystem>

// In-game settings service: gives the settings overlay (in the GPU plugin)
// the game's schema, the saved values of the configuration this run started
// with and the defaults; collects the overlay's edits, applies the host-owned
// live settings (master volume, vibration strength) and saves every edit
// through the validated configuration writer (lock, validation, atomic
// replace), then reports the result back to the overlay.
struct RuntimeSettingsServiceConfig {
    std::filesystem::path configPath;   // the player's configuration (may not exist yet)
    std::filesystem::path examplePath;  // defaults; a run without configPath started from it
    bool persistence = false;           // false: safe mode or a packaged preset
    PcSettingsOffer offer;              // shelved settings this game folder offers
    // 7b: the interface language the launcher chose (launcher_prefs.toml in
    // the saves folder) from <program folder>/languages.
    std::filesystem::path launcherPrefsPath;
    std::filesystem::path languagesFolder;
};

void StartRuntimeSettingsService(const RuntimeSettingsServiceConfig& config);
void StopRuntimeSettingsService() noexcept;
