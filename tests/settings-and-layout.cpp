#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_game_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_ray_density.h"
#include "../src/rr_ray_budget.h"
#include "../src/rr_vrrt_rate.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_sss_mask.h"
#include "../src/rr_responsivity.h"
#include "../src/rr_sky_edge.h"
#include "../src/rr_color_scale.h"
#include "../src/rr_specular_distance.h"
#include "../src/rr_trace_model.h"
#include "../src/rr_hit_condition.h"
#include "../src/rr_water.h"
#include "../src/rr_gate_stats.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// More rays: the readback frame appends the extra rays (bytes 228, 232), the raw requested rays
// (236, 240), the largest raw codes (244, 248) and the requested pixels the reader could not model
// (252); the ray density words end at byte 256, where the gate's non-finite counts start.
static_assert(rrdensity::Limits == 34 && rrdensity::Settings1 == 38 && rrdensity::Extra == 41 &&
              rrdensity::RawRays == 43 && rrdensity::RawMax == 45 && rrdensity::Unsupported == 47 &&
              rrdensity::Count == 48);
static_assert(rrdensity::Offset + 4 * rrdensity::Limits == 200 &&
              rrdensity::Offset + 4 * rrdensity::Extra == 228 &&
              rrdensity::Offset + 4 * rrdensity::RawRays == 236 &&
              rrdensity::Offset + 4 * rrdensity::RawMax == 244 &&
              rrdensity::Offset + 4 * rrdensity::Unsupported == 252 &&
              rrdensity::Offset + 4 * rrdensity::Count == 256);
// The slot's stats buffer: hit distance statistics, ray density words and the gate's non-finite
// counts do not overlap, and all fit the 512 bytes.
static_assert(rrhitcond::StatsOffset + 4 * rrhitcond::StatCount <= rrdensity::Offset &&
              rrdensity::Offset + 4 * rrdensity::Count <= RR_STATS_NON_FINITE_COLOR &&
              RR_STATS_NON_FINITE_COLOR == 256 && RR_STATS_NON_FINITE_OUTPUT == 260 &&
              RR_STATS_NON_FINITE_OUTPUT + 4 <= rrgatestats::StatsBytes &&
              rrgatestats::StatsBytes == 512);

// The ray budget test: the option steps are fixed, unknown values read as Off, and the numbers the
// gate relies on are pinned (list layout of the native apply, the governor's threshold bound).
static_assert(rrraybudget::Off == 0 && rrraybudget::Times15 == 1 && rrraybudget::Times2 == 2 &&
              rrraybudget::OnePerPixel == 3 && rrraybudget::Count == 4);
static_assert(rrraybudget::Valid(rrraybudget::OnePerPixel) == rrraybudget::OnePerPixel &&
              rrraybudget::Valid(rrraybudget::Count) == rrraybudget::Off &&
              rrraybudget::Valid(99u) == rrraybudget::Off);
static_assert(
    std::string_view(rrraybudget::labels[rrraybudget::Off]) == "Off (game's ray budget)" &&
    std::string_view(rrraybudget::labels[rrraybudget::Times15]) == "1.5x the game's rays" &&
    std::string_view(rrraybudget::labels[rrraybudget::Times2]) == "2x the game's rays" &&
    std::string_view(rrraybudget::labels[rrraybudget::OnePerPixel]) == "Up to 1 ray per pixel");
static_assert(rrraybudget::factors[rrraybudget::Off] == 1.0f &&
              rrraybudget::factors[rrraybudget::Times15] == 1.5f &&
              rrraybudget::factors[rrraybudget::Times2] == 2.0f &&
              rrraybudget::factors[rrraybudget::OnePerPixel] == 0.0f);
static_assert(rrraybudget::Resolve == 0x293995352d1f8220ull &&
              rrraybudget::Clear == 0x32cf8b5e62a28a89ull);
static_assert(rrraybudget::ListHeader == 144 && rrraybudget::EntryBytes == 4 &&
              rrraybudget::PixelEntryBytes == 4 && rrraybudget::ConsumerSlack == 512 &&
              rrraybudget::FreshMs == 500 && rrraybudget::RetryMs == 1000);
static_assert(rrraybudget::NegativeLimit == -16 && rrraybudget::ProofFrames == 120 &&
              rrraybudget::TargetFrames == 4 && rrraybudget::UnverifiedLimit == 8 &&
              rrraybudget::UnverifiedStop == 32 && rrraybudget::MaxExtent == 16384 &&
              rrraybudget::Gate::BlockFrames == 300);
// A threshold of -16 or more never adds a ray, positive ones included; INT_MIN (|t| wraps) would.
static_assert(rrraybudget::SafeThreshold(-16) && !rrraybudget::SafeThreshold(-17) &&
              rrraybudget::SafeThreshold(0) && rrraybudget::SafeThreshold(17) &&
              rrraybudget::SafeThreshold(18) && rrraybudget::SafeThreshold(300) &&
              rrraybudget::SafeThreshold((std::numeric_limits<int>::max)()) &&
              !rrraybudget::SafeThreshold((std::numeric_limits<int>::min)()) &&
              !rrraybudget::SafeThreshold((std::numeric_limits<int>::min)() + 1));
// The private ray log: 32 words, one bit per copy the controller recorded into it.
static_assert(rrraybudget::LogWords == 32 &&
              sizeof(rrraybudget::Completed::log) == 4 * rrraybudget::LogWords);
static_assert(rrraybudget::LogResolveTarget == 1 && rrraybudget::LogExecuted == 2 &&
              rrraybudget::LogResolveScene == 4 && rrraybudget::LogApplyTarget == 8 &&
              rrraybudget::LogApplySettings36 == 16 && rrraybudget::LogApplyScene251 == 32 &&
              rrraybudget::LogResolveStats == 64 && rrraybudget::LogApplyScene253 == 128);

namespace rb = rrraybudget;
constexpr int IntMin = (std::numeric_limits<int>::min)(),
              IntMax = (std::numeric_limits<int>::max)();

// Every reason's text (logged and shown verbatim) and key (the 10 s line's `refused` list), one
// assertion per reason, so that swapping two entries does not compile.
static constexpr bool Pinned(rb::Why why, std::string_view text, std::string_view key) {
    return rb::Text(why) == text && rb::keys[unsigned(why)] == key;
}
static_assert(Pinned(rb::Why::None, "none", "none"));
static_assert(Pinned(rb::Why::Mode, "test off", "mode"));
static_assert(Pinned(rb::Why::Stopped, "stopped", "stopped"));
static_assert(Pinned(rb::Why::Unverified, "raised frames were not verified", "unverified"));
static_assert(Pinned(rb::Why::Context, "binding tracker, copy context or state hooks unavailable",
                     "context"));
static_assert(Pinned(rb::Why::Shape, "controller dispatch is not 2x1x1", "shape"));
static_assert(Pinned(rb::Why::Constants, "controller b1/b3 are not proven root constant buffers",
                     "constants"));
static_assert(Pinned(rb::Why::Heap, "controller constants are not in an upload heap", "heap"));
static_assert(Pinned(rb::Why::BudgetBinding, "controller Budget u0 is not a proven table raw view",
                     "budgetBinding"));
static_assert(Pinned(rb::Why::BudgetIdentity,
                     "controller writes a different Budget than the apply reads",
                     "budgetIdentity"));
static_assert(Pinned(rb::Why::Outside, "controller ran outside the RR guide recording", "outside"));
static_assert(Pinned(rb::Why::AfterSR, "controller ran after RR recorded this frame", "afterSR"));
static_assert(Pinned(rb::Why::Repeated, "controller ran twice in one recording", "repeated"));
static_assert(Pinned(rb::Why::Ambiguous, "more than one RR recording on this command list",
                     "ambiguous"));
static_assert(Pinned(rb::Why::AfterApply, "controller ran after the VRRT apply in this recording",
                     "afterApply"));
static_assert(Pinned(rb::Why::Placement,
                     "the VRRT apply did not read the Budget its own resolve wrote", "placement"));
static_assert(Pinned(rb::Why::ListsUnproven, "ray lists not proven at the VRRT apply",
                     "listsUnproven"));
static_assert(Pinned(rb::Why::ListsRoot, "a ray list is a root UAV (no bounds checks)",
                     "listsRoot"));
static_assert(Pinned(rb::Why::ListsView, "a ray list is not a raw table buffer view", "listsView"));
static_assert(Pinned(rb::Why::ListsOverlap, "ray list views overlap", "listsOverlap"));
static_assert(Pinned(rb::Why::ListsSmall, "ray lists hold less than one ray per pixel plus margin",
                     "listsSmall"));
static_assert(Pinned(rb::Why::ListsStable, "waiting for 120 proven ray-list applies",
                     "listsStable"));
static_assert(Pinned(rb::Why::ListsStale, "no recent ray-list proof", "listsStale"));
static_assert(Pinned(rb::Why::ListsChanged, "ray lists changed in this recording", "listsChanged"));
static_assert(Pinned(rb::Why::Evidence, "waiting for 120 measured frames", "evidence"));
static_assert(Pinned(rb::Why::VrrtOff, "the game's VRRT rate control is off", "vrrtOff"));
static_assert(Pinned(rb::Why::Resolution, "render resolution too large for the ray counters",
                     "resolution"));
static_assert(Pinned(rb::Why::TargetUnknown, "the game's target is not known yet",
                     "targetUnknown"));
static_assert(Pinned(rb::Why::StatsUnverified,
                     "the controller's ray statistics are not verified yet", "statsUnverified"));
static_assert(Pinned(rb::Why::RemapUnknown, "the readers' second list segment is not known",
                     "remapUnknown"));
static_assert(Pinned(rb::Why::TargetRange, "the game's target is not in (0,1]", "targetRange"));
static_assert(Pinned(rb::Why::AtCap, "already at the cap (1 ray per requested pixel)", "atCap"));
static_assert(Pinned(rb::Why::Memory, "waiting for GPU memory", "memory"));
static_assert(Pinned(rb::Why::Reconfigure, "RR is reconfiguring", "reconfigure"));
static_assert(Pinned(rb::Why::GovernorCache, "governor cache full; waiting for reconfigure",
                     "governorCache"));
static_assert(Pinned(rb::Why::Governor,
                     "controller threshold beyond the one-ray bound after the governor",
                     "governor"));
static_assert(Pinned(rb::Why::PatchMismatch, "private constants did not reach the controller",
                     "patchMismatch"));
static_assert(Pinned(rb::Why::ListsAfterArming, "ray lists changed after arming",
                     "listsAfterArming"));
static_assert(Pinned(rb::Why::Unsupported, "requested pixels the reader cannot model",
                     "unsupported"));
static_assert(Pinned(rb::Why::StatsMismatch, "the controller's ray statistics lag this frame",
                     "statsMismatch"));
static_assert(Pinned(rb::Why::Overflow,
                     "scheduled rays came within 512 entries of a ray list's capacity",
                     "overflow"));
static_assert(Pinned(rb::Why::Pipeline, "governor pipeline could not be created", "pipeline"));
static_assert(Pinned(rb::Why::Readback, "ray budget readback failed", "readback"));
static_assert(Pinned(rb::Why::Exception, "internal error", "exception"));
static_assert(unsigned(rb::Why::Exception) + 1 == unsigned(rb::Why::Count) &&
              unsigned(rb::Why::Count) == 44 &&
              std::string_view(rb::Text(rb::Why::Count)) == "unknown");

// The governor's premise. A pixel gets no ray when not requested, 1 + extra rays at a code of 16 or
// more (denominator 1) and at most one below (a 1/denominator chance).
static unsigned Rays(const rrvrrt::Rate &r) {
    return r.denominator == 0 ? 0u : r.denominator == 1 ? 1u + r.extra : 1u;
}

// The governor's rule for a raised lobe: a threshold below -16 (INT_MIN included) becomes -16.
static int Governed(int threshold) {
    return threshold < rb::NegativeLimit ? rb::NegativeLimit : threshold;
}

// At one threshold, every request (both high bytes, every raw byte; the game emits codes 0..31),
// flag combination (enabled, promote, clamp) and tile ring position: a pixel never gets more than
// 1 + max(raw - 16, 0) rays. Returns the number of cases; `worst` is the largest extra seen.
static unsigned BoundedRays(int threshold, unsigned &worst) {
    unsigned cases = 0;
    for (unsigned high : {0u, 0xFF00u})
        for (unsigned raw = 0; raw < 256; ++raw)
            for (int flags = 0; flags < 16; ++flags) {
                const unsigned request = high | raw;
                if (request == 0xFFFFu) // not requested
                    continue;
                const int enabled = flags & 1, promote = flags >> 1 & 1, clamp = flags >> 2 & 1,
                          inner = flags >> 3 & 1;
                const auto r = rrvrrt::InverseRate(request, enabled, promote, clamp, threshold,
                                                   inner ? -1 : 1, 0);
                ++cases;
                assert(r.supported && r.raw == raw);
                assert(Rays(r) <= 1 + (raw > 16 ? raw - 16 : 0u));
                worst = (std::max)(worst, r.extra);
            }
    return cases;
}

