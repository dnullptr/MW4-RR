#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include "rr_sr_eligibility.h"
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
// CPU-only counters for completed RR frames and history resets. Each interval is checked, and
// a line is logged when its state (running, rejecting, non-finite pixels, which guides were
// bound) differs from the last line's, or as a slow summary. A line counts everything since
// the previous line.
class Tracker {
    unsigned frames{}, rejected{}, isolated_frames{}, sanitized_frames{}, resets{};
    std::array<unsigned, 4> reasons{};
    std::uint32_t isolated_max{}, sanitized_max{};
    unsigned sss_frames{}, glass_frames{}, hit_frames{}, responsivity_frames{};
    unsigned non_finite_color_frames{}, non_finite_output_frames{};
    std::uint32_t non_finite_color_max{}, non_finite_output_max{};
    std::array<unsigned, 5>
        shown{}; // presents, unfocused presents, FG evaluations, SR replaced, SR native
    std::uint64_t since{}, checked{};
    unsigned logged_state{}, mark{};    // the last line's state; frames... at the last check
    std::array<unsigned, 6> marks{};    // rejected, non-finite, sss, glass, hit, responsivity
    bool started{};

    // The state of the frames since the last check.
    unsigned State() const {
        const std::array<unsigned, 6> now{rejected, non_finite_color_frames + non_finite_output_frames,
                                          sss_frames, glass_frames, hit_frames,
                                          responsivity_frames};
        unsigned state = frames != mark;
        for (unsigned i = 0; i < now.size(); ++i)
            state |= unsigned(now[i] != marks[i]) << (i + 1);
        return state;
    }
    void Mark() {
        mark = frames;
        marks = {rejected, non_finite_color_frames + non_finite_output_frames, sss_frames,
                 glass_frames, hit_frames, responsivity_frames};
    }

  public:
    static constexpr std::uint64_t Interval = 10000, Summary = 300000;
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
    // Checks once per interval; returns a line on a state change or a due summary, with
    // activity since the previous line.
    bool Report(std::uint64_t now, std::string &out) {
        if (!started) {
            started = true;
            since = checked = now;
            Take();
            return false;
        }
        if (now < checked || now - checked < Interval)
            return false;
        checked = now;
        const auto taken = Take();
        for (unsigned i = 0; i < shown.size(); ++i)
            shown[i] += taken[i];
        const unsigned state = State();
        Mark();
        const bool active = frames || resets;
        if (!active) { // nothing to report: drop the presentation counts
            logged_state = state;
            Restart(now);
            return false;
        }
        if (state == logged_state && now - since < Summary)
            return false;
        char text[768];
        std::snprintf(
            text, sizeof(text),
            "RR health (%llus): frames=%u rejected=%u resets=%u "
            "[rejected=%u cut=%u generation=%u gap=%u] "
            "isolatedGuideFrames=%u maxIsolatedPixels=%u "
            "sanitizedSssFrames=%u maxSanitizedSamples=%u "
            "guides[sss=%u glass=%u hit=%u responsivity=%u] "
            "nonFiniteColorFrames=%u maxNonFiniteColorPixels=%u "
            "nonFiniteOutputFrames=%u maxNonFiniteOutputPixels=%u "
            "presents=%u unfocusedPresents=%u focused=%d fgEvaluations=%u "
            "srReplaced=%u srNative=%u srWhy=%s",
            static_cast<unsigned long long>((now - since) / 1000), frames, rejected, resets,
            reasons[0], reasons[1], reasons[2], reasons[3], isolated_frames, isolated_max,
            sanitized_frames, sanitized_max, sss_frames, glass_frames, hit_frames,
            responsivity_frames, non_finite_color_frames, non_finite_color_max,
            non_finite_output_frames, non_finite_output_max, shown[0], shown[1],
            presentation.focused.load(std::memory_order_relaxed) ? 1 : 0, shown[2], shown[3],
            shown[4], rrsr::SkipText(rrsr::counters.last.load(std::memory_order_relaxed)));
        out = text;
        logged_state = state;
        Restart(now);
        return true;
    }

  private:
    void Restart(std::uint64_t now) {
        const auto state = logged_state;
        *this = {};
        started = true;
        since = checked = now;
        logged_state = state;
    }
    // presents, unfocused presents, frame generation evaluations, SR evaluations RR replaced
    // and SR evaluations that ran natively, since the last call.
    static std::array<unsigned, 5> Take() {
        auto &p = presentation;
        auto &sr = rrsr::counters;
        return {p.presents.exchange(0, std::memory_order_relaxed),
                p.unfocused_presents.exchange(0, std::memory_order_relaxed),
                p.fg_evaluations.exchange(0, std::memory_order_relaxed),
                sr.replaced.exchange(0, std::memory_order_relaxed),
                sr.native.exchange(0, std::memory_order_relaxed)};
    }
};
} // namespace rrhealth
