#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_game_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_ray_density.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_sss_mask.h"
#include "../src/rr_responsivity.h"
#include "../src/rr_sky_edge.h"
#include "../src/rr_color_scale.h"
#include "../src/rr_specular_distance.h"
#include "../src/rr_trace_model.h"
#include "../src/rr_hit_condition.h"
#include "../src/rr_water.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr rrsettings::Preferences defaults;
static_assert(defaults.rr && defaults.sss && defaults.glass && defaults.hit &&
              defaults.preset == 6);
static_assert(defaults.tolerate_isolated && defaults.native_lobe == rrlighting::Raw);
static_assert(!defaults.responsivity &&
              defaults.responsivity_strength == rrresponsivity::DefaultStrength);
// Water tests start off; unknown guide modes read as the game mirror.
static_assert(!rrwater::Wanted(rrwater::Settings{}));
static_assert(rrwater::ValidGuide(rrwater::Physical) == rrwater::Physical);
static_assert(rrwater::ValidGuide(rrwater::GuideCount) == rrwater::Mirror);
static_assert(std::string_view(rrwater::guide_labels[rrwater::Mirror]) == "Game mirror (today)");
static_assert(rrwater::Flags(true, false, false) == 1 && rrwater::Flags(false, true, false) == 2 &&
              rrwater::Flags(false, false, true) == 4);
// Stored values 0..2 keep their meaning; unknown values read as raw lighting.
static_assert(rrlighting::NativeLobe1 == 1 && rrlighting::NativeLobe2 == 2);
static_assert(rrlighting::Valid(rrlighting::FillBoth) == rrlighting::FillBoth);
static_assert(rrlighting::Valid(rrlighting::Count) == rrlighting::Raw);
static_assert(std::string_view(rrlighting::labels[rrlighting::FillLobe2]) == "Lobe 2 fill gaps");
static_assert(rrlighting::CapGentle == 5 && rrlighting::CapStrong == 6);
static_assert(rrlighting::Valid(rrlighting::CapStrong) == rrlighting::CapStrong);
static_assert(std::string_view(rrlighting::labels[rrlighting::CapStrong]) ==
              "Raw, cap bright samples (4x)");
static_assert(rrlighting::WFill == 7 && rrlighting::WNative == 8 &&
              rrlighting::Valid(rrlighting::WNative) == rrlighting::WNative);
static_assert(std::string_view(rrlighting::labels[rrlighting::WNative]) ==
              "Raw, AO fix B (native)");
// SSS-masked sources are appended; only they read the SSS guide.
static_assert(rrlighting::SssNative2 == 9 && rrlighting::SssFill2 == 10 &&
              rrlighting::SssNative3 == 11 && rrlighting::SssFill3 == 12 &&
              rrlighting::Count == 13);
static_assert(rrlighting::Valid(rrlighting::SssFill3) == rrlighting::SssFill3);
static_assert(std::string_view(rrlighting::labels[rrlighting::SssNative2]) ==
              "SSS pixels: lobe 2 native temporal");
static_assert(std::string_view(rrlighting::labels[rrlighting::SssFill3]) ==
              "SSS pixels: both lobes fill gaps");
static_assert(!rrlighting::SssMasked(rrlighting::Raw) &&
              !rrlighting::SssMasked(rrlighting::WNative) &&
              rrlighting::SssMasked(rrlighting::SssNative2) &&
              rrlighting::SssMasked(rrlighting::SssFill3) &&
              !rrlighting::SssMasked(rrlighting::Count));
// A guide serves the next few frames of its generation and extent, across wrap.
constexpr rrsssmask::Record guide{true, 100, 7, 1280, 720};
static_assert(rrsssmask::Usable(guide, 101, 7, 1280, 720) &&
              rrsssmask::Usable(guide, 100 + rrsssmask::MaxAge, 7, 1280, 720));
static_assert(!rrsssmask::Usable(guide, 100, 7, 1280, 720) &&
              !rrsssmask::Usable(guide, 101 + rrsssmask::MaxAge, 7, 1280, 720) &&
              !rrsssmask::Usable(guide, 99, 7, 1280, 720));
static_assert(!rrsssmask::Usable(guide, 101, 8, 1280, 720) &&
              !rrsssmask::Usable(guide, 101, 7, 1920, 720) &&
              !rrsssmask::Usable(guide, 101, 7, 1280, 1080));
static_assert(!rrsssmask::Usable({}, 1, 0, 0, 0));
static_assert(rrsssmask::Usable({true, 0xffffffffu, 1, 8, 8}, 1, 1, 8, 8));
static_assert(std::string_view(rrsettings::QualityLabel(0)) == "Performance");
static_assert(std::string_view(rrsettings::QualityLabel(5)) == "Native / DLAA");
static_assert(std::string_view(rrsettings::QualityLabel(-1)) == "Not observed yet");
static_assert(rrsettings::HotkeyToggles(true, false, false, true));
static_assert(!rrsettings::HotkeyToggles(true, true, false, true));
static_assert(!rrsettings::HotkeyToggles(true, false, true, true));
static_assert(!rrsettings::HotkeyToggles(true, false, false, false));
static_assert(!rrsettings::HotkeyToggles(false, false, false, true));

