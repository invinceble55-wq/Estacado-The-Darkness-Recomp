#include "runtime_settings_service.h"

#include "pc_settings_arabic.h"
#include "pc_settings_ui.h"
#include "runtime_audio.h"
#include "runtime_cheats.h"
#include "runtime_graphics.h"
#include "runtime_input.h"
#include "runtime_modules.h"
#include "runtime_msaa_mode.h"
#include "runtime_pc_settings.h"
#include "runtime_single_instance.h"
#include "ui_language.h"

#include <rex/ui/settings_schema.h>
#include <toml++/toml.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ui = rex::ui::settings;
using Clock = std::chrono::steady_clock;

// Edits are saved once the overlay has been quiet this long (slider drags
// produce a stream of values; each save is a validated atomic file replace).
constexpr auto kSaveQuiet = std::chrono::milliseconds(600);
constexpr auto kPollInterval = std::chrono::milliseconds(100);
constexpr int kConfigureAttempts = 600;  // ~60 s for graphics to come up

std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_wake;
bool g_stop = false;

ui::Values Normalize(const ui::Schema& schema, const std::map<std::string, std::string>& raw) {
    ui::Values values;
    for (const auto& [key, value] : raw) {
        const ui::Setting* setting = schema.Find(key);
        if (!setting) continue;
        if (setting->editor == ui::Editor::kNumber) {
            char* end = nullptr;
            const double number = std::strtod(value.c_str(), &end);
            if (end && *end == '\0') {
                values[key] = ui::FormatNumberValue(*setting, number);
                continue;
            }
        }
        values[key] = value;
    }
    return values;
}

ui::Values ReadValues(const ui::Schema& schema, const std::filesystem::path& path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error)) return {};
    try {
        return Normalize(schema, RuntimePcConfigSettingValues(path));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "RUNTIME_SETTINGS_SERVICE read_failed path=%s error=%s\n",
                     path.u8string().c_str(), exception.what());
        return {};
    }
}

// Host-owned settings that apply while the game runs (atomic setters).
void ApplyHostLive(const std::string& key, const std::string& value) {
    if (key == "graphics.msaa_mode") {
        msaa_mode::Policy policy;
        if (msaa_mode::ParsePolicy(value, policy)) ConfigureRuntimeMsaaMode(policy, "overlay");
        return;
    }
    if (key == "cheats.god_mode" && (value == "true" || value == "false")) {
        RuntimeCheatSettings cheats = RuntimeCheatsCurrent();
        cheats.godMode = value == "true";
        ConfigureRuntimeCheats(cheats);
        return;
    }
    if (key == "cheats.give_all") {
        if (value == "true") RuntimeCheatsGiveAll();
        return;
    }
    if (key == "general.achievement_notifications") {
        if (value == "true" || value == "false") ConfigureRuntimeAchievementNotifications(value == "true");
        return;
    }
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    if (!end || *end != '\0') return;
    try {
        if (key == "audio.master_volume") {
            ConfigureRuntimeAudioMasterVolume(number);
        } else if (key == "input.vibration_scale") {
            ConfigureRuntimeInputVibrationScale(number);
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "RUNTIME_SETTINGS_SERVICE live_rejected key=%s error=%s\n",
                     key.c_str(), exception.what());
    }
}

bool Wait(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(g_mutex);
    return !g_wake.wait_for(lock, duration, [] { return g_stop; });
}

