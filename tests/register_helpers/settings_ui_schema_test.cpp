// Shared settings UI (V315+): the game's schema as ReXGlue's generic settings
// description (every setting labelled and described, visibility rules point
// at real settings and values), TOML round trips across the plugin boundary,
// panel value/visibility logic, a headless ImGui draw of every section, and
// the in-game overlay wiring (plugin exports, input ownership, host save).
#include "pc_settings_schema.h"
#include "pc_settings_ui.h"

#include <rex/ui/settings_detection.h>
#include <rex/ui/settings_panel.h>
#include <rex/ui/settings_schema.h>

#include <imgui.h>
#include <imgui_impl_null.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace ui = rex::ui::settings;

namespace {

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

}  // namespace

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };

  // V440: the full schema (every shelved setting offered).
  const PcSettingsOffer all{true, true, true};
  const ui::Schema schema = BuildPcSettingsUiSchema(false, all);
  {
    bool complete = schema.settings.size() == PcEditableSettingsSchema().size();
    bool described = true;
    bool conditions = true;
    for (const PcEditableSettingSpec& spec : PcEditableSettingsSchema()) {
      const ui::Setting* setting = schema.Find(spec.key);
      complete = complete && setting && !setting->label.empty() && !setting->section.empty();
      if (!setting) continue;
      // Key bindings share one explanation on the section's first entry.
      described = described && (!setting->description.empty() ||
                                 setting->editor == rex::ui::settings::Editor::kKey);
      if (!setting->visible_when_key.empty()) {
        const ui::Setting* other = schema.Find(setting->visible_when_key);
        const bool valid_value =
            other && (other->editor == ui::Editor::kBoolean
                          ? (setting->visible_when_value == "true" ||
                             setting->visible_when_value == "false")
                          : std::any_of(other->choices.begin(), other->choices.end(),
                                        [&](const ui::Choice& choice) {
                                          return choice.value == setting->visible_when_value;
                                        }));
        conditions = conditions && valid_value;
      }
    }
    check(complete, "every game setting appears in the UI schema with a label and section");
    check(described, "every setting has a plain description");
    check(conditions, "visibility rules name existing settings and valid values");
    const ui::Setting* key = schema.Find("input.overlay_key");
    check(key && key->editor == ui::Editor::kChoice && key->choices.size() == 8 &&
              key->choices.front().value == "F1" &&
              std::none_of(key->choices.begin(), key->choices.end(),
                           [](const ui::Choice& choice) {
                             return choice.value == "F3" || choice.value == "F9" ||
                                    choice.value == "F10" || choice.value == "F12";
                           }),
          "overlay key offers only keys the host does not reserve");
    const ui::Setting* sensitivity = schema.Find("input.mouse_sensitivity");
    const ui::Setting* volume = schema.Find("audio.master_volume");
    const ui::Setting* scale = schema.Find("resolution_scale");
    check(sensitivity && sensitivity->live && volume && volume->live && scale && !scale->live,
          "live flags: sensitivity/volume apply now, internal scale at next start");
    // V330 plain-language frame-rate settings.
    const ui::Setting* present = schema.Find("display.present_mode");
    check(present && present->label == "VSync" && present->choices.size() == 3 &&
              present->choices[0].value == "vsync" && !present->choices[0].advanced &&
              present->choices[1].value == "immediate" && !present->choices[1].advanced &&
              present->choices[2].value == "vrr" && present->choices[2].advanced,
          "VSync is On/Off with the VRR variant in the advanced view");
    const ui::Setting* menu = schema.Find("display.menu_frame_rate");
    check(menu && menu->live && menu->choices.size() == 2 && menu->choices[0].value == "reduced",
          "menu frame rate applies now and offers reduced first");
    check(scale && scale->choices.size() == 8 && scale->choices[0].value == "0" &&
              scale->choices[2].label == "2x - 2560 x 1440" && !scale->choices[3].advanced &&
              scale->choices[4].advanced,
          "internal scale shows Automatic and the render resolution (4x and up advanced)");
  }
  {
    // V330: frame rates from the display's refresh replace the symbolic list.
    ui::PanelModel model;
    model.schema = &schema;
    ui::DetectFrameRateChoices(model, 144.0);
    const auto detected = model.detected_choices.find("display.frame_rate");
    bool offered_custom_advanced = false;
    size_t basic = 0;
    if (detected != model.detected_choices.end()) {
      for (const ui::Choice& choice : detected->second) {
        if (!choice.advanced) ++basic;
        if (choice.value == "custom") offered_custom_advanced = choice.advanced;
      }
    }
    check(detected != model.detected_choices.end() && basic == 6 && offered_custom_advanced &&
              detected->second[4].label == "144 - Recommended",
          "144 Hz offers six plain rates with 144 recommended and the custom cap advanced");
    check(model.detected_notes["display.frame_rate"] ==
              "Your monitor: 144 Hz - recommended 144 FPS",
          "the frame-rate setting names the monitor's refresh");
    ui::DetectFrameRateChoices(model, 0.0);
    check(!model.detected_choices.count("display.frame_rate") &&
              !model.detected_notes.count("display.frame_rate"),
          "an unknown refresh keeps the schema's own list");
  }
  {
    // V374: HD texture packs are offered only when one is installed, with
    // their disk and video memory cost.
    check(ui::IsTexturePackFileName("0123456789abcdef.dds") &&
              ui::IsTexturePackFileName(std::filesystem::path("sub") / "0123456789ABCDEF.DDS") &&
              !ui::IsTexturePackFileName("0123456789abcdef.png") &&
              !ui::IsTexturePackFileName("0123456789abcde.dds") &&
              !ui::IsTexturePackFileName("0123456789abcdeg.dds"),
          "pack files are <16 hex digits>.dds");
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "darkness_texture_pack_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder / "Some Pack", error);
    std::ofstream(folder / "Some Pack" / "0123456789abcdef.dds", std::ios::binary)
        << std::string(148 + 100000, 'x');
    std::ofstream(folder / "fedcba9876543210.dds", std::ios::binary) << std::string(148 + 10, 'x');
    std::ofstream(folder / "readme.txt") << "not a texture";
    const ui::TexturePackFolder pack = ui::ScanTexturePackFolder(folder);
    check(pack.textures == 2 && pack.disk_bytes == 148 * 2 + 100010 &&
              pack.video_memory_bytes == 128 * 1024 + 64 * 1024,
          "a pack scan counts textures, disk bytes and 64 KB video memory allocations");
    std::filesystem::remove_all(folder, error);
    check(ui::ScanTexturePackFolder(folder).textures == 0, "a missing folder holds no pack");
    check(ui::FormatMemorySize(540ull << 20) == "540 MB" &&
              ui::FormatMemorySize(uint64_t(1.3 * 1024) << 20) == "1.3 GB" &&
              ui::FormatMemorySize(16303ull << 20) == "16 GB" && ui::FormatMemorySize(1) == "1 MB",
          "memory sizes read plainly");

    ui::PanelModel model;
    model.schema = &schema;
    const std::string key(ui::kHdTexturesKey);
    check(schema.Find(key) && schema.Find(key)->editor == ui::Editor::kBoolean &&
              !schema.Find(key)->live,
          "HD texture packs are a next-start switch");
    ui::DetectTexturePack(model, {}, 16ull << 30, "the texture_packs folder in the game folder");
    check(model.unavailable.count(key) &&
              model.detected_notes[key] ==
                  "No texture pack installed. Packs go in the texture_packs folder in the game "
                  "folder.",
          "without a pack the setting is disabled and says where packs go");
    ui::DetectTexturePack(model, pack, 16ull << 30, "x");
    check(!model.unavailable.count(key) && !model.detected_warnings.count(key) &&
              model.detected_notes[key] ==
                  "Installed: 2 textures, 1 MB on disk. Needs about 1 MB more video memory "
                  "(this graphics card has 16 GB).",
          "an installed pack is offered with its size and video memory");
    ui::TexturePackFolder large;
    large.textures = 5000;
    large.disk_bytes = 5ull << 30;
    large.video_memory_bytes = 5ull << 30;
    ui::DetectTexturePack(model, large, 16ull << 30, "x");
    check(!model.unavailable.count(key) && model.detected_warnings.count(key),
          "a pack over a quarter of the card's video memory is cautioned");
    ui::DetectTexturePack(model, pack, 0, "x");
    check(!model.detected_warnings.count(key) &&
              model.detected_notes[key] ==
                  "Installed: 2 textures, 1 MB on disk. Needs about 1 MB more video memory.",
          "an unknown card leaves the comparison out");
    const std::string root = DARKNESS_SOURCE_ROOT;
    const std::string overlay =
        Read(root + "/external/ReXGlue/src/ui/overlay/host_settings_overlay.cpp");
    const std::string plugin = Read(root + "/external/ReXGlue/src/graphics/plugin_main.cpp");
    const std::string launcher = Read(root + "/runtime/settings_launcher.cpp");
    check(overlay.find("settings::DetectTexturePack(model_,") != std::string::npos &&
              plugin.find("ScanTexturePackFolder(rex::graphics::texture_pack::PackFolder())") !=
                  std::string::npos &&
              launcher.find("    DetectTexturePacks(app);\n") != std::string::npos,
          "the launcher and the in-game overlay both detect installed packs");
  }
  {
    // Temporal AA offers the vendor upscalers only with their runtime DLL.
    ui::PanelModel model;
    model.schema = &schema;
    const std::string key(ui::kTemporalAaKey);
    const auto offered = [&] {
      std::string joined;
      for (const ui::Choice& choice : model.detected_choices[key]) joined += choice.value + ",";
      return joined;
    };
    ui::DetectUpscalers(model, {});
    check(offered() == "off,taa,", "without SDK runtimes only Off and TAA are offered");
    ui::DetectUpscalers(model, {true, false, true});
    check(offered() == "off,taa,dlss,xess,", "each runtime present adds its choice");
    model.values[key] = "fsr";
    ui::DetectUpscalers(model, {});
    check(offered() == "off,taa,fsr," &&
              model.detected_choices[key].back().label.find("not installed") != std::string::npos,
          "a chosen upscaler without its runtime stays listed, marked");
    model.values[key] = "taa";
    ui::DetectUpscalers(model, {true, true, true, false});
    check(offered() == "off,taa,fsr,xess,", "DLSS is not offered without an NVIDIA RTX card");
    model.values[key] = "dlss";
    ui::DetectUpscalers(model, {true, true, true, false});
    check(offered() == "off,taa,dlss,fsr,xess," &&
              model.detected_choices[key][2].label.find("needs an NVIDIA RTX card") !=
                  std::string::npos,
          "a chosen DLSS on another card stays listed, marked");
    const std::string root = DARKNESS_SOURCE_ROOT;
    check(Read(root + "/external/ReXGlue/src/ui/overlay/host_settings_overlay.cpp")
                      .find("settings::DetectUpscalers(model_, state_.upscalers)") !=
                  std::string::npos &&
              Read(root + "/runtime/settings_launcher.cpp")
                      .find("ui::DetectUpscalers(app.model, app.upscalers);") != std::string::npos,
          "the launcher and the in-game overlay both filter the upscalers");
  }
  {
    // TOML round trips across the plugin boundary.
    const auto parsed = ui::ParseSchema(ui::SerializeSchema(schema));
    bool same = parsed && parsed->settings.size() == schema.settings.size() &&
                parsed->sections == schema.sections;
    for (size_t index = 0; same && index < schema.settings.size(); ++index) {
      const ui::Setting& a = schema.settings[index];
      const ui::Setting& b = parsed->settings[index];
      same = a.key == b.key && a.label == b.label && a.description == b.description &&
             a.editor == b.editor && a.minimum == b.minimum && a.maximum == b.maximum &&
             a.step == b.step && a.display_multiplier == b.display_multiplier &&
             a.choices.size() == b.choices.size() && a.live == b.live &&
             a.advanced == b.advanced && a.visible_when_key == b.visible_when_key &&
             a.visible_when_value == b.visible_when_value;
    }
    check(same, "schema survives the TOML round trip");
    // V330: advanced choices (VRR, custom cap, 4x+ scales) keep their flag.
    bool advanced_choices = parsed.has_value();
    size_t advanced_count = 0;
    for (size_t index = 0; advanced_choices && index < schema.settings.size(); ++index) {
      for (size_t choice = 0; choice < schema.settings[index].choices.size(); ++choice) {
        const bool flag = schema.settings[index].choices[choice].advanced;
        advanced_count += flag ? 1 : 0;
        advanced_choices = advanced_choices &&
                           parsed->settings[index].choices[choice].advanced == flag &&
                           parsed->settings[index].choices[choice].value ==
                               schema.settings[index].choices[choice].value;
      }
    }
    check(advanced_choices && advanced_count >= 6,
          "advanced choices survive the TOML round trip");
    ui::Values values{{"input.mouse_sensitivity", "1.25"}, {"window_mode", "borderless"},
                      {"input.keyboard_mouse", "true"}};
    const auto round = ui::ParseValues(ui::SerializeValues(values));
    check(round && *round == values, "dotted keys and values survive the TOML round trip");
    check(!ui::ParseSchema("version = 99\n").has_value(), "unknown schema versions are refused");
  }
  {
    const ui::Setting* sensitivity = schema.Find("input.mouse_sensitivity");
    const ui::Setting* fov = schema.Find("camera.field_of_view");
    const ui::Setting* volume = schema.Find("audio.master_volume");
    check(sensitivity && ui::FormatNumberValue(*sensitivity, 1.25) == "1.25" && fov &&
              ui::FormatNumberValue(*fov, 95.0) == "95.0" && volume &&
              ui::FormatNumberValue(*volume, 0.5) == "0.50",
          "numbers are written at the setting's step with a decimal point");
  }
  {
    // Panel logic: precedence, visibility, pending edits.
    ui::PanelModel model;
    model.schema = &schema;
    model.defaults = {{"input.keyboard_mouse", "false"}, {"input.mouse_look", "native"},
                      {"display.frame_rate", "original"}};
    model.saved = {{"input.keyboard_mouse", "true"}};
    check(ui::ValueOf(model, "input.keyboard_mouse") == "true" &&
              ui::ValueOf(model, "input.mouse_look") == "native",
          "saved values win over defaults");
    const ui::Setting* sensitivity = schema.Find("input.mouse_sensitivity");
    const ui::Setting* acceleration = schema.Find("input.mouse_acceleration");
    const ui::Setting* limit = schema.Find("display.frame_limit");
    check(sensitivity && ui::IsShown(model, *sensitivity), "mouse settings show with keyboard/mouse");
    check(acceleration && !ui::IsShown(model, *acceleration),
          "stick-bridge options hide in native mouse look and in the basic view");
    check(limit && !ui::IsShown(model, *limit), "custom cap hides unless the rate is custom");
    model.values["display.frame_rate"] = "custom";
    // V330: the custom cap belongs to the advanced view.
    check(!ui::IsShown(model, *limit), "custom cap stays in the advanced view");
    model.show_advanced = true;
    check(ui::IsShown(model, *limit), "custom cap shows with the custom rate in Advanced");
    model.show_advanced = false;
    model.values["input.keyboard_mouse"] = "false";
    check(!ui::IsShown(model, *sensitivity), "mouse settings hide without keyboard/mouse");
    const auto pending = ui::PendingKeys(model);
    check(pending.size() == 2, "edits that differ from saved values are pending");
  }
  {
    // Headless draw of every section on both surfaces.
    ImGui::CreateContext();
    ImGui_ImplNull_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ui::ApplyStyle(1.5f);
    ui::PanelModel model;
    model.schema = &schema;
    model.show_advanced = true;
    model.defaults = {{"input.keyboard_mouse", "true"}, {"input.mouse_look", "stick"},
                      {"display.frame_rate", "custom"}};
    size_t drawn = 0;
    for (int surface = 0; surface < 2; ++surface) {
      for (size_t section = 0; section < schema.sections.size(); ++section) {
        model.section = section;
        ImGui_ImplNull_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("panel");
        const auto changes =
            ui::DrawPanel(model, surface ? ui::Surface::kInGame : ui::Surface::kLauncher);
        drawn += changes.empty() ? 1 : 0;
        ImGui::End();
        ImGui::Render();
      }
    }
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    check(drawn == schema.sections.size() * 2, "every section draws on both surfaces");
  }
  {
    // V404 Arabic interface: translated, right-to-left, survives the plugin
    // boundary, and every section draws mirrored on both surfaces.
    const ui::Schema arabic = BuildPcSettingsUiSchema(true, all);
    const auto parsed = ui::ParseSchema(ui::SerializeSchema(arabic));
    check(parsed && parsed->right_to_left && !parsed->text.empty() &&
              parsed->text.size() == arabic.text.size() &&
              parsed->sections == arabic.sections,
          "the Arabic schema round-trips with its direction and texts");
    check(arabic.sections.size() == schema.sections.size() && arabic.sections[0] != schema.sections[0],
          "sections are translated");
    const ui::Setting* scale = arabic.Find("resolution_scale");
    check(scale && scale->label != "Internal scale" && scale->choices.size() > 1 &&
              scale->choices[1].label == "1x - 1280 x 720",
          "labels are translated, technical choices stay");
    check(ui::Translate(&arabic, "Reset") != "Reset" && ui::Translate(&schema, "Reset") == "Reset",
          "panel words are translated only in the Arabic schema");
    ImGui::CreateContext();
    ImGui_ImplNull_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ui::ApplyStyle(1.0f);
    ui::PanelModel model;
    model.schema = &arabic;
    model.show_advanced = true;
    model.defaults = {{"input.keyboard_mouse", "true"}, {"anisotropic_override", "5"}};
    model.values = {{"anisotropic_override", "3"}};
    size_t drawn = 0;
    for (int surface = 0; surface < 2; ++surface) {
      for (size_t section = 0; section < arabic.sections.size(); ++section) {
        model.section = section;
        ImGui_ImplNull_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("panel");
        const auto changes =
            ui::DrawPanel(model, surface ? ui::Surface::kInGame : ui::Surface::kLauncher);
        drawn += changes.empty() ? 1 : 0;
        ImGui::End();
        ImGui::Render();
      }
    }
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    check(drawn == arabic.sections.size() * 2, "every Arabic section draws on both surfaces");
  }
  {
    // Key capture: a binding waits for the click to be released, then takes
    // the next key; F1-F12 are refused; Escape is a key, not a cancel.
    ImGui::CreateContext();
    ImGui_ImplNull_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    ui::PanelModel model;
    model.schema = &schema;
    model.defaults = {{"input.keyboard_mouse", "true"}, {"input.bind.x", "R"}};
    for (size_t index = 0; index < schema.sections.size(); ++index) {
      if (schema.sections[index] == "Key bindings") model.section = index;
    }
    model.capture_key = "input.bind.x";
    const auto frame = [&](auto&& input) {
      input();
      ImGui_ImplNull_NewFrame();
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(io.DisplaySize);
      ImGui::Begin("panel");
      auto changes = ui::DrawPanel(model, ui::Surface::kInGame);
      ImGui::End();
      ImGui::Render();
      return changes;
    };
    frame([&] { io.AddMousePosEvent(5.0f, 5.0f); });
    const bool armed = model.capture_armed;
    frame([&] { io.AddKeyEvent(ImGuiKey_F5, true); });
    frame([&] { io.AddKeyEvent(ImGuiKey_F5, false); });
    const bool refused = ui::IsCapturingKey(model) && !model.capture_note.empty();
    const auto taken = frame([&] { io.AddKeyEvent(ImGuiKey_G, true); });
    frame([&] { io.AddKeyEvent(ImGuiKey_G, false); });
    check(armed && refused && taken.size() == 1 && taken[0].key == "input.bind.x" &&
              taken[0].value == "G" && !ui::IsCapturingKey(model),
          "key capture arms after release, refuses F1-F12 and stores the next key");
    model.capture_key = "input.bind.start";
    model.capture_armed = true;
    const auto escape = frame([&] { io.AddKeyEvent(ImGuiKey_Escape, true); });
    frame([&] { io.AddKeyEvent(ImGuiKey_Escape, false); });
    check(escape.size() == 1 && escape[0].value == "Escape", "Escape can be bound");
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    check(ui::BindingLabel("LMB") == "Left mouse" && ui::BindingLabel("") == "(none)" &&
              ui::BindingLabel("Q") == "Q",
          "binding labels are readable");
    // Every binding name the panel can store parses in the input driver.
    const std::string keybinds =
        Read(std::string(DARKNESS_SOURCE_ROOT) + "/external/ReXGlue/src/ui/keybinds.cpp");
    bool known = true;
    for (std::string_view name : ui::kBindingKeyNames) {
      if (name == "WheelUp" || name == "WheelDown") continue;
      known = known && keybinds.find("{\"" + std::string(name) + "\", VirtualKey::") !=
                           std::string::npos;
    }
    check(known, "binding names match rex::ui::ParseVirtualKey");
    const std::string plugin =
        Read(std::string(DARKNESS_SOURCE_ROOT) + "/external/ReXGlue/src/graphics/plugin_main.cpp");
    check(plugin.find("!dialog_->CapturingKey();") != std::string::npos,
          "the overlay does not close on Escape while a binding captures");
  }
  {
    // In-game overlay wiring (source policy).
    const std::string root = DARKNESS_SOURCE_ROOT;
    const std::string plugin = Read(root + "/external/ReXGlue/src/graphics/plugin_main.cpp");
    check(plugin.find("REXCVAR_DEFINE_STRING(input_overlay_key, \"F1\",") != std::string::npos,
          "overlay key cvar is static with default F1");
    check(plugin.find("std::numeric_limits<size_t>::max() - 3") != std::string::npos &&
              plugin.find("window->AddInputListener(overlay_input_blocker.get(), 32);") !=
                  std::string::npos,
          "overlay toggle sits above the title bridge; the blocker between drawer and MnK");
    check(plugin.find("set_is_active_callback") != std::string::npos &&
              plugin.find("settings_overlay_open.load(std::memory_order_acquire)") !=
                  std::string::npos,
          "keyboard/mouse is inactive for the game while the overlay is open");
    for (const char* name : {"rex_gpu_embedded_settings_configure", "rex_gpu_embedded_settings_poll",
                             "rex_gpu_embedded_settings_saved",
                             "rex_gpu_embedded_settings_overlay_open"}) {
      check(plugin.find(std::string("REX_GPU_PLUGIN_EXPORT uint32_t ") + name) !=
                std::string::npos,
            name);
    }
    const std::string input = Read(root + "/runtime/runtime_input.cpp");
    check(input.find("if (RuntimeGraphicsSettingsOverlayOpen()) {") != std::string::npos,
          "the title sees an idle pad while the overlay is open");
    const std::string main_source = Read(root + "/runtime/main.cpp");
    check(main_source.find("settingsService.persistence = startupPcConfig.present() &&") !=
                  std::string::npos &&
              main_source.find("configDirectory != presetDirectory") != std::string::npos,
          "overlay edits never write into packaged presets");
    const std::string service = Read(root + "/runtime/runtime_settings_service.cpp");
    check(service.find("RuntimeSingleInstance lock(RuntimeConfigLockName(config.configPath));") !=
                  std::string::npos &&
              service.find("InstallRuntimePcPresetWithOverrides(\n"
                           "                configExists() ? config.configPath : "
                           "config.examplePath, config.configPath,") != std::string::npos,
          "overlay edits are saved under the config lock by the validated writer (a first "
          "save creates the configuration from the defaults)");
    // V320 enhanced first: without a saved configuration the game runs the
    // packaged defaults (the example), consumers read that file's snapshot.
    check(main_source.find("auto defaults = RuntimePcConfigSnapshot::Capture(packagedDefaultsPath);") !=
                  std::string::npos &&
              main_source.find("!launchOptions.pcConfigExplicit &&\n"
                               "                launchOptions.action == RuntimeLaunchAction::Run") !=
                  std::string::npos &&
              main_source.find("startupFromPackagedDefaults ? packagedDefaultsPath : "
                               "launchOptions.pcConfigPath;") != std::string::npos,
          "a run without a saved configuration uses the packaged Enhanced defaults");
  }

  {
    // V440 shelved features: the public release offers neither temporal AA
    // nor Arabic; their code stays behind DARKNESS_EXPERIMENTAL or a pack.
    const ui::Schema shelved = BuildPcSettingsUiSchema();
    const ui::Setting* language = shelved.Find("general.language");
    bool arabic_choice = false;
    for (const ui::Choice& choice : language ? language->choices : std::vector<ui::Choice>{}) {
      arabic_choice = arabic_choice || choice.value == "arabic";
    }
    check(!shelved.Find("graphics.temporal_aa") && !shelved.Find("streamline.frame_generation") &&
              shelved.settings.size() + 2 == PcEditableSettingsSchema().size(),
          "without the switch the temporal AA and frame generation settings are not offered");
    check(language && language->label == "Language" && !arabic_choice &&
              language->choices.size() == 6 &&
              language->description.find("Arabic") == std::string::npos,
          "without a pack the language setting names only the game's languages");
    const ui::Schema asked = BuildPcSettingsUiSchema(true);
    check(!asked.right_to_left && asked.text.empty(),
          "the Arabic interface needs Arabic to be offered");
    const ui::Setting* offered = schema.Find("general.language");
    check(schema.Find("graphics.temporal_aa") && offered &&
              offered->choices.back().value == "arabic" && offered->label != "Language",
          "offered, both settings are back");

    // The switch: names separated by commas or spaces.
    _putenv_s("DARKNESS_EXPERIMENTAL", "hdr, temporal_aa");
    const bool listed = PcExperimentalFeature("temporal_aa") && !PcExperimentalFeature("arabic") &&
                        !PcExperimentalFeature("temporal");
    _putenv_s("DARKNESS_EXPERIMENTAL", "");
    check(listed && !PcExperimentalFeature("temporal_aa"), "the switch lists features by name");

    // Arabic is offered by a pack: installed, carried by the package or
    // downloadable.
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "darkness_settings_offer_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder / "language_packs", error);
    const PcSettingsOffer none = PcSettingsOfferFor(folder);
    const PcSettingsOffer download = PcSettingsOfferFor(folder, true);
    std::ofstream(folder / "language_packs" / "arabic_language_pack.zip") << "zip";
    const PcSettingsOffer bundled = PcSettingsOfferFor(folder);
    std::filesystem::remove(folder / "language_packs" / "arabic_language_pack.zip", error);
    std::filesystem::create_directories(
        folder / "language_packs" / "arabic" / "content" / "Content" / "Fonts", error);
    const PcSettingsOffer strings_missing = PcSettingsOfferFor(folder);
    std::ofstream(folder / "language_packs" / "arabic" / "strings.tsv") << "x";
    const PcSettingsOffer installed = PcSettingsOfferFor(folder);
    std::filesystem::remove_all(folder, error);
    check(!none.arabic && !none.temporalAa && download.arabic && bundled.arabic &&
              !strings_missing.arabic && installed.arabic,
          "Arabic is offered only with an installed, carried or downloadable pack");

    // Both surfaces build their schema from the folder's offer, and the game
    // turns a saved temporal AA choice off without the switch.
    const std::string root = DARKNESS_SOURCE_ROOT;
    const std::string launcher = Read(root + "/runtime/settings_launcher.cpp");
    const std::string service = Read(root + "/runtime/runtime_settings_service.cpp");
    const std::string main_source = Read(root + "/runtime/main.cpp");
    check(launcher.find("app.schema = BuildPcSettingsUiSchema(arabic, app.offer);") !=
                  std::string::npos &&
              launcher.find("const bool arabic = app.offer.arabic &&") != std::string::npos &&
              launcher.find("    if (app.offer.arabic) {\n        ui::DetectLanguagePack(") !=
                  std::string::npos &&
              launcher.find("if (!busy && !app.offer.arabic) return;") != std::string::npos &&
              service.find("BuildPcSettingsUiSchema(false, config.offer)") != std::string::npos &&
              service.find("config.offer.arabic && language != saved.end()") !=
                  std::string::npos &&
              main_source.find("settingsService.offer = PcSettingsOfferFor(userData.localData, executableDirectory);") !=
                  std::string::npos &&
              main_source.find("PcExperimentalFeature(\"temporal_aa\"), &shelvedTemporalAa);") !=
                  std::string::npos,
          "the launcher, the overlay and the game follow the offer");
  }

  if (!passed) return 1;
  std::cout << "Settings UI schema, panel and overlay wiring: PASS\n";
  return 0;
}