static void GovernorBounds() {
    // Every threshold of -16 and above keeps the bound, positive ones included.
    unsigned long cases = 0;
    unsigned worst = 0;
    for (int threshold = rb::NegativeLimit; threshold <= 300; ++threshold)
        cases += BoundedRays(threshold, worst);
    cases += BoundedRays(IntMax, worst);
    assert(cases == 318ul * (2 * 256 * 16 - 16));
    assert(worst == 255 - 16); // only a raw code above 16 asks for more than one ray
    // The governor's rule brings every threshold into that range. Below it the transform adds rays
    // (-17 promotes an inner pixel to 2) and INT_MIN cannot even be modelled (|t| wraps).
    for (int t : {IntMin, IntMin + 1, -65536, -1000, -17, -16, 0, 17, 18, 300, IntMax}) {
        assert(rb::SafeThreshold(Governed(t)));
        BoundedRays(Governed(t), worst);
    }
    assert(rrvrrt::InverseRate(0xFF00u | 5, 1, 1, 1, -17, -1, 0).extra == 1); // promote, inner
    assert(!rrvrrt::InverseRate(0xFF00u | 5, 1, 1, 1, IntMin, -1, 0).supported);
    assert(!rb::SafeThreshold(-17) && !rb::SafeThreshold(IntMin));
    // The governor's fallback, 17: with clamping on at most one ray per pixel for every raw code,
    // with clamping off the game's own rays (a positive threshold then changes nothing).
    for (unsigned raw = 0; raw < 256; ++raw)
        for (int flags = 0; flags < 4; ++flags) {
            const int promote = flags & 1, boundary = flags & 2 ? -1 : 1;
            const auto on = rrvrrt::InverseRate(raw, 1, promote, 1, 17, boundary, 0);
            const auto off = rrvrrt::InverseRate(raw, 1, promote, 0, 17, boundary, 0);
            assert(on.supported && Rays(on) <= 1);
            assert(Rays(off) == Rays(rrvrrt::InverseRate(raw, 0, 0, 0, 0, 0, 0)));
        }
}

// A proven apply: dispatch groups cover the extent, both ray lists hold `entries` entries and the
// pixel list 4 bytes per pixel of the extent.
static rb::Lists Proven(std::uint64_t entries, std::uint32_t w = 1920, std::uint32_t h = 1080) {
    rb::Lists l;
    l.w = w;
    l.h = h;
    l.gx = (w + 15) / 16;
    l.gy = (h + 15) / 16;
    l.why = rb::Why::None;
    for (unsigned i = 0; i < 2; ++i)
        l.v[i] = {rb::Binding::Table, 0x1000 + i, 0, rb::ListHeader + entries * rb::EntryBytes};
    l.v[2] = {rb::Binding::Table, 0x2000, 0, rb::PixelEntryBytes * l.Pixels()};
    l.v[3] = {rb::Binding::Table, 0x3000, 0, 16};
    l.budget_resource = 0x4000;
    return l;
}

// A completed reader frame: lobe 1 requests half the pixels at full rate, lobe 2 all of them; the
// game's target is `target`, its rate control is on and both thresholds are 9. The controller
// copied the resolve's ray statistics (x = 0, covering the reader's 0 raw rays) and the readers'
// second segment (0: none).
static rb::Completed Measured(const rb::Lists &l, float target = 0.4f) {
    rb::Completed c;
    c.lists = l;
    const std::uint32_t pixels = l.w * l.h;
    c.rates[1] = pixels / 2;
    c.rates[0] = pixels - pixels / 2;
    c.rates[rrdensity::Buckets + 1] = pixels;
    c.rates[rrdensity::Settings1] = 1;
    c.rates[rrdensity::Limits] = c.rates[rrdensity::Limits + 2] = 9;
    c.log[0] = c.log[1] = std::bit_cast<std::uint32_t>(target);
    c.logged = rb::LogResolveTarget | rb::LogResolveStats | rb::LogApplyScene253;
    return c;
}

// A gate that saw `applies` proven applies (at times 0..applies-1) and `frames` measured frames.
static rb::Gate Primed(const rb::Lists &l, unsigned applies = rb::ProofFrames,
                       unsigned frames = rb::ProofFrames, float target = 0.4f) {
    rb::Gate g;
    for (unsigned i = 0; i < applies; ++i)
        g.Apply(l, i);
    for (unsigned i = 0; i < frames; ++i)
        assert(g.Complete(Measured(l, target)) == rb::Why::None);
    return g;
}

// Primed(l), armed for Times2 with both lobes raised: `plan` is what it patched.
static rb::Gate Armed(const rb::Lists &l, rb::Plan &plan) {
    auto g = Primed(l);
    plan = g.Decide(rb::Times2, rb::ProofFrames - 1, nullptr);
    assert(plan.mask == 3);
    return g;
}

// The completed frame of a recording whose resolve was patched with `plan` and whose apply read the
// raised Budget of that plan, with the executed constants copied back.
static rb::Completed Raised(const rb::Lists &l, const rb::Plan &plan) {
    auto c = Measured(l);
    c.patched = c.budget = plan.mask;
    c.written = plan.bits;
    c.armed = plan.armed;
    c.logged |= rb::LogExecuted;
    c.log[4] = plan.bits[0];
    c.log[5] = plan.bits[1];
    return c;
}

static bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-6f;
}

// Decide's refusals, in the order they are checked.
static void GateRefusals() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const std::uint64_t now = rb::ProofFrames - 1; // the time of the last apply of Primed()
    rb::Gate fresh;
    assert(fresh.Decide(rb::Off, 0, nullptr).why == rb::Why::Mode);
    assert(fresh.Decide(99u, 0, nullptr).why == rb::Why::Mode); // unknown steps read as Off
    assert(fresh.Decide(rb::Times2, 0, nullptr).why == rb::Why::ListsUnproven);
    assert(fresh.Decide(rb::Times2, 0, nullptr).mask == 0);
    // Raised recordings that retired without reader stats: from 8 in a row on, Decide waits (right
    // after a stop, before the proofs); any measured frame ends the run.
    rb::Gate unverified;
    for (unsigned i = 0; i < rb::UnverifiedLimit; ++i)
        assert(unverified.Unverified() == rb::Why::None);
    assert(unverified.Decide(rb::Times2, 0, nullptr).why == rb::Why::Unverified);
    assert(unverified.Decide(rb::Off, 0, nullptr).why == rb::Why::Mode);
    assert(unverified.Stop(rb::Why::Readback));
    assert(unverified.Decide(rb::Times2, 0, nullptr).why == rb::Why::Stopped);
    auto g = Primed(l);
    for (unsigned i = 1; i < rb::UnverifiedLimit; ++i)
        g.Unverified();
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    g.Unverified();
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Unverified &&
           g.Decide(rb::Times2, now, nullptr).mask == 0);
    assert(g.Complete(Measured(l)) == rb::Why::None); // the run is not sticky
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // Only a measured raised frame verifies raising: the 32nd raised recording without one since
    // the last stops the test for the session, however many unraised frames came in between.
    for (unsigned i = rb::UnverifiedLimit; i < rb::UnverifiedStop - 1; ++i) {
        assert(g.Unverified() == rb::Why::None);
        assert(g.Complete(Measured(l)) == rb::Why::None);
    }
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None && g.Stopped() == rb::Why::None);
    assert(g.Unverified() == rb::Why::Unverified && g.Stopped() == rb::Why::Unverified);
    assert(g.Unverified() == rb::Why::None); // only the call that newly stops reports
    assert(g.Complete(Measured(l)) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Stopped);
    // Fewer than 120 consecutive proven applies at one extent: waiting.
    g = Primed(l, rb::ProofFrames - 1);
    assert(g.Streak() == rb::ProofFrames - 1 &&
           g.Decide(rb::Times2, now - 1, nullptr).why == rb::Why::ListsStable);
    g.Apply(l, now);
    assert(g.Streak() == rb::ProofFrames && g.Proof() == l);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // An unproven apply restarts the streak.
    auto broken = l;
    broken.why = rb::Why::ListsView;
    g.Apply(broken, now + 1);
    assert(g.Streak() == 0 && g.Decide(rb::Times2, now + 1, nullptr).why == rb::Why::ListsView);
    g.Apply(l, now + 2);
    assert(g.Streak() == 1 && g.Decide(rb::Times2, now + 2, nullptr).why == rb::Why::ListsStable);
    // A proof of an empty or impossible extent is no proof, so a raised frame always has an extent
    // to check (16384 is the largest D3D12 texture side).
    for (auto [w, h] : {std::pair{0u, 1080u},
                        {1920u, 0u},
                        {0u, 0u},
                        {16385u, 1080u},
                        {1920u, 16385u},
                        {0xFFFFFFFFu, 0xFFFFFFFFu}}) {
        rb::Gate d;
        const auto degenerate = Proven(1920ull * 1088 + 100000, w, h);
        for (unsigned i = 0; i < 2 * rb::ProofFrames; ++i)
            d.Apply(degenerate, i);
        assert(d.Streak() == 0 && d.Proof().why == rb::Why::ListsUnproven);
        assert(d.Decide(rb::Times2, 2 * rb::ProofFrames - 1, nullptr).why ==
               rb::Why::ListsUnproven);
        assert(!d.KnownBudget(degenerate.budget_resource, 0));
    }
    rb::Gate largest_extent;
    largest_extent.Apply(Proven(1ull << 30, 16384, 16384), 0);
    assert(largest_extent.Streak() == 1 && largest_extent.Proof().why == rb::Why::None);
    // 120 proofs without measured frames: evidence missing.
    auto e = Primed(l, rb::ProofFrames, 0);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::Evidence);
    e = Primed(l, rb::ProofFrames, rb::ProofFrames - 1);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::Evidence);
    assert(e.Complete(Measured(l)) == rb::Why::None);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // A frame the reader did not count (it returned early and left the zeroed stats; when it runs
    // it counts every pixel, not-requested ones included) measures nothing: it changes no
    // evidence, streak or count. 119 of them and one measured frame are one measured frame.
    auto zero = Measured(l);
    zero.rates = {};
    assert(!rb::Counted(zero.rates) && rb::Counted(Measured(l).rates));
    for (unsigned i = 0; i < 2 * rrdensity::Buckets; ++i) {
        auto one = zero;
        one.rates[i] = 1; // any pixel count, bucket 0 included
        assert(rb::Counted(one.rates));
    }
    zero.rates[rrdensity::RawRays] = zero.rates[rrdensity::Unsupported] = 1; // not pixel counts
    assert(!rb::Counted(zero.rates));
    e = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        assert(e.Complete(zero) == rb::Why::None);
    assert(e.Complete(Measured(l)) == rb::Why::None);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::Evidence);
    auto plain = Measured(l); // measured, without the statistics or the second segment
    plain.logged = rb::LogResolveTarget;
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        e.Complete(plain);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        e.Complete(zero); // they carry the statistics' copy, but count for nothing
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    auto stats_only = Measured(l);
    stats_only.logged &= ~rb::LogApplyScene253;
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        e.Complete(stats_only);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::RemapUnknown);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        e.Complete(zero); // nor for the second segment
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::RemapUnknown);
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        e.Complete(Measured(l));
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // Its target, its rate control word (0) and its extent change nothing either.
    zero.log[0] = std::bit_cast<std::uint32_t>(0.7f);
    zero.lists = Proven(1920ull * 1088 + 100000, 1280, 720);
    assert(e.Complete(zero) == rb::Why::None);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // Nor does it end a run of raised recordings without stats.
    for (unsigned i = 1; i < rb::UnverifiedLimit; ++i)
        e.Unverified();
    e.Complete(zero);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    e.Unverified();
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::Unverified);
    // Proofs go stale after FreshMs, and a clock that went backwards is stale too.
    g = Primed(l);
    assert(g.Decide(rb::Times2, now + rb::FreshMs, nullptr).why == rb::Why::None);
    assert(g.Decide(rb::Times2, now + rb::FreshMs + 1, nullptr).why == rb::Why::ListsStale);
    assert(g.Decide(rb::Times2, now - 1, nullptr).why == rb::Why::ListsStale);
    // This recording's own apply must agree with the streak: proven, same extent, no smaller list.
    auto other = l;
    other.v[0].bytes -= rb::EntryBytes;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.v[1].bytes -= rb::EntryBytes;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.v[2].bytes -= 1; // the pixel list
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.v[3].bytes -= 1; // the counter
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.why = rb::Why::ListsUnproven;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = Proven(1920ull * 1088 + 100000, 1280, 720);
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.h = 1079;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.gx += 1; // another dispatch grid: not the apply the streak was proven for
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.gy -= 1;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.v[0].bytes += 4096;
    other.v[0].resource = 0x9000; // larger lists in other buffers are fine
    other.v[2].bytes += 4;
    other.v[3].bytes += 4;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None); // no apply in this recording
    // Measured frames at another extent than the proofs do not count.
    g = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(Measured(Proven(1920ull * 1088 + 100000, 1280, 720)));
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Evidence);
    // The game's rate control must be on.
    g = Primed(l);
    auto off = Measured(l);
    off.rates[rrdensity::Settings1] = 0;
    assert(g.Complete(off) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::VrrtOff);
    assert(g.Complete(Measured(l)) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // The ray counters hold W*H*240 in 32 bits (a pixel's raw rays are at most 255 - 15): 4230^2
    // and 16384x1092 are the largest such extents, 4231^2 and 16384x1093 too large.
    static_assert(rb::MaxRawRays == 240);
    const auto largest = Proven(4231ull * 4231 + 100000, 4230, 4230);
    const auto too_large = Proven(4231ull * 4231 + 100000, 4231, 4231);
    const auto wide = Proven(16384ull * 1093 + 100000, 16384, 1092);
    const auto too_wide = Proven(16384ull * 1093 + 100000, 16384, 1093);
    assert(Primed(wide).Decide(rb::Times2, now, nullptr).mask == 3);
    assert(Primed(too_wide).Decide(rb::Times2, now, nullptr).why == rb::Why::Resolution);
    assert(Primed(largest).Decide(rb::Times2, now, nullptr).mask == 3);
    assert(Primed(too_large).Decide(rb::Times2, now, nullptr).why == rb::Why::Resolution);
    // The native target must read the same four times in a row.
    g = Primed(l);
    for (unsigned i = 1; i < rb::TargetFrames; ++i) { // it changed: start counting again
        assert(g.Complete(Measured(l, 0.5f)) == rb::Why::None);
        assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::TargetUnknown);
    }
    assert(g.Complete(Measured(l, 0.5f)) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    g = Primed(l);
    auto unlogged = Measured(l, 0.5f); // frames without the resolve's copy never count
    unlogged.logged = 0;
    g.Complete(Measured(l, 0.5f));
    for (unsigned i = 0; i < 10; ++i)
        g.Complete(unlogged);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::TargetUnknown);
    g = Primed(l, rb::ProofFrames, 0); // no frame ever logged the target
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(unlogged);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::TargetUnknown);
    // Then the controller's copies of the resolve's ray statistics (x per lobe) must have covered
    // the reader's raw rays in 120 frames, and the readers' second segment must have been copied in
    // 120 frames at this extent.
    auto bare = Measured(l);
    bare.logged = rb::LogResolveTarget;
    g = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(bare);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    bare.logged |= rb::LogResolveStats;
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(bare);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::RemapUnknown);
    bare.logged |= rb::LogApplyScene253;
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        g.Complete(bare);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::RemapUnknown);
    g.Complete(bare);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // The statistics count: a frame whose x is lower than the raw rays (either lobe) starts it
    // over, a frame without the copy leaves it, an x equal to the raw rays is enough.
    auto lag = Measured(l);
    lag.rates[rrdensity::RawRays + 1] = 7;
    lag.log[26] = 6;
    g.Complete(lag);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    auto equal = Measured(l);
    equal.rates[rrdensity::RawRays] = equal.log[25] = 5;
    equal.rates[rrdensity::RawRays + 1] = 7;
    equal.log[26] = 9;
    auto no_stats = Measured(l);
    no_stats.logged &= ~rb::LogResolveStats;
    no_stats.rates[rrdensity::RawRays] = 100; // not compared: no copy
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        g.Complete(equal);
    for (unsigned i = 0; i < 50; ++i)
        g.Complete(no_stats);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    g.Complete(equal);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    lag = Measured(l);
    lag.rates[rrdensity::RawRays] = 7;
    lag.log[25] = 6;
    g.Complete(lag);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    // The second segment's count starts over at another extent.
    auto no_remap = Measured(l);
    no_remap.logged &= ~rb::LogApplyScene253;
    g = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(Measured(Proven(1920ull * 1088 + 100000, 1280, 720)));
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        g.Complete(Measured(l));
    g.Complete(no_remap);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::RemapUnknown);
    g.Complete(Measured(l));
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    // A root list is never armed, however long it is observed.
    rb::Gate r;
    auto root = l;
    root.why = rb::Why::ListsRoot;
    for (unsigned i = 0; i < 200; ++i)
        r.Apply(root, i);
    assert(r.Streak() == 0 && r.Decide(rb::Times2, 199, nullptr).why == rb::Why::ListsRoot);
    // A resolution change starts the streak over.
    g = Primed(l);
    auto y = Proven(1920ull * 1088 + 100000, 2560, 1440);
    g.Apply(y, now + 1);
    assert(g.Streak() == 1 && g.Decide(rb::Times15, now + 1, nullptr).why == rb::Why::ListsStable);
}

