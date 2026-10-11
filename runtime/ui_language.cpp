#include "ui_language.h"

#include <windows.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace {
// UTF-8 path text under C++17 and C++20 (u8string is std::u8string there).
std::string PathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
}  // namespace

std::string UiLanguage::Tr(std::string_view english) const {
    const auto it = text.find(english);
    if (it == text.end() || it->second.empty()) return std::string(english);
    return it->second;
}

bool ParseUiLanguage(std::string_view toml, std::string_view code, UiLanguage& out,
                     std::string& error) {
    try {
        const toml::table table = toml::parse(toml);
        UiLanguage language;
        language.code = std::string(code);
        language.name = table["name"].value_or(std::string(code));
        if (const auto* windows = table["windows"].as_array()) {
            for (const auto& entry : *windows) {
                if (const auto value = entry.value<std::string>()) language.windows.push_back(*value);
            }
        }
        const auto* strings = table["strings"].as_table();
        if (!strings) {
            error = "no [strings] table";
            return false;
        }
        for (const auto& [key, value] : *strings) {
            if (const auto translated = value.value<std::string>()) {
                language.text.emplace(std::string(key.str()), *translated);
            }
        }
        out = std::move(language);
        return true;
    } catch (const toml::parse_error& parse) {
        error = std::string(parse.description()) + " (line " +
                std::to_string(parse.source().begin.line) + ")";
        return false;
    }
}

std::vector<UiLanguage> LoadUiLanguages(const std::filesystem::path& folder,
                                        std::vector<std::string>* errors) {
    std::vector<UiLanguage> out;
    std::error_code fsError;
    if (!std::filesystem::is_directory(folder, fsError)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(folder, fsError)) {
        if (!entry.is_regular_file(fsError) || entry.path().extension() != L".toml") continue;
        const std::string code = PathUtf8(entry.path().stem());
        if (code.empty() || code == "english" || code == "auto" || code == "en") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        UiLanguage language;
        std::string error;
        if (ParseUiLanguage(text, code, language, error)) {
            out.push_back(std::move(language));
        } else if (errors) {
            errors->push_back(PathUtf8(entry.path().filename()) + ": " + error);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const UiLanguage& a, const UiLanguage& b) { return a.code < b.code; });
    return out;
}

const UiLanguage* PickUiLanguage(const std::vector<UiLanguage>& languages,
                                 std::string_view choice, std::string_view windowsLanguage) {
    if (choice.empty() || choice == "english") return nullptr;
    if (choice != "auto") {
        for (const UiLanguage& language : languages) {
            if (language.code == choice) return &language;
        }
        return nullptr;
    }
    const auto lower = [](std::string_view text) {
        std::string out(text);
        for (char& c : out) c = char(std::tolower(uint8_t(c)));
        return out;
    };
    const std::string windows = lower(windowsLanguage);
    for (const UiLanguage& language : languages) {
        for (const std::string& prefix : language.windows) {
            const std::string want = lower(prefix);
            if (want.empty() || windows.compare(0, want.size(), want) != 0) continue;
            // "ru" matches "ru" and "ru-RU", not "rup".
            if (windows.size() == want.size() || windows[want.size()] == '-') return &language;
        }
    }
    return nullptr;
}

std::string WindowsDisplayLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name,
                          LOCALE_NAME_MAX_LENGTH, 0)) {
        return {};
    }
    std::string out;
    for (const wchar_t* p = name; *p; ++p) out.push_back(*p < 128 ? char(*p) : '?');
    return out;
}

void TranslateSettingsSchema(rex::ui::settings::Schema& schema, const UiLanguage& language) {
    const auto tr = [&](std::string& text) {
        if (!text.empty()) text = language.Tr(text);
    };
    for (std::string& section : schema.sections) tr(section);
    for (rex::ui::settings::Setting& setting : schema.settings) {
        tr(setting.section);
        tr(setting.label);
        tr(setting.description);
        tr(setting.units);
        for (rex::ui::settings::Choice& choice : setting.choices) tr(choice.label);
    }
    for (const auto& [english, translated] : language.text) {
        if (!translated.empty()) schema.text[english] = translated;
    }
}
