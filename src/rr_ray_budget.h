#pragma once
// Session-only ray budget test: CPU-only state, proofs and decisions (std-only).
#include "rr_ray_density.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
namespace rrraybudget {
// The game's shaders, by the hash ShaderDispatches() reports (they are not replaced targets, so
// they stay out of rrbypass::Target): cs_vrrt_resolve writes the per-lobe Budget,
// cs_vrrt_clear_counters zeroes the ray counters. A game update that rebuilds the resolve under
// another hash leaves the test inert; the 10 s line then warns (Summary::Report). One line in
// rrversion::Canonical (rr_shader_versions.h) maps the new hash back: `case <new hash>: return
// 0x293995352d1f8220ull;`.
inline constexpr std::uint64_t Resolve = 0x293995352d1f8220ull, Clear = 0x32cf8b5e62a28a89ull;
enum Mode : unsigned { Off, Times15, Times2, OnePerPixel, Count };
inline constexpr const char *labels[Count] = {"Off (game's ray budget)", "1.5x the game's rays",
                                              "2x the game's rays", "Up to 1 ray per pixel"};
inline constexpr float factors[Count] = {1.0f, 1.5f, 2.0f, 0.0f}; // 0: absolute cap
inline constexpr unsigned Valid(unsigned m) {
    return m < Count ? m : Off;
}
inline std::atomic<unsigned> mode{};
inline constexpr std::uint64_t ListHeader = 144, EntryBytes = 4, PixelEntryBytes = 4,
                               ConsumerSlack = 512, FreshMs = 500, RetryMs = 1000;
inline constexpr int PositiveLimit = 17, NegativeLimit = -16;
inline constexpr unsigned ProofFrames = 120, TargetFrames = 4;
enum class Why : unsigned {
    None,
    Mode,
    Stopped,
    Context,
    Shape,
    Constants,
    Heap,
    BudgetBinding,
    BudgetIdentity,
    Outside,
    AfterSR,
    Repeated,
    Ambiguous,
    ListsUnproven,
    ListsRoot,
    ListsView,
    ListsOverlap,
    ListsSmall,
    ListsStable,
    ListsStale,
    ListsChanged,
    Evidence,
    VrrtOff,
    Resolution,
    TargetUnknown,
    TargetRange,
    AtCap,
    Memory,
    Reconfigure,
    Governor,
    PatchMismatch,
    MultiRay,
    Overflow,
    Pipeline,
    Readback,
    Exception,
    Count
};
inline constexpr const char *texts[] = {
    "none",
    "test off",
    "stopped",
    "binding tracker, copy context or state hooks unavailable",
    "controller dispatch is not 2x1x1",
    "controller b1/b3 are not proven root constant buffers",
    "controller constants are not in an upload heap",
    "controller Budget u0 is not a proven table raw view",
    "controller writes a different Budget than the apply reads",
    "controller ran outside the RR guide recording",
    "controller ran after RR recorded this frame",
    "controller ran twice in one recording",
    "more than one RR recording on this command list",
    "ray lists not proven at the VRRT apply",
    "a ray list is a root UAV (no bounds checks)",
    "a ray list is not a raw table buffer view",
    "ray list views overlap",
    "ray lists hold less than one ray per pixel plus margin",
    "waiting for 120 identical ray-list proofs",
    "no recent ray-list proof",
    "ray lists changed in this recording",
    "waiting for 120 measured frames",
    "the game's VRRT rate control is off",
    "render resolution too large for the ray counters",
    "the game's target is not known yet",
    "the game's target is not in (0,1]",
    "already at the cap (1 ray per requested pixel)",
    "waiting for GPU memory",
    "RR is reconfiguring",
    "controller threshold beyond the one-ray bound after the governor",
    "private constants did not reach the controller",
    "multi-ray requests grew beyond the measured margin",
    "scheduled rays came within 512 entries of a ray list's capacity",
    "governor pipeline could not be created",
    "ray budget readback failed",
    "internal error"};
static_assert(sizeof(texts) / sizeof(texts[0]) == unsigned(Why::Count), "one text per Why");
// The enum names in lower camel case: the keys of the 10 s line's `refused <why>=<n>` list.
inline constexpr const char *keys[] = {
    "none",          "mode",          "stopped",        "context",   "shape",        "constants",
    "heap",          "budgetBinding", "budgetIdentity", "outside",   "afterSR",      "repeated",
    "ambiguous",     "listsUnproven", "listsRoot",      "listsView", "listsOverlap", "listsSmall",
    "listsStable",   "listsStale",    "listsChanged",   "evidence",  "vrrtOff",      "resolution",
    "targetUnknown", "targetRange",   "atCap",          "memory",    "reconfigure",  "governor",
    "patchMismatch", "multiRay",      "overflow",       "pipeline",  "readback",     "exception"};
static_assert(sizeof(keys) / sizeof(keys[0]) == unsigned(Why::Count), "one key per Why");
inline constexpr const char *Text(Why w) {
    return unsigned(w) < unsigned(Why::Count) ? texts[unsigned(w)] : "unknown";
}
inline constexpr bool SafeThreshold(int t) {
    return t == INT_MIN || (t >= NegativeLimit && t <= PositiveLimit);
}
enum class Binding : unsigned { Unknown, Table, Root, Invalid };
struct List {
    Binding binding{};
    std::uint64_t resource{}, offset{}, bytes{};
    bool operator==(const List &) const = default;
};
struct Lists {
    std::array<List, 4> v{}; // u5 lobe-1 rays, u6 lobe-2 rays, u3 pixel list, u2 counter
    std::uint64_t budget_resource{}, budget_offset{};
    std::uint32_t w{}, h{}, gx{}, gy{};
    Why why = Why::ListsUnproven;
    bool operator==(const Lists &) const = default;
    std::uint64_t Area() const {
        return std::uint64_t(gx) * 16 * std::uint64_t(gy) * 16;
    }
    std::uint64_t Entries(unsigned lobe) const {
        const auto &l = v[lobe & 1];
        return l.binding == Binding::Table && l.bytes > ListHeader
                   ? (l.bytes - ListHeader) / EntryBytes
                   : 0;
    }
};
struct Completed {
    rrdensity::Frame rates{};
    std::array<std::uint32_t, 24> log{}; // private readback bytes [0, 96)
    unsigned logged{};                   // Log* bits below
    Lists lists;                         // this recording's apply proof
    unsigned patched{}, budget{};        // lobe masks
    std::array<std::uint32_t, 2> written{};
};
inline constexpr unsigned LogResolveTarget = 1, LogExecuted = 2, LogResolveScene = 4,
                          LogApplyTarget = 8, LogApplySettings36 = 16, LogApplyScene251 = 32;
struct Plan {
    unsigned mask{};
    std::array<float, 2> native{}, target{};
    std::array<std::uint32_t, 2> bits{};
    Why why = Why::Mode;
};
class Gate {
    // Apply proofs: consecutive proven applies at one extent; capacities are the
    // minimum over the streak, so ping-pong list buffers of equal size qualify.
    Lists last;
    unsigned streak{};
    std::uint64_t proven_at{};
    std::array<std::uint64_t, 2> min_entries{};
    std::uint64_t min_pixels{}, min_counter{};
    std::array<std::array<std::uint64_t, 2>, 4> budgets{}; // recent distinct Budget identities
    unsigned next_budget{};
    // Completed-frame evidence: two rolling blocks of BlockFrames frames.
    struct Block {
        unsigned frames{};
        std::array<std::uint32_t, 2> raw_extra{};
        std::array<double, 2> requested{1, 1};
    } cur, prev;
    unsigned clean{};
    std::uint32_t ew{}, eh{};
    std::array<std::uint32_t, 2> target_bits{};
    unsigned target_streak{};
    bool vrrt_enabled{};
    std::array<std::uint64_t, 2> armed_margin{};
    Why stop = Why::None;