// What the gate arms, per lobe.
static void GatePlans() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const std::uint64_t now = rb::ProofFrames - 1;
    const std::uint64_t pixels = 1920ull * 1080;
    auto g = Primed(l);
    // Lobe 1 requests half the pixels, so its target stops at 0.5 (the requested share).
    auto p = g.Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 3);
    assert(Near(p.native[0], 0.4f) && Near(p.native[1], 0.4f));
    assert(Near(p.target[0], 0.5f) && Near(p.target[1], 0.8f));
    assert(p.bits[0] == std::bit_cast<std::uint32_t>(p.target[0]) &&
           p.bits[1] == std::bit_cast<std::uint32_t>(p.target[1]));
    // It carries what the governor and the completed-frame checks need: the extent, each list's
    // capacity and, per raised lobe, the headroom (capacity - W*H - slack) and the margin.
    // Its snapshot holds the u3/u2 sizes Decide checked, the streak's smallest.
    assert(p.armed.w == 1920 && p.armed.h == 1080);
    assert(p.armed.capacity[0] == l.Entries(0) && p.armed.capacity[1] == l.Entries(1));
    assert(p.armed.pixels == l.v[2].bytes && p.armed.counter == l.v[3].bytes);
    assert(l.Entries(0) == 2188960 && p.headroom[0] == 2188960 - pixels - rb::ConsumerSlack &&
           p.headroom[1] == p.headroom[0] && p.headroom[0] == 114848);
    assert(p.armed.margin[0] == 0 && p.armed.margin[1] == 0);
    auto bigger = l; // a later apply with larger u3/u2 does not raise the snapshot
    bigger.v[2].bytes += 64;
    bigger.v[3].bytes += 64;
    auto ping = Primed(l);
    ping.Apply(bigger, now + 1);
    const auto pong_plan = ping.Decide(rb::Times2, now + 1, nullptr);
    assert(pong_plan.armed.pixels == l.v[2].bytes && pong_plan.armed.counter == l.v[3].bytes);
    ping.Apply(Proven(1920ull * 1088 + 100000), now + 2);
    auto lower = l;
    lower.v[3].bytes = 8;
    ping.Apply(lower, now + 3);
    assert(ping.Decide(rb::Times2, now + 3, nullptr).armed.counter == 8);
    assert(rb::Plan{}.armed.w == 0 && g.Decide(rb::Off, now, nullptr).armed.w == 0);
    p = g.Decide(rb::Times15, now, nullptr);
    assert(p.mask == 3 && Near(p.target[0], 0.5f) && Near(p.target[1], 0.6f));
    p = g.Decide(rb::OnePerPixel, now, nullptr); // factor 0: up to one ray per requested pixel
    assert(p.mask == 3 && Near(p.target[0], 0.5f) && Near(p.target[1], 1.0f));
    // The pong buffer: other resources of the same size arm too, and their Budget is known once
    // seen.
    auto pong = l;
    pong.v[0].resource = 0x9000;
    pong.v[1].resource = 0x9001;
    pong.budget_resource = 0x9002;
    assert(g.Decide(rb::Times2, now, &pong).why == rb::Why::None);
    assert(g.KnownBudget(0x4000, 0) && !g.KnownBudget(0x9002, 0) && !g.KnownBudget(0, 0));
    assert(!g.KnownBudget(0x4000, 16)); // the offset is part of the identity
    // Ping-pong buffers keep the streak and are all remembered.
    rb::Gate h;
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto x = l;
        if (i & 1) {
            x.v[0].resource += 0x100;
            x.v[1].resource += 0x100;
            x.budget_resource += 0x100;
        }
        h.Apply(x, i);
        assert(h.Complete(Measured(x)) == rb::Why::None);
    }
    assert(h.Streak() == rb::ProofFrames && h.KnownBudget(0x4000, 0) && h.KnownBudget(0x4100, 0));
    assert(h.Decide(rb::Times15, now, nullptr).mask == 3);
    // Only the four most recent Budgets are remembered.
    for (std::uint64_t k = 1; k <= 4; ++k) {
        auto x = l;
        x.budget_resource = 0x5000 + k;
        h.Apply(x, now + k);
    }
    assert(!h.KnownBudget(0x4000, 0) && !h.KnownBudget(0x4100, 0) && h.KnownBudget(0x5001, 0) &&
           h.KnownBudget(0x5004, 0));
    // The target must be in (0, 1].
    for (float bad : {0.0f, -0.5f, 1.5f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        g = Primed(l, rb::ProofFrames, rb::ProofFrames, bad);
        assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::TargetRange);
        assert(g.Decide(rb::Times2, now, nullptr).mask == 0);
    }
    g = Primed(l, rb::ProofFrames, rb::ProofFrames, 1.0f); // exactly 1 is in range, but at the cap
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::AtCap);
    // Lobes are decided one by one: the first reason is kept only when no lobe arms. A lobe left
    // alone gets no headroom (the governor leaves it alone), but its capacity is still known.
    g = Primed(l);
    auto mixed = Measured(l);
    mixed.log[0] = std::bit_cast<std::uint32_t>(0.5f); // lobe 1 already at its requested share
    for (unsigned i = 0; i < rb::TargetFrames; ++i)
        g.Complete(mixed);
    p = g.Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 2 && p.target[0] == 0 &&
           Near(p.target[1], 0.4f * 2));
    assert(p.headroom[0] == 0 && p.headroom[1] == 114848 && p.armed.capacity[0] == l.Entries(0));
    assert(p.armed.margin[0] == 0 && p.armed.w == 1920);
    mixed.log[1] = std::bit_cast<std::uint32_t>(0.0f);
    for (unsigned i = 0; i < rb::TargetFrames; ++i)
        g.Complete(mixed);
    p = g.Decide(rb::Times2, now, nullptr); // lobe 1 at the cap, lobe 2 out of range
    assert(p.why == rb::Why::AtCap && p.mask == 0);
    // Lists that hold less than one ray per pixel of the extent plus margin and slack never arm.
    // The extent counts, not the dispatch grid (threads past the extent append nothing).
    auto undersized = Proven(pixels);
    g = Primed(undersized);
    assert(g.Decide(rb::OnePerPixel, now, nullptr).why == rb::Why::ListsSmall);
    auto exact = Proven(pixels + rb::ConsumerSlack); // W*H + slack: just enough
    assert(exact.Pixels() == pixels && std::uint64_t(exact.gx) * 16 * exact.gy * 16 > pixels);
    p = Primed(exact).Decide(rb::Times2, now, nullptr);
    assert(p.mask == 3 && p.headroom[0] == 0 && p.headroom[1] == 0);
    exact = Proven(pixels + rb::ConsumerSlack - 1);
    assert(Primed(exact).Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    // The headroom saturates at the governor's 32 bits.
    p = Primed(Proven(pixels + rb::ConsumerSlack + 0xFFFFFFFEull)).Decide(rb::Times2, now, nullptr);
    assert(p.mask == 3 && p.headroom[0] == 0xFFFFFFFEu);
    p = Primed(Proven(pixels + rb::ConsumerSlack + 0x100000000ull))
            .Decide(rb::Times2, now, nullptr);
    assert(p.mask == 3 && p.headroom[0] == 0xFFFFFFFFu && p.headroom[1] == 0xFFFFFFFFu);
    assert(p.armed.capacity[0] == pixels + rb::ConsumerSlack + 0x100000000ull);
    // The smallest list of the streak counts, not the last one.
    g = Primed(l, rb::ProofFrames - 1);
    g.Apply(Proven(pixels + rb::ConsumerSlack - 1), now);
    assert(g.Streak() == rb::ProofFrames);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(Measured(l));
    assert(g.Capacity(0) == pixels + rb::ConsumerSlack - 1);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    // u3 is lobe 1's pixel list: lobe 1 needs 4 bytes per pixel of the extent there; both lobes
    // need a 4-byte counter.
    auto pixel_list = l;
    pixel_list.v[2].bytes -= 1;
    p = Primed(pixel_list).Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 2);
    auto counter = l;
    counter.v[3].bytes = 3;
    assert(Primed(counter).Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    counter.v[3].bytes = 4;
    assert(Primed(counter).Decide(rb::Times2, now, nullptr).mask == 3);
    // Raw requested rays widen the margin the lists must hold: twice their largest per-frame sum.
    g = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto f = Measured(l);
        f.rates[rrdensity::RawRays] = f.log[25] = 10 + i % 3;
        f.rates[rrdensity::RawRays + 1] = f.log[26] = 3;
        g.Complete(f);
    }
    assert(g.Margin(0) == 24 && g.Margin(1) == 6);
    assert(g.Hard(0) && g.Hard(1) && !g.Hard(2));
    p = g.Decide(rb::Times2, now, nullptr);
    assert(p.mask == 3 && p.armed.margin[0] == 24 && p.armed.margin[1] == 6 &&
           p.headroom[0] == 114848);
    auto tight = Proven(pixels + rb::ConsumerSlack + 23); // one entry short of the margin
    g = Primed(tight, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto f = Measured(tight);
        f.rates[rrdensity::RawRays] = f.log[25] = 12;
        g.Complete(f);
    }
    assert(g.Margin(0) == 24 && !g.Hard(0) && g.Hard(1));
    p = g.Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 2 && p.armed.margin[0] == 0 && p.headroom[0] == 0);
    // The maximum decays: each block of BlockFrames frames halves it before new frames count.
    g = Primed(l, rb::ProofFrames, 0);
    auto burst = Measured(l);
    burst.rates[rrdensity::RawRays] = burst.log[25] = 1000;
    burst.rates[rrdensity::RawRays + 1] = burst.log[26] = 41;
    g.Complete(burst);
    for (unsigned i = 1; i < rb::Gate::BlockFrames; ++i)
        g.Complete(Measured(l));
    assert(g.Margin(0) == 2000 && g.Margin(1) == 82); // the first block
    g.Complete(Measured(l));
    assert(g.Margin(0) == 1000 && g.Margin(1) == 40); // the second block: halved (rounded down)
    for (unsigned i = 1; i < rb::Gate::BlockFrames; ++i)
        g.Complete(Measured(l));
    assert(g.Margin(0) == 1000);
    g.Complete(Measured(l));
    assert(g.Margin(0) == 500 && g.Margin(1) == 20);
    auto rise = Measured(l); // a frame above the halved maximum raises it at once
    rise.rates[rrdensity::RawRays] = rise.log[25] = 300;
    g.Complete(rise);
    assert(g.Margin(0) == 600 && g.Margin(1) == 20);
    for (unsigned i = 0; i < 12 * rb::Gate::BlockFrames; ++i)
        g.Complete(Measured(l));
    assert(g.Margin(0) == 0 && g.Margin(1) == 0);
    // Frames the reader did not count do not advance the blocks.
    auto uncounted = Measured(l);
    uncounted.rates = {};
    for (unsigned i = 0; i < 2 * rb::Gate::BlockFrames; ++i)
        g.Complete(uncounted);
    assert(g.Margin(0) == 0);
    g.Complete(burst);
    for (unsigned i = 0; i < 2 * rb::Gate::BlockFrames; ++i)
        g.Complete(uncounted);
    assert(g.Margin(0) == 2000);
    // It starts over with the extent.
    g.Complete(burst);
    assert(g.Margin(0) == 2000);
    g.Complete(Measured(Proven(1920ull * 1088 + 100000, 1280, 720)));
    assert(g.Margin(0) == 0 && g.Margin(1) == 0);
    // The requested share is the smallest over two blocks of BlockFrames frames.
    g = Primed(l, rb::ProofFrames, 0);
    auto sparse = Measured(l);
    sparse.rates[1] = 1000; // lobe 1 requests few pixels in this block
    sparse.rates[0] = l.w * l.h - 1000;
    for (unsigned i = 0; i < rb::Gate::BlockFrames; ++i)
        g.Complete(sparse);
    assert(g.Requested(0) < 0.001);
    for (unsigned i = 0; i < rb::Gate::BlockFrames; ++i)
        g.Complete(Measured(l)); // the sparse block moves to the previous block
    assert(g.Requested(0) < 0.001);
    g.Complete(Measured(l)); // and is forgotten one block later
    assert(Near(float(g.Requested(0)), 0.5f) && g.Requested(1) == 1.0);
    // The readers' second segment (cb3 register 253 .x) bounds both lists: the capacity is the
    // smallest nonzero value of the frames at this extent when it is below the entries (0: none).
    g = Primed(l, rb::ProofFrames, 0);
    auto remap = Measured(l);
    remap.log[28] = 2100000;
    g.Complete(remap);
    remap.log[28] = 0;
    for (unsigned i = 1; i < rb::ProofFrames; ++i)
        g.Complete(remap);
    assert(g.Capacity(0) == 2100000 && g.Capacity(1) == 2100000 && g.Capacity(2) == 0);
    p = g.Decide(rb::Times2, now, nullptr);
    assert(p.mask == 3 && p.armed.capacity[0] == 2100000 && p.armed.capacity[1] == 2100000 &&
           p.headroom[0] == 2100000 - pixels - rb::ConsumerSlack);
    remap.log[28] = 2100001; // the smallest one is kept
    g.Complete(remap);
    assert(g.Capacity(0) == 2100000);
    remap.log[28] = std::uint32_t(pixels + rb::ConsumerSlack - 1);
    g.Complete(remap);
    assert(g.Capacity(1) == pixels + rb::ConsumerSlack - 1 && !g.Hard(0) && !g.Hard(1));
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    remap.lists = Proven(1920ull * 1088 + 100000, 1280, 720); // another extent forgets it
    g.Complete(remap);
    g.Complete(Measured(l));
    assert(g.Capacity(0) == l.Entries(0));
    g = Primed(l, rb::ProofFrames, 0);
    remap = Measured(l);
    remap.log[28] = 0xFFFFFFFFu; // above the entries: the entries stay the capacity
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(remap);
    assert(g.Capacity(0) == l.Entries(0) && g.Decide(rb::Times2, now, nullptr).mask == 3);
    remap.logged &= ~rb::LogApplyScene253; // a value that was not copied is no value
    remap.log[28] = 1;
    g.Complete(remap);
    assert(g.Capacity(0) == l.Entries(0));
}

