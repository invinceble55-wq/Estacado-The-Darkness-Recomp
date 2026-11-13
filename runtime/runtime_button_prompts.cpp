#include "runtime_button_prompts.h"

#include <array>
#include <cstdio>
#include <utility>

#include "runtime_graphics.h"

namespace {

RuntimePromptDeviceTracker g_device;

// The title's controller prompt templates (string keys) and the keyboard
// wording, after its own PC templates (CONTROLLER_PC_BUTTON_*: "Press §p0").
// A2/B2/C2 are the hold forms. Thumbstick movement and the D-pad direction
// templates keep the title's text (their parameters are not single keys).
constexpr std::pair<std::string_view, std::u16string_view> kTemplates[] = {
    {"CONTROLLER_XBOX_BUTTON_1_A", u"Press \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_1_A2", u"Hold \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_1_B", u"press \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_1_B2", u"hold \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_1_C", u"pressing \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_1_C2", u"holding \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_2_A", u"Press \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_2_A2", u"Hold \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_2_B", u"press \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_2_B2", u"hold \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_2_C", u"pressing \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_2_C2", u"holding \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_3_A", u"Press \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_3_A2", u"Hold \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_3_B", u"press \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_3_B2", u"hold \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_3_C", u"pressing \u00A7p0"},
    {"CONTROLLER_XBOX_BUTTON_3_C2", u"holding \u00A7p0"},
    {"CONTROLLER_XBOX_TRIGGER_A", u"Press \u00A7p0"},
    {"CONTROLLER_XBOX_TRIGGER_B", u"press \u00A7p0"},
    {"CONTROLLER_XBOX_TRIGGER_C", u"pressing \u00A7p0"},
    {"CONTROLLER_XBOX_THUMBSTICK_CLICK_A", u"Press \u00A7p0"},
    {"CONTROLLER_XBOX_THUMBSTICK_CLICK_B", u"press \u00A7p0"},
    {"CONTROLLER_XBOX_THUMBSTICK_CLICK_C", u"pressing \u00A7p0"},
    {"CONTROLLER_XBOX_DPAD_A", u"Press \u00A7p0"},
    {"CONTROLLER_XBOX_DPAD_B", u"press \u00A7p0"},
    {"CONTROLLER_XBOX_DPAD_C", u"pressing \u00A7p0"},
};

// The title's button names (CONTROLLER_XBOX_<code - 160>, the prompt
// parameter) and the bindings whose keys replace them. 06/07 are the stick
// clicks (the stick axes never use these names).
constexpr std::pair<std::string_view, std::string_view> kButtonNames[] = {
    {"CONTROLLER_XBOX_00", "a"},
    {"CONTROLLER_XBOX_01", "b"},
    {"CONTROLLER_XBOX_02", "x"},
    {"CONTROLLER_XBOX_03", "y"},
    {"CONTROLLER_XBOX_04", "start"},
    {"CONTROLLER_XBOX_05", "back"},
    {"CONTROLLER_XBOX_06", "lstick_press"},
    {"CONTROLLER_XBOX_07", "rstick_press"},
    {"CONTROLLER_XBOX_08", "left_shoulder"},
    {"CONTROLLER_XBOX_09", "right_shoulder"},
    {"CONTROLLER_XBOX_0A", "left_trigger"},
    {"CONTROLLER_XBOX_0B", "right_trigger"},
    {"CONTROLLER_XBOX_10", "dpad_up"},
    {"CONTROLLER_XBOX_11", "dpad_right"},
    {"CONTROLLER_XBOX_12", "dpad_down"},
    {"CONTROLLER_XBOX_13", "dpad_left"},
};

std::string_view LabelFor(std::string_view labels, std::string_view button) {
    size_t at = 0;
    while (at < labels.size()) {
        size_t end = labels.find('\n', at);
        if (end == std::string_view::npos) end = labels.size();
        const std::string_view line = labels.substr(at, end - at);
        const size_t equals = line.find('=');
        if (equals != std::string_view::npos && line.substr(0, equals) == button) {
            return line.substr(equals + 1);
        }
        at = end + 1;
    }
    return {};
}

}  // namespace

void RuntimeNoteInputActivity(bool controllerActivity, bool keyboardMouseActivity) noexcept {
    const uint32_t device = g_device.Observe(controllerActivity, keyboardMouseActivity);
    if (!device) return;
    RuntimeGraphicsNoteInputDevice(device);
    std::fprintf(stderr, "RUNTIME_INPUT_DEVICE device=%s\n",
                 device == 1 ? "controller" : "keyboard_mouse");
    std::fflush(stderr);
}

std::optional<std::u16string> RuntimeKeyboardPromptTextFor(std::string_view key,
                                                           std::string_view labels) {
    for (const auto& [name, text] : kTemplates) {
        if (key == name) return std::u16string(text);
    }
    for (const auto& [name, button] : kButtonNames) {
        if (key != name) continue;
        const std::string_view label = LabelFor(labels, button);
        // An unbound button keeps the title's name.
        if (label.empty()) return std::nullopt;
        std::u16string text;
        for (unsigned char c : label) {
            // Key names are ASCII (the title's text draws Latin-1 only).
            text.push_back(c < 0x80 ? char16_t(c) : u'?');
        }
        return text;
    }
    return std::nullopt;
}

std::optional<std::u16string> RuntimeKeyboardPromptText(std::string_view key) {
    if (key.size() < 16 || key.substr(0, 16) != "CONTROLLER_XBOX_") return std::nullopt;
    std::array<char, 1024> labels{};
    const uint32_t state = RuntimeGraphicsPromptLabels(labels.data(), uint32_t(labels.size()));
    if (!(state & 1u)) return std::nullopt;
    return RuntimeKeyboardPromptTextFor(key, labels.data());
}

std::string RuntimePcConfigWithPromptIconSource(const std::string& contents,
                                                const std::filesystem::path& guiTextures) {
    if (guiTextures.empty()) return contents;
    // A TOML basic string; top-level keys precede every table.
    const auto path = guiTextures.u8string();  // UTF-8
    std::string line = "gpu_prompt_icon_source = \"";
    for (const auto c : path) {
        if (c == '\\' || c == '"') line.push_back('\\');
        line.push_back(char(c));
    }
    line += "\"\n";
    const bool bom = contents.rfind("\xEF\xBB\xBF", 0) == 0;
    return bom ? contents.substr(0, 3) + line + contents.substr(3) : line + contents;
}