  public:
    static constexpr unsigned BlockFrames = 300;
    void Apply(const Lists &l, std::uint64_t now) noexcept {
        const bool same =
            streak && l.w == last.w && l.h == last.h && l.gx == last.gx && l.gy == last.gy;
        if (l.why != Why::None)
            streak = 0;
        else if (same) {
            ++streak;
            for (unsigned k = 0; k < 2; ++k)
                min_entries[k] = (std::min)(min_entries[k], l.Entries(k));
            min_pixels = (std::min)(min_pixels, l.v[2].bytes);
            min_counter = (std::min)(min_counter, l.v[3].bytes);
        } else {
            streak = 1;
            min_entries = {l.Entries(0), l.Entries(1)};
            min_pixels = l.v[2].bytes;
            min_counter = l.v[3].bytes;
        }
        last = l;
        proven_at = now;
        if (l.why == Why::None && !KnownBudget(l.budget_resource, l.budget_offset)) {
            budgets[next_budget] = {l.budget_resource, l.budget_offset};
            next_budget = (next_budget + 1) % budgets.size();
        }
    }
    bool KnownBudget(std::uint64_t resource, std::uint64_t offset) const noexcept {
        if (!resource)
            return false;
        for (const auto &b : budgets)
            if (b[0] == resource && b[1] == offset)
                return true;
        return false;
    }
    std::uint64_t Margin(unsigned lobe) const noexcept {
        return 2ull * (std::max)(cur.raw_extra[lobe], prev.raw_extra[lobe]);
    }
    double Requested(unsigned lobe) const noexcept {
        return (std::min)(cur.requested[lobe], prev.requested[lobe]);
    }
    // Hard capacity: one ray per dispatch-area pixel plus the raw multi-ray margin
    // plus one 512-wide consumer group, for the smallest list of the streak.
    bool Hard(unsigned lobe) const noexcept {
        if (lobe > 1 || last.why != Why::None)
            return false;
        const auto area = last.Area();
        return min_entries[lobe] >= area + Margin(lobe) + ConsumerSlack && min_counter >= 4 &&
               (lobe || min_pixels >= PixelEntryBytes * area);
    }
    bool Stop(Why w) noexcept {
        if (w == Why::None || stop != Why::None)
            return false;
        stop = w;
        return true;
    }
    Why Stopped() const noexcept {
        return stop;
    }
    const Lists &Proof() const noexcept {
        return last;
    }
    unsigned Streak() const noexcept {
        return streak;
    }
    // Returns the reason when this frame newly stops the test, otherwise Why::None.
    Why Complete(const Completed &c) noexcept {
        using namespace rrdensity;
        const auto &f = c.rates;
        const auto &lists = c.lists.why == Why::None ? c.lists : last;
        const unsigned raised = c.patched | c.budget;
        auto fail = [&](Why w) { return Stop(w) ? w : Why::None; };
        for (unsigned l = 0; l < 2; ++l) {
            if ((c.budget >> l & 1) && !SafeThreshold(int(f[Limits + 2 * l])))
                return fail(Why::Governor);
            if ((c.patched >> l & 1) && (!(c.logged & LogExecuted) || c.log[4 + l] != c.written[l]))
                return fail(Why::PatchMismatch);
            if ((c.budget >> l & 1) && f[RawExtra + l] > armed_margin[l])
                return fail(Why::MultiRay);
        }
        if (raised && lists.why == Why::None && lists.w && lists.h) {
            // The apply stores for requested pixels, their extra rays and the threads past the
            // extent.
            const std::uint64_t area = lists.Area(), pixels = std::uint64_t(lists.w) * lists.h;
            for (unsigned l = 0; l < 2; ++l) {
                std::uint64_t requested = 0;
                for (unsigned i = 1; i < Buckets; ++i)
                    requested += f[l * Buckets + i];
                const auto worst = requested + f[Extra + l] + (area > pixels ? area - pixels : 0);
                if ((raised >> l & 1) && worst + ConsumerSlack > lists.Entries(l))
                    return fail(Why::Overflow);
            }
        }
        if (lists.w != ew || lists.h != eh) {
            ew = lists.w;
            eh = lists.h;
            clean = 0;
            cur = prev = {};
        }
        vrrt_enabled = f[Settings1] != 0;
        ++clean;
        if (cur.frames >= BlockFrames) {
            prev = cur;
            cur = {};
        }
        ++cur.frames;
        for (unsigned l = 0; l < 2; ++l) {
            std::uint64_t requested = 0, total = 0;
            for (unsigned i = 0; i < Buckets; ++i) {
                total += f[l * Buckets + i];
                if (i)
                    requested += f[l * Buckets + i];
            }
            cur.raw_extra[l] = (std::max)(cur.raw_extra[l], f[RawExtra + l]);
            if (total)
                cur.requested[l] = (std::min)(cur.requested[l], double(requested) / double(total));
        }
        if (c.logged & LogResolveTarget) {
            const std::array<std::uint32_t, 2> bits{c.log[0], c.log[1]};
            target_streak = bits == target_bits ? target_streak + 1 : 1;
            target_bits = bits;
        }
        return Why::None;
    }
    Plan Decide(unsigned m, std::uint64_t now, const Lists *recording) const noexcept {
        Plan p;
        m = Valid(m);
        auto refuse = [&](Why w) {
            p.why = w;
            return p;
        };
        if (m == Off)
            return refuse(Why::Mode);
        if (stop != Why::None)
            return refuse(Why::Stopped);
        if (last.why != Why::None)
            return refuse(last.why);
        if (streak < ProofFrames)
            return refuse(Why::ListsStable);
        if (now < proven_at || now - proven_at > FreshMs)
            return refuse(Why::ListsStale);
        if (recording &&
            (recording->why != Why::None || recording->w != last.w || recording->h != last.h ||
             recording->gx != last.gx || recording->gy != last.gy ||
             recording->Entries(0) < min_entries[0] || recording->Entries(1) < min_entries[1]))
            return refuse(Why::ListsChanged);
        if (clean < ProofFrames || ew != last.w || eh != last.h)
            return refuse(Why::Evidence);
        if (!vrrt_enabled)
            return refuse(Why::VrrtOff);
        if (std::uint64_t(last.w) * last.h * 239 >= (1ull << 32))
            return refuse(Why::Resolution);
        if (target_streak < TargetFrames)
            return refuse(Why::TargetUnknown);
        Why first = Why::None;
        for (unsigned l = 0; l < 2; ++l) {
            const float native = std::bit_cast<float>(target_bits[l]);
            p.native[l] = native;
            Why why = Why::None;
            const float wanted = factors[m] > 0 ? native * factors[m] : 1.0f;
            const float target = (std::min)({wanted, 1.0f, float(Requested(l))});
            if (!std::isfinite(native) || !(native > 0) || native > 1)
                why = Why::TargetRange;
            else if (!Hard(l))
                why = Why::ListsSmall;
            else if (!(target > native * 1.001f))
                why = Why::AtCap;
            if (why != Why::None) {
                if (first == Why::None)
                    first = why;
                continue;
            }
            p.mask |= 1u << l;
            p.target[l] = target;
            p.bits[l] = std::bit_cast<std::uint32_t>(target);
        }
        p.why = p.mask ? Why::None : first;
        return p;
    }
    void Patched(const Plan &p) noexcept {
        for (unsigned l = 0; l < 2; ++l)
            if (p.mask >> l & 1)
                armed_margin[l] = Margin(l);
    }
};
// An indirect draw or dispatch that read one of the ray lists: `lobe` is 0 or 1, `type` is
// reshade::api::indirect_command as an integer, `offset` is relative to the list's view.
struct Consumer {
    unsigned type{}, lobe{};
    std::uint64_t offset{};
    std::uint32_t count{}, stride{};
};
// Resource handles of the u5 and u6 lists (reshade::api::resource::handle, 0 when not a proven
// table view), published by the VRRT apply for a lock-free check in the ExecuteIndirect hook.
inline std::atomic<std::uint64_t> published[2]{};
// What the controller is doing, for the settings window and the 10 s line.
struct Status {
    unsigned mode{};
    Why why = Why::Mode, stopped = Why::None;
    bool active{};
    std::array<float, 2> native{}, target{};
    std::array<bool, 2> raised{};
};
// off | stopped for this session: <why> | active: lobe1 0.400 -> 0.800, lobe2 0.400 (not raised) |
// waiting: <why>. Always NUL-terminated; a size of 0 writes nothing.
inline void Describe(const Status &s, char *out, std::size_t size) noexcept {
    if (!out || !size)
        return;
    char text[512];
    if (Valid(s.mode) == Off)
        std::snprintf(text, sizeof(text), "off");
    else if (s.stopped != Why::None)
        std::snprintf(text, sizeof(text), "stopped for this session: %s", Text(s.stopped));
    else if (s.active) {
        int at = std::snprintf(text, sizeof(text), "active:");
        for (unsigned l = 0; l < 2 && at > 0 && at < int(sizeof(text)); ++l) {
            at += std::snprintf(text + at, sizeof(text) - at, "%s lobe%u %.3f", l ? "," : "", l + 1,
                                double(s.native[l]));
            if (at > 0 && at < int(sizeof(text)))
                at += s.raised[l] ? std::snprintf(text + at, sizeof(text) - at, " -> %.3f",
                                                  double(s.target[l]))
                                  : std::snprintf(text + at, sizeof(text) - at, " (not raised)");
        }
    } else
        std::snprintf(text, sizeof(text), "waiting: %s", Text(s.why));
    std::snprintf(out, size, "%s", text);
}
// Counters of the last 10 s window and the two log lines they make, one for the controller and one
// for the ray lists. Every input is noexcept and allocation-free; Report builds the text.
class Summary {
    template <class T> struct Range {
        T lo{}, hi{};
        bool have{};
        void Add(T v) noexcept {
            if constexpr (std::is_floating_point_v<T>)
                if (std::isnan(v))
                    return;
            if (!have || v < lo)
                lo = v;
            if (!have || v > hi)
                hi = v;
            have = true;
        }
    };
    struct Ratio { // `yes` of `all`
        unsigned yes{}, all{};
        void Add(bool y) noexcept {
            ++all;
            yes += y;
        }
    };
    struct Window {
        unsigned in_recording{}, after_apply{}, before_prepass{}, patched{}, verified{};
        std::array<unsigned, unsigned(Why::Count)> refused{}; // per reason, including placement
        Ratio clears, same_b1, same_budget, scene_match;
        unsigned applies{}, proven{};
        std::array<unsigned, 2> lobe{};
        unsigned dispatch{}, rays{}, other{};
        bool have_consumer{};
        rrraybudget::Consumer last_consumer;
        std::array<Range<float>, 2> native;
        std::array<float, 2> native_last{}, apply{}, scene{};
        std::array<std::uint32_t, 2> written{};
        std::uint32_t settings36{}, scene251{};
        bool have_native{}, have_apply{}, have_written{}, have_scene{}, have_settings36{},
            have_scene251{};
        std::array<double, 2> fill{};
        std::array<bool, 2> have_fill{};
        std::array<unsigned, 2> raw_extra_max{};
        std::array<Range<int>, 2> threshold;
    } w;
    rrraybudget::Lists last; // the last apply's proof, kept across windows
    std::uint64_t since{};
    bool started{};
    unsigned warned{};

