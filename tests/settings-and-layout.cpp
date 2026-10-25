#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_ray_density.h"
#include "../src/rr_ray_budget.h"
#include "../src/rr_vrrt_rate.h"
#include "../src/rr_vrrt_target.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_responsivity.h"
#include "../src/rr_specular_distance.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
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
static_assert(std::string_view(rrsettings::QualityLabel(0)) == "Performance");
static_assert(std::string_view(rrsettings::QualityLabel(5)) == "Native / DLAA");
static_assert(std::string_view(rrsettings::QualityLabel(-1)) == "Not observed yet");
// The readback frame keeps its 41 words and appends extra rays (bytes 228, 232) and the apply
// target (bytes 236, 240); the stats buffer holds 4 * Count bytes from Offset.
static_assert(rrdensity::Limits == 34 && rrdensity::RateSettings == 38 && rrdensity::Extra == 41 &&
              rrdensity::Target == 43 && rrdensity::Count == 45);
static_assert(rrdensity::Offset + 4 * rrdensity::Extra == 228 &&
              rrdensity::Offset + 4 * rrdensity::Target == 236 &&
              rrdensity::Offset + 4 * rrdensity::Count == 244);
// More rays: the option steps are fixed, unknown values read as Off, and the list layout matches
// the native apply (144-byte header, 4-byte entries) with caps of half a list per frame.
static_assert(rrraybudget::Off == 0 && rrraybudget::Count == 4);
static_assert(rrraybudget::Valid(rrraybudget::Scale25) == rrraybudget::Scale25 &&
              rrraybudget::Valid(rrraybudget::Count) == rrraybudget::Off &&
              rrraybudget::Valid(99u) == rrraybudget::Off);
static_assert(std::string_view(rrraybudget::labels[rrraybudget::Off]) == "Off" &&
              std::string_view(rrraybudget::labels[rrraybudget::Scale15]) == "1.5x" &&
              std::string_view(rrraybudget::labels[rrraybudget::Scale20]) == "2x" &&
              std::string_view(rrraybudget::labels[rrraybudget::Scale25]) == "2.5x");
static_assert(rrraybudget::scales[0] == 1.0f && rrraybudget::scales[1] == 1.5f &&
              rrraybudget::scales[2] == 2.0f && rrraybudget::scales[3] == 2.5f);
static_assert(rrraybudget::Resolve == 0x293995352d1f8220ull && rrraybudget::ListHeader == 144 &&
              rrraybudget::EntryBytes == 4 && rrraybudget::Fill == 0.5 &&
              rrraybudget::StopFill == 0.75 && rrraybudget::MaxTarget == 1.0);
