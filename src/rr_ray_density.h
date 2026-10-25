#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
namespace rrdensity {
// Layout written by shaders/rr_vrrt_probability.hlsl after the gate's 16 bytes:
// per lobe 17 pixel counts by inverse rate (0 = not requested, 16 = 16 or more),
// then lobe 1 and lobe 2 budget threshold/boundary, then enabled/promote/clamp.
inline constexpr unsigned Buckets = 17, Offset = 64, Count = 2 * Buckets + 7;
using Frame = std::array<std::uint32_t, Count>;
// CPU-only summary of how many rays the game's VRRT schedules, logged periodically to show how
// sparse the lighting RR receives is. Bucket 0 (not requested) is left out of Density, which is
// per requested pixel, but counts in ScreenDensity and in every Share, which are of all pixels.
class Tracker {
    std::array<std::uint64_t, 2 * Buckets> pixels{};
    std::array<std::uint32_t, 7> last{};
    unsigned frames{};
    std::uint64_t since{};
    bool started{};

  public:
    static constexpr std::uint64_t Interval = 10000;
    void Add(const Frame &f) {
        ++frames;
        for (unsigned i = 0; i < 2 * Buckets; ++i)
            pixels[i] += f[i];
        for (unsigned i = 0; i < last.size(); ++i)
            last[i] = f[2 * Buckets + i];
    }
    // Expected rays per requested pixel per frame for one lobe, 0..1.
    double Density(unsigned lobe) const {
        double rays = 0, requested = 0;
        for (unsigned i = 1; i < Buckets; ++i) {
            const double n = double(pixels[lobe * Buckets + i]);
            rays += n / i;
            requested += n;
        }
        return requested ? rays / requested : 0;
    }
    // Expected rays per screen pixel per frame for one lobe, 0..1: a pixel at inverse rate i gets
    // a ray with probability 1/i, a not-requested pixel none. 1 - this is the no-fresh-ray share.
    double ScreenDensity(unsigned lobe) const {
        double rays = 0, all = 0;
        for (unsigned i = 0; i < Buckets; ++i) {
            const double n = double(pixels[lobe * Buckets + i]);
            if (i)
                rays += n / i;
            all += n;
        }
        return all ? rays / all : 0;
    }
    double Share(unsigned lobe, unsigned first, unsigned end) const {
        std::uint64_t part = 0, total = 0;
        for (unsigned i = 0; i < Buckets; ++i) {
            const auto n = pixels[lobe * Buckets + i];
            total += n;
            if (i >= first && i < end)
                part += n;
        }
        return total ? 100.0 * double(part) / double(total) : 0;
    }
    bool Report(std::uint64_t now, std::string &out) {
        if (!started) {
            started = true;
            since = now;
            return false;
        }
        if (now < since || now - since < Interval)
            return false;
        const bool active = frames != 0;
        if (active) {
            char text[1024];
            int at = std::snprintf(text, sizeof(text), "RR ray density (%llus): frames=%u",
                                   static_cast<unsigned long long>((now - since) / 1000), frames);
            for (unsigned lobe = 0; lobe < 2 && at > 0 && at < int(sizeof(text)); ++lobe) {
                const double screen = ScreenDensity(lobe);
                at += std::snprintf(
                    text + at, sizeof(text) - at,
                    " | lobe%u raysPerRequestedPixel=%.3f raysPerScreenPixel=%.3f "
                    "noFreshRay=%.1f%% allPixels[full=%.1f%% 1/2-1/4=%.1f%% 1/5-1/8=%.1f%% "
                    "1/9-1/15=%.1f%% 1/16+=%.1f%% notRequested=%.1f%%] threshold=%d boundary=%d",
                    lobe + 1, Density(lobe), screen, 100.0 * (1 - screen), Share(lobe, 1, 2),
                    Share(lobe, 2, 5), Share(lobe, 5, 9), Share(lobe, 9, 16), Share(lobe, 16, 17),
                    Share(lobe, 0, 1), int(last[lobe * 2]), int(last[lobe * 2 + 1]));
            }
            if (at > 0 && at < int(sizeof(text)))
                std::snprintf(text + at, sizeof(text) - at, " | enabled=%d promote=%d clamp=%d",
                              int(last[4]), int(last[5]), int(last[6]));
            out = text;
        }
        *this = {};
        started = true;
        since = now;
        return active;
    }
};
} // namespace rrdensity
