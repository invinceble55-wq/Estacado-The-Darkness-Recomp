// Interface languages from files (runtime/ui_language.cpp, 7b): parsing,
// the automatic choice from the Windows display language, schema
// translation, and the shipped Russian file covering the schema.
#include "pc_settings_ui.h"
#include "ui_language.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

}  // namespace

int main() {
    UiLanguage ru;
    std::string error;
    Check(ParseUiLanguage("name = \"Russian\"\nwindows = [\"ru\"]\n[strings]\n\"Save\" = \"x\"\n"
                          "\"Play\" = \"\"\n",
                          "ru", ru, error),
          "a language file parses");
    Check(ru.code == "ru" && ru.name == "Russian" && ru.windows.size() == 1, "code, name, windows");
    Check(ru.Tr("Save") == "x" && ru.Tr("Play") == "Play" && ru.Tr("Other") == "Other",
          "translated, empty and missing texts");
    UiLanguage bad;
    Check(!ParseUiLanguage("name = \"x\"\n", "xx", bad, error), "no [strings] table is refused");
    Check(!ParseUiLanguage("[strings\n", "xx", bad, error), "broken TOML is refused");

    const std::vector<UiLanguage> languages{ru};
    Check(PickUiLanguage(languages, "auto", "ru-RU") == &languages[0], "auto on ru-RU");
    Check(PickUiLanguage(languages, "auto", "RU") == &languages[0], "auto, case and bare code");
    Check(!PickUiLanguage(languages, "auto", "en-US"), "auto on English Windows");
    Check(!PickUiLanguage(languages, "auto", "rup-RO"), "prefix must end at a dash");
    Check(!PickUiLanguage(languages, "english", "ru-RU"), "English chosen by hand");
    Check(PickUiLanguage(languages, "ru", "en-US") == &languages[0], "Russian chosen by hand");
    Check(!PickUiLanguage(languages, "de", "ru-RU"), "a missing file means English");

    // The shipped file: loads from the folder and translates every schema
    // text that is not a technical name (scripts/extract-ui-strings.py
    // checks the launcher's and the panel's own texts the same way).
    const std::filesystem::path folder =
        std::filesystem::path(DARKNESS_SOURCE_ROOT) / "config" / "ui_languages";
    std::vector<std::string> errors;
    const std::vector<UiLanguage> shipped = LoadUiLanguages(folder, &errors);
    Check(errors.empty() && shipped.size() == 1 && shipped[0].code == "ru",
          "the shipped languages load");
    if (!shipped.empty()) {
        rex::ui::settings::Schema schema = BuildPcSettingsUiSchema(false, {});
        const rex::ui::settings::Schema english = schema;
        TranslateSettingsSchema(schema, shipped[0]);
        size_t same = 0;
        for (size_t i = 0; i < schema.settings.size(); ++i) {
            if (!english.settings[i].label.empty() &&
                schema.settings[i].label == english.settings[i].label &&
                english.settings[i].label != "VSync") {
                ++same;
                std::fprintf(stderr, "untranslated label: %s\n", english.settings[i].label.c_str());
            }
            if (!english.settings[i].description.empty() &&
                schema.settings[i].description == english.settings[i].description) {
                ++same;
                std::fprintf(stderr, "untranslated description: %s\n",
                             english.settings[i].key.c_str());
            }
        }
        Check(same == 0, "every schema label and description has Russian");
        Check(schema.sections.size() == english.sections.size() && schema.sections[0] != english.sections[0],
              "sections translated");
        for (const auto& setting : schema.settings) {
            bool known = false;
            for (const auto& section : schema.sections) known |= section == setting.section;
            Check(known, "every setting's section is one of the translated sections");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ui_language: all checks passed\n");
    return 0;
}