void Run(RuntimeSettingsServiceConfig config) {
    ui::Schema schema = BuildPcSettingsUiSchema(false, config.offer);
    const ui::Values defaults = ReadValues(schema, config.examplePath);
    // Without a saved configuration the run started from the defaults; the
    // first save creates the configuration from them.
    const auto configExists = [&config] {
        std::error_code error;
        return std::filesystem::is_regular_file(config.configPath, error) && !error;
    };
    ui::Values saved = configExists() ? ReadValues(schema, config.configPath) : defaults;
    // The overlay speaks the language the run started with (a change applies
    // at the next start, like the game's language).
    if (const auto language = saved.find("general.language");
        config.offer.arabic && language != saved.end() &&
        PcSettingsInterfaceArabic(language->second)) {
        schema = BuildPcSettingsUiSchema(true, config.offer);
    } else {
        // 7b: the launcher's interface language (read once; a change applies
        // at the next start).
        std::string choice = "auto";
        try {
            const toml::table prefs = toml::parse_file(config.launcherPrefsPath.u8string());
            choice = prefs["interface_language"].value_or(std::string("auto"));
        } catch (...) {
        }
        static const std::vector<UiLanguage> languages = LoadUiLanguages(config.languagesFolder);
        // DARKNESS_TEST_WINDOWS_LANGUAGE: layout checks on another Windows language.
        const char* forcedWindows = std::getenv("DARKNESS_TEST_WINDOWS_LANGUAGE");
        if (const UiLanguage* language = PickUiLanguage(
                languages, choice, forcedWindows ? std::string(forcedWindows) : WindowsDisplayLanguage())) {
            TranslateSettingsSchema(schema, *language);
            std::fprintf(stderr, "RUNTIME_SETTINGS_SERVICE interface_language=%s\n",
                         language->code.c_str());
        }
    }
    const std::string schemaText = ui::SerializeSchema(schema);

    bool configured = false;
    for (int attempt = 0; !configured && attempt < kConfigureAttempts; ++attempt) {
        if (!Wait(kPollInterval)) return;
        if (!RuntimeGraphicsIsActive()) continue;
        configured = RuntimeGraphicsSettingsConfigure(schemaText, ui::SerializeValues(saved),
                                                      ui::SerializeValues(defaults),
                                                      config.persistence);
    }
    std::fprintf(stderr,
                 "RUNTIME_SETTINGS_SERVICE configured=%u persistence=%u settings=%zu config=%s\n",
                 configured ? 1u : 0u, config.persistence ? 1u : 0u, schema.settings.size(),
                 config.configPath.u8string().c_str());
    std::fflush(stderr);
    if (!configured) return;
    if (std::getenv("DARKNESS_TEST_ACHIEVEMENT_TOAST")) {
        // Developer check of the #22 toast: the catalog's first entry, shown
        // only (nothing unlocked).
        if (!ExecutableAchievements().empty()) {
            const auto& first = ExecutableAchievements().front();
            RuntimeGraphicsShowAchievement(first.id, first.label.c_str(), first.gamerscore);
        }
    }

    ui::Values pending;
    Clock::time_point lastChange{};
    while (Wait(kPollInterval)) {
        std::string text;
        if (RuntimeGraphicsSettingsPoll(text) && !text.empty()) {
            if (auto changes = ui::ParseValues(text)) {
                for (const auto& [key, value] : *changes) {
                    ApplyHostLive(key, value);
                    pending[key] = value;
                }
                lastChange = Clock::now();
            }
        }
        if (pending.empty()) continue;
        if (!config.persistence) {
            pending.clear();  // live settings are applied; nothing is saved
            continue;
        }
        if (Clock::now() - lastChange < kSaveQuiet) continue;

        RuntimeSingleInstance lock(RuntimeConfigLockName(config.configPath));
        if (!lock.acquired()) continue;  // the launcher is saving; retry
        std::vector<RuntimeLaunchOptions::PcConfigOverride> overrides;
        std::string keys;
        for (const auto& [key, value] : pending) {
            overrides.push_back({key, value});
            keys += (keys.empty() ? "" : ",") + key;
        }
        try {
            InstallRuntimePcPresetWithOverrides(
                configExists() ? config.configPath : config.examplePath, config.configPath,
                overrides, true);
            lock.Release();
            saved = ReadValues(schema, config.configPath);
            RuntimeGraphicsSettingsSaved(ui::SerializeValues(saved),
                                         "Saved. Settings marked \"next start\" apply the next "
                                         "time the game starts.");
            std::fprintf(stderr, "RUNTIME_SETTINGS_SAVED keys=%s\n", keys.c_str());
        } catch (const std::exception& exception) {
            lock.Release();
            RuntimeGraphicsSettingsSaved(ui::SerializeValues(saved),
                                         std::string("Not saved: ") + exception.what());
            std::fprintf(stderr, "RUNTIME_SETTINGS_SAVE_FAILED keys=%s error=%s\n", keys.c_str(),
                         exception.what());
        }
        std::fflush(stderr);
        pending.clear();
    }
}

}  // namespace

void StartRuntimeSettingsService(const RuntimeSettingsServiceConfig& config) {
    StopRuntimeSettingsService();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = false;
    }
    g_thread = std::thread(Run, config);
}

void StopRuntimeSettingsService() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
}
