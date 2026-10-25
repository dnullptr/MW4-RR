#pragma once
#include "rr_hit_condition_math.h"
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
    // Returns false when nothing happened in the window.
    bool Summary(char *text, size_t size) {
        std::lock_guard lock(mutex);
        uint64_t total{};
        for (auto c : counts)
            total += c;
        auto n = [&](Outcome o) { return static_cast<unsigned long long>(counts[unsigned(o)]); };
        snprintf(text, size,
                 "Specular hit distance tests (10s): conditioned=%llu lastSkip=%s "
                 "[no-slot=%llu no-copy=%llu weights=%llu surface=%llu context=%llu "
                 "memory=%llu failed=%llu]",
                 n(Outcome::Conditioned), OutcomeName(last), n(Outcome::NoSlot),
                 n(Outcome::NoCopy), n(Outcome::Weights), n(Outcome::Surface),
                 n(Outcome::Context), n(Outcome::Memory), n(Outcome::Failed));
        counts = {};
        last = Outcome::Count;
        return total != 0;
    }
};
using Frame = std::array<uint32_t, StatCount>;
// CPU-only summary of the pass's pixel counts, read back with the gate's.
class Tracker {
    std::array<uint64_t, StatCount> pixels{};
    unsigned frames{}, mode{};
    uint64_t since{};
    bool started{};
    static double Percent(uint64_t part, uint64_t total) {
        return total ? 100.0 * double(part) / double(total) : 0.0;
    }

  public:
    static constexpr uint64_t Interval = 10000;
    void Add(const Frame &f, unsigned frame_mode) {
        ++frames;
        mode |= frame_mode;
        for (unsigned i = 0; i < StatCount; ++i)
            pixels[i] += f[i];
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
        const bool active = frames != 0;
        if (active) {
            const auto &p = pixels;
            const uint64_t traced = p[CountHit] + p[CountMiss] + p[CountHole];
            const uint64_t all = traced + p[CountSky] + p[CountViewmodel] +
                                 p[CountNoRtSpecular] + p[CountOther];
            char text[640];
            snprintf(text, sizeof(text),
                     "Specular hit distance pixels (%llus): frames=%u skyMiss=%d fill=%d | "
                     "sky=%.1f%% viewmodel=%.1f%% noRtSpecular=%.1f%% other=%.1f%% traced=%.1f%% "
                     "| of traced: hit=%.1f%% miss=%.1f%% noRay=%.1f%% | of noRay: "
                     "filledFromHits=%.1f%% filledAsMiss=%.1f%% | checks: hitWithoutLobe1Ray=%.2f%% "
                     "hitWithoutLobe2Ray=%.2f%% noRtSpecularHits=%llu",
                     static_cast<unsigned long long>((now - since) / 1000), frames,
                     (mode & SkyMiss) ? 1 : 0, (mode & FillHoles) ? 1 : 0,
                     Percent(p[CountSky], all), Percent(p[CountViewmodel], all),
                     Percent(p[CountNoRtSpecular], all), Percent(p[CountOther], all),
                     Percent(traced, all), Percent(p[CountHit], traced),
                     Percent(p[CountMiss], traced), Percent(p[CountHole], traced),
                     Percent(p[CountFilledHit], p[CountHole]),
                     Percent(p[CountFilledMiss], p[CountHole]),
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