// Completed frames can stop the test for the session; none of the stops is lifted again.
static void GateStops() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const std::uint64_t now = rb::ProofFrames - 1;
    // Stop is sticky: the first call reports it, the reason stays.
    rb::Gate s;
    assert(s.Stopped() == rb::Why::None && !s.Stop(rb::Why::None) && s.Stopped() == rb::Why::None);
    assert(s.Stop(rb::Why::Pipeline) && !s.Stop(rb::Why::Exception));
    assert(s.Stopped() == rb::Why::Pipeline);
    auto g = Primed(l);
    assert(g.Decide(rb::Times2, now, nullptr).mask == 3);
    assert(g.Stop(rb::Why::Exception));
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Stopped);
    assert(g.Decide(rb::Off, now, nullptr).why == rb::Why::Mode); // Off still reads as Off
    // A raised frame that passes every check, also with any threshold of -16 or more on a raised
    // lobe (the game runs at 15..18 natively).
    rb::Plan plan;
    g = Armed(l, plan);
    auto c = Raised(l, plan);
    for (int t : {-16, 0, 17, 18, 300, IntMax}) {
        c.rates[rrdensity::Limits] = c.rates[rrdensity::Limits + 2] = std::uint32_t(t);
        assert(g.Complete(c) == rb::Why::None);
    }
    assert(g.Stopped() == rb::Why::None && g.Decide(rb::Times2, now, nullptr).mask == 3);
    // The governor left a raised lobe's threshold below -16, or at INT_MIN.
    c.rates[rrdensity::Limits + 2] = std::bit_cast<std::uint32_t>(IntMin);
    c.budget = 1; // lobe 2's apply read the game's own Budget: its threshold is the game's business
    assert(g.Complete(c) == rb::Why::None && g.Stopped() == rb::Why::None);
    c.budget = 3;
    assert(g.Complete(c) == rb::Why::Governor && g.Stopped() == rb::Why::Governor);
    assert(g.Complete(c) == rb::Why::None); // only the frame that newly stops reports
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Stopped);
    g = Armed(l, plan);
    c.rates[rrdensity::Limits + 2] = 9;
    c.rates[rrdensity::Limits] = std::uint32_t(-17);
    assert(g.Complete(c) == rb::Why::Governor);
    // The constants the controller executed differ from the ones written.
    g = Primed(l);
    const auto q = g.Decide(rb::Times15, now, nullptr);
    assert(q.mask == 3);
    auto d = Raised(l, q);
    d.budget = 0;
    assert(g.Complete(d) == rb::Why::None);
    auto d1 = d;
    d1.patched = 1; // only lobe 1 patched: lobe 2's words are not compared
    d1.log[5] ^= 1;
    assert(g.Complete(d1) == rb::Why::None);
    auto d2 = d;
    d2.logged &= ~rb::LogExecuted; // the copy after the patch never ran
    assert(g.Complete(d2) == rb::Why::PatchMismatch && g.Stopped() == rb::Why::PatchMismatch);
    g = Primed(l);
    d.log[5] ^= 1;
    assert(g.Complete(d) == rb::Why::PatchMismatch);
    // Raw requested rays beyond the margin the lists were armed with do not stop the test: the
    // governor bounds each frame by its own statistics (the summary counts these frames).
    rb::Gate m = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto f = Measured(l);
        f.rates[rrdensity::RawRays] = f.log[25] = 10;
        m.Complete(f);
    }
    const auto margin_plan = m.Decide(rb::Times2, now, nullptr);
    assert(margin_plan.mask == 3 && margin_plan.armed.margin[0] == 20 &&
           margin_plan.armed.margin[1] == 0);
    auto grown = Raised(l, margin_plan);
    grown.rates[rrdensity::RawRays] = grown.log[25] = 50000;
    grown.rates[rrdensity::RawRays + 1] = grown.log[26] = 1;
    assert(m.Complete(grown) == rb::Why::None && m.Stopped() == rb::Why::None);
    // A raised frame is judged by its own apply's lists, which must be the ones its plan armed
    // (Completed::armed): proven (a failed or missing proof stops), the armed extent, no raised
    // lobe's list or counter smaller than at arming (the pixel list counts for lobe 1), no second
    // segment below the capacity.
    auto changed = [&](auto edit) {
        rb::Plan armed;
        auto gate = Armed(l, armed);
        auto frame = Raised(l, armed);
        edit(frame);
        const auto why = gate.Complete(frame);
        assert(gate.Stopped() == why);
        return why;
    };
    const auto after = rb::Why::ListsAfterArming;
    assert(changed([](rb::Completed &) {}) == rb::Why::None);
    assert(changed([](rb::Completed &f) { f.lists.why = rb::Why::ListsRoot; }) == after);
    assert(changed([](rb::Completed &f) { f.lists = rb::Lists{}; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.w = 1919; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.h = 1081; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.w = f.lists.h = 0; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.v[0].bytes -= rb::EntryBytes; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.v[1].bytes -= rb::EntryBytes; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.v[1].binding = rb::Binding::Root; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.v[2].bytes -= 1; }) == after);
    assert(changed([](rb::Completed &f) { f.lists.v[3].bytes -= 1; }) == after);
    assert(changed([](rb::Completed &f) { f.log[28] = 2188959; }) == after);
    assert(changed([](rb::Completed &f) { f.log[28] = 2188960; }) == rb::Why::None);
    assert(changed([](rb::Completed &f) {
               f.log[28] = 1; // not copied: no value
               f.logged &= ~rb::LogApplyScene253;
           }) == rb::Why::None);
    assert(changed([](rb::Completed &f) {
               f.lists.v[0].bytes += 4096; // larger lists, other buffers, another dispatch grid
               f.lists.v[0].resource = 0x9000;
               f.lists.v[2].bytes += 4;
               f.lists.gx = f.lists.gy = 1;
           }) == rb::Why::None);
    assert(changed([](rb::Completed &f) {
               f.patched = f.budget = 2; // lobe 1 was not raised: its lists are the game's
               f.lists.v[0].bytes -= rb::EntryBytes;
               f.lists.v[2].bytes -= 1;
           }) == rb::Why::None);
    assert(changed([](rb::Completed &f) {
               f.patched = f.budget = 2;
               f.lists.v[3].bytes -= 1;
           }) == after);
    assert(changed([](rb::Completed &f) {
               f.patched = 0; // the apply read a raised Budget of an earlier resolve
               f.budget = 1;
               f.lists.v[0].bytes -= rb::EntryBytes;
           }) == after);
    // A raised frame without the snapshot of the plan that raised it (the controller did not
    // supply one) has nothing to match.
    assert(changed([](rb::Completed &f) { f.armed = rb::Armed{}; }) == after);
    assert(changed([](rb::Completed &f) {
               f.armed = rb::Armed{};
               f.lists.w = f.lists.h = 0;
           }) == after);
    assert(changed([](rb::Completed &f) { f.armed.w = 0; }) == after);
    assert(changed([](rb::Completed &f) { f.armed.h = 1081; }) == after);
    assert(changed([](rb::Completed &f) { f.armed.capacity[1] += 1; }) == after);
    assert(changed([](rb::Completed &f) { f.armed.pixels += 1; }) == after);
    assert(changed([](rb::Completed &f) { f.armed.counter += 1; }) == after);
    assert(changed([](rb::Completed &f) { f.armed.pixels = f.armed.counter = 0; }) ==
           rb::Why::None); // a smaller snapshot: the lists are still at least the ones armed
    // Each raised frame is judged by the snapshot of the plan that raised it, never by a later
    // one: readbacks retire frames after the patch, and the resolve decides every frame. Frame N's
    // apply shows half the pixel list lobe 1 needs; the next plan raises lobe 2 only and snapshots
    // the smaller list, but frame N, raised on both lobes, still stops.
    rb::Plan first;
    g = Armed(l, first);
    auto shrunk = l;
    shrunk.v[2].bytes = 2 * l.Pixels();
    g.Apply(shrunk, now + 1);
    const auto next = g.Decide(rb::Times2, now + 1, nullptr);
    assert(next.mask == 2 && next.armed.pixels == shrunk.v[2].bytes &&
           first.armed.pixels == l.v[2].bytes);
    assert(g.Complete(Raised(shrunk, first)) == after);
    // Conversely, a frame armed with the larger lists may schedule what its own governor allowed;
    // a later plan armed with a smaller lobe-2 list does not make that an overflow.
    g = Armed(l, first);
    auto smaller = l;
    smaller.v[1].bytes -= 50000 * rb::EntryBytes;
    g.Apply(smaller, now + 1);
    const auto later = g.Decide(rb::Times2, now + 1, nullptr);
    assert(later.mask == 3 && later.armed.capacity[1] == first.armed.capacity[1] - 50000 &&
           later.headroom[1] == first.headroom[1] - 50000);
    auto earlier = Raised(l, first);
    earlier.rates[rrdensity::Extra + 1] = first.headroom[1];
    assert(g.Complete(earlier) == rb::Why::None);
    // Ping-pong Budgets written by the two plans: each frame carries its own plan's snapshot.
    for (unsigned i = 0; i < 8; ++i) {
        const auto &by = i & 1 ? later : first;
        auto frame = Raised(i & 1 ? smaller : l, by);
        frame.rates[rrdensity::Extra + 1] = by.headroom[1];
        assert(g.Complete(frame) == rb::Why::None);
    }
    assert(g.Stopped() == rb::Why::None);
    earlier.armed = later.armed; // judged by the later snapshot, the same frame would overflow
    assert(g.Complete(earlier) == rb::Why::Overflow);
    // A measured raised frame ends both counts of raised recordings without stats.
    g = Armed(l, plan);
    for (unsigned i = 1; i < rb::UnverifiedStop; ++i)
        assert(g.Unverified() == rb::Why::None);
    assert(g.Complete(Raised(l, plan)) == rb::Why::None);
    for (unsigned i = 1; i < rb::UnverifiedStop; ++i)
        assert(g.Unverified() == rb::Why::None);
    assert(g.Unverified() == rb::Why::Unverified && g.Stopped() == rb::Why::Unverified);
    // A raised frame the reader did not count is not verified: it counts like Unverified() and
    // ends neither count. The checks that read the rates do not run on it (here its threshold,
    // unsupported pixels, statistics and extra rays would each stop the test).
    g = Armed(l, plan);
    auto skipped = Raised(l, plan);
    for (unsigned i = 0; i < 2 * rrdensity::Buckets; ++i)
        skipped.rates[i] = 0;
    skipped.rates[rrdensity::Limits] = std::bit_cast<std::uint32_t>(IntMin);
    skipped.rates[rrdensity::Unsupported] = 5;
    skipped.rates[rrdensity::RawRays + 1] = 10;
    skipped.log[26] = 9;
    skipped.rates[rrdensity::Extra + 1] = 0xFFFFFFFFu;
    assert(!rb::Counted(skipped.rates));
    for (unsigned i = 1; i < rb::UnverifiedLimit; ++i)
        assert(g.Complete(skipped) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    assert(g.Complete(skipped) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Unverified);
    assert(g.Complete(Measured(l)) == rb::Why::None); // a measured frame ends the run
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
    for (unsigned i = rb::UnverifiedLimit; i < rb::UnverifiedStop - 1; ++i)
        assert(g.Complete(skipped) == rb::Why::None);
    assert(g.Complete(skipped) == rb::Why::Unverified && g.Stopped() == rb::Why::Unverified);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Stopped);
    // The checks that read only the log and the lists still run on it: the executed constants,
    // the lists armed and the snapshot itself. Only then does it count as unverified.
    auto uncounted = [&](auto edit) {
        rb::Plan armed;
        auto gate = Armed(l, armed);
        auto frame = Raised(l, armed);
        frame.rates = {};
        edit(frame);
        const auto why = gate.Complete(frame);
        assert(gate.Stopped() == why);
        if (why == rb::Why::None) { // counted as unverified: 8 in a row make Decide wait
            for (unsigned i = 2; i < rb::UnverifiedLimit; ++i)
                gate.Unverified();
            assert(gate.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
            gate.Unverified();
            assert(gate.Decide(rb::Times2, now, nullptr).why == rb::Why::Unverified);
        }
        return why;
    };
    assert(uncounted([](rb::Completed &) {}) == rb::Why::None);
    assert(uncounted([](rb::Completed &f) { f.log[5] ^= 1; }) == rb::Why::PatchMismatch);
    assert(uncounted([](rb::Completed &f) { f.logged &= ~rb::LogExecuted; }) ==
           rb::Why::PatchMismatch);
    assert(uncounted([](rb::Completed &f) { f.lists.v[2].bytes -= 1; }) == after);
    assert(uncounted([](rb::Completed &f) { f.lists.why = rb::Why::ListsRoot; }) == after);
    assert(uncounted([](rb::Completed &f) { f.log[28] = 2188959; }) == after);
    assert(uncounted([](rb::Completed &f) { f.armed = rb::Armed{}; }) == after);
    assert(uncounted([](rb::Completed &f) {
               f.patched = 0; // a raised Budget only: no executed constants to compare
               f.budget = 2;
               f.log[5] ^= 1;
           }) == rb::Why::None);
    // An uncounted frame that raised nothing is not checked at all.
    g = Armed(l, plan);
    auto native = Measured(l);
    native.rates = {};
    native.lists = rb::Lists{};
    assert(g.Complete(native) == rb::Why::None && g.Stopped() == rb::Why::None);
    for (unsigned i = 1; i < rb::UnverifiedLimit; ++i)
        g.Unverified();
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::None); // nor counted as unverified
    // The reader could not model some requested pixels of a raised frame (only then).
    assert(changed([](rb::Completed &f) { f.rates[rrdensity::Unsupported] = 1; }) ==
           rb::Why::Unsupported);
    g = Armed(l, plan);
    auto unraised = Measured(l);
    unraised.rates[rrdensity::Unsupported] = 1000;
    assert(g.Complete(unraised) == rb::Why::None);
    // The controller's statistics for a raised lobe lag the reader's raw rays: the governor judged
    // this frame by another frame's numbers. A lobe that was not raised only resets the count.
    assert(changed([](rb::Completed &f) {
               f.rates[rrdensity::RawRays + 1] = 10;
               f.log[26] = 9;
           }) == rb::Why::StatsMismatch);
    assert(changed([](rb::Completed &f) {
               f.rates[rrdensity::RawRays] = 10;
               f.log[25] = 10;
           }) == rb::Why::None);
    assert(changed([](rb::Completed &f) {
               f.rates[rrdensity::RawRays] = 10; // not copied in this frame
               f.logged &= ~rb::LogResolveStats;
           }) == rb::Why::None);
    g = Armed(l, plan);
    c = Raised(l, plan);
    c.patched = c.budget = 1;
    c.rates[rrdensity::RawRays + 1] = 10;
    c.log[26] = 9;
    assert(g.Complete(c) == rb::Why::None);
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::StatsUnverified);
    // The scheduled rays (requested pixels plus their extra rays) came within 512 entries of the
    // armed capacity.
    const auto tight = Proven(1920ull * 1080 + 600);
    const std::uint64_t pixels = tight.Pixels();
    const std::uint64_t room1 = tight.Entries(0) - pixels / 2 - rb::ConsumerSlack,
                        room2 = tight.Entries(1) - pixels - rb::ConsumerSlack;
    assert(room1 == 1036888 && room2 == 88);
    rb::Plan tight_plan;
    auto o = Armed(tight, tight_plan);
    auto full = Raised(tight, tight_plan);
    full.patched = full.budget = 2;
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2); // exactly full: still fine
    assert(o.Complete(full) == rb::Why::None);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    assert(o.Complete(full) == rb::Why::Overflow && o.Stopped() == rb::Why::Overflow);
    // Lobe 1 counts the requested pixels of its own histogram.
    o = Armed(tight, tight_plan);
    full = Raised(tight, tight_plan);
    full.patched = full.budget = 1;
    full.rates[rrdensity::Extra] = std::uint32_t(room1);
    assert(o.Complete(full) == rb::Why::None);
    full.rates[rrdensity::Extra] = std::uint32_t(room1 + 1);
    assert(o.Complete(full) == rb::Why::Overflow);
    // Lobes that were not raised are not checked; a lobe whose apply read a raised Budget is.
    o = Armed(tight, tight_plan);
    full = Measured(tight);
    full.rates[rrdensity::Extra] = std::uint32_t(room1 + 1);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    assert(o.Complete(full) == rb::Why::None);
    full.budget = 2; // lobe 2 only, with the snapshot of the plan that wrote that Budget
    full.armed = tight_plan.armed;
    assert(o.Complete(full) == rb::Why::Overflow);
    o = Armed(tight, tight_plan);
    full = Raised(tight, tight_plan);
    full.patched = full.budget = 1; // lobe 2 runs the game's own budget in this frame
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    assert(o.Complete(full) == rb::Why::None);
    // The capacity is the armed one, even when the frame's own list is larger.
    o = Armed(tight, tight_plan);
    full = Raised(tight, tight_plan);
    full.lists.v[1].bytes += 1000 * rb::EntryBytes;
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    assert(o.Complete(full) == rb::Why::Overflow);
    // The armed capacity includes the readers' second segment.
    o = Primed(tight, rb::ProofFrames, 0);
    auto segment = Measured(tight);
    segment.log[28] = std::uint32_t(tight.Entries(0) - 10);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        o.Complete(segment);
    tight_plan = o.Decide(rb::Times2, now, nullptr);
    assert(tight_plan.mask == 3 && tight_plan.armed.capacity[1] == tight.Entries(1) - 10);
    full = Raised(tight, tight_plan);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 - 10);
    assert(o.Complete(full) == rb::Why::None);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 - 9);
    assert(o.Complete(full) == rb::Why::Overflow);
}

