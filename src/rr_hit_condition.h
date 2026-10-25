#pragma once
#include "rr_hit_condition_math.h"
#include "rr_guide_test.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
namespace rrhitcond {
// Development tests for RR's specular hit distance (shaders/rr_hit_condition.hlsl),
// session-only and off by default. The game writes 0 at sky, at misses and at
// every pixel with no specular ray this frame. Sky and misses: FP16_MAX at
// primary sky and where a fresh specular ray missed. Fill: pixels with no ray
// take nearby hits on the same surface (or the miss value where fresh misses
// dominate). Viewmodel and no-RT-specular pixels are always left raw.
inline std::atomic<bool> sky_miss{}, fill{};
// Bumped by the panel on every change (retries a test blocked for GPU memory).
inline std::atomic<unsigned> changes{};
inline unsigned Mode() {
    return (sky_miss.load(std::memory_order_relaxed) ? SkyMiss : 0u) |
           (fill.load(std::memory_order_relaxed) ? FillHoles : 0u);
}
// Guide tests that give surfaces the view direction as their normal, which
// would pass the fill's normal check across creases (rrguidetest::Mode).
inline bool NormalsReplaced(unsigned guide_test) {
    return guide_test == rrguidetest::FaceCamera || guide_test == rrguidetest::AllFlat;
}
// The tests a guide recording runs: the fill is left out while a guide test
// replaces the normals it compares.
inline unsigned Mode(unsigned guide_test) {
    return Mode() & (NormalsReplaced(guide_test) ? ~FillHoles : ~0u);
}
// Why a frame's distance was or was not conditioned at the native temporal pass.
enum class Outcome : unsigned {
    Off,         // no test on, or RR or the hit distance off
    Repeat,      // a later temporal dispatch in the same recording
    NoSlot,      // no single guide recording on this list
    NoCopy,      // the distance was not copied before the temporal pass
    Weights,     // the temporal t5 lighting weights do not match the guides
    Surface,     // PackedSurface's state on this list is not provably readable
    Context,     // the temporal pass's compute state could not be captured
    Memory,      // no GPU memory for the shared scratch copy
    Failed,      // pipeline creation failed; the tests stay off this session
    Conditioned, // recorded
    Count
};
inline const char *OutcomeName(Outcome o) {
    constexpr const char *names[] = {"off",     "repeat",  "no-slot", "no-copy",
                                     "weights", "surface", "context", "memory",
                                     "failed",  "conditioned"};
    return unsigned(o) < unsigned(Outcome::Count) ? names[unsigned(o)] : "none";
}
// Log-only counters, summarized and cleared every 10 s.
class Diagnostics {
    std::mutex mutex;
    std::array<uint64_t, unsigned(Outcome::Count)> counts{};
    // Conditioned frames whose PackedSurface had an aliasing barrier on its list
    // since its last transition (see mw4_rr.cpp ConditionSpecularDistance), and
    // of those, frames where that barrier named PackedSurface itself rather than
    // having a null side, which covers every resource.
    uint64_t aliased{}, named{};
    Outcome last = Outcome::Count;

  public:
    void Observe(Outcome o) {
        std::lock_guard lock(mutex);
        if (o == Outcome::Off || o == Outcome::Repeat || unsigned(o) >= counts.size())
            return;
        ++counts[unsigned(o)];
        if (o != Outcome::Conditioned)
            last = o;
    }
    void SurfaceAliased(bool naming_surface) {
        std::lock_guard lock(mutex);
        ++aliased;
        named += naming_surface ? 1 : 0;
    }
    // Returns false when nothing happened in the window.
    bool Summary(char *text, size_t size) {
        std::lock_guard lock(mutex);
        uint64_t total{};
        for (auto c : counts)
            total += c;
        auto n = [&](Outcome o) { return static_cast<unsigned long long>(counts[unsigned(o)]); };
        snprintf(text, size,
                 "Specular hit distance tests (10s): conditioned=%llu surfaceAliased=%llu "
                 "(named=%llu) lastSkip=%s [no-slot=%llu no-copy=%llu weights=%llu "
                 "surface=%llu context=%llu memory=%llu failed=%llu]",
                 n(Outcome::Conditioned), static_cast<unsigned long long>(aliased),
                 static_cast<unsigned long long>(named), OutcomeName(last), n(Outcome::NoSlot),
                 n(Outcome::NoCopy), n(Outcome::Weights), n(Outcome::Surface),
                 n(Outcome::Context), n(Outcome::Memory), n(Outcome::Failed));
        counts = {};
        aliased = named = 0;
        last = Outcome::Count;
        return total != 0;
    }
};
using Frame = std::array<uint32_t, StatCount>;
// CPU-only summary of the pass's pixel counts, read back with the gate's.
class Tracker {
    std::array<uint64_t, StatCount> pixels{};
    uint64_t other{};      // pixels in none of the GPU's categories
    uint64_t fill_holes{}; // holes in frames that ran the fill
    unsigned frames{}, sky_frames{}, fill_frames{};
    unsigned raw_frames{}; // RR frames that got the raw distance while a test was on
    uint64_t since{};
    bool started{};
    static double Percent(uint64_t part, uint64_t total) {
        return total ? 100.0 * double(part) / double(total) : 0.0;
    }

