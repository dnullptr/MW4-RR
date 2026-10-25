#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_ray_density.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_sss_mask.h"
#include "../src/rr_responsivity.h"
#include "../src/rr_sky_edge.h"
#include "../src/rr_color_scale.h"
#include "../src/rr_specular_distance.h"
#include "../src/rr_water.h"
#include <cassert>
#include <limits>
#include <string_view>

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

int main() {
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
    rrhealth::Tracker health;
    std::string report;
    assert(!health.Report(1000, report));
    health.Frame({0, 1, 0, 0});
    health.Frame({0, 1, 12, 0});
    health.Frame({1, 0, 0, 30});
    health.Reset(rrlive::HistoryPolicy::Rejected | rrlive::HistoryPolicy::Gap);
    assert(!health.Report(1000 + rrhealth::Tracker::Interval - 1, report));
    assert(health.Report(1000 + rrhealth::Tracker::Interval, report));
    assert(report.find("frames=3 rejected=1 resets=1") != std::string::npos);
    assert(report.find("[rejected=1 cut=0 generation=0 gap=1]") != std::string::npos);
    assert(report.find("isolatedGuideFrames=1 maxIsolatedPixels=12") != std::string::npos);
    assert(report.find("sanitizedSssFrames=1 maxSanitizedSamples=30") != std::string::npos);
    report.clear();
    assert(!health.Report(1000 + 2 * rrhealth::Tracker::Interval, report) && report.empty());

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
    assert(density.Report(1000 + rrdensity::Tracker::Interval, report));
    assert(report.find("lobe1 raysPerPixel=1.000 full=100.0%") != std::string::npos);
    assert(report.find("lobe2 raysPerPixel=0.250 full=0.0% 1/2-1/4=50.0%") != std::string::npos);
    assert(report.find("none=50.0% threshold=0") != std::string::npos);
    assert(report.find("lobe1 raysPerPixel=1.000 full=100.0% 1/2-1/4=0.0% 1/5-1/8=0.0% "
                       "1/9-1/15=0.0% 1/16+=0.0% none=0.0% threshold=7") != std::string::npos);
    assert(report.find("clamp=1") != std::string::npos);
    report.clear();
    assert(!density.Report(1000 + 2 * rrdensity::Tracker::Interval, report) && report.empty());

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
}