    // One line of at most N-1 characters; what does not fit is cut.
    template <std::size_t N> class Line {
        char text[N]{};
        std::size_t length{};

      public:
        template <class First, class... Rest>
        void Add(const char *format, First first, Rest... rest) noexcept {
            if (length + 1 >= N)
                return;
            const int n = std::snprintf(text + length, N - length, format, first, rest...);
            if (n > 0)
                length = (std::min)(length + std::size_t(n), N - 1);
        }
        void Put(const char *literal) noexcept {
            Add("%s", literal);
        }
        std::string Str() const {
            return std::string(text, length);
        }
    };
    static constexpr unsigned long long U(std::uint64_t v) {
        return v;
    }
    static constexpr double D(float v) {
        return double(v);
    }
    static const char *BindingName(Binding b) {
        switch (b) {
        case Binding::Table:
            return "table";
        case Binding::Root:
            return "root";
        case Binding::Invalid:
            return "view";
        default:
            return "unknown";
        }
    }
    unsigned Seen() const noexcept {
        return w.in_recording + w.refused[unsigned(Why::Outside)] +
               w.refused[unsigned(Why::AfterSR)] + w.refused[unsigned(Why::Repeated)] +
               w.refused[unsigned(Why::Ambiguous)];
    }
    std::string ControllerLine(unsigned long long seconds, const Status &status) const {
        Line<1024> c;
        char state[256];
        if (Valid(status.mode) == Off)
            std::snprintf(state, sizeof(state), "off");
        else if (status.stopped != Why::None)
            std::snprintf(state, sizeof(state), "stopped: %s", Text(status.stopped));
        else if (status.active)
            std::snprintf(state, sizeof(state), "active");
        else
            std::snprintf(state, sizeof(state), "waiting: %s", Text(status.why));
        c.Add("RR ray budget (%llus): test=%s state=%s", seconds, labels[Valid(status.mode)],
              state);
        c.Add(" patched=%u/%u verified=%u", w.patched, w.in_recording, w.verified);
        c.Add(" | resolve seen=%u inRecording=%u afterApply=%u beforeApply=%u", Seen(),
              w.in_recording, w.after_apply, w.in_recording - w.after_apply);
        c.Add(" outside=%u beforePrepass=%u afterSR=%u repeated=%u",
              w.refused[unsigned(Why::Outside)], w.before_prepass,
              w.refused[unsigned(Why::AfterSR)], w.refused[unsigned(Why::Repeated)]);
        c.Add(" clears=%u/%u sameB1AsApply=%u/%u sameBudget=%u/%u", w.clears.yes, w.clears.all,
              w.same_b1.yes, w.same_b1.all, w.same_budget.yes, w.same_budget.all);
        if (w.have_native) {
            c.Add(" | target native=%.3f/%.3f (", D(w.native_last[0]), D(w.native_last[1]));
            for (unsigned l = 0; l < 2; ++l) {
                if (w.native[l].have)
                    c.Add("%.3f..%.3f", D(w.native[l].lo), D(w.native[l].hi));
                else
                    c.Put("n/a");
                c.Put(l ? ")" : "/");
            }
        } else
            c.Put(" | target native=n/a");
        if (w.have_apply)
            c.Add(" apply=%.3f/%.3f", D(w.apply[0]), D(w.apply[1]));
        else
            c.Put(" apply=n/a");
        if (w.have_written)
            c.Add(" written=%.3f/%.3f", D(std::bit_cast<float>(w.written[0])),
                  D(std::bit_cast<float>(w.written[1])));
        else
            c.Put(" written=-");
        if (w.have_scene)
            c.Add(" scene=%.0fx%.0f", D(w.scene[0]), D(w.scene[1]));
        else
            c.Put(" scene=n/a");
        c.Add(" match=%u/%u", w.scene_match.yes, w.scene_match.all);
        if (w.have_settings36)
            c.Add(" settings36.w=0x%x", unsigned(w.settings36));
        else
            c.Put(" settings36.w=n/a");
        if (w.have_scene251)
            c.Add(" scene251.x=0x%x", unsigned(w.scene251));
        else
            c.Put(" scene251.x=n/a");
        c.Add(" | consumers lobe1=%u lobe2=%u dispatch=%u rays=%u other=%u", w.lobe[0], w.lobe[1],
              w.dispatch, w.rays, w.other);
        if (w.have_consumer)
            c.Add(" last=%u/%u/%llu", w.last_consumer.type, unsigned(w.last_consumer.stride),
                  U(w.last_consumer.offset));
        else
            c.Put(" last=n/a");
        // Refusals, the most frequent first (equal counts keep the enum's order).
        std::array<unsigned, unsigned(Why::Count)> order{};
        unsigned n = 0;
        for (unsigned i = 1; i < unsigned(Why::Count); ++i) {
            if (!w.refused[i])
                continue;
            unsigned at = n++;
            for (; at > 0 && w.refused[order[at - 1]] < w.refused[i]; --at)
                order[at] = order[at - 1];
            order[at] = i;
        }
        for (unsigned k = 0; k < n; ++k)
            c.Add(k ? " %s=%u" : " | refused %s=%u", keys[order[k]], w.refused[order[k]]);
        return c.Str();
    }
    std::string ListsLine(unsigned long long seconds, const Gate &gate) const {
        Line<1024> c;
        const auto area = last.Area();
        c.Add("RR ray lists (%llus): applies=%u proven=%u streak=%u", seconds, w.applies, w.proven,
              gate.Streak());
        c.Add(" %ux%u groups=%ux%u area=%llu", unsigned(last.w), unsigned(last.h),
              unsigned(last.gx), unsigned(last.gy), U(area));
        for (unsigned l = 0; l < 2; ++l) {
            const auto &list = last.v[l];
            const auto entries = last.Entries(l);
            char fill[24], threshold[32];
            if (w.have_fill[l])
                std::snprintf(fill, sizeof(fill), "%.1f%%", w.fill[l]);
            else
                std::snprintf(fill, sizeof(fill), "n/a");
            if (w.threshold[l].have)
                std::snprintf(threshold, sizeof(threshold), "%d..%d", w.threshold[l].lo,
                              w.threshold[l].hi);
            else
                std::snprintf(threshold, sizeof(threshold), "n/a");
            c.Add(" | lobe%u u%u=%s %lluB entries=%llu (%.3fx area)", l + 1, 5 + l,
                  BindingName(list.binding), U(list.bytes), U(entries),
                  area ? double(entries) / double(area) : 0.0);
            c.Add(" need=%llu tier=%s worstFill=%s", U(area + gate.Margin(l) + ConsumerSlack),
                  gate.Hard(l) ? "hard" : "small", fill);
            c.Add(" rawExtraMax=%u threshold=%s", w.raw_extra_max[l], threshold);
        }
        const auto &pixels = last.v[2], &counter = last.v[3];
        c.Add(" | pixels u3=%s %lluB (%.3f B/px)", BindingName(pixels.binding), U(pixels.bytes),
              area ? double(pixels.bytes) / double(area) : 0.0);
        c.Add(" counter u2=%s %lluB | why=%s", BindingName(counter.binding), U(counter.bytes),
              Text(last.why));
        return c.Str();
    }

