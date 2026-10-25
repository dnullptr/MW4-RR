#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_responsivity.h"
#include "../src/rr_specular_distance.h"
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
static_assert(std::string_view(rrsettings::QualityLabel(0)) == "Performance");
static_assert(std::string_view(rrsettings::QualityLabel(5)) == "Native / DLAA");
static_assert(std::string_view(rrsettings::QualityLabel(-1)) == "Not observed yet");

int main() {
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
}
