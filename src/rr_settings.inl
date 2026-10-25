#pragma once
#include "rr_settings_model.h"
#include "rr_lighting_source.h"
#include "rr_sss_mask.h"
#include "rr_responsivity.h"
#include "rr_guide_test.h"
#include "rr_sky_depth_test.h"
// CPU settings and UI only. Rendering threads service preset changes.
static std::mutex settings_mutex;
static bool settings_loaded{}, config_allowed{};
static rrsettings::Preferences preferences;
static bool &requested_rr = preferences.rr;
static bool &requested_sss = preferences.sss;
static bool &requested_glass = preferences.glass;
static bool &requested_tolerate = preferences.tolerate_isolated;
static unsigned &requested_lobe = preferences.native_lobe;
static bool &requested_responsivity = preferences.responsivity;
static float &requested_strength = preferences.responsivity_strength;
static bool &requested_hit = preferences.hit;
static constexpr const char *SettingsSection = "MW4RayReconstruction";
static void SaveToggles() {
    reshade::set_config_value(nullptr, SettingsSection, "Enabled", requested_rr);
    reshade::set_config_value(nullptr, SettingsSection, "SSS", requested_sss);
    reshade::set_config_value(nullptr, SettingsSection, "Transparency", requested_glass);
    reshade::set_config_value(nullptr, SettingsSection, "TolerateIsolatedGuidePixels",
                              requested_tolerate);
    reshade::set_config_value(nullptr, SettingsSection, "NativeLobe", requested_lobe);
    reshade::set_config_value(nullptr, SettingsSection, "ResponsivityMask", requested_responsivity);
    reshade::set_config_value(nullptr, SettingsSection, "ResponsivityStrength",
                              requested_strength);
    reshade::set_config_value(nullptr, SettingsSection, "SpecularHitDistance", requested_hit);
}
static void ApplyEnabled() {
    config_allowed = VrsConfigAllowsRR();
    LiveRR().SetEnabled(requested_rr && config_allowed);
    LiveRR().SetSssEnabled(requested_sss);
    LiveRR().SetTransparencyEnabled(requested_glass);
    LiveRR().SetSpecularHitEnabled(requested_hit);
    AlbedoInspector().observe_transitions = requested_glass;
    LiveRR().SetTolerateIsolated(requested_tolerate);
    requested_lobe = rrlighting::Valid(requested_lobe);
    rrbypass::lobe_source = requested_lobe;
    requested_strength = rrresponsivity::Clamp(requested_strength);
    LiveRR().SetResponsivity(requested_responsivity, requested_strength);
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
    reshade::get_config_value(nullptr, SettingsSection, "ResponsivityMask", requested_responsivity);
    reshade::get_config_value(nullptr, SettingsSection, "ResponsivityStrength",
                              requested_strength);
    reshade::get_config_value(nullptr, SettingsSection, "SpecularHitDistance", requested_hit);
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
        const bool ctrl_or_alt = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                 (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool changed = false;
        for (unsigned i = 0; i < 3; ++i) {
            const bool pressed = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (rrsettings::HotkeyToggles(pressed, down[i], ctrl_or_alt,
                                          foreground == GetCurrentProcessId())) {
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
    // Idle windows are reported a few times so a missing blur dispatch is visible.
    static ULONGLONG distance_report{};
    static unsigned idle_reports{};
    const auto now = GetTickCount64();
    if (now - distance_report >= 10000) {
        distance_report = now;
        char line[320];
        const bool active = DistanceStats().Summary(line, sizeof(line));
        if (LiveRR().Enabled() && LiveRR().SpecularHitEnabled() &&
            (active || idle_reports++ < 3))
            WriteLog(line);
    }
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
    changed |= ImGui::Checkbox("Specular hit distance", &requested_hit);
    ImGui::Separator();
    ImGui::TextUnformatted("Subsurface Scattering noise experiments");
    changed |= ImGui::Checkbox("Tolerate isolated bad guide pixels", &requested_tolerate);
    ImGui::TextWrapped("On: a few unusable guide pixels no longer reject the frame and reset RR "
                       "history; they are counted in ReShade.log instead. Off: previous behavior.");
    // Session-only: not saved, so a test mode never sticks across restarts.
    int guide_test = static_cast<int>(rrguidetest::Valid(rrguidetest::mode));
    if (ImGui::Combo("Guide test (thin geometry)", &guide_test, rrguidetest::labels,
                     rrguidetest::Count))
        rrguidetest::mode = static_cast<unsigned>(guide_test);
    bool sky_depth = rrskydepth::enabled;
    if (ImGui::Checkbox("Sky depth test: finite far plane", &sky_depth))
        rrskydepth::enabled = sky_depth;
    int lobe = static_cast<int>(rrlighting::Valid(requested_lobe));
    if (ImGui::Combo("Lighting input", &lobe, rrlighting::labels, rrlighting::Count)) {
        requested_lobe = static_cast<unsigned>(lobe);
        changed = true;
    }
    ImGui::TextWrapped("Native temporal replaces a lobe with the game's own accumulation across the "
                       "whole frame (lobe 2 behaves as diffuse): clean but soft. Fill gaps keeps "
                       "each fresh ray sample unboosted and uses native accumulation only where no "
                       "ray landed this frame. Cap bright samples keeps raw lighting but limits "
                       "each ray sample to 8x or 4x the game's own accumulated brightness before "
                       "it is boosted, so the SSS blur cannot spread it into blotches; rare "
                       "highlights get slightly darker. AO fix keeps raw colour but stops the "
                       "boost from washing out the occlusion value each lighting pixel carries: "
                       "A uses each fresh ray's own value and the game's where no ray landed, B "
                       "uses the game's everywhere. SSS pixels options apply native temporal or "
                       "fill gaps only where the previous frame's Subsurface Scattering guide is "
                       "nonzero and keep raw lighting everywhere else; they need the SSS guide "
                       "(F11) and show raw lighting without it.");
    if (rrlighting::SssMasked(rrlighting::Valid(requested_lobe)))
        ImGui::Text("SSS pixel mask: %s", rrsssmask::Label(SssMaskState()));
    changed |= ImGui::Checkbox("Responsivity mask on SSS pixels", &requested_responsivity);
    if (ImGui::SliderFloat("Mask strength", &requested_strength, -1.0f, 0.0f, "%.2f"))
        LiveRR().SetResponsivity(requested_responsivity, requested_strength);
    if (ImGui::IsItemDeactivatedAfterEdit())
        changed = true;
    ImGui::Text("Responsivity mask: %s", LiveRR().ResponsivityActive()      ? "active"
                                         : !requested_responsivity            ? "off"
                                         : rrresponsivity::Clamp(requested_strength) == 0
                                             ? "off (strength 0)"
                                             : "waiting (needs preset F and the SSS guide)");
    ImGui::TextWrapped("Tells RR to hold SSS pixels steadier over time: less boiling, but moving "
                       "SSS surfaces may ghost. Supported on RR preset F only.");
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
    StatusRow("Specular hit distance", status.hit);
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