static_assert(rrraybudget::Registers[rrraybudget::PixelCounter] == 2 &&
              rrraybudget::Registers[rrraybudget::PixelList] == 3 &&
              rrraybudget::Registers[rrraybudget::Lobe1] == 5 &&
              rrraybudget::Registers[rrraybudget::Lobe2] == 6);

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

    // Extra rays at pixels granted several rays count toward the density, so it can exceed 1.
    rrdensity::Tracker dense;
    assert(!dense.Report(2000, report));
    rrdensity::Frame granted{};
    granted[1] = 100;
    granted[rrdensity::Extra] = 100;
    granted[rrdensity::Extra + 1] = 30;
    granted[rrdensity::Buckets + 2] = 50;
    granted[rrdensity::Limits] = 18;
    granted[rrdensity::Limits + 3] = 15;
    granted[rrdensity::RateSettings] = 1;
    granted[rrdensity::RateSettings + 2] = 1;
    granted[rrdensity::Target] = 0x3f000000u; // apply targets are not part of the density
    dense.Add(granted);
    assert(dense.Density(0) == 2.0 && dense.Density(1) == 1.1); // 50 pixels: 25 + 30 extra rays
    granted[rrdensity::Extra] = 50;
    granted[rrdensity::Extra + 1] = 10;
    dense.Add(granted);
    assert(dense.Density(0) == 1.75 && dense.Density(1) == 0.9); // 100 pixels: 50 + 40 extra rays
    assert(dense.Report(2000 + rrdensity::Tracker::Interval, report));
    assert(report.find("lobe1 raysPerPixel=1.750 full=100.0%") != std::string::npos);
    assert(report.find("lobe2 raysPerPixel=0.900") != std::string::npos);
    assert(report.find("threshold=18 boundary=0") != std::string::npos);
    assert(report.find("threshold=0 boundary=15") != std::string::npos);
    assert(report.find("| enabled=1 promote=0 clamp=1") != std::string::npos);
    assert(dense.Density(0) == 0 && dense.Density(1) == 0);

    // More rays: the session option starts off.
    namespace rb = rrraybudget;
    assert(rb::choice.load() == rb::Off);

    // A list proof needs table-bound raw views for both lobe lists and the lobe-1 pixel list.
    auto table = [](std::uint64_t bytes) { return rb::View{rb::Binding::Table, true, 7, bytes}; };
    const rb::View root{rb::Binding::Root, false, 7, 0}, unknown{}, sizeless{rb::Binding::Table};
    using Views = std::array<rb::View, rb::ListCount>;
    // Counter, pixel list, lobe 1, lobe 2.
    auto lists = [&](std::uint64_t pixels, std::uint64_t lobe1, std::uint64_t lobe2) {
        return Views{root, table(pixels), table(lobe1), table(lobe2)};
    };
    auto why = [](const Views &v, unsigned w = 1920, unsigned h = 1080) {
        return std::string_view(rb::Prove(v, w, h).reason);
    };
    Views views = lists(8294400, 8294544, 8294544);
    rb::Lists proof = rb::Prove(views, 1920, 1080);
    assert(proof.valid && why(views) == "proven");
    assert(proof.entries[0] == 2073600 && proof.entries[1] == 2073600);
    assert(proof.pixel_entries == 2073600 && proof.w == 1920 && proof.h == 1080);
    assert(proof.views[rb::Lobe1].bytes == 8294544 &&
           proof.views[rb::PixelCounter].binding == rb::Binding::Root);
    for (const rb::View &counter : {unknown, sizeless, table(0)}) {
        views[rb::PixelCounter] = counter; // described, never required
        assert(rb::Prove(views, 1920, 1080).valid);
    }
    // The first failing rule wins: size, lobe 1, lobe 2, pixel list.
    Views bad{root, root, root, root};
    assert(why(bad, 0, 1080) == "render size unknown" &&
           why(bad, 1920, 0) == "render size unknown");
    proof = rb::Prove(bad, 0, 0);
    assert(!proof.valid && proof.w == 0 && proof.views[rb::Lobe1].binding == rb::Binding::Root);
    assert(why(bad) == "lobe 1 list is a root UAV");
    bad[rb::Lobe1] = unknown;
    assert(why(bad) == "lobe 1 list binding unknown");
    bad[rb::Lobe1] = sizeless;
    assert(why(bad) == "lobe 1 list size unknown");
    bad[rb::Lobe1] = table(rb::ListHeader);
    assert(why(bad) == "lobe 1 list too small");
    bad[rb::Lobe1] = table(rb::ListHeader + 4);
    assert(why(bad) == "lobe 2 list is a root UAV");
    bad[rb::Lobe2] = unknown;
    assert(why(bad) == "lobe 2 list binding unknown");
    bad[rb::Lobe2] = sizeless;
    assert(why(bad) == "lobe 2 list size unknown");
    bad[rb::Lobe2] = table(rb::ListHeader);
    assert(why(bad) == "lobe 2 list too small");
    bad[rb::Lobe2] = table(rb::ListHeader + 4);
    assert(why(bad) == "pixel list is a root UAV");
    bad[rb::PixelList] = unknown;
    assert(why(bad) == "pixel list binding unknown");
    bad[rb::PixelList] = sizeless;
    assert(why(bad) == "pixel list size unknown");
    bad[rb::PixelList] = table(rb::PixelEntryBytes - 1);
    assert(why(bad) == "pixel list too small");
    bad[rb::PixelList] = table(rb::PixelEntryBytes);
    proof = rb::Prove(bad, 1, 1);
    assert(proof.valid && proof.entries[0] == 1 && proof.entries[1] == 1 &&
           proof.pixel_entries == 1);
    // A lobe list of header + 1..3 bytes is not too small, but holds no entry.
    proof = rb::Prove(lists(40, rb::ListHeader + 1, rb::ListHeader + 3), 1, 1);
    assert(proof.valid && proof.entries[0] == 0 && proof.entries[1] == 0);
    assert(why(lists(40, rb::ListHeader, rb::ListHeader + 1), 1, 1) == "lobe 1 list too small");
    assert(why(lists(40, rb::ListHeader + 1, rb::ListHeader), 1, 1) == "lobe 2 list too small");
    // The lobe-1 pixel list bounds lobe 1 (only) when it holds fewer than w*h entries.
    proof = rb::Prove(lists(4 * 4000, rb::ListHeader + 4 * 10000, rb::ListHeader + 4 * 10000), 100,
                      100);
    assert(proof.valid && proof.entries[0] == 4000 && proof.entries[1] == 10000);
    assert(proof.pixel_entries == 4000);
    proof = rb::Prove(lists(4 * 10000, rb::ListHeader + 4 * 20000, rb::ListHeader + 4 * 20000), 100,
                      100);
    assert(proof.valid && proof.entries[0] == 20000 && proof.entries[1] == 20000); // u3 == w*h
    assert(proof.pixel_entries == 10000);
    proof = rb::Prove(lists(4 * 10001, rb::ListHeader + 4 * 20000, rb::ListHeader + 4 * 20000), 100,
                      100);
    assert(proof.valid && proof.entries[0] == 20000 && proof.pixel_entries == 10001); // u3 > w*h
    proof =
        rb::Prove(lists(4 * 4000, rb::ListHeader + 4 * 2000, rb::ListHeader + 4 * 10000), 100, 100);
    assert(proof.entries[0] == 2000 && proof.entries[1] == 10000); // lobe 1 list is smaller anyway

    // Caps keep expected rays at half a list and never above one ray per pixel.
    assert(rb::Cap(2073600, 1920, 1080) == 0.5f && rb::Cap(1036800, 1920, 1080) == 0.25f);
    assert(rb::Cap(4147200, 1920, 1080) == 1.0f && rb::Cap(99999999, 1920, 1080) == 1.0f);
    assert(rb::Cap(0, 1920, 1080) == 0 && rb::Cap(2073600, 0, 1080) == 0 &&
           rb::Cap(2073600, 1920, 0) == 0);
    assert(rb::Cap((std::numeric_limits<std::uint64_t>::max)(), 16384, 16384) == 1.0f);
    // A raised target only grows, to min(target * scale, cap).
    constexpr float nan = std::numeric_limits<float>::quiet_NaN(),
                    inf = std::numeric_limits<float>::infinity();
    assert(rb::Raised(0.2f, 2.0f, 0.9f) == 0.2f * 2.0f &&
           rb::Raised(0.4f, 1.5f, 0.9f) == 0.4f * 1.5f);
    assert(rb::Raised(0.4f, 2.5f, 0.5f) == 0.5f && rb::Raised(0.3f, 2.0f, 0.5f) == 0.5f);
    assert(rb::Raised(0.5f, 2.0f, 0.5f) == 0.5f && rb::Raised(0.8f, 2.0f, 0.5f) == 0.8f);
    assert(rb::Raised(0.2f, 2.0f, 0.0f) == 0.2f); // unknown cap
    assert(rb::Raised(0.0f, 2.0f, 0.5f) == 0.0f && rb::Raised(-0.1f, 2.0f, 0.5f) == -0.1f);
    assert(std::isnan(rb::Raised(nan, 2.0f, 0.5f)) && rb::Raised(inf, 2.0f, 1.0f) == inf);
    assert(rb::Raised(-inf, 2.0f, 0.5f) == -inf && rb::Raised(inf, 2.0f, inf) == inf);
    assert(rb::Raised(0.2f, 1.0f, 0.5f) == 0.2f && rb::Raised(0.2f, 0.5f, 0.5f) == 0.2f);
    assert(rb::Raised(0.2f, nan, 0.5f) == 0.2f && rb::Raised(0.2f, 2.0f, nan) == 0.2f);
    assert(rb::Raised(0.2f, inf, 0.5f) == 0.5f);
    for (unsigned c = rb::Scale15; c < rb::Count; ++c)
        assert(rb::Raised(0.01f, rb::scales[c], 0.5f) == 0.01f * rb::scales[c]);
    // The shader shares this rule (src/rr_vrrt_target.h); the CPU copy forwards to it, bit for bit.
    constexpr float targets[] = {0.0f, -0.0f, 1e-30f, 0.01f, 0.2f, 0.5f, 0.99f,
                                 1.0f, 3.0f,  -0.1f,  -inf,  inf,  nan},
                    scales[] = {0.0f, 0.5f,  1.0f,  1.0001f, 1.5f, 2.0f,
                                2.5f, 1e30f, -2.0f, inf,     nan},
                    caps[] = {0.0f, 0.25f, 0.5f, 1.0f, 1e-31f, -1.0f, inf, nan};
    for (float target : targets)
        for (float scale : scales)
            for (float cap : caps) {
                const bool raise = target > 0 && target < cap && scale > 1 && !std::isinf(target);
                const float want = raise ? (std::min)(target * scale, cap) : target;
                const float shared = rrvrrt::RaisedTarget(target, scale, cap),
                            cpu = rb::Raised(target, scale, cap);
                assert(std::memcmp(&shared, &want, sizeof(float)) == 0 ||
                       (std::isnan(shared) && std::isnan(want)));
                assert(std::memcmp(&cpu, &shared, sizeof(float)) == 0 ||
                       (std::isnan(cpu) && std::isnan(shared)));
                assert(!raise || (shared > target && shared <= cap));
                assert(raise || std::isnan(target) || shared == target);
            }
    assert(rrvrrt::RaisedTarget(0.2f, 2.0f, 0.9f) == 0.2f * 2.0f);
    assert(rrvrrt::RaisedTarget(0.4f, 2.5f, 0.5f) == 0.5f);
    assert(rrvrrt::RaisedTarget(0.5f, 2.0f, 0.5f) == 0.5f);
    assert(rrvrrt::RaisedTarget(0.2f, 2.0f, 0.0f) == 0.2f);
    assert(rrvrrt::RaisedTarget(0.2f, 1.0f, 0.5f) == 0.2f);

    // The list proof is logged in one line.
    assert(rb::Describe(rb::Prove(lists(8294400, 8294544, 8294544), 1920, 1080)) ==
           "RR ray lists 1920x1080: lobe1=table 8294544B lobe2=table 8294544B pixels=table "
           "8294400B counter=root | rays/px lobe1=1.00 lobe2=1.00 | proven");
    assert(rb::Describe(rb::Prove(
               lists(4 * 4000, rb::ListHeader + 4 * 10000, rb::ListHeader + 4 * 2500), 100, 100)) ==
           "RR ray lists 100x100: lobe1=table 40144B lobe2=table 10144B pixels=table 16000B "
           "counter=root | rays/px lobe1=0.40 lobe2=0.25 | proven");
    assert(
        rb::Describe(rb::Prove({table(64), sizeless, root, unknown}, 1920, 1080)) ==
        "RR ray lists 1920x1080: lobe1=root lobe2=unknown pixels=table (size unknown) "
        "counter=table 64B | rays/px lobe1=0.00 lobe2=0.00 | unproven: lobe 1 list is a root UAV");
    assert(rb::Describe(rb::Lists{}) ==
           "RR ray lists 0x0: lobe1=unknown lobe2=unknown pixels=unknown counter=unknown | "
           "rays/px lobe1=0.00 lobe2=0.00 | unproven: not observed");

    // One frame's sample: bucket 0 is ignored, extra rays count in both sums, targets are float
    // bits.
    rrdensity::Frame hand{};
    hand[0] = 999;
    hand[1] = 100;
    hand[2] = 50;
    hand[16] = 32;
    hand[rrdensity::Extra] = 40;
    hand[rrdensity::Extra + 1] = 12;
    hand[rrdensity::Buckets] = 7;
    hand[rrdensity::Buckets + 4] = 80;
    const float target1 = 0.375f, target2 = 0.125f;
    std::memcpy(&hand[rrdensity::Target], &target1, sizeof(float));
    std::memcpy(&hand[rrdensity::Target + 1], &target2, sizeof(float));
    proof = rb::Prove(lists(8294400, 8294544, 4147344), 1920, 1080);
    rb::FrameSample sample = rb::Sample(hand, proof, true);
    assert(sample.expected[0] == 100 + 25 + 2 + 40 && sample.worst[0] == 100 + 50 + 32 + 40);
    assert(sample.expected[1] == 20 + 12 && sample.worst[1] == 80 + 12);
    assert(sample.target[0] == 0.375f && sample.target[1] == 0.125f && sample.raised);
    assert(sample.entries[0] == 2073600 && sample.entries[1] == 1036800);
    assert(sample.w == 1920 && sample.h == 1080);
    sample = rb::Sample(hand, rb::Prove({root, root, root, root}, 1280, 720), false);
    assert(sample.entries[0] == 0 && sample.entries[1] == 0 && !sample.raised);
    assert(sample.w == 1280 && sample.h == 720 && sample.expected[0] == 167);
    // Capacities of an unproven result are never used, even when they are filled in.
    rb::Lists unproven_lists;
    unproven_lists.w = 1920;
    unproven_lists.h = 1080;
    unproven_lists.reason = "lobe 2 list too small";
    unproven_lists.entries = {2073600, 1036800};
    unproven_lists.pixel_entries = 2073600;
    sample = rb::Sample(hand, unproven_lists, true);
    assert(sample.entries[0] == 0 && sample.entries[1] == 0 && sample.raised);
    assert(sample.w == 1920 && sample.h == 1080);
    assert(rb::Describe(unproven_lists) ==
           "RR ray lists 1920x1080: lobe1=unknown lobe2=unknown pixels=unknown counter=unknown | "
           "rays/px lobe1=0.00 lobe2=0.00 | unproven: lobe 2 list too small");

    // The session stops raising when a raised frame is too full or has no proven list.
    auto frame = [](double expected1, double expected2, std::uint64_t entries, bool raised) {
        rb::FrameSample f;
        f.expected = {expected1, expected2};
        f.worst = {2 * expected1, 2 * expected2};
        f.entries = {entries, entries};
        f.target = {0.5f, 0.0f};
        f.w = f.h = 1000;
        f.raised = raised;
        return f;
    };
    rb::Monitor monitor;
    assert(!monitor.Stopped() && std::string_view(monitor.StopReason()).empty());
    assert(!monitor.Add(frame(2e6, 2e6, 1000000, false)) && !monitor.Stopped()); // not raised
    assert(!monitor.Add(frame(2e6, 2e6, 0, false)) && !monitor.Stopped());
    assert(!monitor.Add(frame(750000, 750000, 1000000, true)) && !monitor.Stopped()); // 75% runs
    assert(monitor.Add(frame(500000, 800000, 1000000, true)) && monitor.Stopped());
    assert(std::string_view(monitor.StopReason()) ==
           "lobe 2 expected 800000 rays, 80% of its list");
    assert(!monitor.Add(frame(900000, 900000, 1000000, true)) && monitor.Stopped());
    assert(std::string_view(monitor.StopReason()) ==
           "lobe 2 expected 800000 rays, 80% of its list");
    monitor.Restart();
    assert(!monitor.Stopped() && std::string_view(monitor.StopReason()).empty());
    assert(monitor.Add(frame(900000, 900000, 1000000, true))); // lobe 1 is checked first
    assert(std::string_view(monitor.StopReason()) ==
           "lobe 1 expected 900000 rays, 90% of its list");
    monitor.Restart();
    auto unproven = frame(1, 1, 0, true);
    unproven.entries = {1000000, 0};
    assert(monitor.Add(unproven));
    assert(std::string_view(monitor.StopReason()) == "a raised frame had no proven ray list");
    monitor.Restart();
    unproven.entries = {0, 1000000};
    assert(monitor.Add(unproven) && monitor.Stopped());
    assert(std::string_view(monitor.StopReason()) == "a raised frame had no proven ray list");

    // Restart clears the stop but keeps the window's statistics.
    rb::Monitor kept;
    std::string text;
    assert(!kept.Report(0, text) && kept.Add(frame(800000, 0, 1000000, true)));
    kept.Restart();
    assert(kept.Report(rb::Monitor::Interval, text));
    assert(text.find("frames=1 raised=1 | lobe1") != std::string::npos &&
           text.ends_with("| running"));
    // Reports reset the statistics every window but keep the stop.
    rb::Monitor budget;
    assert(!budget.Report(1000, text) && !budget.Report(999, text));
    rb::FrameSample first = frame(300000, 100000, 1000000, false),
                    second = frame(500000, 150000, 500000, true);
    first.worst = {600000, 200000};
    first.target = {0.4f, 0.3f};
    second.entries = {1000000, 500000};
    second.worst = {900000, 300000};
    second.target = {0.5f, 0.0f};
    assert(!budget.Add(first) && !budget.Add(second));
    text.clear();
    assert(!budget.Report(1000 + rb::Monitor::Interval - 1, text) && text.empty());
    assert(budget.Report(1000 + rb::Monitor::Interval, text));
    assert(text == "RR ray budget (10s): frames=2 raised=1 | lobe1 rays/px=0.400 worst/px=0.90 "
                   "peakFill=50.0% capacity/px=1.00 target=0.500 | lobe2 rays/px=0.125 "
                   "worst/px=0.30 peakFill=30.0% capacity/px=0.50 target=0.300 | running");
    text.clear();
    assert(!budget.Report(1000 + 2 * rb::Monitor::Interval, text) && text.empty()); // nothing new
    assert(budget.Add(frame(800000, 0, 1000000, true)));
    assert(!budget.Report(1000 + 3 * rb::Monitor::Interval - 1, text));
    assert(budget.Report(1000 + 3 * rb::Monitor::Interval, text));
    assert(text == "RR ray budget (10s): frames=1 raised=1 | lobe1 rays/px=0.800 worst/px=1.60 "
                   "peakFill=80.0% capacity/px=1.00 target=0.500 | lobe2 rays/px=0.000 "
                   "worst/px=0.00 peakFill=0.0% capacity/px=1.00 target=0.000 | stopped: lobe 1 "
                   "expected 800000 rays, 80% of its list");
    assert(!budget.Report(1000 + 4 * rb::Monitor::Interval, text)); // empty window keeps the stop
    assert(!budget.Add(frame(0, 0, 0, false)) && budget.Stopped());
    assert(budget.Report(1000 + 5 * rb::Monitor::Interval, text));
    assert(text.find("frames=1 raised=0 | lobe1 rays/px=0.000") != std::string::npos);
    assert(text.find("capacity/px=0.00 target=0.500 | lobe2") != std::string::npos);
    assert(text.find("| stopped: lobe 1 expected 800000 rays") != std::string::npos);
    budget.Restart();
    assert(!budget.Add(frame(0, 0, 0, false)));
    assert(budget.Report(1000 + 6 * rb::Monitor::Interval, text));
    assert(text.ends_with("| running"));
    rb::FrameSample unsized; // no render size: counted, but not part of the per-pixel averages
    unsized.expected = {5, 5};
    assert(!budget.Add(unsized));
    assert(budget.Report(1000 + 7 * rb::Monitor::Interval, text));
    assert(text.find("frames=1 raised=0 | lobe1 rays/px=0.000 worst/px=0.00 peakFill=0.0%") !=
           std::string::npos);
    assert(!budget.Add(unsized) && !budget.Add(frame(500000, 0, 1000000, false)));
    assert(budget.Report(1000 + 8 * rb::Monitor::Interval, text));
    assert(text.find("frames=2 raised=0 | lobe1 rays/px=0.500 worst/px=1.00 peakFill=50.0%") !=
           std::string::npos);

    // The resolve gate's verdict is logged when it changes, at most once per two seconds.
    rb::Verdicts verdicts;
    const char *ready = "ready (option off)", *noproof = "no recent list proof";
    assert(verdicts.Changed(ready, 0));
    assert(!verdicts.Changed(ready, 100));     // same reason
    assert(!verdicts.Changed(noproof, 1999));  // changed too soon: not logged yet
    assert(verdicts.Changed(noproof, 2000));   // the suppressed change is logged later
    assert(!verdicts.Changed(noproof, 9000));  // same reason
    assert(verdicts.Changed(ready, 9001));     // changed long after the last log
    assert(!verdicts.Changed(noproof, 11000)); // 1999 ms after the last log
    assert(verdicts.Changed(noproof, 11001));  // 2000 ms after it
    assert(!verdicts.Changed(ready, 11500));   // too soon again
    assert(!verdicts.Changed(noproof, 12000)); // back to the logged reason: nothing to log
    assert(!verdicts.Changed(ready, 5));       // the clock went backwards: suppressed

    // Extra rays per pixel come from the shared rate rule: a request code of 16 or more is
    // code - 15 rays, so code - 16 beyond the first. The denominator is unchanged.
    // rate(packed, enabled, threshold, clamp, promote, boundary, ring); no budget unless enabled.
    auto rate = [](unsigned packed, int enabled = 0, int threshold = 0, int clamp = 0,
                   int promote = 0, int boundary = 0, int ring = 0) {
        return rrvrrt::InverseRate(packed, enabled, promote, clamp, threshold, boundary, ring);
    };
    for (unsigned code = 0; code < 256; ++code) { // no budget: the code alone decides
        const rrvrrt::Rate r = rate(code);
        assert(r.supported && r.denominator == (code >= 16 ? 1u : 16u - code));
        assert(r.extra == (code >= 16 ? code - 16 : 0u));
    }
    assert(rate(15).extra == 0 && rate(16).extra == 0 && rate(17).extra == 1);
    assert(rate(200).extra == 184 && rate(200).denominator == 1 && rate(255).extra == 239);
    // The code is the low byte of the low 16 bits; 0xFFFF alone means "not requested".
    assert(rate(0x1211).extra == 1 && rate(0x10000 | 17).extra == 1);
    assert(rate(0x00FF).extra == 239);
    const rrvrrt::Rate none = rate(0xFFFF);
    assert(none.supported && none.denominator == 0 && none.extra == 0);
    // Budget clamping happens first. Threshold 18 allows codes below 17; codes 17 and above become
    // 16 inside a tile ring at or below the boundary and 17 outside it.
    assert(rate(30, 1, 18, 1, 0, 0, 1).extra == 1 && rate(30, 1, 18, 1, 0, 0, 0).extra == 0);
    assert(rate(30, 1, 18, 1, 0, 2, 2).extra == 0 && rate(30, 1, 18, 1, 0, 1, 2).extra == 1);
    assert(rate(255, 1, 18, 1, 0, 0, 1).extra == 1);
    assert(rate(17, 1, 18, 1, 0, 0, 1).extra == 1 && rate(17, 1, 18, 1, 0, 0, 0).extra == 0);
    assert(rate(30, 1, 18, 1, 0, 0, 1).denominator == 1 && rate(5, 1, 18, 1, 0, 0, 1).extra == 0);
    assert(rate(5, 1, 18, 1, 0, 0, 1).denominator == 11 && rate(16, 1, 18, 1, 0, 0, 1).extra == 0);
    assert(rate(255, 1, 100, 1, 0, 0, 0).extra == 82 && rate(255, 1, 100, 1, 0, 0, 1).extra == 83);
    assert(rate(30, 1, 18, 0, 0, 0, 1).extra == 14); // clamping off
    assert(rate(30, 0, 18, 1, 0, 0, 1).extra == 14); // budget control off
    assert(rate(30, 1, 0, 1, 0, 0, 1).extra == 14 && rate(30, 1, -18, 1, 0, 0, 1).extra == 14);
    assert(rate(30, 1, 1, 1, 0, 0, 0).extra == 0 && rate(30, 1, 1, 1, 0, 0, 0).denominator == 17);
    // A negative threshold promotes codes up to its limit to the limit (+1 outside the boundary).
    assert(rate(3, 1, -18, 1, 1, 0, 0).extra == 1 && rate(3, 1, -18, 1, 1, 0, 1).extra == 2);
    assert(rate(17, 1, -18, 1, 1, 0, 0).extra == 1 && rate(18, 1, -18, 1, 1, 0, 0).extra == 2);
    assert(rate(30, 1, -18, 1, 1, 0, 1).extra == 14 && rate(3, 1, -18, 1, 0, 0, 1).extra == 0);
    assert(rate(3, 1, 18, 1, 1, 0, 1).extra == 0 && rate(3, 1, 18, 1, 1, 0, 1).denominator == 13);
    // An unsupported budget gives no rays and no extra.
    const rrvrrt::Rate unsupported = rate(200, 1, (std::numeric_limits<int>::min)());
    assert(!unsupported.supported && unsupported.denominator == 0 && unsupported.extra == 0);
    assert(rate(200, 0, (std::numeric_limits<int>::min)()).extra == 184); // only when enabled
}
