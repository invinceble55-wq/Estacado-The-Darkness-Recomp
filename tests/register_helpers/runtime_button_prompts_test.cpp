// Keyboard button prompts (0.9.1): device tracking, activity rules, the
// keyboard wording of the title's controller prompt strings and the GPU
// configuration key.

#include "runtime_button_prompts.h"

#include <cstdint>
#include <cstdio>
#include <string>

// The plugin side is not loaded in this test.
static uint32_t g_noted_device = 0;
void RuntimeGraphicsNoteInputDevice(uint32_t device) noexcept { g_noted_device = device; }
uint32_t RuntimeGraphicsPromptLabels(char* buffer, uint32_t size) noexcept {
    if (buffer && size) buffer[0] = '\0';
    return 0;
}

namespace {
int failures = 0;
void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}
}  // namespace

int main() {
    RuntimePromptDeviceTracker tracker;
    Check(tracker.Current() == 0, "no device before any input");
    Check(tracker.Observe(false, false) == 0, "an idle poll reports nothing");
    Check(tracker.Observe(false, true) == 2, "keyboard input reports the keyboard");
    Check(tracker.Observe(false, true) == 0, "the same device is reported once");
    Check(tracker.Observe(true, true) == 0, "both in one poll keep the current device");
    Check(tracker.Observe(true, false) == 1, "controller input reports the controller");
    Check(tracker.Current() == 1, "the controller is current");

    Check(!RuntimeControllerActivity(0x0010, 0x0010, 0, 0, 3000, -3000, 0, 0),
          "a held button and a resting stick are not activity");
    Check(RuntimeControllerActivity(0x1010, 0x0010, 0, 0, 0, 0, 0, 0),
          "a newly pressed button is activity");
    Check(RuntimeControllerActivity(0, 0, 0, 200, 0, 0, 0, 0), "a pulled trigger is activity");
    Check(!RuntimeControllerActivity(0, 0, 40, 0, 0, 0, 0, 0), "a worn trigger is not activity");
    Check(RuntimeControllerActivity(0, 0, 0, 0, 0, 0, -20000, 0), "a pushed stick is activity");
    Check(!RuntimeKeyboardMouseActivity(0, 0, 0, 0, 0, 0, 0), "idle keys are not activity");
    Check(RuntimeKeyboardMouseActivity(0, 0, 0, 0, 32767, 0, 0), "a movement key is activity");
    Check(RuntimeKeyboardMouseActivity(0, 255, 0, 0, 0, 0, 0), "a mouse button is activity");
    // #16: mouse jitter must not flip the prompts away from a controller.
    Check(!RuntimeKeyboardMouseActivity(0, 0, 0, 0, 0, 900, -600),
          "a small stick-mode mouse deflection is not activity");
    Check(RuntimeKeyboardMouseActivity(0, 0, 0, 0, 0, 20000, 0),
          "a deliberate stick-mode mouse movement is activity");
    {
        RuntimeMouseActivityGate gate;
        Check(!gate.Observe(1, 0, 1000) && !gate.Observe(-2, 1, 1100) && !gate.Observe(1, 1, 1200),
              "sensor jitter stays below the gate");
        Check(!gate.Observe(3, 0, 2000), "jitter after a pause starts a new window");
        Check(!gate.Observe(20, 5, 3000) && gate.Observe(15, 4, 3100),
              "a deliberate movement passes the gate");
    }

    RuntimeNoteInputActivity(false, true);
    Check(g_noted_device == 2, "the plugin hears about the keyboard");
    g_noted_device = 0;
    RuntimeNoteInputActivity(false, true);
    Check(g_noted_device == 0, "and only once");

    struct TemplateCase {
        const char* suffix;
        const char16_t* text;
    };
    constexpr TemplateCase buttonTemplates[] = {
        {"A", u"Press \u00A7p0"},   {"A2", u"Hold \u00A7p0"},
        {"B", u"press \u00A7p0"},   {"B2", u"hold \u00A7p0"},
        {"C", u"pressing \u00A7p0"}, {"C2", u"holding \u00A7p0"},
    };
    for (int group = 1; group <= 3; ++group) {
        for (const auto& test : buttonTemplates) {
            const std::string key =
                "CONTROLLER_XBOX_BUTTON_" + std::to_string(group) + "_" + test.suffix;
            const auto text = RuntimeKeyboardPromptTextFor(key, {});
            Check(text && *text == test.text, key.c_str());
        }
    }
    constexpr TemplateCase otherTemplates[] = {
        {"TRIGGER_A", u"Press \u00A7p0"}, {"TRIGGER_B", u"press \u00A7p0"},
        {"TRIGGER_C", u"pressing \u00A7p0"},
        {"THUMBSTICK_CLICK_A", u"Press \u00A7p0"},
        {"THUMBSTICK_CLICK_B", u"press \u00A7p0"},
        {"THUMBSTICK_CLICK_C", u"pressing \u00A7p0"},
        {"DPAD_A", u"Press \u00A7p0"}, {"DPAD_B", u"press \u00A7p0"},
        {"DPAD_C", u"pressing \u00A7p0"},
    };
    for (const auto& test : otherTemplates) {
        const std::string key = "CONTROLLER_XBOX_" + std::string(test.suffix);
        const auto text = RuntimeKeyboardPromptTextFor(key, {});
        Check(text && *text == test.text, key.c_str());
    }

    const std::string labels =
        "a=E\nb=Shift\nx=R\ny=Space\nstart=Escape\nback=Tab\nlstick_press=C\n"
        "rstick_press=MMB\nleft_shoulder=Q\nright_shoulder=WheelUp\nleft_trigger=LMB\n"
        "right_trigger=RMB\ndpad_up=1\ndpad_right=Right\ndpad_down=2\ndpad_left=Left\n";
    struct ButtonCase {
        const char* key;
        const char16_t* label;
    };
    constexpr ButtonCase buttons[] = {
        {"CONTROLLER_XBOX_00", u"E"},       {"CONTROLLER_XBOX_01", u"Shift"},
        {"CONTROLLER_XBOX_02", u"R"},       {"CONTROLLER_XBOX_03", u"Space"},
        {"CONTROLLER_XBOX_04", u"Escape"},  {"CONTROLLER_XBOX_05", u"Tab"},
        {"CONTROLLER_XBOX_06", u"C"},       {"CONTROLLER_XBOX_07", u"MMB"},
        {"CONTROLLER_XBOX_08", u"Q"},       {"CONTROLLER_XBOX_09", u"WheelUp"},
        {"CONTROLLER_XBOX_0A", u"LMB"},     {"CONTROLLER_XBOX_0B", u"RMB"},
        {"CONTROLLER_XBOX_10", u"1"},       {"CONTROLLER_XBOX_11", u"Right"},
        {"CONTROLLER_XBOX_12", u"2"},       {"CONTROLLER_XBOX_13", u"Left"},
    };
    for (const auto& test : buttons) {
        const auto text = RuntimeKeyboardPromptTextFor(test.key, labels);
        Check(text && *text == test.label, test.key);
    }
    Check(!RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_02", "x=\n"),
          "an unbound button keeps the title's name");
    Check(!RuntimeKeyboardPromptTextFor("CONTROLLER_XBOX_THUMBSTICK_A", labels),
          "stick movement keeps the title's text");
    Check(!RuntimeKeyboardPromptTextFor("MENU_RESUME", labels), "other strings are untouched");
    Check(!RuntimeKeyboardPromptText("CONTROLLER_XBOX_00"),
          "nothing changes while the plugin wants Xbox prompts");

    const std::string config = "pc_config_version = 1\n[input]\n";
    const std::string with = RuntimePcConfigWithPromptIconSource(
        config, std::filesystem::path(u8"C:\\Games\\Dark \"ness\"\\Content\\Textures\\GUI.xtc"));
    Check(with.rfind("gpu_prompt_icon_source = \"C:\\\\Games\\\\Dark \\\"ness\\\"\\\\Content"
                     "\\\\Textures\\\\GUI.xtc\"\n",
                     0) == 0 &&
              with.find(config) != std::string::npos,
          "the icon source is a top-level escaped TOML string");
    Check(RuntimePcConfigWithPromptIconSource(config, {}) == config,
          "no source leaves the configuration unchanged");
    const std::string bom = "\xEF\xBB\xBFpc_config_version = 1\n";
    Check(RuntimePcConfigWithPromptIconSource(bom, "G.xtc").rfind("\xEF\xBB\xBFgpu_prompt", 0) == 0,
          "the key goes after a byte order mark");

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("runtime_button_prompts: all checks passed\n");
    return 0;
}
