#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_ray_density.h"
#include "../src/rr_ray_budget.h"
#include "../src/rr_vrrt_rate.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_responsivity.h"
#include "../src/rr_specular_distance.h"
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
static_assert(rrsettings::HotkeyToggles(true, false, false, true));
static_assert(!rrsettings::HotkeyToggles(true, true, false, true));
static_assert(!rrsettings::HotkeyToggles(true, false, true, true));
static_assert(!rrsettings::HotkeyToggles(true, false, false, false));
static_assert(!rrsettings::HotkeyToggles(false, false, false, true));
// More rays: the readback frame appends the extra rays (bytes 228, 232), the raw multi-ray
// potential (236, 240) and the largest raw codes (244, 248); the stats buffer ends at byte 256.
static_assert(rrdensity::Limits == 34 && rrdensity::Settings1 == 38 && rrdensity::Extra == 41 &&
              rrdensity::RawExtra == 43 && rrdensity::RawMax == 45 && rrdensity::Count == 47);
static_assert(rrdensity::Offset + 4 * rrdensity::Limits == 200 &&
              rrdensity::Offset + 4 * rrdensity::Extra == 228 &&
              rrdensity::Offset + 4 * rrdensity::RawExtra == 236 &&
              rrdensity::Offset + 4 * rrdensity::RawMax == 244 &&
              rrdensity::Offset + 4 * rrdensity::Count == 252 &&
              rrdensity::Offset + 4 * rrdensity::Count <= 256);

// The ray budget test: the option steps are fixed, unknown values read as Off, and the numbers the
// gate relies on are pinned (list layout of the native apply, the governor's threshold range).
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
static_assert(rrraybudget::PositiveLimit == 17 && rrraybudget::NegativeLimit == -16 &&
              rrraybudget::ProofFrames == 120 && rrraybudget::TargetFrames == 4 &&
              rrraybudget::Gate::BlockFrames == 300);
static_assert(rrraybudget::SafeThreshold(17) && !rrraybudget::SafeThreshold(18) &&
              rrraybudget::SafeThreshold(-16) && !rrraybudget::SafeThreshold(-17) &&
              rrraybudget::SafeThreshold(0) &&
              rrraybudget::SafeThreshold((std::numeric_limits<int>::min)()));
// Every reason has a text and the table ends where the enum does.
static_assert(std::string_view(rrraybudget::Text(rrraybudget::Why::None)) == "none" &&
              std::string_view(rrraybudget::Text(rrraybudget::Why::Mode)) == "test off" &&
              std::string_view(rrraybudget::Text(rrraybudget::Why::ListsStable)) ==
                  "waiting for 120 identical ray-list proofs" &&
              std::string_view(rrraybudget::Text(rrraybudget::Why::Reconfigure)) ==
                  "RR is reconfiguring" &&
              std::string_view(rrraybudget::Text(rrraybudget::Why::Governor)) ==
                  "controller threshold beyond the one-ray bound after the governor" &&
              std::string_view(rrraybudget::Text(rrraybudget::Why::Exception)) ==
                  "internal error" &&
              std::string_view(rrraybudget::Text(rrraybudget::Why::Count)) == "unknown");

namespace rb = rrraybudget;

// The governor's premise: with a raised lobe's threshold clamped to [NegativeLimit, PositiveLimit]
// the budget transform never adds a ray, so a pixel gets 1 + max(raw - 16, 0) rays at most.
// Enumerated over every threshold, request, flag combination and tile ring position.
static void GovernorBounds() {
    long checked = 0, supported = 0;
    unsigned worst = 0;
    for (int threshold = rb::NegativeLimit; threshold <= rb::PositiveLimit; ++threshold)
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
                    ++checked;
                    assert(r.raw == raw);
                    if (!r.supported)
                        continue;
                    ++supported;
                    assert(r.extra <= (raw > 16 ? raw - 16 : 0u));
                    worst = (std::max)(worst, r.extra);
                }
    assert(checked == 34 * 2 * 256 * 16 - 34 * 16 && supported == checked);
    assert(worst == 255 - 16); // only a raw code above 16 asks for more than one ray
    // Outside the range the transform itself creates extra rays.
    assert(rrvrrt::InverseRate(0xFF00u | 5, 1, 1, 1, -17, -1, 0).extra ==
           1); // promote, inner pixel
    assert(rrvrrt::InverseRate(0xFF00u | 30, 1, 0, 1, 18, -1, 0).extra == 1); // clamp, inner pixel
    assert(rrvrrt::InverseRate(0xFF00u | 30, 1, 0, 1, 17, -1, 0).extra == 0);
    assert(rrvrrt::InverseRate(0xFF00u | 5, 1, 1, 1, -16, -1, 0).extra == 0 &&
           rrvrrt::InverseRate(0xFF00u | 5, 1, 1, 1, -16, -1, 0).denominator == 1);
    assert(!rb::SafeThreshold(-17) && !rb::SafeThreshold(18));
}