static void GateTests() {
    assert(rb::mode.load() == rb::Off); // the session option starts off
    assert(rb::published[0].load() == 0 && rb::published[1].load() == 0);
    GovernorBounds();
    GateRefusals();
    GatePlans();
    GateStops();
}

// The controller's state as one line for the settings window.
static void StatusText() {
    auto describe = [](const rb::Status &s, std::size_t size = 256) {
        char text[256] = {};
        rb::Describe(s, text, (std::min)(size, sizeof(text)));
        return std::string(text);
    };
    rb::Status s;
    assert(s.mode == rb::Off && s.why == rb::Why::Mode && s.stopped == rb::Why::None && !s.active);
    assert(describe(s) == "off");
    s.mode = rb::Times2;
    assert(describe(s) == "waiting: test off");
    s.why = rb::Why::ListsStable;
    assert(describe(s) == "waiting: waiting for 120 proven ray-list applies");
    s.why = rb::Why::AtCap;
    assert(describe(s) == "waiting: already at the cap (1 ray per requested pixel)");
    // Active lines show the game's target and ours, three decimals; a lobe left alone says so.
    s.active = true;
    s.native = {0.4f, 0.4f};
    s.target = {0.8f, 0.8f};
    s.raised = {true, true};
    assert(describe(s) == "active: lobe1 0.400 -> 0.800, lobe2 0.400 -> 0.800");
    s.raised = {true, false};
    assert(describe(s) == "active: lobe1 0.400 -> 0.800, lobe2 0.400 (not raised)");
    s.raised = {false, true};
    assert(describe(s) == "active: lobe1 0.400 (not raised), lobe2 0.400 -> 0.800");
    s.raised = {false, false};
    assert(describe(s) == "active: lobe1 0.400 (not raised), lobe2 0.400 (not raised)");
    s.raised = {true, true};
    s.native = {1.0f / 3, 0.25f};
    s.target = {0.5f, 1.0f};
    assert(describe(s) == "active: lobe1 0.333 -> 0.500, lobe2 0.250 -> 1.000");
    // A stop wins over everything but the test being off.
    s.stopped = rb::Why::Governor;
    assert(describe(s) ==
           "stopped for this session: controller threshold beyond the one-ray bound after the "
           "governor");
    s.stopped = rb::Why::Exception;
    assert(describe(s) == "stopped for this session: internal error");
    s.mode = rb::Off;
    assert(describe(s) == "off");
    s.mode = 99; // an unknown step reads as Off
    assert(describe(s) == "off");
    s.mode = rb::OnePerPixel;
    s.stopped = rb::Why::None;
    s.active = false;
    s.why = rb::Why::Count; // an unknown reason has a text too
    assert(describe(s) == "waiting: unknown");
    // The output is always NUL-terminated and never longer than the buffer.
    s.why = rb::Why::Context;
    const std::string full = describe(s);
    assert(full == "waiting: binding tracker, copy context or state hooks unavailable");
    for (std::size_t size = 1; size <= full.size() + 1; ++size) {
        char text[256];
        std::memset(text, 'x', sizeof(text));
        rb::Describe(s, text, size);
        assert(std::strlen(text) == (std::min)(size - 1, full.size()));
        assert(full.compare(0, std::strlen(text), text) == 0);
        assert(text[size] == 'x'); // nothing written past the size
    }
    char untouched[4] = {'a', 'b', 'c', 'd'};
    rb::Describe(s, untouched, 0);
    assert(untouched[0] == 'a' && untouched[1] == 'b' && untouched[3] == 'd');
    rb::Describe(s, nullptr, 0); // nothing to write to
}

// The 10 s lines. Expect() prints what was missing before the test stops.
static void Expect(const std::string &text, const std::string &part) {
    if (text.find(part) == std::string::npos) {
        std::fprintf(stderr, "missing: %s\n   in: %s\n", part.c_str(), text.c_str());
        std::abort();
    }
}

static void ExpectNot(const std::string &text, const std::string &part) {
    if (text.find(part) != std::string::npos) {
        std::fprintf(stderr, "unexpected: %s\n   in: %s\n", part.c_str(), text.c_str());
        std::abort();
    }
}

// A raised frame whose patch was executed and verified, with every optional log word present.
static rb::Completed Verified(const rb::Lists &l, const rb::Plan &plan) {
    auto c = Raised(l, plan);
    c.logged |=
        rb::LogResolveScene | rb::LogApplyTarget | rb::LogApplySettings36 | rb::LogApplyScene251;
    c.log[8] = std::bit_cast<std::uint32_t>(float(l.w));
    c.log[9] = std::bit_cast<std::uint32_t>(float(l.h));
    c.log[12] = std::bit_cast<std::uint32_t>(plan.target[0]);
    c.log[13] = std::bit_cast<std::uint32_t>(plan.target[1]);
    c.log[19] = 0x10000;
    c.log[20] = 0x400;
    return c;
}

static constexpr std::uint64_t Interval = rb::Summary::Interval;