  public:
    static constexpr std::uint64_t Interval = rrdensity::Tracker::Interval;
    static constexpr unsigned IdleWarnings = 3;
    // A resolve inside the RR guide recording; `after_apply` when this recording's VRRT apply came
    // first, in which case `same_b1` says whether it read the same constant block.
    void Resolve(bool after_apply, bool same_b1) noexcept {
        ++w.in_recording;
        if (after_apply) {
            ++w.after_apply;
            w.same_b1.Add(same_b1);
        }
    }
    // The apply ran after this recording's resolve: whether it read the same constant block.
    void SameBlock(bool same) noexcept {
        w.same_b1.Add(same);
    }
    // A resolve that ran before this recording's guide prepass (counted as outside when it ran).
    void BeforePrepass() noexcept {
        ++w.before_prepass;
    }
    // A resolve that was not patched. Outside, AfterSR, Repeated and Ambiguous are placements of
    // resolves that never reached the recording: they also count as resolves seen.
    void Refused(Why why) noexcept {
        if (unsigned(why) > 0 && unsigned(why) < unsigned(Why::Count))
            ++w.refused[unsigned(why)];
    }
    void Budget(bool same) noexcept {
        w.same_budget.Add(same);
    }
    void Clear(bool in_recording) noexcept {
        w.clears.Add(in_recording);
    }
    void Patched(const Plan &plan) noexcept {
        w.patched += plan.mask != 0;
    }
    void Lists(const rrraybudget::Lists &lists) noexcept {
        ++w.applies;
        w.proven += lists.why == Why::None;
        last = lists;
    }
    // ReShade's indirect_command: 1 draw, 2 draw_indexed, 3 dispatch, 4 dispatch_mesh,
    // 5 dispatch_rays; draws, mesh dispatches, multi-draw types of newer builds and unknown values
    // count as other.
    void Consumer(const rrraybudget::Consumer &consumer) noexcept {
        if (consumer.lobe < 2)
            ++w.lobe[consumer.lobe];
        ++(consumer.type == 3 ? w.dispatch : consumer.type == 5 ? w.rays : w.other);
        w.last_consumer = consumer;
        w.have_consumer = true;
    }
    void Completed(const rrraybudget::Completed &c) noexcept {
        using namespace rrdensity;
        const auto &f = c.rates;
        if (c.patched) {
            bool verified = (c.logged & LogExecuted) != 0;
            for (unsigned l = 0; l < 2; ++l)
                if ((c.patched >> l & 1) && c.log[4 + l] != c.written[l])
                    verified = false;
            w.verified += verified;
            w.written = c.written;
            w.have_written = true;
        }
        // Same rule as Gate::Complete: the frame's own apply, else the last one.
        const auto &lists = c.lists.why == Why::None ? c.lists : last;
        if (c.logged & LogResolveTarget) {
            for (unsigned l = 0; l < 2; ++l) {
                w.native_last[l] = std::bit_cast<float>(c.log[l]);
                w.native[l].Add(w.native_last[l]);
            }
            w.have_native = true;
        }
        if (c.logged & LogResolveScene) {
            w.scene = {std::bit_cast<float>(c.log[8]), std::bit_cast<float>(c.log[9])};
            w.have_scene = true;
            w.scene_match.Add(lists.w && lists.h && w.scene[0] == float(lists.w) &&
                              w.scene[1] == float(lists.h));
        }
        if (c.logged & LogApplyTarget) {
            w.apply = {std::bit_cast<float>(c.log[12]), std::bit_cast<float>(c.log[13])};
            w.have_apply = true;
        }
        if (c.logged & LogApplySettings36) {
            w.settings36 = c.log[19];
            w.have_settings36 = true;
        }
        if (c.logged & LogApplyScene251) {
            w.scene251 = c.log[20];
            w.have_scene251 = true;
        }
        const std::uint64_t area = lists.Area(), pixels = std::uint64_t(lists.w) * lists.h;
        for (unsigned l = 0; l < 2; ++l) {
            w.raw_extra_max[l] = (std::max)(w.raw_extra_max[l], f[RawExtra + l]);
            if (const int t = int(f[Limits + 2 * l]); t != INT_MIN)
                w.threshold[l].Add(t);
            if (lists.why != Why::None || !lists.w || !lists.h || !lists.Entries(l))
                continue;
            std::uint64_t requested = 0;
            for (unsigned i = 1; i < Buckets; ++i)
                requested += f[l * Buckets + i];
            // What the apply can store: requested pixels, their extra rays and the threads past
            // the extent.
            const double scheduled = double(requested) + double(f[Extra + l]) +
                                     double(area > pixels ? area - pixels : 0);
            w.fill[l] = (std::max)(w.fill[l], 100.0 * scheduled / double(lists.Entries(l)));
            w.have_fill[l] = true;
        }
    }
    // At most every Interval. The first call only starts the window; a clock that went backwards
    // restarts it and drops what it held. Returns true, with both strings set (`lists` empty
    // without an apply in the window), when something is worth printing: a resolve or apply was
    // seen or the test is on. Without resolves but with applies the controller line is replaced by
    // a warning (at most 3 times per Summary).
    bool Report(std::uint64_t now, const Gate &gate, const Status &status, std::string &controller,
                std::string &lists) {
        if (!started) {
            started = true;
            since = now;
            return false;
        }
        if (now < since) {
            w = {};
            since = now;
            return false;
        }
        if (now - since < Interval)
            return false;
        const unsigned seen = Seen();
        const bool print = seen || w.applies || Valid(status.mode) != Off;
        if (print) {
            const unsigned long long seconds = (now - since) / 1000;
            if (!seen && w.applies && warned < IdleWarnings) {
                ++warned;
                Line<1024> warning;
                warning.Add("RR ray budget (%llus): cs_vrrt_resolve 0x%llx not seen while %u VRRT "
                            "applies were (game update?); the ray budget test is unavailable",
                            seconds, U(rrraybudget::Resolve), w.applies);
                controller = warning.Str();
            } else
                controller = ControllerLine(seconds, status);
            lists = w.applies ? ListsLine(seconds, gate) : std::string();
        }
        w = {};
        since = now;
        return print;
    }
};
} // namespace rrraybudget