// A proven apply: dispatch groups cover the extent, every list holds `entries` entries.
static rb::Lists Proven(std::uint64_t entries, std::uint32_t w = 1920, std::uint32_t h = 1080) {
    rb::Lists l;
    l.w = w;
    l.h = h;
    l.gx = (w + 15) / 16;
    l.gy = (h + 15) / 16;
    l.why = rb::Why::None;
    for (unsigned i = 0; i < 2; ++i)
        l.v[i] = {rb::Binding::Table, 0x1000 + i, 0, rb::ListHeader + entries * rb::EntryBytes};
    l.v[2] = {rb::Binding::Table, 0x2000, 0, rb::PixelEntryBytes * l.Area()};
    l.v[3] = {rb::Binding::Table, 0x3000, 0, 16};
    l.budget_resource = 0x4000;
    return l;
}

// A completed reader frame: lobe 1 requests half the pixels at full rate, lobe 2 all of them; the
// game's target is `target`, its rate control is on and both thresholds are inside the range.
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
    c.logged = rb::LogResolveTarget;
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
    // Fewer than 120 consecutive proven applies at one extent: waiting.
    auto g = Primed(l, rb::ProofFrames - 1);
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
    // 120 proofs without measured frames: evidence missing.
    auto e = Primed(l, rb::ProofFrames, 0);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::Evidence);
    e = Primed(l, rb::ProofFrames, rb::ProofFrames - 1);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::Evidence);
    assert(e.Complete(Measured(l)) == rb::Why::None);
    assert(e.Decide(rb::Times2, now, nullptr).why == rb::Why::None);
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
    other.why = rb::Why::ListsUnproven;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = Proven(1920ull * 1088 + 100000, 1280, 720);
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.h = 1079;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.gx += 1; // the dispatch covers more threads than the streak was proven for
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.gy -= 1;
    assert(g.Decide(rb::Times2, now, &other).why == rb::Why::ListsChanged);
    other = l;
    other.v[0].bytes += 4096;
    other.v[0].resource = 0x9000; // a larger list in another buffer is fine
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
    // The pixel counters hold W*H*239 in 32 bits.
    assert(std::uint64_t(4239) * 4239 * 239 < (1ull << 32) &&
           std::uint64_t(4240) * 4240 * 239 >= (1ull << 32));
    const auto largest = Proven(4240ull * 4240 + 100000, 4239, 4239);
    const auto too_large = Proven(4240ull * 4240 + 100000, 4240, 4240);
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
    auto g = Primed(l);
    // Lobe 1 requests half the pixels, so its target stops at 0.5 (the requested share).
    auto p = g.Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 3);
    assert(Near(p.native[0], 0.4f) && Near(p.native[1], 0.4f));
    assert(Near(p.target[0], 0.5f) && Near(p.target[1], 0.8f));
    assert(p.bits[0] == std::bit_cast<std::uint32_t>(p.target[0]) &&
           p.bits[1] == std::bit_cast<std::uint32_t>(p.target[1]));
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
    // Lobes are decided one by one: the first reason is kept only when no lobe arms.
    g = Primed(l);
    auto mixed = Measured(l);
    mixed.log[0] = std::bit_cast<std::uint32_t>(0.5f); // lobe 1 already at its requested share
    for (unsigned i = 0; i < rb::TargetFrames; ++i)
        g.Complete(mixed);
    p = g.Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 2 && p.target[0] == 0 &&
           Near(p.target[1], 0.4f * 2));
    mixed.log[1] = std::bit_cast<std::uint32_t>(0.0f);
    for (unsigned i = 0; i < rb::TargetFrames; ++i)
        g.Complete(mixed);
    p = g.Decide(rb::Times2, now, nullptr); // lobe 1 at the cap, lobe 2 out of range
    assert(p.why == rb::Why::AtCap && p.mask == 0);
    // Lists that hold less than one ray per pixel plus margin never arm.
    auto small = Proven(1920ull * 1080);
    g = Primed(small);
    assert(g.Decide(rb::OnePerPixel, now, nullptr).why == rb::Why::ListsSmall);
    auto exact = Proven(1920ull * 1088 + rb::ConsumerSlack); // area + slack: just enough
    assert(Primed(exact).Decide(rb::Times2, now, nullptr).mask == 3);
    exact = Proven(1920ull * 1088 + rb::ConsumerSlack - 1);
    assert(Primed(exact).Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    // The smallest list of the streak counts, not the last one.
    g = Primed(l, rb::ProofFrames - 1);
    g.Apply(Proven(1920ull * 1088), now);
    assert(g.Streak() == rb::ProofFrames);
    for (unsigned i = 0; i < rb::ProofFrames; ++i)
        g.Complete(Measured(l));
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    // Lobe 1 needs a pixel list of 4 bytes per dispatched pixel, both lobes a 4-byte counter.
    auto pixels = l;
    pixels.v[2].bytes -= 1;
    p = Primed(pixels).Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 2);
    auto counter = l;
    counter.v[3].bytes = 3;
    assert(Primed(counter).Decide(rb::Times2, now, nullptr).why == rb::Why::ListsSmall);
    // Raw multi-ray requests widen the margin the lists must hold; it covers two blocks of frames.
    g = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto f = Measured(l);
        f.rates[rrdensity::RawExtra] = 10 + i % 3;
        f.rates[rrdensity::RawExtra + 1] = 3;
        g.Complete(f);
    }
    assert(g.Margin(0) == 24 && g.Margin(1) == 6);
    assert(g.Hard(0) && g.Hard(1) && !g.Hard(2));
    auto tight = Proven(1920ull * 1088 + rb::ConsumerSlack + 23); // one entry short of the margin
    g = Primed(tight, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto f = Measured(tight);
        f.rates[rrdensity::RawExtra] = 12;
        g.Complete(f);
    }
    assert(g.Margin(0) == 24 && !g.Hard(0) && g.Hard(1));
    p = g.Decide(rb::Times2, now, nullptr);
    assert(p.why == rb::Why::None && p.mask == 2);
    // Margin and requested share are rolling minima/maxima over two blocks of BlockFrames.
    g = Primed(l, rb::ProofFrames, 0);
    auto burst = Measured(l);
    burst.rates[rrdensity::RawExtra] = 10;
    burst.rates[1] = 1000; // lobe 1 requests few pixels in this block
    burst.rates[0] = l.w * l.h - 1000;
    for (unsigned i = 0; i < rb::Gate::BlockFrames; ++i)
        g.Complete(burst);
    assert(g.Margin(0) == 20 && g.Requested(0) < 0.001);
    for (unsigned i = 0; i < rb::Gate::BlockFrames; ++i)
        g.Complete(Measured(l)); // the burst moves to the previous block
    assert(g.Margin(0) == 20 && g.Requested(0) < 0.001);
    g.Complete(Measured(l)); // and is forgotten one block later
    assert(g.Margin(0) == 0 && Near(float(g.Requested(0)), 0.5f) && g.Requested(1) == 1.0);
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
    // The governor did not keep a raised lobe's threshold inside [-16, 17].
    g = Primed(l);
    g.Patched(g.Decide(rb::Times2, now, nullptr));
    auto c = Measured(l);
    c.budget = 3;
    c.rates[rrdensity::Limits] = 17;
    c.rates[rrdensity::Limits + 2] = std::uint32_t(-16);
    assert(g.Complete(c) == rb::Why::None); // both bounds are fine
    c.rates[rrdensity::Limits + 2] =
        std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    assert(g.Complete(c) == rb::Why::None); // INT_MIN is the unsupported value, not a threshold
    c.rates[rrdensity::Limits + 2] = std::uint32_t(-17);
    c.budget = 1; // lobe 2 was not raised: its threshold is the game's business
    assert(g.Complete(c) == rb::Why::None && g.Stopped() == rb::Why::None);
    c.budget = 3;
    assert(g.Complete(c) == rb::Why::Governor && g.Stopped() == rb::Why::Governor);
    assert(g.Complete(c) == rb::Why::None); // only the frame that newly stops reports
    assert(g.Decide(rb::Times2, now, nullptr).why == rb::Why::Stopped);
    g = Primed(l);
    c.rates[rrdensity::Limits + 2] = 9;
    c.rates[rrdensity::Limits] = 18;
    assert(g.Complete(c) == rb::Why::Governor);
    // The constants the controller executed differ from the ones written.
    g = Primed(l);
    const auto q = g.Decide(rb::Times15, now, nullptr);
    assert(q.mask == 3);
    g.Patched(q);
    auto d = Measured(l);
    d.patched = 3;
    d.written = q.bits;
    d.logged |= rb::LogExecuted;
    d.log[4] = q.bits[0];
    d.log[5] = q.bits[1];
    assert(g.Complete(d) == rb::Why::None);
    auto d1 = d;
    d1.patched = 1; // only lobe 1 patched: lobe 2's words are not compared
    d1.log[5] ^= 1;
    assert(g.Complete(d1) == rb::Why::None);
    auto d2 = d;
    d2.logged &= ~rb::LogExecuted; // the copy after the patch never ran
    assert(g.Complete(d2) == rb::Why::PatchMismatch && g.Stopped() == rb::Why::PatchMismatch);
    g = Primed(l);
    g.Patched(q);
    d.log[5] ^= 1;
    assert(g.Complete(d) == rb::Why::PatchMismatch);
    // Multi-ray requests grew beyond the margin the lists were armed with.
    rb::Gate m;
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        m.Apply(l, i);
        auto f = Measured(l);
        f.rates[rrdensity::RawExtra] = 10;
        m.Complete(f);
    }
    const auto armed = m.Decide(rb::Times2, now, nullptr);
    assert(armed.mask == 3);
    m.Patched(armed);
    auto grown = Measured(l);
    grown.budget = 1;
    grown.rates[rrdensity::RawExtra] = 20;
    assert(m.Complete(grown) == rb::Why::None); // 2 x 10 is the margin: equal is fine
    grown.budget = 2;
    grown.rates[rrdensity::RawExtra + 1] = 1; // lobe 2 was armed with a margin of 0
    assert(m.Complete(grown) == rb::Why::MultiRay);
    m = Primed(l, rb::ProofFrames, 0);
    for (unsigned i = 0; i < rb::ProofFrames; ++i) {
        auto f = Measured(l);
        f.rates[rrdensity::RawExtra] = 10;
        m.Complete(f);
    }
    m.Patched(m.Decide(rb::Times2, now, nullptr));
    grown.budget = 1;
    grown.rates[rrdensity::RawExtra] = 21;
    assert(m.Complete(grown) == rb::Why::MultiRay);
    // The scheduled rays came within 512 entries of a list's capacity: requested pixels, extra rays
    // and the threads past the extent (the dispatch covers more than w*h) plus one consumer group.
    const auto tight = Proven(1920ull * 1088 + 600);
    const std::uint64_t pixels = tight.w * tight.h, past = tight.Area() - pixels;
    const std::uint64_t room1 = tight.Entries(0) - pixels / 2 - past - rb::ConsumerSlack,
                        room2 = tight.Entries(1) - pixels - past - rb::ConsumerSlack;
    assert(room1 == 1036888 && room2 == 88);
    rb::Gate o = Primed(tight);
    const auto plan = o.Decide(rb::Times2, now, nullptr);
    assert(plan.mask == 3);
    o.Patched(plan);
    auto full = Measured(tight);
    full.budget = 2;
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2); // exactly full: still fine
    assert(o.Complete(full) == rb::Why::None);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    assert(o.Complete(full) == rb::Why::Overflow && o.Stopped() == rb::Why::Overflow);
    // Lobe 1 counts the requested pixels of its own histogram.
    o = Primed(tight);
    o.Patched(plan);
    full = Measured(tight);
    full.budget = 1;
    full.rates[rrdensity::Extra] = std::uint32_t(room1);
    assert(o.Complete(full) == rb::Why::None);
    full.rates[rrdensity::Extra] = std::uint32_t(room1 + 1);
    assert(o.Complete(full) == rb::Why::Overflow);
    // Lobes that were not raised are not checked.
    o = Primed(tight);
    full = Measured(tight);
    full.rates[rrdensity::Extra] = std::uint32_t(room1 + 1);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    full.budget = 0;
    assert(o.Complete(full) == rb::Why::None);
    full.budget = 2; // lobe 2 only
    assert(o.Complete(full) == rb::Why::Overflow);
    // A frame whose own apply was not proven is judged against the gate's last proof; a patched
    // lobe counts as raised even when the budget mask is empty.
    o = Primed(tight);
    full = Measured(tight);
    full.rates[rrdensity::Extra + 1] = std::uint32_t(room2 + 1);
    full.patched = 2;
    full.logged |= rb::LogExecuted;
    full.written = {0, 0};
    full.log[4] = full.log[5] = 0;
    full.lists = rb::Lists{};
    assert(o.Complete(full) == rb::Why::Overflow);
    // A dispatch that covers fewer threads than the extent has none past it (and cannot wrap).
    o = Primed(tight);
    auto narrow = tight;
    narrow.gx = narrow.gy = 1;
    full = Measured(narrow);
    full.budget = 3;
    assert(o.Complete(full) == rb::Why::None && o.Stopped() == rb::Why::None);
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
    assert(describe(s) == "waiting: waiting for 120 identical ray-list proofs");
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