static void SummaryWindow() {
    const auto l = Proven(1920ull * 1088 + 100000);
    auto gate = Primed(l);
    for (unsigned i = 0; i < 20; ++i) { // a raw-ray margin of 24 for lobe 1
        auto f = Measured(l);
        f.rates[rrdensity::RawRays] = f.log[25] = 12;
        gate.Complete(f);
    }
    const auto plan = gate.Decide(rb::Times2, 119, nullptr);
    assert(plan.mask == 3 && plan.armed.margin[0] == 24 && plan.armed.margin[1] == 0);
    rb::Status status;
    status.mode = rb::Times2;
    status.why = rb::Why::None;
    status.active = true;
    status.native = {0.4f, 0.4f};
    status.target = plan.target;
    status.raised = {true, true};

    rb::Summary sm;
    std::string controller, lists;
    assert(!sm.Report(1000, gate, status, controller, lists)); // the first call starts the window
    // Resolves: 12 inside the recording (10 after the apply, 8 of them with the apply's constants).
    for (unsigned i = 0; i < 10; ++i)
        sm.Resolve(true, i < 8);
    sm.Resolve(false, false);
    sm.Resolve(false, true); // before the apply: only the apply can compare, via SameBlock
    sm.SameBlock(true);
    sm.SameBlock(true);
    sm.SameBlock(true);
    sm.SameBlock(false);
    sm.BeforePrepass();
    for (unsigned i = 0; i < 2; ++i)
        sm.Refused(rb::Why::Outside);
    sm.Refused(rb::Why::AfterSR);
    sm.Refused(rb::Why::Repeated);
    sm.Refused(rb::Why::Ambiguous);
    for (unsigned i = 0; i < 12; ++i)
        sm.Refused(rb::Why::ListsStable);
    for (unsigned i = 0; i < 3; ++i)
        sm.Refused(rb::Why::Heap);
    for (unsigned i = 0; i < 7; ++i)
        sm.Budget(true);
    sm.Budget(false);
    for (unsigned i = 0; i < 4; ++i)
        sm.Clear(true);
    sm.Clear(false);
    sm.Patched(rb::Plan{}); // nothing was patched: not counted
    for (unsigned i = 0; i < 5; ++i)
        sm.Patched(plan);
    for (unsigned i = 0; i < 6; ++i)
        sm.Lists(l);
    auto broken = l;
    broken.why = rb::Why::ListsView;
    sm.Lists(broken);
    sm.Lists(l);
    for (unsigned i = 0; i < 5; ++i) // five of the eight applies read their own resolve's Budget
        sm.SameFrame();
    // Five patched frames: three verified, one with a different word, one that never executed.
    // One (f3) asks for more raw rays than both lobes' margins, and its statistics were not copied
    // to cover them; f2 and f5 copied the readers' second segment. The apply's register 251 .y
    // (low 12 bits) reads 0, 5, 0, 5, 5: three changes.
    auto f1 = Verified(l, plan), f2 = Verified(l, plan), f3 = Verified(l, plan);
    f2.log[0] = std::bit_cast<std::uint32_t>(0.25f);
    f2.log[1] = std::bit_cast<std::uint32_t>(0.6f);
    f3.log[0] = std::bit_cast<std::uint32_t>(0.5f);
    f3.rates[rrdensity::Extra] = 1000;
    f3.rates[rrdensity::Extra + 1] = 20000;
    f3.rates[rrdensity::RawRays] = 30;
    f3.rates[rrdensity::RawRays + 1] = 40;
    f3.rates[rrdensity::Limits] = 17;
    f3.rates[rrdensity::Limits + 2] = std::uint32_t(-16);
    f2.log[28] = 5;
    f2.log[21] = 0x1005;
    auto f4 = Verified(l, plan), f5 = Verified(l, plan);
    f4.log[21] = 0x7005;
    f5.log[21] = 5;
    f4.log[5] ^= 1;
    f4.log[9] = std::bit_cast<std::uint32_t>(1081.0f);
    f5.logged &= ~rb::LogExecuted;
    f5.log[0] = std::bit_cast<std::uint32_t>(0.4f);
    f5.log[1] = std::bit_cast<std::uint32_t>(0.45f);
    f5.rates[rrdensity::Limits] = std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    f5.log[28] = 2100000;
    for (const auto &f : {f1, f2, f3, f4, f5})
        sm.Completed(f);
    assert(!sm.Report(1000 + Interval - 1, gate, status, controller, lists));
    assert(controller.empty() && lists.empty());
    assert(sm.Report(1000 + Interval, gate, status, controller, lists));
    Expect(controller,
           "RR ray budget (10s): test=2x the game's rays state=active patched=5/12 "
           "verified=3 | resolve seen=17 inRecording=12 afterApply=10 beforeApply=2 "
           "outside=2 beforePrepass=1 afterSR=1 repeated=1 clears=4/5 "
           "sameB1AsApply=11/14 sameBudget=7/8 sameFrame=5/8 | target native=0.400/0.450 "
           "(0.250..0.500/0.400..0.600) apply=0.500/0.800 written=0.500/0.800 "
           "scene=1920x1080 match=4/5 settings36.w=0x10000 scene251.x=0x400 "
           "scene251.y=5 changes=3 scene253.x=2100000 stats=4/5 uncounted=0 | "
           "consumers lobe1=0 lobe2=0 dispatch=0 "
           "rays=0 other=0 last=n/a | refused listsStable=12 heap=3 outside=2 "
           "afterSR=1 repeated=1 ambiguous=1");
    // Area is the extent's W*H (2073600); capacity and need are the gate's (Gate::Capacity and
    // Gate::Need); worst fill = requested pixels plus extra rays over the frame's own entries:
    // lobe 1 (1036800 + 1000) / 2188960, lobe 2 (2073600 + 20000) / 2188960. INT_MIN (f5) is a
    // threshold like any other.
    Expect(lists, "RR ray lists (10s): applies=8 proven=7 streak=120 1920x1080 groups=120x68 "
                  "area=2073600 | lobe1 u5=table 8755984B capacity=2188960 (1.056x area) "
                  "need=2074136 tier=hard worstFill=47.4% rawRaysMax=30 marginExceeded=1 "
                  "threshold=-2147483648..17 | lobe2 u6=table 8755984B capacity=2188960 "
                  "(1.056x area) need=2074112 tier=hard worstFill=95.6% rawRaysMax=40 "
                  "marginExceeded=1 "
                  "threshold=-16..9 | pixels u3=table 8294400B (4.000 B/px) counter u2=table 16B "
                  "| why=none");
    // The window starts over; a quiet one prints only while the test is on.
    assert(!sm.Report(1000 + 2 * Interval, gate, rb::Status{}, controller, lists));
    controller = lists = "stale";
    assert(sm.Report(1000 + 3 * Interval, gate, status, controller, lists));
    Expect(controller, "patched=0/0 verified=0 | resolve seen=0 inRecording=0");
    Expect(controller,
           "| target native=n/a apply=n/a written=-/- scene=n/a match=0/0 settings36.w=n/a "
           "scene251.x=n/a scene251.y=n/a changes=0 scene253.x=n/a stats=0/0 uncounted=0 | "
           "consumers lobe1=0");
    ExpectNot(controller, "refused");
    assert(lists.empty()); // no apply in this window
}

static void SummaryFormat() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const auto gate = Primed(l);
    rb::Status status;
    status.mode = rb::Times2;
    status.why = rb::Why::ListsStable;
    rb::Summary sm;
    std::string controller, lists;
    assert(!sm.Report(0, gate, status, controller, lists));
    assert(sm.Report(Interval, gate, status, controller, lists) && lists.empty());
    assert(controller ==
           "RR ray budget (10s): test=2x the game's rays state=waiting: waiting for 120 proven "
           "ray-list applies patched=0/0 verified=0 | resolve seen=0 inRecording=0 afterApply=0 "
           "beforeApply=0 outside=0 beforePrepass=0 afterSR=0 repeated=0 clears=0/0 "
           "sameB1AsApply=0/0 sameBudget=0/0 sameFrame=0/0 | target native=n/a apply=n/a "
           "written=-/- "
           "scene=n/a match=0/0 settings36.w=n/a scene251.x=n/a scene251.y=n/a changes=0 "
           "scene253.x=n/a stats=0/0 uncounted=0 | consumers lobe1=0 lobe2=0 dispatch=0 rays=0 "
           "other=0 last=n/a");
    status.mode = rb::Off;
    assert(!sm.Report(2 * Interval, gate, status, controller, lists)); // nothing seen, test off
    // The state word: off, waiting, active, stopped.
    sm.Resolve(false, false);
    assert(sm.Report(3 * Interval, gate, status, controller, lists));
    Expect(controller, "test=Off (game's ray budget) state=off patched=0/1");
    status.mode = rb::OnePerPixel;
    sm.Resolve(false, false);
    assert(sm.Report(4 * Interval, gate, status, controller, lists));
    Expect(controller, "test=Up to 1 ray per pixel state=waiting: waiting for 120 proven "
                       "ray-list applies patched=0/1");
    status.active = true;
    sm.Resolve(false, false);
    assert(sm.Report(5 * Interval, gate, status, controller, lists));
    Expect(controller, "state=active patched=0/1");
    status.stopped = rb::Why::PatchMismatch;
    sm.Resolve(false, false);
    assert(sm.Report(6 * Interval, gate, status, controller, lists));
    Expect(controller, "state=stopped: private constants did not reach the controller patched=");
    status.mode = 99; // an unknown step reads as Off
    sm.Resolve(false, false);
    assert(sm.Report(7 * Interval, gate, status, controller, lists));
    Expect(controller, "test=Off (game's ray budget) state=off");
    // The seconds are the window's length.
    status.mode = rb::Times15;
    status.stopped = rb::Why::None;
    assert(sm.Report(7 * Interval + 12345 + Interval, gate, status, controller, lists));
    Expect(controller, "RR ray budget (22s):");
    // A target that is not a number has no range.
    auto c = Measured(l, std::numeric_limits<float>::quiet_NaN());
    sm.Completed(c);
    assert(sm.Report(30 * Interval, gate, status, controller, lists));
    Expect(controller, "(n/a/n/a)");
    c = Measured(l, 0.4f);
    sm.Completed(c);
    assert(sm.Report(31 * Interval, gate, status, controller, lists));
    Expect(controller, "verified=0 | resolve seen=0");
    Expect(controller, "target native=0.400/0.400 (0.400..0.400/0.400..0.400) apply=n/a "
                       "written=-/- scene=n/a match=0/0");
    // A lobe the last patch did not raise has no written value.
    rb::Plan lobe2;
    lobe2.mask = 2;
    lobe2.bits = {0, std::bit_cast<std::uint32_t>(0.8f)};
    sm.Completed(Verified(l, lobe2));
    assert(sm.Report(32 * Interval, gate, status, controller, lists));
    Expect(controller, "verified=1 | resolve");
    Expect(controller, " written=-/0.800 scene=");
    rb::Plan lobe1;
    lobe1.mask = 1;
    lobe1.bits = {std::bit_cast<std::uint32_t>(0.5f), 0};
    sm.Completed(Verified(l, lobe2));
    sm.Completed(Verified(l, lobe1)); // the last patched frame counts
    sm.Completed(Measured(l));        // a frame that was not patched leaves it
    assert(sm.Report(33 * Interval, gate, status, controller, lists));
    Expect(controller, " written=0.500/- scene=");
    assert(sm.Report(34 * Interval, gate, status, controller, lists));
    Expect(controller, " written=-/- scene="); // a new window forgets it
    // The statistics count frames with the copy, and how many covered the reader's raw rays on
    // both lobes; the second segment shows the last value copied.
    auto stats = Measured(l);
    stats.rates[rrdensity::RawRays] = stats.log[25] = 7;
    stats.log[28] = 1234;
    sm.Completed(stats);
    stats.log[26] = 3;
    stats.rates[rrdensity::RawRays + 1] = 4; // lobe 2 lags
    stats.log[28] = 0;
    sm.Completed(stats);
    stats.logged &= ~(rb::LogResolveStats | rb::LogApplyScene253); // neither copied
    stats.log[28] = 99;
    sm.Completed(stats);
    assert(sm.Report(35 * Interval, gate, status, controller, lists));
    Expect(controller, " scene251.x=n/a scene251.y=n/a changes=0 scene253.x=0 stats=1/2 "
                       "uncounted=0 | consumers");
    // Each field follows its own copy's bit: the statistics without the second segment and the
    // other way round, register 36 without register 251 and the other way round.
    auto one = Measured(l);
    one.logged = rb::LogResolveStats;
    one.log[28] = 77;
    one.log[19] = 0x20000;
    one.log[20] = 0x800;
    one.log[21] = 9;
    sm.Completed(one);
    assert(sm.Report(36 * Interval, gate, status, controller, lists));
    Expect(controller, " settings36.w=n/a scene251.x=n/a scene251.y=n/a changes=0 "
                       "scene253.x=n/a stats=1/1 ");
    one.logged = rb::LogApplyScene253;
    sm.Completed(one);
    assert(sm.Report(37 * Interval, gate, status, controller, lists));
    Expect(controller, " scene253.x=77 stats=0/0 ");
    one.logged = rb::LogApplySettings36;
    sm.Completed(one);
    assert(sm.Report(38 * Interval, gate, status, controller, lists));
    Expect(controller, " settings36.w=0x20000 scene251.x=n/a scene251.y=n/a changes=0 "
                       "scene253.x=n/a ");
    // Register 251 .y: the last low 12 bits, and how many frames read another value than the
    // frame before them that copied the register (that frame may be in an earlier window: here a
    // verified frame above read 0).
    one.logged = rb::LogApplyScene251;
    sm.Completed(one);
    assert(sm.Report(39 * Interval, gate, status, controller, lists));
    Expect(controller, " settings36.w=n/a scene251.x=0x800 scene251.y=9 changes=1 scene253.x=n/a ");
    sm.Completed(one);
    one.log[21] = 0xFFFFF009; // the same low 12 bits
    sm.Completed(one);
    one.logged = rb::LogApplySettings36; // not copied: neither a value nor a change
    one.log[21] = 10;
    sm.Completed(one);
    assert(sm.Report(40 * Interval, gate, status, controller, lists));
    Expect(controller, " scene251.x=0x800 scene251.y=9 changes=0 ");
    one.logged = rb::LogApplyScene251;
    sm.Completed(one);
    one.log[21] = 4095;
    sm.Completed(one);
    one.log[21] = 4096; // 0
    sm.Completed(one);
    assert(sm.Report(41 * Interval, gate, status, controller, lists));
    Expect(controller, " scene251.y=0 changes=3 ");
}

