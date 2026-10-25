#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
namespace rrhealth {
// CPU-only counters for completed RR frames and history resets. Reported
// periodically so frame rejection and reset frequency show up in ReShade.log.
class Tracker {
    unsigned frames{}, rejected{}, isolated_frames{}, sanitized_frames{}, resets{};
    std::array<unsigned, 4> reasons{};
    std::uint32_t isolated_max{}, sanitized_max{};
    std::uint64_t since{};
    bool started{};

  public:
    static constexpr std::uint64_t Interval = 10000;
    // stats: bad frame, copied frame, isolated guide pixels, sanitized SSS samples.
    void Frame(const std::array<std::uint32_t, 4> &stats) {
        ++frames;
        rejected += stats[0] != 0 || stats[1] == 0;
        isolated_frames += stats[2] != 0;
        sanitized_frames += stats[3] != 0;
        if (stats[2] > isolated_max)
            isolated_max = stats[2];
        if (stats[3] > sanitized_max)
            sanitized_max = stats[3];
    }
    // reason: rrlive::HistoryPolicy::Reason bits.
    void Reset(unsigned reason) {
        ++resets;
        for (unsigned i = 0; i < reasons.size(); ++i)
            reasons[i] += (reason >> i) & 1;
    }
    // Returns a summary once per interval with activity, then starts a new interval.
    bool Report(std::uint64_t now, std::string &out) {
        if (!started) {
            started = true;
            since = now;
            return false;
        }
        if (now < since || now - since < Interval)
            return false;
        const bool active = frames || resets;
        if (active) {
            char text[384];
            std::snprintf(text, sizeof(text),
                          "RR health (%llus): frames=%u rejected=%u resets=%u "
                          "[rejected=%u cut=%u generation=%u gap=%u] "
                          "isolatedGuideFrames=%u maxIsolatedPixels=%u "
                          "sanitizedSssFrames=%u maxSanitizedSamples=%u",
                          static_cast<unsigned long long>((now - since) / 1000), frames, rejected,
                          resets, reasons[0], reasons[1], reasons[2], reasons[3], isolated_frames,
                          isolated_max, sanitized_frames, sanitized_max);
            out = text;
        }
        *this = {};
        started = true;
        since = now;
        return active;
    }
};
} // namespace rrhealth