// A frame whose patch was executed and verified, with every optional log word present.
static rb::Completed Verified(const rb::Lists &l, const rb::Plan &plan) {
    auto c = Measured(l);
    c.patched = plan.mask;
    c.written = plan.bits;
    c.logged |= rb::LogExecuted | rb::LogResolveScene | rb::LogApplyTarget |
                rb::LogApplySettings36 | rb::LogApplyScene251;
    c.log[4] = plan.bits[0];
    c.log[5] = plan.bits[1];
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
    for (unsigned i = 0; i < 20; ++i) { // a raw multi-ray margin of 24 for lobe 1
        auto f = Measured(l);
        f.rates[rrdensity::RawExtra] = 12;
        gate.Complete(f);
    }
    const auto plan = gate.Decide(rb::Times2, 119, nullptr);
    assert(plan.mask == 3);
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
    // Five patched frames: three verified, one with a different word, one that never executed.
    auto f1 = Verified(l, plan), f2 = Verified(l, plan), f3 = Verified(l, plan);
    f2.log[0] = std::bit_cast<std::uint32_t>(0.25f);
    f2.log[1] = std::bit_cast<std::uint32_t>(0.6f);
    f3.log[0] = std::bit_cast<std::uint32_t>(0.5f);
    f3.rates[rrdensity::Extra] = 1000;
    f3.rates[rrdensity::Extra + 1] = 20000;
    f3.rates[rrdensity::RawExtra] = 30;
    f3.rates[rrdensity::RawExtra + 1] = 40;
    f3.rates[rrdensity::Limits] = 17;
    f3.rates[rrdensity::Limits + 2] = std::uint32_t(-16);
    auto f4 = Verified(l, plan), f5 = Verified(l, plan);
    f4.log[5] ^= 1;
    f4.log[9] = std::bit_cast<std::uint32_t>(1081.0f);
    f5.logged &= ~rb::LogExecuted;
    f5.log[0] = std::bit_cast<std::uint32_t>(0.4f);
    f5.log[1] = std::bit_cast<std::uint32_t>(0.45f);
    f5.rates[rrdensity::Limits] = std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    for (const auto &f : {f1, f2, f3, f4, f5})
        sm.Completed(f);
    assert(!sm.Report(1000 + Interval - 1, gate, status, controller, lists));
    assert(controller.empty() && lists.empty());
    assert(sm.Report(1000 + Interval, gate, status, controller, lists));
    Expect(controller, "RR ray budget (10s): test=2x the game's rays state=active patched=5/12 "
                       "verified=3 | resolve seen=17 inRecording=12 afterApply=10 beforeApply=2 "
                       "outside=2 beforePrepass=1 afterSR=1 repeated=1 clears=4/5 "
                       "sameB1AsApply=11/14 sameBudget=7/8 | target native=0.400/0.450 "
                       "(0.250..0.500/0.400..0.600) apply=0.500/0.800 written=0.500/0.800 "
                       "scene=1920x1080 match=4/5 settings36.w=0x10000 scene251.x=0x400 | "
                       "consumers lobe1=0 lobe2=0 dispatch=0 rays=0 other=0 last=n/a | refused "
                       "listsStable=12 heap=3 outside=2 afterSR=1 repeated=1 ambiguous=1");
    Expect(lists, "RR ray lists (10s): applies=8 proven=7 streak=120 1920x1080 groups=120x68 "
                  "area=2088960 | lobe1 u5=table 8755984B entries=2188960 (1.048x area) "
                  "need=2089496 tier=hard worstFill=48.1% rawExtraMax=30 "
                  "threshold=9..17 | lobe2 u6=table 8755984B entries=2188960 (1.048x area) "
                  "need=2089472 tier=hard worstFill=96.3% rawExtraMax=40 "
                  "threshold=-16..9 | pixels u3=table 8355840B (4.000 B/px) counter u2=table 16B "
                  "| why=none");
    // The window starts over; a quiet one prints only while the test is on.
    assert(!sm.Report(1000 + 2 * Interval, gate, rb::Status{}, controller, lists));
    controller = lists = "stale";
    assert(sm.Report(1000 + 3 * Interval, gate, status, controller, lists));
    Expect(controller, "patched=0/0 verified=0 | resolve seen=0 inRecording=0");
    Expect(controller,
           "| target native=n/a apply=n/a written=- scene=n/a match=0/0 settings36.w=n/a "
           "scene251.x=n/a | consumers lobe1=0");
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
           "RR ray budget (10s): test=2x the game's rays state=waiting: waiting for 120 identical "
           "ray-list proofs patched=0/0 verified=0 | resolve seen=0 inRecording=0 afterApply=0 "
           "beforeApply=0 outside=0 beforePrepass=0 afterSR=0 repeated=0 clears=0/0 "
           "sameB1AsApply=0/0 sameBudget=0/0 | target native=n/a apply=n/a written=- scene=n/a "
           "match=0/0 settings36.w=n/a scene251.x=n/a | consumers lobe1=0 lobe2=0 dispatch=0 "
           "rays=0 other=0 last=n/a");
    status.mode = rb::Off;
    assert(!sm.Report(2 * Interval, gate, status, controller, lists)); // nothing seen, test off
    // The state word: off, waiting, active, stopped.
    sm.Resolve(false, false);
    assert(sm.Report(3 * Interval, gate, status, controller, lists));
    Expect(controller, "test=Off (game's ray budget) state=off patched=0/1");
    status.mode = rb::OnePerPixel;
    sm.Resolve(false, false);
    assert(sm.Report(4 * Interval, gate, status, controller, lists));
    Expect(controller, "test=Up to 1 ray per pixel state=waiting: waiting for 120 identical "
                       "ray-list proofs patched=0/1");
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
    Expect(controller, "target native=0.400/0.400 (0.400..0.400/0.400..0.400) apply=n/a written=- "
                       "scene=n/a match=0/0");
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
    // counts keep the enum's order.
    std::string expected;
    for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
        expected += std::string(w > 1 ? " " : "") + rb::keys[w] + "=1";
    Expect(controller, " | refused " + expected.substr(0, 400));
    for (unsigned a = 0; a < unsigned(rb::Why::Count); ++a) {
        assert(rb::keys[a][0] >= 'a' && rb::keys[a][0] <= 'z' && rb::Text(rb::Why(a))[0] != 0);
        for (unsigned b = a + 1; b < unsigned(rb::Why::Count); ++b) {
            assert(std::string(rb::keys[a]) != rb::keys[b]);
            assert(std::string(rb::Text(rb::Why(a))) != rb::Text(rb::Why(b)));
        }
    }
    assert(std::string(rb::keys[unsigned(rb::Why::AfterSR)]) == "afterSR" &&
           std::string(rb::keys[unsigned(rb::Why::ListsStable)]) == "listsStable" &&
           std::string(rb::keys[unsigned(rb::Why::Exception)]) == "exception");
    // The line is one 1024-char buffer: a refusal list that does not fit is cut, never overrun.
    for (unsigned k = 0; k < 100000; ++k)
        for (unsigned w = 1; w < unsigned(rb::Why::Count); ++w)
            sm.Refused(rb::Why(w));
    assert(sm.Report(3 * Interval, gate, status, controller, lists));
    assert(controller.size() < 1024 && controller.size() > 900);
    Expect(controller, " | refused mode=100000 stopped=100000 context=100000 shape=100000 ");
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
    // The last proof is what is described, proven or not.
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
    Expect(lists, "| lobe1 u5=root 8755984B entries=0 (0.000x area) need=2089472 tier=");
    Expect(lists, "| lobe2 u6=view 8755984B entries=0 (0.000x area) need=2089472 tier=");
    Expect(lists, "worstFill=n/a rawExtraMax=0 threshold=n/a");
    Expect(lists,
           "| pixels u3=unknown 8355840B (4.000 B/px) counter u2=table 0B | why=a ray list is a "
           "root UAV (no bounds checks)");
    // Small lists, a margin that grew and a list that is not there at all.
    auto small = Proven(1920ull * 1080);
    gate = Primed(small);
    sm.Lists(small);
    assert(sm.Report(2 * Interval, gate, status, controller, lists));
    Expect(lists, "area=2088960 | lobe1 u5=table 8294544B entries=2073600 (0.993x area) "
                  "need=2089472 tier=small");
    rb::Lists none;
    sm.Lists(none);
    assert(sm.Report(3 * Interval, gate, status, controller, lists));
    Expect(lists, "applies=1 proven=0 streak=120 0x0 groups=0x0 area=0 | lobe1 u5=unknown 0B "
                  "entries=0 (0.000x area) need=512 tier=small worstFill=n/a");
    Expect(lists, "| pixels u3=unknown 0B (0.000 B/px) counter u2=unknown 0B | why=ray lists not "
                  "proven at the VRRT apply");
    // Fill uses the last proof when the frame's own apply was not proven.
    gate = Primed(l);
    sm.Lists(l);
    auto frame = Measured(l);
    frame.lists = rb::Lists{};
    frame.rates[rrdensity::Extra + 1] = 20000;
    sm.Completed(frame);
    assert(sm.Report(4 * Interval, gate, status, controller, lists));
    Expect(lists, "worstFill=96.3%");
    // Without any proof there is nothing to measure against.
    sm.Lists(none);
    sm.Completed(frame);
    assert(sm.Report(5 * Interval, gate, status, controller, lists));
    Expect(lists, "worstFill=n/a");
    // A lobe whose list holds nothing has no fill; the other still does.
    auto empty = l;
    empty.v[0].bytes = rb::ListHeader;
    sm.Lists(empty);
    sm.Completed(Measured(empty));
    assert(sm.Report(6 * Interval, gate, status, controller, lists));
    Expect(lists,
           "lobe1 u5=table 144B entries=0 (0.000x area) need=2089472 tier=hard worstFill=n/a");
    Expect(lists, "lobe2 u6=table 8755984B entries=2188960 (1.048x area) need=2089472 tier=hard "
                  "worstFill=95.4%");
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

    // Extra rays at pixels granted several rays are summed per window next to the raw multi-ray
    // potential and the largest raw code. They feed ScreenRays, never the per requested pixel
    // density, whose value stays what the base report gave.
    rrdensity::Tracker screen;
    assert(!screen.Report(5000, report));
    rrdensity::Frame granted{};
    granted[1] = 100;                  // lobe 1: 100 pixels at full rate
    granted[rrdensity::Buckets] = 100; // lobe 2: 100 pixels not requested
    granted[rrdensity::Extra] = 50;
    granted[rrdensity::RawExtra] = 80;
    granted[rrdensity::RawMax] = 20;
    granted[rrdensity::Limits] = std::bit_cast<std::uint32_t>((std::numeric_limits<int>::min)());
    screen.Add(granted);
    assert(screen.Density(0) == 1.0 && screen.Density(1) == 0);
    assert(screen.ScreenRays(0) == 1.5 && screen.ScreenRays(1) == 0);
    granted[rrdensity::Extra] = 10;
    granted[rrdensity::RawExtra] = 5;
    granted[rrdensity::RawMax] = 30;
    granted[rrdensity::Extra + 1] = 7;
    granted[rrdensity::RawExtra + 1] = 9;
    granted[rrdensity::RawMax + 1] = 4;
    granted[rrdensity::Limits] = 0;
    screen.Add(granted); // sums 60 / 85 / 7 / 9; maxima 30 / 4
    assert(screen.Density(0) == 1.0 && screen.ScreenRays(0) == 1.3); // (200 + 60) / 200
    assert(screen.ScreenRays(1) == 7.0 / 200);                       // 7 extra rays, 200 pixels
    assert(screen.Report(5000 + rrdensity::Tracker::Interval, report));
    assert(report.find("threshold=0 boundary=0 raysPerScreenPixel=1.300 extraRays=60 rawExtra=85 "
                       "rawMax=30 | lobe2 raysPerPixel=0.000") != std::string::npos);
    assert(report.find("raysPerScreenPixel=0.035 extraRays=7 rawExtra=9 rawMax=4 | enabled=0") !=
           std::string::npos);
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
    assert(report.find("threshold=-2147483648 boundary=0 raysPerScreenPixel=") !=
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
}