  public:
    static constexpr uint64_t Interval = 10000;
    // frame_pixels: the pass's width x height.
    void Add(const Frame &f, unsigned frame_mode, uint64_t frame_pixels) {
        ++frames;
        sky_frames += (frame_mode & SkyMiss) ? 1 : 0;
        if (frame_mode & FillHoles) {
            ++fill_frames;
            fill_holes += f[CountHole];
        }
        for (unsigned i = 0; i < StatCount; ++i)
            pixels[i] += f[i];
        const uint64_t counted = uint64_t(f[CountSky]) + f[CountViewmodel] +
                                 f[CountNoRtSpecular] + f[CountHit] + f[CountMiss] + f[CountHole];
        other += frame_pixels > counted ? frame_pixels - counted : 0;
    }
    void Raw() {
        ++raw_frames;
    }
    uint64_t Pixels(unsigned i) const {
        return i < StatCount ? pixels[i] : 0;
    }
    bool Report(uint64_t now, std::string &out) {
        if (!started) {
            started = true;
            since = now;
            return false;
        }
        if (now < since || now - since < Interval)
            return false;
        const bool active = frames != 0 || raw_frames != 0;
        if (!frames && raw_frames) {
            char text[160];
            snprintf(text, sizeof(text),
                     "Specular hit distance pixels (%llus): frames=0 rawFrames=%u (no frame "
                     "conditioned)",
                     static_cast<unsigned long long>((now - since) / 1000), raw_frames);
            out = text;
        } else if (active) {
            const auto &p = pixels;
            // Surfaces the tests may change: a hit, a miss or no ray this frame.
            const uint64_t surfaces = p[CountHit] + p[CountMiss] + p[CountHole];
            const uint64_t all = surfaces + p[CountSky] + p[CountViewmodel] +
                                 p[CountNoRtSpecular] + other;
            char text[700];
            snprintf(text, sizeof(text),
                     "Specular hit distance pixels (%llus): frames=%u rawFrames=%u "
                     "skyMissFrames=%u fillFrames=%u | sky=%.1f%% viewmodel=%.1f%% "
                     "noRtSpecular=%.1f%% other=%.1f%% surfaces=%.1f%% | of surfaces: "
                     "hit=%.1f%% miss=%.1f%% noRay=%.1f%% | of noRay in fill frames: "
                     "filledFromHits=%.1f%% filledAsMiss=%.1f%% | checks: "
                     "hitWithoutLobe1Ray=%.2f%% hitWithoutLobe2Ray=%.2f%% noRtSpecularHits=%llu",
                     static_cast<unsigned long long>((now - since) / 1000), frames, raw_frames,
                     sky_frames, fill_frames, Percent(p[CountSky], all),
                     Percent(p[CountViewmodel], all), Percent(p[CountNoRtSpecular], all),
                     Percent(other, all), Percent(surfaces, all), Percent(p[CountHit], surfaces),
                     Percent(p[CountMiss], surfaces), Percent(p[CountHole], surfaces),
                     Percent(p[CountFilledHit], fill_holes),
                     Percent(p[CountFilledMiss], fill_holes),
                     Percent(p[CountHitNoLobe1], p[CountHit]),
                     Percent(p[CountHitNoLobe2], p[CountHit]),
                     static_cast<unsigned long long>(p[CountNoRtSpecularHit]));
            out = text;
        }
        *this = {};
        started = true;
        since = now;
        return active;
    }
};
} // namespace rrhitcond
