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
#include "../src/rr_hit_condition.h"
#include <cassert>
#include <limits>
#include <string_view>

constexpr rrsettings::Preferences defaults;
static_assert(defaults.rr && defaults.sss && defaults.glass && defaults.hit &&
              defaults.preset == 6);
static_assert(defaults.tolerate_isolated && defaults.native_lobe == rrlighting::Raw);
static_assert(!defaults.responsivity &&
              defaults.responsivity_strength == rrresponsivity::DefaultStrength);
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

    // Specular hit distance tests: classification, hole filling and the summaries.
    {
        using namespace rrhitcond;
        static_assert(Far == 65504.0f && StatsOffset >= 16 && StatsOffset + 4 * StatCount <= 64);
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
        assert(Classify(1, 0, std::numeric_limits<float>::max(), 3.0f, true) == Leave);

        assert(Slope(0.5f, 0.25f, true, 1.0f, true) == 0.25f);
        assert(Slope(0.5f, 0.75f, true, 0.375f, true) == -0.125f);
        assert(Slope(0.5f, 0.25f, true, 1.0f, false) == 0.25f);
        assert(Slope(0.5f, 0.25f, false, 1.0f, true) == 0.5f);
        assert(Slope(0.5f, 0.25f, false, 1.0f, false) == 0.0f);
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
        assert(std::string_view(text).find("conditioned=0 lastSkip=none") != npos);
        outcomes.Observe(Outcome::Surface);
        outcomes.Observe(Outcome::Conditioned);
        outcomes.Observe(Outcome::Conditioned);
        assert(outcomes.Summary(text, sizeof(text)));
        assert(std::string_view(text).find("conditioned=2 lastSkip=surface") != npos);
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
        pixels.Add(f, FillHoles);
        pixels.Add(f, SkyMiss);
        assert(pixels.Pixels(CountHit) == 400 && pixels.Pixels(StatCount) == 0);
        assert(!pixels.Report(1000 + Tracker::Interval - 1, report));
        assert(pixels.Report(1000 + Tracker::Interval, report));
        assert(report.find("(10s): frames=2 skyMiss=1 fill=1 | sky=10.0% viewmodel=5.0% "
                           "noRtSpecular=5.0% other=0.0% traced=80.0% | of traced: hit=25.0% "
                           "miss=12.5% noRay=62.5% | of noRay: filledFromHits=50.0% "
                           "filledAsMiss=10.0% | checks: hitWithoutLobe1Ray=1.00% "
                           "hitWithoutLobe2Ray=10.00% noRtSpecularHits=6") != npos);
        report.clear();
        assert(!pixels.Report(1000 + 2 * Tracker::Interval, report) && report.empty());
    }
}
