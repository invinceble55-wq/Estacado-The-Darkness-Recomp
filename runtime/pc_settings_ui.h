#pragma once

// The game's settings schema (pc_settings_schema.h: editors, ranges, choices,
// presentation) as ReXGlue's generic settings description, rendered by the
// launcher window and the in-game overlay with the same panel.

#include <rex/ui/settings_schema.h>

#include <filesystem>
#include <string>
#include <string_view>

// Shelved features (V440): their code stays, but their settings are offered
// only when these are true.
struct PcSettingsOffer {
    // Arabic: the general.language choice and the Arabic interface.
    bool arabic = false;
    // Temporal AA and its upscalers: the graphics.temporal_aa setting.
    bool temporalAa = false;
    // NVIDIA DLSS Frame Generation (#3, experimental test builds):
    // streamline.frame_generation.
    bool frameGeneration = false;

    bool operator==(const PcSettingsOffer&) const = default;
};

// The developer switch for shelved features: DARKNESS_EXPERIMENTAL names them
// (temporal_aa, arabic), separated by commas or spaces.
bool PcExperimentalFeature(std::string_view name);

// What a game folder offers. Arabic: a pack installed in
// language_packs\arabic, a pack archive the package carries
// (language_packs\arabic_language_pack.zip), a pack this build can download
// (downloadable) or the switch. Temporal AA: the switch.
PcSettingsOffer PcSettingsOfferFor(const std::filesystem::path& gameFolder,
                                   bool downloadable = false);
// The same with the installed copy's two folders (runtime_user_data.h):
// installed packs live in the local data folder, the package's carried
// archive beside the executables (a portable copy passes the same folder
// twice).
PcSettingsOffer PcSettingsOfferFor(const std::filesystem::path& dataFolder,
                                   const std::filesystem::path& programFolder,
                                   bool downloadable = false);

// arabic: the Arabic interface (pc_settings_arabic.h), right-to-left; only
// with offer.arabic.
rex::ui::settings::Schema BuildPcSettingsUiSchema(bool arabic = false,
                                                  const PcSettingsOffer& offer = {});

// UTF-16 -> UTF-8 without platform APIs.
std::string PcSettingsUtf8(std::wstring_view text);
