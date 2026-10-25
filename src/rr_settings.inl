#pragma once
#include "rr_settings_model.h"
#include "rr_lighting_source.h"
#include "rr_sss_mask.h"
#include "rr_responsivity.h"
#include "rr_water.h"
#include "rr_guide_test.h"
#include "rr_sky_depth_test.h"
#include "rr_weapon_depth.h"
#include "rr_sky_edge.h"
#include "rr_color_scale.h"
#include "rr_contrast_test.h"
#include "rr_hit_condition.h"
// CPU settings and UI only. Rendering threads service preset changes.
static std::mutex settings_mutex;
static bool settings_loaded{}, config_allowed{};
static unsigned config_warnings{}; // settings that differ from INSTALLATION.txt
static rrsettings::Preferences preferences;
static bool &requested_rr = preferences.rr;
static bool &requested_sss = preferences.sss;
static bool &requested_glass = preferences.glass;
static bool &requested_tolerate = preferences.tolerate_isolated;
static unsigned &requested_lobe = preferences.native_lobe;
static bool &requested_responsivity = preferences.responsivity;
static float &requested_strength = preferences.responsivity_strength;
static bool &requested_hit = preferences.hit;
static bool &requested_weapon_depth = preferences.weapon_depth;
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
    reshade::set_config_value(nullptr, SettingsSection, "WeaponDepth", requested_weapon_depth);
}
static void ApplyEnabled(bool allowed) {
    config_allowed = allowed;
    // Before RR can be enabled: RR takes its depth type when it is created. A change restarts
    // a running RR and retries linear depth held off since the last change.
    if (rrweapondepth::enabled.exchange(requested_weapon_depth) != requested_weapon_depth)
        ++rrweapondepth::changes;
    LiveRR().SetEnabled(requested_rr && config_allowed);
    LiveRR().SetSssEnabled(requested_sss);
    LiveRR().SetTransparencyEnabled(requested_glass);
    LiveRR().SetSpecularHitEnabled(requested_hit);
    AlbedoInspector().observe_transitions = requested_glass;
    LiveRR().SetTolerateIsolated(requested_tolerate);
    requested_lobe = rrlighting::Valid(requested_lobe);
    rrbypass::lobe_source = requested_lobe;
    LiveRR().SetCaptureLighting(requested_lobe);
    requested_strength = rrresponsivity::Clamp(requested_strength);
    LiveRR().SetResponsivity(requested_responsivity, requested_strength);
}
static void ApplyEnabled() {
    ApplyEnabled(VrsConfigAllowsRR());
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
    reshade::get_config_value(nullptr, SettingsSection, "WeaponDepth", requested_weapon_depth);
    reshade::get_config_value(nullptr, SettingsSection, "Preset", preset);
    if (!LiveRR().RequestPreset(preset))
        LiveRR().RequestPreset(6);
    // One read for the RR gate and the config report, so the report names the gate's answer.
    const auto config = ReadGameConfig();
    ApplyEnabled(VrsGateAllows(config));
    config_warnings = ReportGameConfig(config, config_allowed);
}
static const char *PresetLabel(unsigned value) {
    for (unsigned i = 0; i < 4; ++i)
        if (rrpreset::values[i] == value)
            return rrpreset::names[i];
    return "Unknown";
}
// The game window has the focus. Lock-free: OnPresent tests it before any lock.
static bool Foreground() {
    DWORD foreground{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    return foreground == GetCurrentProcessId();
}
static void OnRuntime(reshade::api::effect_runtime *runtime) {
    Observer().Poll();
    {
        std::lock_guard lock(settings_mutex);
        LoadSettings();
        const bool foreground = Foreground();
        static bool down[3]{};
        const int keys[] = {VK_F8, VK_F9, VK_F11};
        const bool ctrl_or_alt = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                 (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool changed = false;
        for (unsigned i = 0; i < 3; ++i) {
            const bool pressed = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (rrsettings::HotkeyToggles(pressed, down[i], ctrl_or_alt, foreground)) {
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
        // Pause/Break, as every F key is taken; not with Ctrl (Ctrl+Pause is Break) or Alt held.
        // Its down and up arrive together, too briefly to poll, so ReShade's key messages are
        // read: on release only, as both in one frame show as released and a press whose down
        // and up arrive apart then still requests once. A request is ignored while active.
        if (runtime && runtime->is_key_released(VK_PAUSE) && foreground &&
            !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000))
            LiveRR().RequestCapture(); // Session only: not saved.
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
        if (ConditionStats().Summary(line, sizeof(line)))
            WriteLog(line);
    }
}
// Every present, before OnRuntime takes any lock: lock-free counts for the RR health line.
static void OnPresent(reshade::api::effect_runtime *runtime) {
    const bool focused = Foreground();
    auto &shown = rrhealth::presentation;
    shown.presents.fetch_add(1, std::memory_order_relaxed);
    if (!focused)
        shown.unfocused_presents.fetch_add(1, std::memory_order_relaxed);
    shown.focused.store(focused, std::memory_order_relaxed);
    OnRuntime(runtime);
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
    {
        // Session-only A/B tests of what RR is told; not saved.
        ImGui::Indent();
        bool sky_miss = rrhitcond::sky_miss;
        if (ImGui::Checkbox("Hit distance test: sky and misses far (65504)", &sky_miss)) {
            rrhitcond::sky_miss = sky_miss;
            ++rrhitcond::changes;
        }
        bool fill = rrhitcond::fill;
        if (ImGui::Checkbox("Hit distance test: fill pixels with no ray", &fill)) {
            rrhitcond::fill = fill;
            ++rrhitcond::changes;
        }
        using TestState = rrlive::Controller::TestState;
        const auto hit_test = LiveRR().HitDistanceTest();
        // The fill is left out while a guide test replaces the normals it compares.
        const bool no_fill = fill && rrhitcond::NormalsReplaced(rrguidetest::Valid(
                                         rrguidetest::mode.load(std::memory_order_relaxed)));
        ImGui::Text("Hit distance tests: %s%s",
                    !sky_miss && !fill                  ? "off"
                    : !requested_hit                    ? "off (needs Specular hit distance)"
                    : LiveRR().HitDistanceTestFailed()  ? "off: shader setup failed (see log)"
                    : hit_test == TestState::Blocked    ? "off: not enough GPU memory (change a "
                                                          "test to retry)"
                    : hit_test == TestState::Active     ? "active"
                    : !LiveRR().Running()               ? "waiting for RR"
                                                        : "waiting",
                    no_fill ? "; fill off while the guide test replaces normals" : "");
        ImGui::TextWrapped("Session-only tests. The game gives RR 0 at sky, where a reflection "
                           "ray missed, and wherever no reflection ray was traced this frame "
                           "(about 60%% of pixels). Sky and misses: RR gets 65504 there, as "
                           "NVIDIA asks for sky. Fill: pixels with no ray take nearby hits on "
                           "the same surface. The weapon and surfaces the game never traces "
                           "reflections for stay as they are. Judge while strafing or walking "
                           "(turning alone cannot show it), with frame generation off; "
                           "ReShade.log shows the shares every 10 s.");
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Weapon depth", &requested_weapon_depth);
    {
        ImGui::Indent();
        using WeaponState = rrlive::Controller::WeaponState;
        const auto weapon = LiveRR().WeaponDepthState();
        ImGui::Text("Weapon depth: %s",
                    weapon == WeaponState::Active       ? "active (RR uses linear depth)"
                    : weapon == WeaponState::Off        ? "off"
                    : weapon == WeaponState::Restarting ? "restarting RR with the new depth type"
                    : weapon == WeaponState::NoMemory
                        ? "off: not enough GPU memory (turn it off and on to retry)"
                    : weapon == WeaponState::Unsupported
                        ? "off: the game's projection is not supported (turn it off and on to "
                          "retry)"
                    : weapon == WeaponState::Refused
                        ? "off: RR did not accept linear depth (turn it off and on to retry)"
                    : !LiveRR().Running() ? "waiting for RR"
                                          : "waiting");
        ImGui::TextWrapped("The game draws the weapon and hands with their own camera, so with "
                           "the game's depth RR sees them as one flat card. On, RR gets linear "
                           "depth in which they keep their own depth steps, still in front of "
                           "everything else; the rest of the scene keeps its distances, with the "
                           "sky very far instead of infinitely far. Changing it restarts RR, like "
                           "a preset change.");
        ImGui::Unindent();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Subsurface Scattering noise experiments");
    changed |= ImGui::Checkbox("Tolerate isolated bad guide pixels", &requested_tolerate);
    ImGui::TextWrapped("On: a few unusable guide pixels no longer reject the frame and reset RR "
                       "history; they are counted in ReShade.log instead. Off: previous behavior.");
    // Session-only: not saved, so a test mode never sticks across restarts.
    int guide_test = static_cast<int>(rrguidetest::Valid(rrguidetest::mode));
    using TestState = rrlive::Controller::TestState;
    const bool rr_running = LiveRR().Running();
    if (ImGui::Combo("Guide test (thin geometry)", &guide_test, rrguidetest::labels,
                     rrguidetest::Count)) {
        rrguidetest::mode = rrguidetest::Valid(static_cast<unsigned>(guide_test));
        ++rrguidetest::changes;
    }
    if (rrguidetest::Valid(static_cast<unsigned>(guide_test)) == rrguidetest::SkySurface) {
        int sky_distance = static_cast<int>(rrguidetest::ValidFloor(rrguidetest::floor));
        if (ImGui::Combo("Sky distance", &sky_distance, rrguidetest::floor_labels,
                         rrguidetest::FloorCount)) {
            rrguidetest::floor = rrguidetest::ValidFloor(static_cast<unsigned>(sky_distance));
            ++rrguidetest::changes;
        }
        ImGui::TextWrapped("How far away RR sees the sky, in game units. Hills sit a few times "
                           "farther than the trees in front of them; normally the sky is "
                           "infinitely far. Scenery past the chosen distance ends up behind "
                           "the sky for RR, so judge trees close by.");
        const auto floor = LiveRR().DepthFloorTest();
        ImGui::Text("Sky depth floor: %s",
                    floor == TestState::Active    ? "active"
                    : floor == TestState::Blocked ? "off: not enough GPU memory (switch to "
                                                    "another mode and back to retry)"
                    : !rr_running                 ? "waiting for RR"
                                                  : "waiting");
    }
    bool sky_depth = rrskydepth::enabled;
    if (ImGui::Checkbox("Sky depth test: finite far plane", &sky_depth))
        rrskydepth::enabled = sky_depth;
    bool sky_edge = rrskyedge::enabled;
    if (ImGui::Checkbox("Steadier sky edges (responsivity mask)", &sky_edge)) {
        rrskyedge::enabled = sky_edge;
        ++rrskyedge::changes;
    }
    float edge_strength = rrskyedge::strength;
    if (ImGui::SliderFloat("Sky edge strength", &edge_strength, -1.0f, 0.0f, "%.2f")) {
        rrskyedge::strength = rrresponsivity::Clamp(edge_strength);
        ++rrskyedge::changes;
    }
    int edge_radius = static_cast<int>(rrskyedge::ClampRadius(rrskyedge::radius));
    if (ImGui::SliderInt("Sky edge width (pixels)", &edge_radius, 1,
                         static_cast<int>(rrskyedge::MaxRadius), "%d",
                         ImGuiSliderFlags_AlwaysClamp)) {
        rrskyedge::radius =
            edge_radius < 1 ? 1u : rrskyedge::ClampRadius(static_cast<unsigned>(edge_radius));
        ++rrskyedge::changes;
    }
    const auto edge = LiveRR().SkyEdgeTest();
    ImGui::Text("Sky edge mask: %s",
                edge == TestState::Active    ? "active"
                : !sky_edge                  ? "off"
                : edge == TestState::Blocked ? "off: not enough GPU memory (change a sky edge "
                                               "setting to retry)"
                : rrresponsivity::Clamp(edge_strength) == 0 ? "off (strength 0)"
                : !rr_running                               ? "waiting for RR"
                                                            : "waiting (needs preset F)");
    ImGui::TextWrapped("Session-only test. Tells RR to hold pixels where geometry meets sky "
                       "steadier over time; moving branches may ghost. While on, it replaces the "
                       "SSS and water responsivity masks.");
    int color_scale = static_cast<int>(rrcolorscale::Valid(rrcolorscale::mode));
    if (ImGui::Combo("Color brightness test", &color_scale, rrcolorscale::labels,
                     rrcolorscale::Count)) {
        rrcolorscale::mode = rrcolorscale::Valid(static_cast<unsigned>(color_scale));
        ++rrcolorscale::changes;
    }
    const auto brightness = LiveRR().ColorTest();
    ImGui::Text("Color brightness test: %s",
                brightness == TestState::Active    ? "active"
                : color_scale == 0                 ? "off"
                : brightness == TestState::Blocked ? "off: not enough GPU memory (change the "
                                                     "setting to retry)"
                : !rr_running                      ? "waiting for RR"
                                                   : "waiting");
    ImGui::TextWrapped("Session-only test. Sends RR the color darker or brighter and scales its "
                       "output back, so the picture looks the same unless RR treats the levels "
                       "differently. The SSS and transparency guides are left out while on.");
    int contrast = static_cast<int>(rrcontrast::Valid(rrcontrast::mode));
    if (ImGui::Combo("Color contrast test", &contrast, rrcontrast::labels, rrcontrast::Count)) {
        rrcontrast::mode = rrcontrast::Valid(static_cast<unsigned>(contrast));
        ++rrcontrast::changes;
    }
    const auto squeeze = LiveRR().ContrastTest();
    ImGui::Text("Color contrast test: %s",
                squeeze == TestState::Active    ? "active"
                : contrast == 0                 ? "off"
                : squeeze == TestState::Blocked ? "off: not enough GPU memory (change the "
                                                  "setting to retry)"
                : !rr_running                   ? "waiting for RR"
                                                : "waiting");
    ImGui::TextWrapped("Session-only test. Squeezes bright colors before RR and stretches RR's "
                       "output back, so the sky behind branches is only a few times brighter "
                       "than the needles for RR instead of about 28x. Shows whether the tree "
                       "flicker follows that contrast. Stronger settings can darken noisy, "
                       "bright spots a little. The SSS and transparency guides are left out "
                       "while on.");
    if (ImGui::Button("Capture RR inputs (Pause)"))
        LiveRR().RequestCapture();
    const auto capture = LiveRR().CaptureStatus();
    switch (capture.state) {
    case rrcapture::State::Armed:
        ImGui::TextUnformatted("Capture: waiting for RR");
        break;
    case rrcapture::State::Collecting:
        ImGui::Text("Capture: collecting %u/8 frames", capture.collected);
        break;
    case rrcapture::State::Writing:
        ImGui::TextUnformatted("Capture: writing files...");
        break;
    case rrcapture::State::Saved:
        ImGui::TextWrapped("Capture saved to %s", capture.message.c_str());
        break;
    case rrcapture::State::Failed:
        ImGui::TextWrapped("Capture failed: %s", capture.message.c_str());
        break;
    case rrcapture::State::Idle:
        break;
    }
    ImGui::TextWrapped("Saves 8 consecutive frames of everything RR is given, plus the Status "
                       "guide and RR's output, to mw4-rr-captures in the game folder. About 50 "
                       "bytes per render pixel plus 8 per output pixel, per frame. Session only.");
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
                       "(F11) and show raw lighting without it. The (test) options keep fill gaps' fresh "
                       "samples unboosted; history + spatial uses the game's accumulation only where "
                       "it has lobe-2 history and otherwise averages this frame's rays within 2 "
                       "pixels; spatial fill only never uses the game's accumulation.");
    if (rrlighting::SssMasked(rrlighting::Valid(requested_lobe)))
        ImGui::Text("SSS pixel mask: %s", rrsssmask::Label(SssMaskState()));
    changed |= ImGui::Checkbox("Responsivity mask on SSS pixels", &requested_responsivity);
    if (ImGui::SliderFloat("Mask strength", &requested_strength, -1.0f, 0.0f, "%.2f"))
        LiveRR().SetResponsivity(requested_responsivity, requested_strength);
    if (ImGui::IsItemDeactivatedAfterEdit())
        changed = true;
    ImGui::Text("Responsivity mask: %s", LiveRR().ResponsivityActive()      ? "active"
                                         : !requested_responsivity            ? "off"
                                         : sky_edge && edge == TestState::Active
                                             ? "replaced by sky edge mask"
                                         : rrresponsivity::Clamp(requested_strength) == 0
                                             ? "off (strength 0)"
                                             : "waiting (needs preset F and the SSS guide)");
    ImGui::TextWrapped("Tells RR to hold SSS pixels steadier over time: less boiling, but moving "
                       "SSS surfaces may ghost. Supported on RR preset F only.");
    ImGui::Separator();
    ImGui::TextUnformatted("Running water experiments (this session only)");
    // Session-only: not saved, so a test mode never sticks across restarts.
    static rrwater::Settings water;
    bool water_changed = ImGui::Checkbox("Water: responsivity mask", &water.responsivity);
    water_changed |=
        ImGui::SliderFloat("Water mask strength", &water.strength, 0.0f, 1.0f, "%.2f");
    water_changed |= ImGui::Checkbox("Water: discard RR history", &water.discard);
    water_changed |= ImGui::SliderFloat("Water discard strength", &water.discard_strength, 0.0f,
                                        1.0f, "%.2f");
    water_changed |= ImGui::Checkbox("Water: animated texture mask", &water.animated);
    int water_guide = static_cast<int>(rrwater::ValidGuide(water.guide));
    if (ImGui::Combo("Water guides", &water_guide, rrwater::guide_labels, rrwater::GuideCount)) {
        water.guide = static_cast<unsigned>(water_guide);
        water_changed = true;
    }
    if (water_changed)
        LiveRR().SetWater(water);
    const auto water_status = LiveRR().WaterStatus();
    if (!water_status.wanted)
        ImGui::TextUnformatted("Water tests: off");
    else if (water_status.unavailable)
        ImGui::TextUnformatted("Water tests: unavailable this session (see ReShade.log)");
    else if (water_status.waiting_memory)
        ImGui::TextUnformatted("Water tests: waiting for GPU memory (retrying)");
    else if (!water_status.ready)
        ImGui::TextUnformatted("Water tests: waiting for RR frames");
    else {
        if (water_status.coverage >= 0)
            ImGui::Text("Water in the latest frame: %.1f%% of pixels",
                        100.0f * water_status.coverage);
        else
            ImGui::TextUnformatted("Water in the latest frame: measuring");
        // What the game's prepass marks, to tell whether a surface is flagged at all.
        const auto &flags = water_status.prepass;
        if (flags[rrwater::Special] >= 0)
            ImGui::Text("Game flags: special %.1f%% (water flag %.1f%%, override %.1f%%), "
                        "marker %.1f%%, rejected %.1f%%",
                        100.0f * flags[rrwater::Special], 100.0f * flags[rrwater::Marked],
                        100.0f * flags[rrwater::Override], 100.0f * flags[rrwater::Bit31],
                        100.0f * flags[rrwater::SpecialRejected]);
        if (water.responsivity)
            ImGui::Text("Water responsivity mask: %s",
                        water_status.responsivity               ? "active"
                        : sky_edge && edge == TestState::Active ? "replaced by sky edge mask"
                        : rrwater::Clamp(water.strength) == 0   ? "off (strength 0)"
                                                                : "waiting (needs preset F)");
        if (water.discard)
            ImGui::Text("Water history discard: %s",
                        water_status.discard                                 ? "active"
                        : rrwater::DiscardLevel(water.discard_strength) == 0 ? "off (strength 0)"
                                                                             : "waiting");
        if (water.animated)
            ImGui::Text("Water animated texture mask: %s",
                        water_status.animated ? "active" : "waiting");
    }
    ImGui::TextWrapped(
        "Water pixels are the game's special surfaces: mirror-like water that its prepass marks "
        "with its water flag (lakes, distant water) or its override (rivers). Responsivity tells "
        "RR to lean on the current frame there (preset F only); with the SSS responsivity mask "
        "also on, SSS pixels keep their own value. Discard RR history marks water as disoccluded, "
        "as RTX Remix does for animated water, so RR rebuilds it every frame: expect sharper but "
        "noisier water. Discard strength keeps part of the history instead: 1 drops it all, "
        "0.5 sends RR 100 instead of 10000 (it reads the mask in log scale), so try 0.5 to 0.8 "
        "for less blur with calmer reflections. The animated texture mask is an RR input NVIDIA "
        "does not document; it "
        "is here to see whether it changes anything. Water guides changes the normal and albedo "
        "guides on water only: the game mirror is what RR gets today, smoothed normal averages "
        "the wave normal over 7x7 pixels, water material uses the game's own reflectance and "
        "diffuse arrays (not yet checked on rivers), and physical uses water's 0.02 reflectance "
        "with a smoothed normal. "
        "Water coverage and these settings are logged to ReShade.log every 10 seconds. Game "
        "flags shows how much of the frame the game's prepass marks special, split into its "
        "water flag and its override, plus the water lighting marker and special pixels the "
        "add-on could not convert: if special stays near 0%% while you look at water, the "
        "game does not mark that water.");
    ImGui::Separator();
    // Session-only: not saved, so a test mode never sticks across restarts.
    ImGui::TextUnformatted("Ray selection test (which pixels get the game's rays)");
    int ray_noise = static_cast<int>(rrapply::NoiseOf(rrapply::noise.load()));
    if (ImGui::Combo("Ray draws per lobe", &ray_noise, rrapply::noise_labels, rrapply::NoiseCount))
        rrapply::noise = rrapply::NoiseOf(static_cast<unsigned>(ray_noise));
    bool ray_untile = rrapply::untile;
    if (ImGui::Checkbox("Break the 128-pixel noise repeat", &ray_untile))
        rrapply::untile = ray_untile;
    const unsigned ray_variant = rrapply::Selected();
    const char *ray_reason = rrapply::variant_status[ray_variant].load();
    switch (rrapply::Current(ray_variant)) {
    case rrapply::State::Off:
        ImGui::TextUnformatted("Ray selection: off (game's own)");
        break;
    case rrapply::State::Active:
        ImGui::Text("Ray selection: active (%s)", rrapply::Name(ray_variant));
        break;
    case rrapply::State::Unavailable:
        ImGui::Text("Ray selection: unavailable (%s); game's own in use",
                    ray_reason ? ray_reason : "not prepared");
        break;
    case rrapply::State::Waiting:
        ImGui::TextUnformatted("Ray selection: waiting for RR");
        break;
    }
    ImGui::TextWrapped("Session-only test. The game uses one noise value per pixel for both ray "
                       "lobes, so lobe 2's rays land on the pixels lobe 1's rays hit. Separate "
                       "draws gives lobe 2 its own noise value. Opposite draws makes lobe 2's "
                       "rays avoid lobe 1's, so fewer pixels get no fresh ray at all. Either way "
                       "each lobe keeps the game's ray rate. The repeat break shifts the game's "
                       "128-pixel noise tile differently in each 128-pixel block of the screen, "
                       "which can leave faint fixed lines along the block edges. The first switch "
                       "to an option pauses the game briefly while its shader is built. Judge "
                       "with frame generation off.");
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
    // "RR still runs" only while the gate allows RR, never under "RR configuration unavailable".
    if (config_warnings)
        ImGui::TextWrapped("Game config: %u %s from INSTALLATION.txt%s. See ReShade.log.",
                           config_warnings,
                           config_warnings == 1 ? "setting differs" : "settings differ",
                           config_allowed ? " (RR still runs)" : "");
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