// Settings file lines as the game writes them, CRLF included.
static std::string ConfigLine(const std::string &key, const std::string &value) {
    return key + ";57752;20945 = " + value + " // one of a, b\r\n";
}
static const std::pair<const char *, const char *> compliant[] = {
    {"MotionBlur@0", "Off"},
    {"WeaponMotionBlur@0", "false"},
    {"EnableVelocityBasedBlur@0", "false"},
    {"DofEnable@0", "Off"},
    {"DofWeaponDisable@0", "true"},
    {"DofWorldDisable@0", "true"},
    {"AATechniquePreferredMP@0", "DLSS"},
    {"DLSSModeMP@0", "DLSS"},
    {"DLSSModelUI@0", "TRANSFORMER"},
    {"DxrDenoiser@0", "DLSS Ray Reconstruction"},
    {"DynamicSceneResolution@0", "false"},
    {"DxrMode@0", "On"},
    {"DxrMode@1", "High"},
    {"VRS@0", "true"},
    {"DLSSSharpnessMP@0", "0"},
    {"DLSSFrameGeneration@0", "false"},
    {"DLSSFrameGenerationmultiType@0", "Dynamic Auto"},
    {"DLSSPerfModeMP@0", "Maximum Quality"},
    {"MaxFpsInGame@0", "250"},
    {"MaxFpsOutOfFocus@0", "30"},
    {"SustainabilityReduceQualityIdle@0", "full"},
    {"SustainabilityReduceQualityIdle@1", "30 seconds"},
};
// A compliant file with one key's value replaced, or its entry dropped when value is null.
static std::string Config(const std::string &key = {}, const char *value = nullptr) {
    std::string text = "// Settings\r\n\r\n";
    for (const auto &[name, original] : compliant)
        if (key != name)
            text += ConfigLine(name, original);
        else if (value)
            text += ConfigLine(name, value);
    return text;
}
static std::vector<std::string> Lines(const std::string &text, unsigned &warnings) {
    std::vector<std::string> lines;
    warnings = rrgameconfig::Check(text, lines);
    return lines;
}
static bool Has(const std::vector<std::string> &lines, const std::string &line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}
static void Swap(std::string &text, const std::string &from, const std::string &to) {
    const auto at = text.find(from);
    assert(at != std::string::npos);
    text.replace(at, from.size(), to);
}
static std::string Lower(std::string text) {
    for (auto &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return text;
}
static void GameConfigTests() {
    using namespace rrgameconfig;
    // Entries: name before '@', index up to ';' or '=', value up to '//'; all trimmed.
    const auto entries = Parse("// header\r\n\r\nDxrMode@0;57752;20945 = On // allowed: On, Off\r\n"
                               "DxrMode@1;57752;20946 = High\r\n"
                               "  DxrDenoiser@0 = DLSS Ray Reconstruction  \r\n"
                               "DLSSPerfModeMP@0=Maximum Quality//x\r\nbare line\r\n"
                               "// Hidden@0 = true\r\nLate // note = true\r\nNoSuffix = 5\r\n"
                               "Tabs@2\t=\tz\r\nEmpty@0 =\r\nDxrMode@0;1;2 = Off");
    const char *expected[][3] = {{"DxrMode", "0", "On"},
                                 {"DxrMode", "1", "High"},
                                 {"DxrDenoiser", "0", "DLSS Ray Reconstruction"},
                                 {"DLSSPerfModeMP", "0", "Maximum Quality"},
                                 {"NoSuffix", "", "5"},
                                 {"Tabs", "2", "z"},
                                 {"Empty", "0", ""},
                                 {"DxrMode", "0", "Off"}};
    assert(entries.size() == std::size(expected));
    for (size_t i = 0; i < entries.size(); ++i)
        assert(entries[i].name == expected[i][0] && entries[i].index == expected[i][1] &&
               entries[i].value == expected[i][2]);
    assert(Parse("").empty() && Parse("\r\n\r\n// only\r\n").empty());
    // Find takes the first entry of the name, or of the name and index.
    assert(Find(entries, "dxrmode")->value == "On");
    assert(Find(entries, "DxrMode", "1")->value == "High");
    assert(!Find(entries, "DxrMode", "2") && !Find(entries, "Missing"));

    // A compliant file warns about nothing and logs every value, in this order.
    unsigned warnings{};
    auto lines = Lines(Config(), warnings);
    assert(warnings == 0 && lines.size() == 1);
    assert(lines[0] ==
           "Game config values: EnableVelocityBasedBlur=false, DLSSModelUI=TRANSFORMER, "
           "DxrDenoiser=DLSS Ray Reconstruction, DLSSSharpnessMP=0, DLSSFrameGeneration=false, "
           "DLSSFrameGenerationmultiType=Dynamic Auto, DLSSPerfModeMP=Maximum Quality, "
           "DxrMode@0=On, DxrMode@1=High, VRS=true, MaxFpsInGame=250, MaxFpsOutOfFocus=30, "
           "SustainabilityReduceQualityIdle@0=full, SustainabilityReduceQualityIdle@1=30 seconds");
    // Every entry of a shown name is logged, by index when there are several: a second VRS shows.
    lines = Lines(Config() + ConfigLine("VRS@0", "false"), warnings);
    assert(warnings == 0 && lines[0].find(", VRS@0=true, VRS@0=false, ") != std::string::npos);

    // Each required key: a wrong value, a different case and a missing entry.
    const char *required[][3] = {{"MotionBlur", "Off", "Cinematic Only"},
                                 {"WeaponMotionBlur", "false", "true"},
                                 {"EnableVelocityBasedBlur", "false", "true"},
                                 {"DofEnable", "Off", "Script"},
                                 {"DofWeaponDisable", "true", "false"},
                                 {"DofWorldDisable", "true", "false"},
                                 {"AATechniquePreferredMP", "DLSS", "SMAA"},
                                 {"DLSSModeMP", "DLSS", "Off"},
                                 {"DLSSModelUI", "TRANSFORMER", "CNN"},
                                 {"DxrDenoiser", "DLSS Ray Reconstruction", "Default"},
                                 {"DynamicSceneResolution", "false", "true"}};
    for (const auto &[name, wanted, wrong] : required) {
        const std::string key = std::string(name) + "@0";
        lines = Lines(Config(key, wrong), warnings);
        assert(warnings == 1 && lines.size() == 2);
        assert(lines[1] == std::string("Game config warning: ") + name + "=" + wrong +
                               "; INSTALLATION.txt requires " + wanted);
        for (const auto &spelling : {Lower(wanted), std::string(wanted) + "x"}) {
            lines = Lines(Config(key, spelling.c_str()), warnings);
            assert(warnings == (spelling == Lower(wanted) ? 0u : 1u));
        }
        lines = Lines(Config(key, nullptr), warnings);
        assert(warnings == 1 && lines.size() == 2);
        assert(lines[1] == std::string("Game config warning: ") + name +
                               " not found; INSTALLATION.txt requires " + wanted);
        // The gate looks only at read-only and VRS: a broken rule never turns RR off.
        assert(rrvrsconfig::Allowed(Config(key, wrong), true, true));
    }
    // Every entry of a ruled name must comply, whatever its index; when the name has several,
    // each warning names its entry.
    lines = Lines(Config() + ConfigLine("MotionBlur@1", "On"), warnings);
    assert(warnings == 1 && Has(lines, "Game config warning: MotionBlur@1=On; INSTALLATION.txt "
                                       "requires Off"));
    lines = Lines(Config("MotionBlur@0", "On") + ConfigLine("MotionBlur@1", "Script"), warnings);
    assert(warnings == 2 && lines.size() == 3);
    assert(lines[1] == "Game config warning: MotionBlur@0=On; INSTALLATION.txt requires Off");
    assert(lines[2] == "Game config warning: MotionBlur@1=Script; INSTALLATION.txt requires Off");
    lines = Lines(Config() + ConfigLine("MotionBlur@1", "off"), warnings);
    assert(warnings == 0);
    // Names and values match in any case: a lower-case key line, an upper-case value.
    lines = Lines(Config("MotionBlur@0", nullptr) + ConfigLine("motionblur@0", "On"), warnings);
    assert(warnings == 1 && lines.size() == 2 &&
           lines[1] == "Game config warning: MotionBlur=On; INSTALLATION.txt requires Off");
    lines = Lines(Config("MotionBlur@0", nullptr) + ConfigLine("motionblur@0", "off"), warnings);
    assert(warnings == 0);
    lines = Lines(Config("WeaponMotionBlur@0", "FALSE"), warnings);
    assert(warnings == 0);

    // DxrMode@0 must be On; @1 picks a quality and must not be On.
    lines = Lines(Config("DxrMode@0", "Off"), warnings);
    assert(warnings == 1 && lines[1] == "Game config warning: DxrMode@0=Off; INSTALLATION.txt "
                                        "requires On");
    lines = Lines(Config("DxrMode@0", nullptr), warnings);
    assert(warnings == 1 && lines[1] == "Game config warning: DxrMode@0 not found; "
                                        "INSTALLATION.txt requires On");
    for (const char *on : {"On", "on"}) {
        lines = Lines(Config("DxrMode@1", on), warnings);
        assert(warnings == 1 && lines[1] == std::string("Game config warning: DxrMode@1=") + on +
                                                "; INSTALLATION.txt requires a value other than "
                                                "On");
    }
    for (const char *quality : {"Off", "Low", "Ultra"}) {
        lines = Lines(Config("DxrMode@1", quality), warnings);
        assert(warnings == 0);
    }
    lines = Lines(Config("DxrMode@1", nullptr), warnings); // nothing set to On: no warning
    assert(warnings == 0 && lines[0].find("DxrMode@1=<missing>") != std::string::npos);

    // An empty or unrelated file warns for every required key and logs <missing>.
    for (const char *text : {"", "\r\n// nothing\r\nVRS@0 = true\r\n"}) {
        lines = Lines(text, warnings);
        assert(warnings == 12 && lines.size() == 13);
    }
    lines = Lines("", warnings);
    assert(lines[0] ==
           "Game config values: EnableVelocityBasedBlur=<missing>, DLSSModelUI=<missing>, "
           "DxrDenoiser=<missing>, DLSSSharpnessMP=<missing>, DLSSFrameGeneration=<missing>, "
           "DLSSFrameGenerationmultiType=<missing>, DLSSPerfModeMP=<missing>, "
           "DxrMode@0=<missing>, DxrMode@1=<missing>, VRS=<missing>, MaxFpsInGame=<missing>, "
           "MaxFpsOutOfFocus=<missing>, SustainabilityReduceQualityIdle=<missing>");
    assert(lines[1] == "Game config warning: MotionBlur not found; INSTALLATION.txt requires Off");

    // Notes are advice, not rules: logged after the warnings and never counted.
    const char *frame_generation_note =
        "Game config note: frame generation is on; RR health frames= counts rendered frames only, "
        "judge RR with it off";
    for (const char *on : {"true", "True"}) {
        lines = Lines(Config("DLSSFrameGeneration@0", on), warnings);
        assert(warnings == 0 && lines.size() == 2 && lines[1] == frame_generation_note);
    }
    for (const char *off : {"false", ""}) {
        lines = Lines(Config("DLSSFrameGeneration@0", off), warnings);
        assert(lines.size() == 1);
    }
    lines = Lines(Config("DLSSFrameGeneration@0", nullptr), warnings);
    assert(warnings == 0 && lines.size() == 1);
    lines = Lines(Config("DLSSSharpnessMP@0", "0.5"), warnings);
    assert(warnings == 0 && lines.size() == 2 &&
           lines[1] == "Game config note: DLSSSharpnessMP=0.5 may drive a game sharpening pass "
                       "after RR");
    for (const char *zero : {"0", "0.0", "0.00", "-0.0", "0e5", "off", ""}) {
        lines = Lines(Config("DLSSSharpnessMP@0", zero), warnings);
        assert(lines.size() == 1);
    }
    for (const char *some : {"1", "0.01", "1e-3", "-0.25"}) {
        lines = Lines(Config("DLSSSharpnessMP@0", some), warnings);
        assert(lines.size() == 2 && Has(lines, std::string("Game config note: DLSSSharpnessMP=") +
                                                   some + " may drive a game sharpening pass "
                                                          "after RR"));
    }
    lines = Lines(Config("DLSSSharpnessMP@0", nullptr), warnings);
    assert(lines.size() == 1);
    // Only the first entry of a name is judged for the notes; both are logged.
    lines = Lines(Config() + ConfigLine("DLSSFrameGeneration@0", "true"), warnings);
    assert(lines.size() == 1 &&
           lines[0].find(", DLSSFrameGeneration@0=false, DLSSFrameGeneration@0=true, ") !=
               std::string::npos);
    // Values, then warnings, then both notes in this order; only the warning counts.
    auto mixed = Config("MotionBlur@0", "On");
    Swap(mixed, "DLSSFrameGeneration@0;57752;20945 = false", "DLSSFrameGeneration@0 = true");
    Swap(mixed, "DLSSSharpnessMP@0;57752;20945 = 0", "DLSSSharpnessMP@0 = 0.5");
    lines = Lines(mixed, warnings);
    assert(warnings == 1 && lines.size() == 4);
    assert(lines[0].rfind("Game config values: ", 0) == 0 &&
           lines[0].find("DLSSFrameGeneration=true") != std::string::npos);
    assert(lines[1] == "Game config warning: MotionBlur=On; INSTALLATION.txt requires Off");
    assert(lines[2] == frame_generation_note);
    assert(lines[3].rfind("Game config note: DLSSSharpnessMP=0.5 ", 0) == 0);
    // The state line comes first and names the gate's own answer.
    const auto text = Config("MotionBlur@0", "On");
    File file;
    file.state = FileState::Read;
    file.readonly = true;
    file.text = text;
    lines.clear();
    assert(Report(file, rrvrsconfig::Allowed(file.text, file.readonly, true), lines) == 1);
    assert(lines.size() == 3 && lines[0] == "Game config: read-only=yes, VRS gate allows RR=yes" &&
           lines[1].rfind("Game config values: ", 0) == 0 &&
           lines[2] == "Game config warning: MotionBlur=On; INSTALLATION.txt requires Off");
    file.readonly = false;
    lines.clear();
    assert(Report(file, rrvrsconfig::Allowed(file.text, file.readonly, true), lines) == 1);
    assert(lines[0] == "Game config: read-only=no, VRS gate allows RR=no" && lines.size() == 3);
    file.state = FileState::NotFound;
    lines.clear();
    assert(Report(file, false, lines) == 0 &&
           lines == std::vector<std::string>{"Game config: file not found"});
    file.state = FileState::Unreadable;
    lines.clear();
    assert(Report(file, false, lines) == 0 &&
           lines == std::vector<std::string>{"Game config: file unreadable"});
    assert(File{}.state == FileState::NotFound);
}

int main() {
    GameConfigTests();
    // Sky edge mask: preset F only, strength 0 or off sends nothing.
    assert(rrskyedge::Use(true, -0.5f, true, 6));
    assert(!rrskyedge::Use(false, -0.5f, true, 6));
    assert(!rrskyedge::Use(true, 0.0f, true, 6));
    assert(!rrskyedge::Use(true, -0.5f, false, 6));
    assert(!rrskyedge::Use(true, -0.5f, true, 5));
    assert(rrskyedge::ClampRadius(0) == 1 && rrskyedge::ClampRadius(9) == rrskyedge::MaxRadius &&
           rrskyedge::ClampRadius(2) == 2);
    // Color brightness test: off is exactly 1, every factor is a power of two
    // (exact in R11G11B10 and FP16), and an out-of-range mode falls back to off.
    static_assert(rrcolorscale::Factor(rrcolorscale::Off) == 1.0f);
    static_assert(rrcolorscale::Factor(rrcolorscale::Count) == 1.0f);
    static_assert(rrcolorscale::Factor(rrcolorscale::Sixteenth) == 0.0625f);
    for (unsigned m = 1; m < rrcolorscale::Count; ++m) {
        int e{};
        assert(std::frexp(rrcolorscale::Factor(m), &e) == 0.5f && rrcolorscale::Factor(m) != 1.0f);
    }
    assert(rrvrsconfig::Allowed("VRS@0;123 = true // preserved\n", true, true));
    assert(rrvrsconfig::Allowed("VRS@0;123 = false\n", true, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = true\n", false, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = true\nVRS@1;123 = true\n", true, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = invalid\n", true, true));
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 4;
    for (auto size : {std::pair{640u, 368u}, {1280u, 720u}, {3440u, 1440u}, {5120u, 2880u}}) {
        d.Width = size.first;
        d.Height = size.second;
        assert(rrglass::Shape(d, 1));
        assert(!rrglass::Shape(d, 0));
    }
    // The SSS mask must be the add-on's R16 guide at the lighting extent.
    D3D12_RESOURCE_DESC lighting{};
    lighting.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    lighting.Width = 1280;
    lighting.Height = 720;
    lighting.DepthOrArraySize = lighting.MipLevels = 1;
    lighting.SampleDesc.Count = 1;
    lighting.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    auto mask = lighting;
    mask.Format = DXGI_FORMAT_R16_FLOAT;
    assert(rrsssmask::Shape(mask, lighting));
    assert(!rrsssmask::Shape(lighting, lighting));
    auto wrong = mask;
    wrong.Width = 640;
    assert(!rrsssmask::Shape(wrong, lighting));
    wrong = mask;
    wrong.Height = 360;
    assert(!rrsssmask::Shape(wrong, lighting));
    wrong = mask;
    wrong.SampleDesc.Count = 4;
    assert(!rrsssmask::Shape(wrong, lighting));
    wrong = mask;
    wrong.DepthOrArraySize = 4;
    assert(!rrsssmask::Shape(wrong, lighting));
    assert(std::string_view(rrsssmask::Label(rrsssmask::State::Active)) == "active");
    // Specular hit distance follows the runtime guide extent, not a fixed resolution.
    D3D12_RESOURCE_DESC h{};
    h.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    h.DepthOrArraySize = h.MipLevels = 1;
    h.SampleDesc.Count = 1;
    h.Format = DXGI_FORMAT_R16_FLOAT;
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (auto size : {std::pair{640u, 368u}, {1280u, 720u}, {1920u, 1088u}, {2560u, 1440u},
                      {3440u, 1440u}}) {
        h.Width = size.first;
        h.Height = size.second;
        assert(rrspecular::ValidSource(h, size.first, size.second, read));
        assert(rrspecular::ValidSource(h, size.first, size.second,
                                       read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        assert(!rrspecular::ValidSource(h, size.first + 2, size.second, read));
        assert(!rrspecular::ValidSource(h, size.first, size.second,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        assert(rrspecular::DispatchShape(size.first, size.second, (size.first + 15) / 16,
                                         (size.second + 15) / 16, 1));
        assert(!rrspecular::DispatchShape(size.first, size.second, (size.first + 7) / 8,
                                          (size.second + 15) / 16, 1));
    }
    h.Format = DXGI_FORMAT_R32_FLOAT;
    assert(!rrspecular::ValidSource(h, UINT(h.Width), h.Height, read));
    assert(rrspecular::DistanceBlur(0xc9ca6daa80593f3aull));
    assert(rrspecular::DistanceBlur(0x4894e5e7e717b468ull));
    assert(!rrspecular::DistanceBlur(0xd63e707761558424ull));
    rrspecular::Diagnostics stats;
    char line[320];
    assert(!stats.Summary(line, sizeof(line)));
    assert(std::string_view(line).find("blurDispatches=0") != std::string_view::npos);
    stats.Dispatch();
    stats.Observe(rrspecular::Stage::Shape);
    assert(stats.Summary(line, sizeof(line)));
    assert(std::string_view(line).find("lastReject=shape") != std::string_view::npos);
    assert(!stats.Summary(line, sizeof(line)));
    assert(stats.Scan(1) && !stats.Scan(1) && stats.Candidate(1) && !stats.Candidate(1));
    d.Width = 16385;
    assert(!rrglass::Shape(d, 1));
    d.Width = 1920;
    d.SampleDesc.Count = 1;
    assert(!rrglass::Shape(d, 1));

    // Reset causes are reported without changing reset decisions.
    rrlive::HistoryPolicy policy;
    assert(policy.Next(1, 0, false) && policy.LastReason() == (rrlive::HistoryPolicy::Rejected |
                                                                rrlive::HistoryPolicy::Gap));
    assert(!policy.Next(2, 0, false) && policy.LastReason() == 0);
    assert(policy.Next(3, 0, true) && policy.LastReason() == rrlive::HistoryPolicy::Cut);
    assert(policy.Next(4, 1, false) && policy.LastReason() == rrlive::HistoryPolicy::Generation);
    assert(policy.Next(6, 1, false) && policy.LastReason() == rrlive::HistoryPolicy::Gap);
    policy.Reject();
    assert(policy.Next(7, 1, false) && policy.LastReason() == rrlive::HistoryPolicy::Rejected);

    // The responsivity mask is sent only on preset F with a written SSS guide.
    assert(rrresponsivity::Use(true, -0.5f, true, true, 6));
    assert(!rrresponsivity::Use(false, -0.5f, true, true, 6));
    assert(!rrresponsivity::Use(true, 0.0f, true, true, 6));
    assert(!rrresponsivity::Use(true, 0.5f, true, true, 6)); // clamps to 0
    assert(!rrresponsivity::Use(true, -0.5f, false, true, 6));
    assert(!rrresponsivity::Use(true, -0.5f, true, false, 6));
    for (unsigned preset : {0u, 4u, 5u})
        assert(!rrresponsivity::Use(true, -0.5f, true, true, preset));
    assert(rrresponsivity::Clamp(-2.0f) == -1.0f && rrresponsivity::Clamp(-0.25f) == -0.25f);
    assert(rrresponsivity::Clamp(std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    // Health reports once per interval, only with activity, then starts over.
    // Presentation counters arrive lock-free; each interval takes its own window.
    auto &shown = rrhealth::presentation;
    shown.presents = 50; // before the first interval: discarded
    shown.fg_evaluations = 9;
    rrhealth::Tracker health;
    std::string report;
    assert(!health.Report(1000, report));
    assert(shown.presents == 0 && shown.fg_evaluations == 0);
    // Each guide gets its own count (sss 4, glass 1, hit 2, responsivity 3), so a swap shows.
    health.Frame({0, 1, 0, 0}, {0, 0, true, false, false, true});
    health.Frame({0, 1, 12, 0}, {3, 0, true, false, true, true});
    health.Frame({1, 0, 0, 30}, {1, 7, true, true, true, true});
    health.Frame({0, 1, 0, 0}, {0, 0, true, false, false, false});
    health.Reset(rrlive::HistoryPolicy::Rejected | rrlive::HistoryPolicy::Gap);
    shown.presents += 16;
    shown.unfocused_presents += 5;
    shown.focused = true;
    shown.fg_evaluations += 12;
    assert(!health.Report(1000 + rrhealth::Tracker::Interval - 1, report));
    assert(shown.presents == 16);
    assert(health.Report(1000 + rrhealth::Tracker::Interval, report));
    assert(report.find("frames=4 rejected=1 resets=1") != std::string::npos);
    assert(report.find("[rejected=1 cut=0 generation=0 gap=1]") != std::string::npos);
    assert(report.find("isolatedGuideFrames=1 maxIsolatedPixels=12") != std::string::npos);
    assert(report.find("sanitizedSssFrames=1 maxSanitizedSamples=30") != std::string::npos);
    assert(report.find("maxSanitizedSamples=30 guides[sss=4 glass=1 hit=2 responsivity=3] "
                       "nonFiniteColorFrames=2 maxNonFiniteColorPixels=3 "
                       "nonFiniteOutputFrames=1 maxNonFiniteOutputPixels=7 presents=16 "
                       "unfocusedPresents=5 focused=1 fgEvaluations=12") != std::string::npos);
    assert(shown.presents == 0 && shown.unfocused_presents == 0 && shown.fg_evaluations == 0);
    // An inactive interval logs nothing and still drops its presentation counts.
    shown.presents = 7;
    shown.unfocused_presents = 7;
    shown.fg_evaluations = 7;
    report.clear();
    assert(!health.Report(1000 + 2 * rrhealth::Tracker::Interval, report) && report.empty());
    assert(shown.presents == 0 && shown.unfocused_presents == 0 && shown.fg_evaluations == 0);
    // The next line holds only its own interval; focus is the latest present's.
    health.Frame({0, 1, 0, 0});
    shown.presents = 2;
    shown.unfocused_presents = 2;
    shown.focused = false;
    assert(health.Report(1000 + 3 * rrhealth::Tracker::Interval, report));
    assert(report.find("frames=1 rejected=0 resets=0 [rejected=0 cut=0 generation=0 gap=0] "
                       "isolatedGuideFrames=0 maxIsolatedPixels=0 sanitizedSssFrames=0 "
                       "maxSanitizedSamples=0 guides[sss=0 glass=0 hit=0 responsivity=0] "
                       "nonFiniteColorFrames=0 maxNonFiniteColorPixels=0 "
                       "nonFiniteOutputFrames=0 maxNonFiniteOutputPixels=0 presents=2 "
                       "unfocusedPresents=2 focused=0 fgEvaluations=0") != std::string::npos);
    // The widest line is never truncated.
    rrhealth::Tracker wide;
    assert(!wide.Report(0, report));
    wide.Frame({~0u, 0, ~0u, ~0u}, {~0u, ~0u, true, true, true, true});
    shown.presents = shown.unfocused_presents = shown.fg_evaluations = ~0u;
    assert(wide.Report(~0ull, report));
    assert(report.size() > 400 && report.ends_with(" fgEvaluations=4294967295"));
    shown.presents = shown.unfocused_presents = shown.fg_evaluations = 0;
    // NGX feature kinds the probe tells apart (NVSDK_NGX_Feature values).
    assert(rrtrace::FeatureKind(1) == rrtrace::Kind::SR);
    assert(rrtrace::FeatureKind(13) == rrtrace::Kind::RR);
    assert(rrtrace::FeatureKind(11) == rrtrace::Kind::FG);
    assert(rrtrace::FeatureKind(12) == rrtrace::Kind::Other);
    assert(std::string_view(rrtrace::Name(rrtrace::Kind::FG)) == "FG");

    // Ray density: lobe 1 fully sampled, lobe 2 at 1/4 with some unrequested pixels.
    rrdensity::Tracker density;
    assert(!density.Report(1000, report));
    rrdensity::Frame rates{};
    rates[1] = 100;
    rates[rrdensity::Buckets + 4] = 100;
    rates[rrdensity::Buckets] = 100;
    rates[2 * rrdensity::Buckets] = 7;
    rates[2 * rrdensity::Buckets + 6] = 1;
    density.Add(rates);
    assert(density.Density(0) == 1.0 && density.Density(1) == 0.25);
    // Per screen pixel the 100 unrequested lobe 2 pixels count too: 25 rays over 200 pixels.
    assert(density.ScreenDensity(0) == 1.0 && density.ScreenDensity(1) == 0.125);
    assert(density.Report(1000 + rrdensity::Tracker::Interval, report));
    assert(report ==
           "RR ray density (10s): frames=1"
           " | lobe1 raysPerRequestedPixel=1.000 raysPerScreenPixel=1.000 noFreshRay=0.0% "
           "allPixels[full=100.0% 1/2-1/4=0.0% 1/5-1/8=0.0% 1/9-1/15=0.0% 1/16+=0.0% "
           "notRequested=0.0%] threshold=7 boundary=0"
           " | lobe2 raysPerRequestedPixel=0.250 raysPerScreenPixel=0.125 noFreshRay=87.5% "
           "allPixels[full=0.0% 1/2-1/4=50.0% 1/5-1/8=0.0% 1/9-1/15=0.0% 1/16+=0.0% "
           "notRequested=50.0%] threshold=0 boundary=0"
           " | enabled=0 promote=0 clamp=1");
    report.clear();
    assert(!density.Report(1000 + 2 * rrdensity::Tracker::Interval, report) && report.empty());

    // Unrequested pixels set the per-requested and per-screen rates and no-fresh-ray share apart.
    rrdensity::Tracker mixed;
    assert(!mixed.Report(0, report));
    rrdensity::Frame mix{};
    mix[0] = 20, mix[1] = 10, mix[2] = 30, mix[5] = 20, mix[10] = 10, mix[16] = 10;
    mix[rrdensity::Buckets] = 50, mix[rrdensity::Buckets + 3] = 50;
    mix[2 * rrdensity::Buckets] = 16, mix[2 * rrdensity::Buckets + 1] = 28;
    mix[2 * rrdensity::Buckets + 2] = ~0u, mix[2 * rrdensity::Buckets + 3] = 5;
    mix[2 * rrdensity::Buckets + 4] = mix[2 * rrdensity::Buckets + 5] = 1;
    mixed.Add(mix);
    mixed.Add(mix); // a second identical frame leaves every rate and share unchanged
    // Lobe 1: 10 + 15 + 4 + 1 + 0.625 rays over 80 requested of 100 pixels.
    assert(mixed.Density(0) == 30.625 / 80 && mixed.ScreenDensity(0) == 0.30625);
    assert(mixed.Report(rrdensity::Tracker::Interval, report));
    assert(report ==
           "RR ray density (10s): frames=2"
           " | lobe1 raysPerRequestedPixel=0.383 raysPerScreenPixel=0.306 noFreshRay=69.4% "
           "allPixels[full=10.0% 1/2-1/4=30.0% 1/5-1/8=20.0% 1/9-1/15=10.0% 1/16+=10.0% "
           "notRequested=20.0%] threshold=16 boundary=28"
           " | lobe2 raysPerRequestedPixel=0.333 raysPerScreenPixel=0.167 noFreshRay=83.3% "
           "allPixels[full=0.0% 1/2-1/4=50.0% 1/5-1/8=0.0% 1/9-1/15=0.0% 1/16+=0.0% "
           "notRequested=50.0%] threshold=-1 boundary=5"
           " | enabled=1 promote=1 clamp=0");
    // The longest numbers the budget fields can print still fit (this line outgrew 512 bytes).
    rrdensity::Tracker widest;
    assert(!widest.Report(0, report));
    rrdensity::Frame extreme = mix;
    for (unsigned i = 2 * rrdensity::Buckets; i < rrdensity::Count; ++i)
        extreme[i] = 0x80000000u;
    widest.Add(extreme);
    assert(widest.Report(rrdensity::Tracker::Interval, report));
    assert(report.size() > 512 && report.size() < 1024);
    assert(report.ends_with(" | enabled=-2147483648 promote=-2147483648 clamp=-2147483648"));

    // Water responsivity needs preset F, a nonzero strength and this frame's water.
    rrwater::Settings water;
    water.responsivity = true;
    assert(rrwater::UseResponsivity(water, true, true, 6));
    assert(!rrwater::UseResponsivity(water, false, true, 6));
    assert(!rrwater::UseResponsivity(water, true, false, 6));
    for (unsigned preset : {0u, 4u, 5u})
        assert(!rrwater::UseResponsivity(water, true, true, preset));
    water.strength = -0.5f; // clamps to 0
    assert(!rrwater::UseResponsivity(water, true, true, 6));
    assert(rrwater::Clamp(2.0f) == 1.0f && rrwater::Clamp(0.25f) == 0.25f);
    assert(rrwater::Clamp(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    // A guide mode alone still classifies water, so coverage is logged.
    water = {};
    water.guide = rrwater::Material;
    assert(rrwater::Wanted(water) && !rrwater::Masks(water));
    water.guide = 99;
    assert(!rrwater::Wanted(water));

    // Water coverage reports once per interval, only with frames, then starts over.
    rrwater::Tracker coverage;
    assert(!coverage.Report(1000, report, water) && coverage.Last() < 0);
    coverage.Add(25, 100);
    coverage.Add(0, 100);
    coverage.Add(5, 0); // no pixels: ignored
    assert(coverage.Last() == 0.0);
    water.discard = true;
    assert(!coverage.Report(1000 + rrwater::Tracker::Interval - 1, report, water));
    assert(coverage.Report(1000 + rrwater::Tracker::Interval, report, water));
    assert(report.find("frames=2 waterPixels avg=12.5% max=25.0% framesWithoutWater=1") !=
           std::string::npos);
    assert(report.find("responsivity=off(1.00) discard=on animatedMask=off guides=0") !=
           std::string::npos);
    report.clear();
    assert(!coverage.Report(1000 + 2 * rrwater::Tracker::Interval, report, water) &&
           report.empty() && coverage.Last() == 0.0);

    // Specular hit distance tests: classification, hole filling and the summaries.
    {
        using namespace rrhitcond;
        static_assert(Far == 65504.0f && StatsOffset >= 20 && StatsOffset + 4 * StatCount <= 64);
        constexpr auto npos = std::string::npos;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        assert(SpecularRay(1u << 20) && !SpecularRay(0xfffffu));
        assert(DiffuseRay(1u << 8) && !DiffuseRay(0xffu) && !DiffuseRay(0xfff00000u));
        assert(Classify(2, 0, 0.0f, 0.0f, false) == Sky);
        assert(Classify(1, 0, 0.5f, 3.0f, false) == Hit);
        assert(Classify(3, 8, 0.5f, 3.0f, true) == Hit);
        assert(Classify(1, 0, 0.5f, 0.0f, true) == Miss);
        assert(Classify(1, 0, 0.5f, nan, true) == Miss);
        assert(Classify(1, 0, 0.5f, -1.0f, false) == Hole);
        assert(Classify(1, 0, 0.5f, 0.0f, false) == Hole);
        assert(Classify(1, Alternate, 0.5f, 3.0f, true) == Leave); // viewmodel
        assert(Classify(1, NoRtSpecular | 4, 0.5f, 0.0f, true) == Leave);
        for (unsigned status : {0u, 4u})
            assert(Classify(status, 0, 0.5f, 3.0f, true) == Leave);
        assert(Classify(1, 0, 0.0f, 3.0f, true) == Leave); // sky depth without the sky status
        assert(Classify(1, 0, (std::numeric_limits<float>::max)(), 3.0f, true) == Leave);

        // Slopes: limit 0.04 * 0.5 = 0.02 of device depth.
        assert(Slope(0.5f, 0.25f, true, 0.75f, true) == 0.25f);        // steep plane
        assert(Slope(0.5f, 0.25f, true, 0.5078125f, true) == 0.0078125f); // edge before
        assert(Slope(0.5f, 0.75f, true, 0.4921875f, true) == -0.0078125f); // edge before
        assert(Slope(0.5f, 0.25f, true, 0.25f, true) == 0.0f);          // 1-px feature
        assert(Slope(0.5f, 0.25f, true, 1.0f, true) == 0.0f);           // edges both sides
        assert(Slope(0.5f, 0.4921875f, true, 1.0f, false) == 0.0078125f);
        assert(Slope(0.5f, 0.25f, true, 1.0f, false) == 0.0f);          // lone edge
        assert(Slope(0.5f, 0.46875f, true, 1.0f, false) == 0.0f); // steep lone step: not trusted
        assert(Slope(0.5f, 0.25f, false, 0.5078125f, true) == 0.0078125f);
        assert(Slope(0.5f, 0.25f, false, 1.0f, false) == 0.0f);
        assert(SurfaceStatus(1) && SurfaceStatus(3) && !SurfaceStatus(0) && !SurfaceStatus(2) &&
               !SurfaceStatus(4));
        assert(SameSurface(0, 0, 1, 0.5f, 0.51f, 0.5f, 1.0f));
        assert(SameSurface(4, 4, 3, 0.5f, 0.49f, 0.5f, 0.95f));
        assert(!SameSurface(0, 0, 1, 0.5f, 0.53f, 0.5f, 1.0f)); // off the plane
        assert(SameSurface(0, 0, 1, 0.5f, 0.53f, 0.52f, 1.0f));  // on a sloped one
        assert(!SameSurface(0, 4, 1, 0.5f, 0.5f, 0.5f, 1.0f));   // another class
        assert(!SameSurface(0, 0, 2, 0.5f, 0.5f, 0.5f, 1.0f));   // sky
        assert(!SameSurface(0, 0, 1, 0.5f, 0.5f, 0.5f, 0.8f));   // bent normal
        assert(!SameSurface(0, 0, 1, 0.5f, 0.5f, 0.5f, nan));
        assert(!SameSurface(0, 0, 1, 0.5f, 0.0f, 0.0f, 1.0f));
        assert(Weight(0, 1) == 0.5f && Weight(1, 1) == 1.0f / 3.0f);

        const unsigned both = SkyMiss | FillHoles;
        assert(Resolve(Sky, 0.0f, FillHoles, 0, 0, 0) == 0.0f);
        assert(Resolve(Sky, 0.0f, SkyMiss, 0, 0, 0) == Far);
        assert(Resolve(Miss, 0.0f, FillHoles, 0, 0, 0) == 0.0f);
        assert(Resolve(Miss, 0.0f, both, 0, 0, 0) == Far);
        assert(Resolve(Hit, 12.5f, both, 0, 0, 0) == 12.5f);
        assert(Resolve(Hit, std::numeric_limits<float>::infinity(), SkyMiss, 0, 0, 0) == Far);
        assert(Resolve(Hole, 0.0f, SkyMiss, 1.0f, 10.0f, 0.0f) == 0.0f); // fill off
        assert(Resolve(Hole, 0.0f, FillHoles, 1.0f, 10.0f, 0.5f) == 10.0f);
        assert(Resolve(Hole, 0.0f, FillHoles, 0.5f, 5.0f, 0.5f) == 10.0f); // ties go to hits
        assert(FilledAsMiss(0.5f, 1.0f) && !FilledAsMiss(0.5f, 0.5f));
        assert(Resolve(Hole, 0.0f, FillHoles, 0.5f, 5.0f, 1.0f) == 0.0f);
        assert(Resolve(Hole, 0.0f, both, 0.5f, 5.0f, 1.0f) == Far);
        assert(Resolve(Hole, 0.0f, both, 0.0f, 0.0f, 0.0f) == 0.0f); // nothing to fill from
        assert(Resolve(Leave, 3.0f, both, 1.0f, 10.0f, 0.0f) == 3.0f);
        assert(Resolve(Leave, 0.0f, both, 0.0f, 0.0f, 1.0f) == 0.0f);

        assert(Mode() == 0);
        sky_miss = true;
        assert(Mode() == SkyMiss);
        fill = true;
        assert(Mode() == both);
        sky_miss = false;
        assert(Mode() == FillHoles);
        fill = false;

        Diagnostics outcomes;
        char text[320];
        outcomes.Observe(Outcome::Off);
        outcomes.Observe(Outcome::Repeat);
        assert(!outcomes.Summary(text, sizeof(text)));
        assert(std::string_view(text).find("conditioned=0 surfaceAliased=0 lastSkip=none") !=
               npos);
        outcomes.Observe(Outcome::Surface);
        outcomes.Observe(Outcome::Conditioned);
        outcomes.Observe(Outcome::Conditioned);
        outcomes.SurfaceAliased();
        assert(outcomes.Summary(text, sizeof(text)));
        assert(std::string_view(text).find("conditioned=2 surfaceAliased=1 lastSkip=surface") !=
               npos);
        assert(std::string_view(text).find("weights=0 surface=1 context=0") != npos);
        assert(!outcomes.Summary(text, sizeof(text)));
        assert(std::string_view(OutcomeName(Outcome::Count)) == "none");

        Tracker pixels;
        assert(!pixels.Report(1000, report));
        Frame f{};
        f[CountSky] = 100;
        f[CountViewmodel] = f[CountNoRtSpecular] = 50;
        f[CountHit] = 200;
        f[CountMiss] = 100;
        f[CountHole] = 500;
        f[CountFilledHit] = 250;
        f[CountFilledMiss] = 50;
        f[CountHitNoLobe1] = 2;
        f[CountHitNoLobe2] = 20;
        f[CountNoRtSpecularHit] = 3;
        pixels.Add(f, FillHoles, 1000);
        Frame g = f; // no fill
        g[CountFilledHit] = g[CountFilledMiss] = 0;
        pixels.Add(g, SkyMiss, 1100); // 100 more pixels in none of the categories
        assert(pixels.Pixels(CountHit) == 400 && pixels.Pixels(StatCount) == 0);
        assert(!pixels.Report(1000 + Tracker::Interval - 1, report));
        assert(pixels.Report(1000 + Tracker::Interval, report));
        assert(report.find("(10s): frames=2 skyMissFrames=1 fillFrames=1 | sky=9.5% "
                           "viewmodel=4.8% noRtSpecular=4.8% other=4.8% surfaces=76.2% | of "
                           "surfaces: hit=25.0% miss=12.5% noRay=62.5% | of noRay in fill "
                           "frames: filledFromHits=50.0% "
                           "filledAsMiss=10.0% | checks: hitWithoutLobe1Ray=1.00% "
                           "hitWithoutLobe2Ray=10.00% noRtSpecularHits=6") != npos);
        report.clear();
        assert(!pixels.Report(1000 + 2 * Tracker::Interval, report) && report.empty());
    }
}
