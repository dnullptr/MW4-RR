#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include <cassert>
#include <string_view>

constexpr rrsettings::Preferences defaults;
static_assert(defaults.rr && defaults.sss && defaults.glass && defaults.preset == 6);
static_assert(defaults.tolerate_isolated && defaults.native_lobe == 0);
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
