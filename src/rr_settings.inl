#pragma once
#include "rr_settings_model.h"
// CPU settings and UI only. Rendering threads service preset changes.
static std::mutex settings_mutex;
static bool settings_loaded{}, config_allowed{};
static rrsettings::Preferences preferences;
static bool &requested_rr = preferences.rr;
static bool &requested_sss = preferences.sss;
static bool &requested_glass = preferences.glass;
static bool &requested_tolerate = preferences.tolerate_isolated;
static unsigned &requested_lobe = preferences.native_lobe;
static constexpr const char *SettingsSection = "MW4RayReconstruction";
static void SaveToggles() {
    reshade::set_config_value(nullptr, SettingsSection, "Enabled", requested_rr);
    reshade::set_config_value(nullptr, SettingsSection, "SSS", requested_sss);
    reshade::set_config_value(nullptr, SettingsSection, "Transparency", requested_glass);
    reshade::set_config_value(nullptr, SettingsSection, "TolerateIsolatedGuidePixels",
                              requested_tolerate);
    reshade::set_config_value(nullptr, SettingsSection, "NativeLobe", requested_lobe);
}
static void ApplyEnabled() {
    config_allowed = VrsConfigAllowsRR();
    LiveRR().SetEnabled(requested_rr && config_allowed);
    LiveRR().SetSssEnabled(requested_sss);
    LiveRR().SetTransparencyEnabled(requested_glass);
    AlbedoInspector().observe_transitions = requested_glass;
    LiveRR().SetTolerateIsolated(requested_tolerate);
    if (requested_lobe > 2)
        requested_lobe = 0;
    rrbypass::lobe_source = requested_lobe;
}
static void LoadSettings() {
    if (settings_loaded)
        return;
    settings_loaded = true;
    unsigned preset = preferences.preset;
    reshade::get_config_value(nullptr, SettingsSection, "Enabled", requested_rr);
    reshade::get_config_value(nullptr, SettingsSection, "SSS", requested_sss);
    reshade::get_config_value(nullptr, SettingsSection, "Transparency", requested_glass);
    reshade::get_config_value(nullptr, SettingsSection, "TolerateIsolatedGuidePixels",
                              requested_tolerate);
    reshade::get_config_value(nullptr, SettingsSection, "NativeLobe", requested_lobe);
    reshade::get_config_value(nullptr, SettingsSection, "Preset", preset);
    if (!LiveRR().RequestPreset(preset))
        LiveRR().RequestPreset(6);
    ApplyEnabled();
}
static const char *PresetLabel(unsigned value) {
    for (unsigned i = 0; i < 4; ++i)
        if (rrpreset::values[i] == value)
            return rrpreset::names[i];
    return "Unknown";
}
static void OnRuntime(reshade::api::effect_runtime *) {
    Observer().Poll();
    {
        std::lock_guard lock(settings_mutex);
        LoadSettings();
        DWORD foreground{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
        static bool down[3]{};
        const int keys[] = {VK_F8, VK_F9, VK_F11};
        bool changed = false;
        for (unsigned i = 0; i < 3; ++i) {
            const bool pressed = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (pressed && !down[i] && foreground == GetCurrentProcessId()) {
                if (i == 0)
                    requested_rr = !requested_rr;
                else if (i == 1)
                    requested_glass = !requested_glass;
                else
                    requested_sss = !requested_sss;
                changed = true;
            }
            down[i] = pressed;
        }
        if (changed) {
            ApplyEnabled();
            SaveToggles();
        }
    }
    SparsePool().Poll();
    LiveRR().Poll();
}
static void StatusRow(const char *name, rrstatus::State value) {
    ImGui::Text("%s: %s", name, rrstatus::Label(value));
}
static void DrawSettings(reshade::api::effect_runtime *) {
    std::lock_guard lock(settings_mutex);
    LoadSettings();
    ImGui::TextUnformatted("MW4 Ray Reconstruction — Beta 1");
    bool changed = ImGui::Checkbox("Ray reconstruction (F8)", &requested_rr);
    changed |= ImGui::Checkbox("Transparency guide (F9)", &requested_glass);
    changed |= ImGui::Checkbox("Subsurface Scattering guide (F11)", &requested_sss);
    ImGui::Separator();
    ImGui::TextUnformatted("Subsurface Scattering noise experiments");
    changed |= ImGui::Checkbox("Tolerate isolated bad guide pixels", &requested_tolerate);
    ImGui::TextWrapped("On: a few unusable guide pixels no longer reject the frame and reset RR "
                       "history; they are counted in ReShade.log instead. Off: previous behavior.");
    const char *lobe_names[] = {"Off (raw lighting)", "Lobe 1 from native temporal",
                                "Lobe 2 from native temporal"};
    int lobe = static_cast<int>(requested_lobe);
    if (ImGui::Combo("Native lighting lobe", &lobe, lobe_names, 3)) {
        requested_lobe = static_cast<unsigned>(lobe);
        changed = true;
    }
    ImGui::TextWrapped("Feeds one lighting lobe from the game's own temporal accumulation instead "
                       "of raw samples, across the whole frame. One lobe is diffuse and one is "
                       "specular: the diffuse one should remove blotches on skin. Compare both.");
    ImGui::Separator();
    if (changed) {
        ApplyEnabled();
        SaveToggles();
    }
#ifdef RR_FILTER_DIAGNOSTIC
    ImGui::Separator();
    ImGui::TextUnformatted("Development diagnostic: lighting filtering");
    auto filter = LiveRR().NativeFilterStatus();
    const char *filter_names[] = {rrfilter::Label(rrfilter::Mode::Raw),
                                 rrfilter::Label(rrfilter::Mode::TemporalSpatial),
                                 rrfilter::Label(rrfilter::Mode::TemporalOnly),
                                 rrfilter::Label(rrfilter::Mode::SpatialRemaining)};
    int filter_choice = static_cast<int>(filter.desired);
    if (ImGui::Combo("Lighting comparison", &filter_choice, filter_names, 4))
        LiveRR().RequestFilterMode(static_cast<rrfilter::Mode>(filter_choice));
    if (ImGui::Button("Restart comparison history"))
        LiveRR().RestartFilterComparison();
    filter = LiveRR().NativeFilterStatus();
    ImGui::Text("Applied lighting: %s", rrfilter::Label(filter.effective));
    if (filter.pending)
        ImGui::TextUnformatted("Waiting for pending frames to retire...");
    ImGui::TextWrapped("Session-only comparison. Switching paths restarts RR history. Temporal only keeps native lighting history and bypasses spatial filtering. Temporal + spatial is the previous checkbox-on mode. Spatial only keeps raw preparation in place of the first spatial pass, then runs the remaining native spatial passes; it is not the complete native spatial chain.");
    ImGui::Separator();
#endif
    auto preset = LiveRR().PresetStatus();
    int selected = 3;
    for (int i = 0; i < 4; ++i)
        if (rrpreset::values[i] == preset.desired)
            selected = i;
    if (ImGui::Combo("RR preset", &selected, rrpreset::names, 4) &&
        LiveRR().RequestPreset(rrpreset::values[selected]))
        reshade::set_config_value(nullptr, SettingsSection, "Preset", rrpreset::values[selected]);
    preset = LiveRR().PresetStatus();
    if (preset.pending)
        ImGui::TextUnformatted("Applying change after pending frames finish...");
    if (preset.has_effective)
        ImGui::Text("Applied preset request: %s", PresetLabel(preset.effective));
    ImGui::Separator();
    const auto status = LiveRR().Status();
    StatusRow("RR", status.rr);
    StatusRow("Transparency", status.glass);
    StatusRow("Subsurface Scattering", status.sss);
    ImGui::Text("Reconstruction path: %s", rrstatus::RouteLabel(status.route));
    ImGui::TextWrapped("Waiting means enabled but not delivered in recent completed frames. A "
                       "guide may be unavailable in some scenes.");
    if (!config_allowed)
        ImGui::TextWrapped("RR configuration unavailable. Check that the campaign settings file is "
                           "read-only and contains a supported VRS value. See README.");
    if (status.rr == rrstatus::State::Unavailable)
        ImGui::TextWrapped("The game shut down its NGX runtime. Restart the game to restore RR.");
    const auto options = LiveRR().Options();
    ImGui::Separator();
    ImGui::Text("Observed DLSS mode: %s", rrsettings::QualityLabel(options.quality));
    if (rrresolution::Texture(options.width, options.height) &&
        rrresolution::Texture(options.out_width, options.out_height))
        ImGui::Text("Render: %u x %u    Output: %u x %u", options.width, options.height,
                    options.out_width, options.out_height);
    ImGui::TextWrapped("DLSS quality is configured using DLSSPerfModeMP in the game settings file. "
                       "Restart the game after changing it. RR preset changes above apply live.");
    ImGui::TextWrapped("Keep enhanced FOV, depth of field, and motion blur disabled. See README "
                       "for the exact configuration entries.");
}