static void SummaryRefusals() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const auto gate = Primed(l);
    rb::Status status;
    status.mode = rb::Times2;
    rb::Summary sm;
    std::string controller, lists;
    assert(!sm.Report(0, gate, status, controller, lists));
    // Only non-zero counters, the biggest first, equal ones in the enum's order; a reason that is
    // not a refusal and values outside the enum are ignored.
    sm.Refused(rb::Why::None);
    sm.Refused(rb::Why::Count);
    sm.Refused(rb::Why(unsigned(rb::Why::Count) + 5));
    for (unsigned i = 0; i < 4; ++i)
        sm.Refused(rb::Why::BudgetIdentity);
    for (unsigned i = 0; i < 4; ++i)
        sm.Refused(rb::Why::Constants);
    for (unsigned i = 0; i < 9; ++i)
        sm.Refused(rb::Why::Mode);
    sm.Refused(rb::Why::Exception);
    assert(sm.Report(Interval, gate, status, controller, lists));
    Expect(controller, "last=n/a | refused mode=9 constants=4 budgetIdentity=4 exception=1");
    // The placement reasons also count as resolves seen; every other one only as a refusal.
    for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
        sm.Refused(rb::Why(w));
    assert(sm.Report(2 * Interval, gate, status, controller, lists));
    Expect(controller, "resolve seen=4 inRecording=0 afterApply=0 beforeApply=0 outside=1 "
                       "beforePrepass=0 afterSR=1 repeated=1");
    // Each reason has its own key (the enum's name in lower camel case) and its own text; equal
    // counts keep the enum's order. All of them fit here, where the targets and scene fields are
    // n/a; with real values the line can be cut (below). Max: the longest line the buffer holds.
    constexpr std::size_t Max = rb::Summary::ControllerBytes - 1;
    std::string expected;
    for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
        expected += std::string(w > 1 ? " " : "") + rb::keys[w] + "=1";
    Expect(controller, " | refused " + expected);
    assert(controller.size() < Max && controller.ends_with(" | refused " + expected));
    const std::size_t ones = controller.size(); // every reason at 1
    for (unsigned a = 0; a < unsigned(rb::Why::Count); ++a) {
        assert(rb::keys[a][0] >= 'a' && rb::keys[a][0] <= 'z' && rb::Text(rb::Why(a))[0] != 0);
        for (unsigned b = a + 1; b < unsigned(rb::Why::Count); ++b) {
            assert(std::string(rb::keys[a]) != rb::keys[b]);
            assert(std::string(rb::Text(rb::Why(a))) != rb::Text(rb::Why(b)));
        }
    }
    // The line is one ControllerBytes buffer built from whole pieces: the first piece that does not
    // fit ends it with " ..." (room for it is kept), so no count is ever cut short. With 100000 of
    // each reason the refusal list no longer fits.
    for (unsigned k = 0; k < 100000; ++k)
        for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
            sm.Refused(rb::Why(w));
    assert(sm.Report(3 * Interval, gate, status, controller, lists));
    expected.clear();
    for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
        expected += std::string(w > 1 ? " " : "") + rb::keys[w] + "=100000";
    const auto at = controller.find(" | refused mode=100000 stopped=100000 unverified=100000 ");
    assert(at != std::string::npos && controller.starts_with("RR ray budget (10s): test=2x"));
    const std::string uncut = controller.substr(0, at) + " | refused " + expected;
    assert(uncut.size() > Max + 50 && controller.size() <= Max && controller.ends_with(" ..."));
    const auto kept = controller.substr(0, controller.size() - 4);
    assert(uncut.starts_with(kept) && kept.ends_with("=100000") && uncut[kept.size()] == ' ');
    const auto token = uncut.substr(kept.size(), uncut.find(' ', kept.size() + 1) - kept.size());
    assert(token.ends_with("=100000") && kept.size() + token.size() + 4 > Max); // it did not fit
    assert(kept.find(" ...") == std::string::npos);
    // At the boundary: a line that fits whole is printed whole up to Max - 4 characters; from
    // Max - 3 on it would leave no room for the marker, so it is cut there too. Every reason at
    // `low` (10^digits) is `digits` characters per reason longer than at 1, and each reason at
    // `high` (10 * low) instead lengthens it one more: the sweep crosses the boundary.
    const unsigned reasons = unsigned(rb::Why::Count) - 1;
    const std::size_t digits = (Max - 3 - ones) / reasons;
    unsigned low = 1;
    for (std::size_t d = 0; d < digits; ++d)
        low *= 10;
    const unsigned high = 10 * low;
    bool boundary = false;
    for (unsigned tens = 0; tens < unsigned(rb::Why::Count); ++tens) {
        rb::Summary edge;
        assert(!edge.Report(0, gate, status, controller, lists));
        std::string list;
        for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
            for (unsigned k = 0; k < (w <= tens ? high : low); ++k)
                edge.Refused(rb::Why(w));
        for (unsigned pass = 0; pass < 2; ++pass) // the highs first, then the lows, in enum order
            for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
                if ((w <= tens) == (pass == 0))
                    list += std::string(list.empty() ? "" : " ") + rb::keys[w] + "=" +
                            std::to_string(w <= tens ? high : low);
        assert(edge.Report(Interval, gate, status, controller, lists));
        const auto refused = controller.find(" | refused ");
        assert(refused != std::string::npos);
        const auto whole = controller.substr(0, refused) + " | refused " + list;
        boundary |= whole.size() >= Max - 3 && whole.size() <= Max;
        if (whole.size() <= Max - 4) {
            assert(controller == whole);
            continue;
        }
        assert(controller.size() <= Max && controller.ends_with(" ..."));
        const auto cut = controller.substr(0, controller.size() - 4);
        assert(whole.starts_with(cut) && whole[cut.size()] == ' ');
        const auto next = whole.find(' ', cut.size() + 1); // the end of the piece that did not fit
        assert((next == std::string::npos ? whole.size() : next) + 4 > Max);
    }
    assert(boundary);
}

static void SummaryConsumers() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const auto gate = Primed(l);
    rb::Status status;
    status.mode = rb::Times2;
    rb::Summary sm;
    std::string controller, lists;
    assert(!sm.Report(0, gate, status, controller, lists));
    // ReShade's indirect_command: 1 draw, 2 draw_indexed, 3 dispatch, 4 dispatch_mesh,
    // 5 dispatch_rays, anything else (multi-draw types of newer builds, unknown) is other.
    auto consumer = [](unsigned type, unsigned lobe, std::uint64_t offset = 0,
                       std::uint32_t stride = 0) {
        rb::Consumer c;
        c.type = type;
        c.lobe = lobe;
        c.offset = offset;
        c.count = 1;
        c.stride = stride;
        return c;
    };
    for (unsigned i = 0; i < 5; ++i)
        sm.Consumer(consumer(3, 0));
    for (unsigned i = 0; i < 3; ++i)
        sm.Consumer(consumer(3, 1));
    sm.Consumer(consumer(5, 1));
    sm.Consumer(consumer(1, 0));
    sm.Consumer(consumer(4, 0));
    sm.Consumer(consumer(6, 0));
    sm.Consumer(consumer(0, 7)); // not one of the two lobes: counted by type only
    sm.Consumer(consumer(3, 1, 144, 16));
    assert(sm.Report(Interval, gate, status, controller, lists));
    Expect(controller, "| consumers lobe1=8 lobe2=5 dispatch=9 rays=1 other=4 last=3/16/144");
    sm.Consumer(consumer(2, 0, 0, 20));
    assert(sm.Report(2 * Interval, gate, status, controller, lists));
    Expect(controller, "| consumers lobe1=1 lobe2=0 dispatch=0 rays=0 other=1 last=2/20/0");
}

static void SummaryLists() {
    const auto l = Proven(1920ull * 1088 + 100000);
    rb::Status status;
    status.mode = rb::Times2;
    rb::Summary sm;
    std::string controller, lists;
    auto gate = Primed(l);
    assert(!sm.Report(0, gate, status, controller, lists));
    // The last proof is what is described, proven or not; capacity and need are the gate's
    // (0 capacity while its streak is broken).
    auto odd = l;
    odd.v[0].binding = rb::Binding::Root;
    odd.v[1].binding = rb::Binding::Invalid;
    odd.v[2].binding = rb::Binding::Unknown;
    odd.v[3].bytes = 0;
    odd.why = rb::Why::ListsRoot;
    sm.Lists(l);
    sm.Lists(odd);
    gate.Apply(odd, 200); // the gate's streak starts over
    assert(sm.Report(Interval, gate, status, controller, lists));
    Expect(lists, "applies=2 proven=1 streak=0 1920x1080");
    Expect(lists, "| lobe1 u5=root 8755984B capacity=0 (0.000x area) need=2074112 tier=small");
    Expect(lists, "| lobe2 u6=view 8755984B capacity=0 (0.000x area) need=2074112 tier=small");
    Expect(lists, "worstFill=n/a rawRaysMax=0 marginExceeded=0 threshold=n/a");
    Expect(lists,
           "| pixels u3=unknown 8294400B (4.000 B/px) counter u2=table 0B | why=a ray list is a "
           "root UAV (no bounds checks)");
    // Small lists and a list that is not there at all.
    auto undersized = Proven(1920ull * 1080);
    gate = Primed(undersized);
    sm.Lists(undersized);
    assert(sm.Report(2 * Interval, gate, status, controller, lists));
    Expect(lists, "area=2073600 | lobe1 u5=table 8294544B capacity=2073600 (1.000x area) "
                  "need=2074112 tier=small");
    rb::Lists none;
    sm.Lists(none);
    assert(sm.Report(3 * Interval, gate, status, controller, lists));
    Expect(lists, "applies=1 proven=0 streak=120 0x0 groups=0x0 area=0 | lobe1 u5=unknown 0B "
                  "capacity=2073600 (0.000x area) need=2074112 tier=small worstFill=n/a");
    Expect(lists, "| pixels u3=unknown 0B (0.000 B/px) counter u2=unknown 0B | why=ray lists not "
                  "proven at the VRRT apply");
    // Fill uses the last proof when the frame's own apply was not proven, or proved no extent.
    gate = Primed(l);
    sm.Lists(l);
    auto frame = Measured(l);
    frame.lists = rb::Lists{};
    frame.rates[rrdensity::Extra + 1] = 20000;
    sm.Completed(frame);
    assert(sm.Report(4 * Interval, gate, status, controller, lists));
    Expect(lists, "worstFill=95.6%"); // (2073600 + 20000) / 2188960
    sm.Lists(l);
    auto zero = frame;
    zero.lists = Proven(100, 0, 0);
    sm.Completed(zero);
    assert(sm.Report(5 * Interval, gate, status, controller, lists));
    Expect(lists, "worstFill=95.6%");
    // Without any proof there is nothing to measure against, and a scene matches no zero extent.
    sm.Lists(none);
    frame.logged |= rb::LogResolveScene; // the resolve's scene: 0x0
    sm.Completed(frame);
    assert(sm.Report(6 * Interval, gate, status, controller, lists));
    Expect(lists, "worstFill=n/a");
    Expect(controller, " scene=0x0 match=0/1 ");
    // Nor against a proof the gate cannot use, lists or not: unproven, or an extent above 16384.
    auto unproven = l;
    unproven.why = rb::Why::ListsView;
    for (const auto &proof : {unproven, Proven(1920ull * 1088 + 100000, 20000, 1080)}) {
        auto unusable = frame; // its resolve's scene is the proof's extent
        unusable.log[8] = std::bit_cast<std::uint32_t>(float(proof.w));
        unusable.log[9] = std::bit_cast<std::uint32_t>(float(proof.h));
        rb::Summary other;
        assert(!other.Report(0, gate, status, controller, lists));
        other.Lists(proof);
        other.Resolve(false, false);
        other.Completed(unusable);
        assert(other.Report(Interval, gate, status, controller, lists));
        Expect(lists, "worstFill=n/a");
        Expect(controller, " scene=" + std::to_string(proof.w) + "x1080 match=0/1 ");
    }
    sm.Lists(none);
    zero.logged |= rb::LogResolveScene;
    sm.Completed(zero);
    assert(sm.Report(7 * Interval, gate, status, controller, lists));
    Expect(controller, " scene=0x0 match=0/1 ");
    // A lobe whose list holds nothing has no fill; the other still does.
    auto empty = l;
    empty.v[0].bytes = rb::ListHeader;
    sm.Lists(empty);
    sm.Completed(Measured(empty));
    assert(sm.Report(8 * Interval, gate, status, controller, lists));
    Expect(lists, "lobe1 u5=table 144B capacity=2188960 (1.056x area) need=2074112 tier=hard "
                  "worstFill=n/a");
    Expect(lists, "lobe2 u6=table 8755984B capacity=2188960 (1.056x area) need=2074112 tier=hard "
                  "worstFill=94.7%");
    // Raised lobes whose raw rays exceeded the margin they were armed with, per lobe; equal is not
    // more, and a lobe that was not raised is not counted.
    sm.Lists(l);
    rb::Plan plan;
    plan.mask = 2;
    plan.armed.margin = {0, 50};
    auto margin = Raised(l, plan);
    margin.rates[rrdensity::RawRays] = 1000; // lobe 1 not raised
    margin.rates[rrdensity::RawRays + 1] = 50;
    sm.Completed(margin);
    margin.rates[rrdensity::RawRays + 1] = 51;
    sm.Completed(margin);
    sm.Completed(margin);
    margin.budget = 1; // the apply read a raised Budget for lobe 1 too, armed with margin 0
    sm.Completed(margin);
    assert(sm.Report(9 * Interval, gate, status, controller, lists));
    Expect(lists, "rawRaysMax=1000 marginExceeded=1 threshold=9..9 | lobe2");
    Expect(lists, "rawRaysMax=51 marginExceeded=3 threshold=9..9 | pixels");
    // A frame the reader did not count is counted as such and adds nothing measured: no
    // statistics comparison, raw rays, margin excess, threshold or fill.
    auto skipped = Raised(l, plan);
    skipped.rates = {};
    skipped.rates[rrdensity::RawRays + 1] = 1000;
    skipped.rates[rrdensity::Extra + 1] = 1000;
    sm.Lists(l);
    sm.Completed(skipped);
    sm.Completed(skipped);
    assert(sm.Report(10 * Interval, gate, status, controller, lists));
    Expect(controller, " stats=0/0 uncounted=2 | ");
    Expect(lists, "worstFill=n/a rawRaysMax=0 marginExceeded=0 threshold=n/a | lobe2");
    Expect(lists, "worstFill=n/a rawRaysMax=0 marginExceeded=0 threshold=n/a | pixels");
    sm.Completed(Measured(l));
    assert(sm.Report(11 * Interval, gate, status, controller, lists));
    Expect(controller, " stats=1/1 uncounted=0 | ");
}

