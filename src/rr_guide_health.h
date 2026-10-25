#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
namespace rrhealth {
// What one RR evaluation was given and produced: the gate's non-finite pixel counts in the
// color RR received and in the RR output the gate displayed (none if rejected or blocked), and
// the optional guides that were bound.
struct Evaluation {
    std::uint32_t non_finite_color{}, non_finite_output{};
    bool sss{}, glass{}, hit{}, responsivity{};
};
// Written lock-free by the present thread and the NGX frame generation hook, which must never
// take the controller mutex. Tracker::Report takes the counters at the start of every interval.
struct Presentation {
    std::atomic<unsigned> presents{}, unfocused_presents{}, fg_evaluations{};
    std::atomic<bool> focused{}; // at the latest present
};
inline Presentation presentation;
// CPU-only counters for completed RR frames and history resets. Reported
// periodically so frame rejection and reset frequency show up in ReShade.log.
class Tracker {
    unsigned frames{}, rejected{}, isolated_frames{}, sanitized_frames{}, resets{};
    std::array<unsigned, 4> reasons{};
    std::uint32_t isolated_max{}, sanitized_max{};
    unsigned sss_frames{}, glass_frames{}, hit_frames{}, responsivity_frames{};
    unsigned non_finite_color_frames{}, non_finite_output_frames{};
    std::uint32_t non_finite_color_max{}, non_finite_output_max{};
    std::uint64_t since{};
    bool started{};

  public:
    static constexpr std::uint64_t Interval = 10000;
    // stats: bad frame, copied frame, isolated guide pixels, sanitized SSS samples.
    void Frame(const std::array<std::uint32_t, 4> &stats, const Evaluation &e = {}) {
        ++frames;
        rejected += stats[0] != 0 || stats[1] == 0;
        isolated_frames += stats[2] != 0;
        sanitized_frames += stats[3] != 0;
        if (stats[2] > isolated_max)
            isolated_max = stats[2];
        if (stats[3] > sanitized_max)
            sanitized_max = stats[3];
        sss_frames += e.sss;
        glass_frames += e.glass;
        hit_frames += e.hit;
        responsivity_frames += e.responsivity;
        non_finite_color_frames += e.non_finite_color != 0;
        non_finite_output_frames += e.non_finite_output != 0;
        if (e.non_finite_color > non_finite_color_max)
            non_finite_color_max = e.non_finite_color;
        if (e.non_finite_output > non_finite_output_max)
            non_finite_output_max = e.non_finite_output;
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
            Take();
            return false;
        }
        if (now < since || now - since < Interval)
            return false;
        const bool active = frames || resets;
        const auto taken = Take();
        if (active) {
            char text[640];
            std::snprintf(text, sizeof(text),
                          "RR health (%llus): frames=%u rejected=%u resets=%u "
                          "[rejected=%u cut=%u generation=%u gap=%u] "
                          "isolatedGuideFrames=%u maxIsolatedPixels=%u "
                          "sanitizedSssFrames=%u maxSanitizedSamples=%u "
                          "guides[sss=%u glass=%u hit=%u responsivity=%u] "
                          "nonFiniteColorFrames=%u maxNonFiniteColorPixels=%u "
                          "nonFiniteOutputFrames=%u maxNonFiniteOutputPixels=%u "
                          "presents=%u unfocusedPresents=%u focused=%d fgEvaluations=%u",
                          static_cast<unsigned long long>((now - since) / 1000), frames, rejected,
                          resets, reasons[0], reasons[1], reasons[2], reasons[3], isolated_frames,
                          isolated_max, sanitized_frames, sanitized_max, sss_frames, glass_frames,
                          hit_frames, responsivity_frames, non_finite_color_frames,
                          non_finite_color_max, non_finite_output_frames, non_finite_output_max,
                          taken[0], taken[1],
                          presentation.focused.load(std::memory_order_relaxed) ? 1 : 0, taken[2]);
            out = text;
        }
        *this = {};
        started = true;
        since = now;
        return active;
    }

  private:
    // presents, unfocused presents, frame generation evaluations since the last call.
    static std::array<unsigned, 3> Take() {
        auto &p = presentation;
        return {p.presents.exchange(0, std::memory_order_relaxed),
                p.unfocused_presents.exchange(0, std::memory_order_relaxed),
                p.fg_evaluations.exchange(0, std::memory_order_relaxed)};
    }
};
} // namespace rrhealth
