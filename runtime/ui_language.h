#pragma once

// Interface languages from files (7b, Russian first): every
// <program folder>/languages/<code>.toml is one language for the launcher
// and the F1 overlay, so a language is added by adding a file:
//
//   code = "ru"                 # file name without .toml
//   name = "Русский (Russian)"  # shown in the language choice
//   windows = ["ru"]            # Windows display languages that pick it
//                               # automatically (ISO 639-1 prefixes)
//   [strings]
//   "English text" = "Translated text"
//
// Untranslated texts stay English. Arabic keeps its built-in table and
// right-to-left layout (pc_settings_arabic.h).

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ui/settings_schema.h>

struct UiLanguage {
    std::string code;
    std::string name;
    std::vector<std::string> windows;
    std::map<std::string, std::string, std::less<>> text;

    // The translation, or english itself.
    std::string Tr(std::string_view english) const;
};

// The languages in folder (sorted by code); files that fail to parse are
// skipped and named in errors.
std::vector<UiLanguage> LoadUiLanguages(const std::filesystem::path& folder,
                                        std::vector<std::string>* errors = nullptr);
bool ParseUiLanguage(std::string_view toml, std::string_view code, UiLanguage& out,
                     std::string& error);

// The language for a choice: "english" (or empty) = none, "auto" = the one
// whose `windows` list matches windowsLanguage (e.g. "ru-RU"), else the code.
const UiLanguage* PickUiLanguage(const std::vector<UiLanguage>& languages,
                                 std::string_view choice, std::string_view windowsLanguage);

// The Windows display language as a locale name ("ru-RU"), empty if unknown.
std::string WindowsDisplayLanguage();

// Every text of a settings schema (sections, labels, descriptions, units,
// choices) and the panel's own words (schema.text) in the language.
void TranslateSettingsSchema(rex::ui::settings::Schema& schema, const UiLanguage& language);