static void SummaryTiming() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const auto gate = Primed(l);
    rb::Status status; // the test is off: only activity prints
    std::string controller, lists;
    rb::Summary sm;
    assert(rb::Summary::Interval == rrdensity::Tracker::Interval);
    sm.Resolve(true, true);
    assert(!sm.Report(5000, gate, status, controller, lists)); // the first call starts the window
    assert(!sm.Report(5000 + Interval - 1, gate, status, controller, lists));
    assert(sm.Report(5000 + Interval, gate, status, controller, lists));
    Expect(controller, "resolve seen=1 inRecording=1 afterApply=1");
    assert(!sm.Report(5000 + Interval, gate, status, controller, lists)); // a new window started
    sm.Resolve(false, false);
    assert(!sm.Report(5000 + 2 * Interval - 1, gate, status, controller, lists));
    assert(sm.Report(5000 + 2 * Interval, gate, status, controller, lists));
    Expect(controller, "resolve seen=1 inRecording=1 afterApply=0");
    // A clock that went backwards restarts the window; the events of the old one are dropped.
    sm.Resolve(false, false);
    assert(!sm.Report(4000, gate, status, controller, lists));
    assert(!sm.Report(4000 + Interval - 1, gate, status, controller, lists));
    assert(!sm.Report(4000 + Interval, gate, status, controller, lists)); // nothing seen since
    sm.Resolve(false, false);
    assert(!sm.Report(4000 + 2 * Interval - 1, gate, status, controller, lists));
    assert(sm.Report(4000 + 2 * Interval, gate, status, controller, lists));
    Expect(controller, "RR ray budget (10s): test=Off (game's ray budget) state=off patched=0/1");
    // Any apply or resolve prints; so does the test being on; a quiet window with the test off not.
    sm.Lists(l);
    assert(sm.Report(4000 + 3 * Interval, gate, status, controller, lists) && !lists.empty());
    sm.Refused(rb::Why::Outside);
    assert(sm.Report(4000 + 4 * Interval, gate, status, controller, lists));
    Expect(controller, "resolve seen=1 inRecording=0 afterApply=0 beforeApply=0 outside=1");
    sm.Clear(true);
    sm.Consumer(rb::Consumer{});
    sm.Budget(true);
    assert(!sm.Report(4000 + 5 * Interval, gate, status, controller, lists));
    status.mode = rb::Times15;
    assert(sm.Report(4000 + 6 * Interval, gate, status, controller, lists));
    // The quiet window still started over: its counters are gone.
    Expect(controller, " clears=0/0 sameB1AsApply=0/0 sameBudget=0/0 sameFrame=0/0 | ");
    Expect(controller, "| consumers lobe1=0 lobe2=0 dispatch=0 rays=0 other=0 last=n/a");
}

static void SummaryIdle() {
    const auto l = Proven(1920ull * 1088 + 100000);
    const auto gate = Primed(l);
    rb::Status status;
    rb::Summary sm;
    std::string controller, lists;
    assert(!sm.Report(0, gate, status, controller, lists));
    // Applies without a single resolve: the resolve's shader is not the one we know.
    const char *warning = "RR ray budget (10s): cs_vrrt_resolve 0x293995352d1f8220 not seen while "
                          "7 VRRT applies were (game update?); the ray budget test is unavailable";
    for (unsigned window = 1; window <= 3; ++window) {
        for (unsigned i = 0; i < 7; ++i)
            sm.Lists(l);
        controller = lists = "stale";
        assert(sm.Report(window * Interval, gate, status, controller, lists));
        assert(controller == warning);
        Expect(lists, "RR ray lists (10s): applies=7 proven=7");
    }
    // After three warnings the normal line is back, still without resolves.
    for (unsigned i = 0; i < 7; ++i)
        sm.Lists(l);
    assert(sm.Report(4 * Interval, gate, status, controller, lists));
    Expect(controller, "RR ray budget (10s): test=Off (game's ray budget) state=off patched=0/0 "
                       "verified=0 | resolve seen=0");
    ExpectNot(controller, "not seen");
    assert(!lists.empty());
    // A window with a resolve or refusal in it is not idle, and no warning is spent on it.
    rb::Summary busy;
    assert(!busy.Report(0, gate, status, controller, lists));
    busy.Lists(l);
    busy.Refused(rb::Why::Outside);
    assert(busy.Report(Interval, gate, status, controller, lists));
    ExpectNot(controller, "not seen");
    busy.Lists(l);
    busy.Resolve(false, false);
    assert(busy.Report(2 * Interval, gate, status, controller, lists));
    ExpectNot(controller, "not seen");
    // No applies and no resolves: nothing to warn about.
    assert(!busy.Report(3 * Interval, gate, status, controller, lists));
    // The test being on does not change the warning, only that it prints without applies.
    status.mode = rb::Times2;
    rb::Summary on;
    assert(!on.Report(0, gate, status, controller, lists));
    assert(on.Report(Interval, gate, status, controller, lists));
    ExpectNot(controller, "not seen");
    on.Lists(l);
    assert(on.Report(2 * Interval, gate, status, controller, lists));
    assert(
        controller.find("cs_vrrt_resolve 0x293995352d1f8220 not seen while 1 VRRT applies were") !=
        std::string::npos);
}

static void SummaryTests() {
    SummaryWindow();
    SummaryFormat();
    SummaryRefusals();
    SummaryConsumers();
    SummaryLists();
    SummaryTiming();
    SummaryIdle();
}

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
           "notRequested=0.0%] threshold=7 boundary=0 scheduledRaysPerScreenPixel=1.000 "
           "extraRays=0 rawRays=0 rawMax=0"
           " | lobe2 raysPerRequestedPixel=0.250 raysPerScreenPixel=0.125 noFreshRay=87.5% "
           "allPixels[full=0.0% 1/2-1/4=50.0% 1/5-1/8=0.0% 1/9-1/15=0.0% 1/16+=0.0% "
           "notRequested=50.0%] threshold=0 boundary=0 scheduledRaysPerScreenPixel=0.125 "
           "extraRays=0 rawRays=0 rawMax=0"
           " | enabled=0 promote=0 clamp=1 | unsupported=0");
    assert(report.find("lobe1 raysPerRequestedPixel=1.000 raysPerScreenPixel=1.000 noFreshRay=0.0% "
                       "allPixels[full=100.0%") != std::string::npos);
    assert(report.find("lobe2 raysPerRequestedPixel=0.250 raysPerScreenPixel=0.125 "
                       "noFreshRay=87.5% allPixels[full=0.0% 1/2-1/4=50.0%") != std::string::npos);
    assert(report.find("notRequested=50.0%] threshold=0") != std::string::npos);
    assert(report.find("lobe1 raysPerRequestedPixel=1.000 raysPerScreenPixel=1.000 noFreshRay=0.0% "
                       "allPixels[full=100.0% 1/2-1/4=0.0% 1/5-1/8=0.0% 1/9-1/15=0.0% 1/16+=0.0% "
                       "notRequested=0.0%] threshold=7") != std::string::npos);
    assert(report.find("clamp=1") != std::string::npos);
    assert(report.ends_with(" | enabled=0 promote=0 clamp=1 | unsupported=0"));
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
           "notRequested=20.0%] threshold=16 boundary=28 scheduledRaysPerScreenPixel=0.306 "
           "extraRays=0 rawRays=0 rawMax=0"
           " | lobe2 raysPerRequestedPixel=0.333 raysPerScreenPixel=0.167 noFreshRay=83.3% "
           "allPixels[full=0.0% 1/2-1/4=50.0% 1/5-1/8=0.0% 1/9-1/15=0.0% 1/16+=0.0% "
           "notRequested=50.0%] threshold=-1 boundary=5 scheduledRaysPerScreenPixel=0.167 "
           "extraRays=0 rawRays=0 rawMax=0"
           " | enabled=1 promote=1 clamp=0 | unsupported=0");
    // The longest numbers the budget fields can print still fit (this line outgrew 512 bytes).
    rrdensity::Tracker budget_widest;
    assert(!budget_widest.Report(0, report));
    rrdensity::Frame extreme = mix;
    for (unsigned i = 2 * rrdensity::Buckets; i < rrdensity::Count; ++i)
        extreme[i] = 0x80000000u;
    budget_widest.Add(extreme);
    assert(budget_widest.Report(rrdensity::Tracker::Interval, report));
    assert(report.size() > 512);
    assert(report.ends_with(" | enabled=-2147483648 promote=-2147483648 clamp=-2147483648"
                            " | unsupported=2147483648"));

    // Extra rays at pixels granted several rays are summed per window next to the raw requested
    // rays (raw code - 15 of the pixels asking for one or more) and the largest raw code. They feed
    // ScreenRays, never the per requested pixel density, whose value stays what the base report
    // gave.
    rrdensity::Tracker screen;
    assert(!screen.Report(5000, report));
    rrdensity::Frame granted{};
    granted[1] = 100;                  // lobe 1: 100 pixels at full rate
    granted[rrdensity::Buckets] = 100; // lobe 2: 100 pixels not requested
    granted[rrdensity::Extra] = 50;
    granted[rrdensity::RawRays] = 80;
    granted[rrdensity::RawMax] = 20;
    granted[rrdensity::Limits] = std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    granted[rrdensity::Unsupported] = 3; // requested pixels the reader could not model
    screen.Add(granted);
    assert(screen.Density(0) == 1.0 && screen.Density(1) == 0);
    assert(screen.ScreenRays(0) == 1.5 && screen.ScreenRays(1) == 0);
    granted[rrdensity::Extra] = 10;
    granted[rrdensity::RawRays] = 5;
    granted[rrdensity::RawMax] = 30;
    granted[rrdensity::Extra + 1] = 7;
    granted[rrdensity::RawRays + 1] = 9;
    granted[rrdensity::RawMax + 1] = 4;
    granted[rrdensity::Limits] = 0;
    granted[rrdensity::Unsupported] = 4;
    screen.Add(granted); // sums 60 / 85 / 7 / 9 and 7 unsupported; maxima 30 / 4
    assert(screen.Density(0) == 1.0 && screen.ScreenRays(0) == 1.3); // (200 + 60) / 200
    assert(screen.ScreenRays(1) == 7.0 / 200);                       // 7 extra rays, 200 pixels
    assert(screen.Report(5000 + rrdensity::Tracker::Interval, report));
    assert(report.find("threshold=0 boundary=0 scheduledRaysPerScreenPixel=1.300 extraRays=60 "
                       "rawRays=85 rawMax=30 | lobe2 raysPerRequestedPixel=0.000") !=
           std::string::npos);
    assert(report.find(" scheduledRaysPerScreenPixel=0.035 extraRays=7 rawRays=9 rawMax=4 | "
                       "enabled=0") != std::string::npos);
    // The per screen pixel rate leaves the extra rays out.
    assert(report.find("lobe1 raysPerRequestedPixel=1.000 raysPerScreenPixel=1.000 ") !=
           std::string::npos);
    assert(report.ends_with(" clamp=0 | unsupported=7"));
    assert(screen.ScreenRays(0) == 0 && screen.ScreenRays(1) == 0); // the window starts over
    // Lobe 2 has its own pixel counts: 60 pixels at full rate, 40 at 1/2, 100 not requested.
    rrdensity::Frame second{};
    second[rrdensity::Buckets] = 100;
    second[rrdensity::Buckets + 1] = 60;
    second[rrdensity::Buckets + 2] = 40;
    second[rrdensity::Extra + 1] = 30;
    screen.Add(second);
    assert(screen.Density(1) == 80.0 / 100 && screen.ScreenRays(1) == (60 + 20 + 30) / 200.0);
    assert(screen.ScreenRays(0) == 0);
    // An unsupported threshold (INT_MIN) is printed as the int it is.
    granted = {};
    granted[1] = 1;
    granted[rrdensity::Limits] = std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    screen.Add(granted);
    assert(screen.Report(5000 + 2 * rrdensity::Tracker::Interval, report));
    assert(report.find("threshold=-2147483648 boundary=0 scheduledRaysPerScreenPixel=") !=
           std::string::npos);
    // The longest report: every count at its widest stays inside the 1024-char buffer, untruncated.
    rrdensity::Tracker widest;
    assert(!widest.Report(0, report));
    rrdensity::Frame full{};
    for (auto &word : full)
        word = 0xFFFFFFFFu;
    for (unsigned i = 0; i < 2 * rrdensity::Buckets; ++i)
        full[i] = 1;
    for (unsigned i = rrdensity::Limits; i < rrdensity::Extra; ++i)
        full[i] = std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    for (unsigned k = 0; k < 100000; ++k)
        widest.Add(full);
    assert(widest.Report(rrdensity::Tracker::Interval * 100000, report));
    assert(report.size() < 1024 && report.find("frames=100000") != std::string::npos);
    assert(report.find("rawMax=4294967295 | enabled=-2147483648 promote=-2147483648 "
                       "clamp=-2147483648") != std::string::npos);
    assert(report.ends_with(" clamp=-2147483648 | unsupported=429496729500000"));
    std::printf("longest density report: %zu chars\n", report.size());

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

    // raw is the request code before the budget transform (0 when nothing is requested), so the
    // rays the game asked for can be told apart from the ones the transform adds.
    for (unsigned code = 0; code < 256; ++code)
        assert(rate(code).raw == code && rate(0x1200 | code).raw == code);
    assert(rate(0xFFFF).raw == 0 && rate(0xFFFF, 1, 18, 1, 0, 0, 1).raw == 0);
    assert(rate(30, 1, 18, 1, 0, 0, 1).raw == 30 && rate(30, 1, 18, 1, 0, 0, 1).extra == 1);
    assert(rate(3, 1, -18, 1, 1, 0, 1).raw == 3 && rate(3, 1, -18, 1, 1, 0, 1).extra == 2);
    assert(unsupported.raw == 200 && rate(200, 1, (std::numeric_limits<int>::min)()).raw == 200);
    assert(none.raw == 0 && rate(0).raw == 0 && rate(0).denominator == 16);

    GateTests();
    StatusText();
    SummaryTests();

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
