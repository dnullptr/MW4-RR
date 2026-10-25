#pragma once
// Session-only ray budget test: CPU-only state, proofs and decisions (std-only).
#include "rr_ray_density.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdarg>
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
// The governor stores NegativeLimit for a raised lobe's threshold below it (INT_MIN included).
// From there up, positive thresholds included, a pixel gets at most 1 + max(raw - 16, 0) rays.
inline constexpr int NegativeLimit = -16;
// UnverifiedLimit: raised recordings in a row that retired without reader stats (Unverified()).
inline constexpr unsigned ProofFrames = 120, TargetFrames = 4, UnverifiedLimit = 8;
// The largest extent side a proof may have (D3D12's texture limit), and the most raw rays a pixel
// can ask for (raw - 15 for an 8-bit raw code): the reader's 32-bit sums must hold W*H of them.
inline constexpr std::uint32_t MaxExtent = 16384;
inline constexpr std::uint64_t MaxRawRays = 255 - 15;
enum class Why : unsigned {
    None,
    Mode,
    Stopped,
    Unverified,
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
    StatsUnverified,
    RemapUnknown,
    TargetRange,
    AtCap,
    Memory,
    Reconfigure,
    GovernorCache,
    // Sticky stops from here on.
    Governor,
    PatchMismatch,
    ListsAfterArming,
    Unsupported,
    StatsMismatch,
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
    "raised frames were not verified",
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
    "waiting for 120 proven ray-list applies",
    "no recent ray-list proof",
    "ray lists changed in this recording",
    "waiting for 120 measured frames",
    "the game's VRRT rate control is off",
    "render resolution too large for the ray counters",
    "the game's target is not known yet",
    "the controller's ray statistics are not verified yet",
    "the readers' second list segment is not known",
    "the game's target is not in (0,1]",
    "already at the cap (1 ray per requested pixel)",
    "waiting for GPU memory",
    "RR is reconfiguring",
    "governor cache full; waiting for reconfigure",
    "controller threshold beyond the one-ray bound after the governor",
    "private constants did not reach the controller",
    "ray lists changed after arming",
    "requested pixels the reader cannot model",
    "the controller's ray statistics lag this frame",
    "scheduled rays came within 512 entries of a ray list's capacity",
    "governor pipeline could not be created",
    "ray budget readback failed",
    "internal error"};
static_assert(sizeof(texts) / sizeof(texts[0]) == unsigned(Why::Count), "one text per Why");
// The enum names in lower camel case: the keys of the 10 s line's `refused <why>=<n>` list.
inline constexpr const char *keys[] = {
    "none",          "mode",           "stopped",         "unverified",
    "context",       "shape",          "constants",       "heap",
    "budgetBinding", "budgetIdentity", "outside",         "afterSR",
    "repeated",      "ambiguous",      "listsUnproven",   "listsRoot",
    "listsView",     "listsOverlap",   "listsSmall",      "listsStable",
    "listsStale",    "listsChanged",   "evidence",        "vrrtOff",
    "resolution",    "targetUnknown",  "statsUnverified", "remapUnknown",
    "targetRange",   "atCap",          "memory",          "reconfigure",
    "governorCache", "governor",       "patchMismatch",   "listsAfterArming",
    "unsupported",   "statsMismatch",  "overflow",        "pipeline",
    "readback",      "exception"};
static_assert(sizeof(keys) / sizeof(keys[0]) == unsigned(Why::Count), "one key per Why");
inline constexpr const char *Text(Why w) {
    return unsigned(w) < unsigned(Why::Count) ? texts[unsigned(w)] : "unknown";
}
// A threshold a raised lobe may run with after the governor. INT_MIN is named although it is below
// the limit: |INT_MIN| wraps and would give every pixel about 2^31 rays.
inline constexpr bool SafeThreshold(int t) {
    return t >= NegativeLimit && t != INT_MIN;
}
enum class Binding : unsigned { Unknown, Table, Root, Invalid };
struct List {
    Binding binding{};
    std::uint64_t resource{}, offset{}, bytes{};
    bool operator==(const List &) const = default;
};
struct Lists {
    // u5 lobe-1 rays, u6 lobe-2 rays, u3 lobe 1's pixel list, u2 the counter
    std::array<List, 4> v{};
    std::uint64_t budget_resource{}, budget_offset{};
    // The extent (cb3 register 86) and the apply's dispatch grid. Threads past the extent append
    // nothing, so only W*H enters the capacity math; the grid is part of the proof's identity.
    std::uint32_t w{}, h{}, gx{}, gy{};
    Why why = Why::ListsUnproven;
    bool operator==(const Lists &) const = default;
    std::uint64_t Pixels() const noexcept {
        return std::uint64_t(w) * h;
    }
    std::uint64_t Entries(unsigned lobe) const noexcept {
        const auto &l = v[lobe & 1];
        return l.binding == Binding::Table && l.bytes > ListHeader
                   ? (l.bytes - ListHeader) / EntryBytes
                   : 0;
    }
};
// A proof the gate can measure against: proven, with an extent of 1..MaxExtent on each side.
inline bool Usable(const Lists &l) noexcept {
    return l.why == Why::None && l.w && l.h && l.w <= MaxExtent && l.h <= MaxExtent;
}
// The private ray log the controller copies into (bytes; Completed::log word = byte / 4):
//     0..15  resolve b1 register 2 (the game's per-lobe target .x/.y)  LogResolveTarget
//    16..31  register 2 as our patched resolve executed it             LogExecuted
//    32..47  resolve b3 register 86 (W, H as floats)                   LogResolveScene
//    48..63  apply b1 register 2                                       LogApplyTarget
//    64..79  apply b1 register 36 (.w)                                 LogApplySettings36
//    80..95  apply b3 register 251 (.x)                                LogApplyScene251
//    96..99  the recording's serial (controller only; the gate never reads it)
//   100..107 per lobe x, the resolve's t0 at lobe << 16 (the sum of r over the pixels asking for
//            r > 0 rays): the governor's input                         LogResolveStats
//   108..111 unused
//   112..127 apply b3 register 253 (.x: where the readers' second list segment starts)
//                                                                      LogApplyScene253
inline constexpr unsigned LogWords = 32;
inline constexpr unsigned LogResolveTarget = 1, LogExecuted = 2, LogResolveScene = 4,
                          LogApplyTarget = 8, LogApplySettings36 = 16, LogApplyScene251 = 32,
                          LogResolveStats = 64, LogApplyScene253 = 128;
// A retired recording, as the controller reports it to Gate::Complete and Summary::Completed.
struct Completed {
    // The reader's readback of this recording; a raised recording without one is reported with
    // Gate::Unverified() instead.
    rrdensity::Frame rates{};
    std::array<std::uint32_t, LogWords> log{}; // the private ray log (layout above)
    unsigned logged{};                         // Log* bits
    Lists lists; // this recording's apply proof (ListsUnproven when no apply was proven)
    // Lobe masks: raised by this recording's patch; raised in the Budget its apply read.
    unsigned patched{}, budget{};
    std::array<std::uint32_t, 2> written{}; // Plan::bits of the patch
    std::array<std::uint64_t, 2> margin{};  // Plan::margin of the plan that raised the frame
};
// One copy of each per-frame formula, shared by Gate::Complete and Summary::Completed.
// The requested pixels of a lobe (inverse rate 1..16).
inline std::uint64_t Requested(const rrdensity::Frame &f, unsigned lobe) noexcept {
    std::uint64_t pixels = 0;
    for (unsigned i = 1; i < rrdensity::Buckets; ++i)
        pixels += f[(lobe & 1) * rrdensity::Buckets + i];
    return pixels;
}
// The most entries the apply stores for a lobe: a ray per requested pixel plus their extra rays.
inline std::uint64_t Scheduled(const rrdensity::Frame &f, unsigned lobe) noexcept {
    return Requested(f, lobe) + f[rrdensity::Extra + (lobe & 1)];
}
// A patched lobe whose executed constants are the ones written.
inline bool Executed(const Completed &c, unsigned lobe) noexcept {
    lobe &= 1;
    return (c.patched >> lobe & 1) && (c.logged & LogExecuted) &&
           c.log[4 + lobe] == c.written[lobe];
}
// The controller's x covers the reader's raw rays of the lobe (meaningful with LogResolveStats).
inline bool StatsCover(const Completed &c, unsigned lobe) noexcept {
    lobe &= 1;
    return c.log[25 + lobe] >= c.rates[rrdensity::RawRays + lobe];
}
// The lists a completed frame is measured against: its own apply's proof, else `last`.
inline const Lists &FrameLists(const Completed &c, const Lists &last) noexcept {
    return Usable(c.lists) ? c.lists : last;
}
// What Decide armed. The controller writes `bits` into register 2 of its b1 copy and, after the
// game's 608 bytes, the governor's constants (mask, headroom[0], headroom[1]); after recording the
// patch it calls Gate::Patched(plan) and keeps `bits` and `margin` for the Completed it reports.
struct Plan {
    unsigned mask{}; // raised lobes
    std::array<float, 2> native{}, target{};
    std::array<std::uint32_t, 2> bits{};
    // Per raised lobe capacity - W*H - ConsumerSlack, saturated to 32 bits: the governor sets the
    // threshold to 17 when this frame's x exceeds it. 0 for a lobe not raised.
    std::array<std::uint32_t, 2> headroom{};
    std::uint32_t w{}, h{};                  // the armed extent
    std::array<std::uint64_t, 2> capacity{}; // each list's (both lobes)
    std::array<std::uint64_t, 2> margin{};   // Gate::Margin per raised lobe, else 0
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
    // Completed-frame evidence at one extent, in blocks of BlockFrames frames.
    struct Block {
        unsigned frames{};
        std::array<double, 2> requested{1, 1};
    } cur, prev;
    std::array<std::uint32_t, 2> raw_peak{}; // the RawRays maximum, halved at each new block
    unsigned clean{};
    std::uint32_t ew{}, eh{};
    std::array<std::uint32_t, 2> target_bits{};
    unsigned target_streak{};
    bool vrrt_enabled{};
    unsigned stats_streak{};   // frames whose copied x covered the reader's raw rays (both lobes)
    unsigned remap_frames{};   // frames at this extent that copied cb3 register 253
    std::uint32_t remap_min{}; // its smallest nonzero .x (0: none seen)
    unsigned unverified{};     // raised recordings in a row retired without reader stats
    // What the last Patched plan armed (the extent is 0x0 before the first).
    std::uint32_t armed_w{}, armed_h{};
    std::array<std::uint64_t, 2> armed_capacity{};
    std::uint64_t armed_pixels{}, armed_counter{};
    Why stop = Why::None;

    // A raised frame whose own apply did not use the lists armed: an unproven or missing proof,
    // another extent, a raised lobe's list or the counter smaller than at arming (the pixel list
    // for lobe 1), or a second segment that starts below a raised lobe's armed capacity.
    bool ChangedAfterArming(const Completed &c, unsigned raised) const noexcept {
        const auto &own = c.lists;
        if (own.why != Why::None || !armed_w || own.w != armed_w || own.h != armed_h ||
            own.v[3].bytes < armed_counter || ((raised & 1) && own.v[2].bytes < armed_pixels))
            return true;
        const bool remap = (c.logged & LogApplyScene253) && c.log[28];
        for (unsigned l = 0; l < 2; ++l)
            if ((raised >> l & 1) &&
                (own.Entries(l) < armed_capacity[l] || (remap && c.log[28] < armed_capacity[l])))
                return true;
        return false;
    }

  public:
    static constexpr unsigned BlockFrames = 300;
    // Every recognised native apply in the recording: its list proof, unproven when not proven.
    void Apply(const Lists &proof, std::uint64_t now) noexcept {
        Lists l = proof;
        if (l.why == Why::None && !Usable(l))
            l.why = Why::ListsUnproven; // an empty or impossible extent proves nothing
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
    // Room for raw multi-ray requests beyond one ray per pixel (an arming preference; the governor
    // bounds each frame by its own statistics).
    std::uint64_t Margin(unsigned lobe) const noexcept {
        return lobe > 1 ? 0 : 2ull * raw_peak[lobe];
    }
    double Requested(unsigned lobe) const noexcept {
        return lobe > 1 ? 0 : (std::min)(cur.requested[lobe], prev.requested[lobe]);
    }
    // A list's usable entries: the smallest of the streak, and no more than where the readers'
    // second segment starts when one was seen (indices from there on are remapped into it).
    std::uint64_t Capacity(unsigned lobe) const noexcept {
        if (lobe > 1)
            return 0;
        return remap_min ? (std::min)(min_entries[lobe], std::uint64_t(remap_min))
                         : min_entries[lobe];
    }
    // Arming capacity: a ray per pixel of the extent plus the margin plus one 512-wide consumer
    // group, a 4-byte counter (u2), and for lobe 1, whose pixel list u3 is, 4 bytes per pixel.
    bool Hard(unsigned lobe) const noexcept {
        if (lobe > 1 || last.why != Why::None)
            return false;
        const auto pixels = last.Pixels();
        return Capacity(lobe) >= pixels + Margin(lobe) + ConsumerSlack && min_counter >= 4 &&
               (lobe || min_pixels >= PixelEntryBytes * pixels);
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
    // The controller calls this when a raised recording (patched, or its apply read a raised
    // Budget) retires without reader stats, so no Complete covers it. From UnverifiedLimit in a row
    // Decide waits; the next Complete ends the run.
    void Unverified() noexcept {
        if (unverified < UnverifiedLimit)
            ++unverified;
    }
    // Every reported slot's frame. Returns the reason when this frame newly stops the test,
    // otherwise Why::None.
    Why Complete(const Completed &c) noexcept {
        using namespace rrdensity;
        unverified = 0;
        const auto &f = c.rates;
        const unsigned raised = (c.patched | c.budget) & 3;
        auto fail = [&](Why w) { return Stop(w) ? w : Why::None; };
        for (unsigned l = 0; l < 2; ++l) {
            if ((c.budget >> l & 1) && !SafeThreshold(int(f[Limits + 2 * l])))
                return fail(Why::Governor);
            if ((c.patched >> l & 1) && !Executed(c, l))
                return fail(Why::PatchMismatch);
        }
        if (raised) {
            if (ChangedAfterArming(c, raised))
                return fail(Why::ListsAfterArming);
            if (f[Unsupported])
                return fail(Why::Unsupported);
            for (unsigned l = 0; l < 2; ++l)
                if ((raised >> l & 1) && (c.logged & LogResolveStats) && !StatsCover(c, l))
                    return fail(Why::StatsMismatch);
            for (unsigned l = 0; l < 2; ++l)
                if ((raised >> l & 1) && Scheduled(f, l) + ConsumerSlack > armed_capacity[l])
                    return fail(Why::Overflow);
        }
        const auto &lists = FrameLists(c, last);
        if (lists.w != ew || lists.h != eh) {
            ew = lists.w;
            eh = lists.h;
            clean = 0;
            cur = prev = {};
            raw_peak = {};
            remap_frames = 0;
            remap_min = 0;
        }
        vrrt_enabled = f[Settings1] != 0;
        ++clean;
        if (cur.frames >= BlockFrames) {
            prev = cur;
            cur = {};
            for (auto &peak : raw_peak)
                peak /= 2;
        }
        ++cur.frames;
        for (unsigned l = 0; l < 2; ++l) {
            const auto requested = rrraybudget::Requested(f, l), total = requested + f[l * Buckets];
            raw_peak[l] = (std::max)(raw_peak[l], f[RawRays + l]);
            if (total)
                cur.requested[l] = (std::min)(cur.requested[l], double(requested) / double(total));
        }
        if (c.logged & LogResolveTarget) {
            const std::array<std::uint32_t, 2> bits{c.log[0], c.log[1]};
            target_streak = bits == target_bits ? target_streak + 1 : 1;
            target_bits = bits;
        }
        if (c.logged & LogResolveStats)
            stats_streak = StatsCover(c, 0) && StatsCover(c, 1) ? stats_streak + 1 : 0;
        if (c.logged & LogApplyScene253) {
            ++remap_frames;
            if (c.log[28] && (!remap_min || c.log[28] < remap_min))
                remap_min = c.log[28];
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
        if (unverified >= UnverifiedLimit)
            return refuse(Why::Unverified);
        if (last.why != Why::None)
            return refuse(last.why);
        if (streak < ProofFrames)
            return refuse(Why::ListsStable);
        if (now < proven_at || now - proven_at > FreshMs)
            return refuse(Why::ListsStale);
        if (recording &&
            (recording->why != Why::None || recording->w != last.w || recording->h != last.h ||
             recording->gx != last.gx || recording->gy != last.gy ||
             recording->Entries(0) < min_entries[0] || recording->Entries(1) < min_entries[1] ||
             recording->v[2].bytes < min_pixels || recording->v[3].bytes < min_counter))
            return refuse(Why::ListsChanged);
        if (clean < ProofFrames || ew != last.w || eh != last.h)
            return refuse(Why::Evidence);
        if (!vrrt_enabled)
            return refuse(Why::VrrtOff);
        const auto pixels = last.Pixels();
        if (pixels * MaxRawRays >= (1ull << 32))
            return refuse(Why::Resolution);
        if (target_streak < TargetFrames)
            return refuse(Why::TargetUnknown);
        if (stats_streak < ProofFrames)
            return refuse(Why::StatsUnverified);
        if (remap_frames < ProofFrames)
            return refuse(Why::RemapUnknown);
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
        if (p.mask) {
            p.w = last.w;
            p.h = last.h;
            for (unsigned l = 0; l < 2; ++l) {
                p.capacity[l] = Capacity(l);
                if (!(p.mask >> l & 1))
                    continue;
                // Hard(l) holds, so the capacity covers the pixels and the slack.
                const auto used = pixels + ConsumerSlack;
                const auto room = p.capacity[l] > used ? p.capacity[l] - used : 0;
                p.headroom[l] = std::uint32_t((std::min)(room, std::uint64_t(0xFFFFFFFFu)));
                p.margin[l] = Margin(l);
            }
        }
        return p;
    }
    // Call right after recording the patch of the Decide that made `p` (no Apply in between): the
    // completed frames of raised recordings are checked against what it armed.
    void Patched(const Plan &p) noexcept {
        if (!p.mask)
            return;
        armed_w = p.w;
        armed_h = p.h;
        armed_capacity = p.capacity;
        armed_pixels = min_pixels;
        armed_counter = min_counter;
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
// The controller's state, in the order it wins: off, stopped, active, waiting.
enum class Phase : unsigned { Off, Stopped, Active, Waiting };
inline constexpr Phase State(const Status &s) noexcept {
    return Valid(s.mode) == Off     ? Phase::Off
           : s.stopped != Why::None ? Phase::Stopped
           : s.active               ? Phase::Active
                                    : Phase::Waiting;
}
// off | stopped for this session: <why> | active: lobe1 0.400 -> 0.800, lobe2 0.400 (not raised) |
// waiting: <why>. Always NUL-terminated; a size of 0 writes nothing.
inline void Describe(const Status &s, char *out, std::size_t size) noexcept {
    if (!out || !size)
        return;
    char text[512];
    switch (State(s)) {
    case Phase::Off:
        std::snprintf(text, sizeof(text), "off");
        break;
    case Phase::Stopped:
        std::snprintf(text, sizeof(text), "stopped for this session: %s", Text(s.stopped));
        break;
    case Phase::Active: {
        int at = std::snprintf(text, sizeof(text), "active:");
        for (unsigned l = 0; l < 2 && at > 0 && at < int(sizeof(text)); ++l) {
            at += std::snprintf(text + at, sizeof(text) - at, "%s lobe%u %.3f", l ? "," : "", l + 1,
                                double(s.native[l]));
            if (at > 0 && at < int(sizeof(text)))
                at += s.raised[l] ? std::snprintf(text + at, sizeof(text) - at, " -> %.3f",
                                                  double(s.target[l]))
                                  : std::snprintf(text + at, sizeof(text) - at, " (not raised)");
        }
        break;
    }
    case Phase::Waiting:
        std::snprintf(text, sizeof(text), "waiting: %s", Text(s.why));
        break;
    }
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
        Ratio clears, same_b1, same_budget, scene_match, stats;
        unsigned applies{}, proven{};
        std::array<unsigned, 2> lobe{};
        unsigned dispatch{}, rays{}, other{};
        bool have_consumer{};
        rrraybudget::Consumer last_consumer;
        std::array<Range<float>, 2> native;
        std::array<float, 2> native_last{}, apply{}, scene{};
        std::array<std::uint32_t, 2> written{};
        unsigned written_mask{}; // the lobes the last patched frame raised
        std::uint32_t settings36{}, scene251{}, scene253{};
        bool have_native{}, have_apply{}, have_scene{}, have_settings36{}, have_scene251{},
            have_scene253{};
        std::array<double, 2> fill{};
        std::array<bool, 2> have_fill{};
        std::array<unsigned, 2> raw_rays_max{}, margin_exceeded{};
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
        // clang-format off
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3))) // `this` is argument 1: -Wformat checks every caller
#endif
        void Add(const char *format, ...) noexcept {
            // clang-format on
            if (length + 1 >= N)
                return;
            va_list args;
            va_start(args, format);
            const int n = std::vsnprintf(text + length, N - length, format, args);
            va_end(args);
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
        c.Add("RR ray budget (%llus): test=%s state=", seconds, labels[Valid(status.mode)]);
        switch (State(status)) {
        case Phase::Off:
            c.Put("off");
            break;
        case Phase::Stopped:
            c.Add("stopped: %s", Text(status.stopped));
            break;
        case Phase::Active:
            c.Put("active");
            break;
        case Phase::Waiting:
            c.Add("waiting: %s", Text(status.why));
            break;
        }
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
        for (unsigned l = 0; l < 2; ++l) { // `-` for a lobe the last patch did not raise
            c.Put(l ? "/" : " written=");
            if (w.written_mask >> l & 1)
                c.Add("%.3f", D(std::bit_cast<float>(w.written[l])));
            else
                c.Put("-");
        }
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
        if (w.have_scene253)
            c.Add(" scene253.x=%u", unsigned(w.scene253));
        else
            c.Put(" scene253.x=n/a");
        c.Add(" stats=%u/%u", w.stats.yes, w.stats.all);
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
        const auto area = last.Pixels(); // the extent: threads past it append nothing
        c.Add("RR ray lists (%llus): applies=%u proven=%u streak=%u", seconds, w.applies, w.proven,
              gate.Streak());
        c.Add(" %ux%u groups=%ux%u area=%llu", unsigned(last.w), unsigned(last.h),
              unsigned(last.gx), unsigned(last.gy), U(area));
        for (unsigned l = 0; l < 2; ++l) {
            const auto &list = last.v[l];
            const auto entries = last.Entries(l);
            c.Add(" | lobe%u u%u=%s %lluB entries=%llu (%.3fx area)", l + 1, 5 + l,
                  BindingName(list.binding), U(list.bytes), U(entries),
                  area ? double(entries) / double(area) : 0.0);
            c.Add(" need=%llu tier=%s worstFill=", U(area + gate.Margin(l) + ConsumerSlack),
                  gate.Hard(l) ? "hard" : "small");
            if (w.have_fill[l])
                c.Add("%.1f%%", w.fill[l]);
            else
                c.Put("n/a");
            c.Add(" rawRaysMax=%u marginExceeded=%u threshold=", w.raw_rays_max[l],
                  w.margin_exceeded[l]);
            if (w.threshold[l].have)
                c.Add("%d..%d", w.threshold[l].lo, w.threshold[l].hi);
            else
                c.Put("n/a");
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
        const unsigned raised = (c.patched | c.budget) & 3;
        if (c.patched) {
            bool verified = true;
            for (unsigned l = 0; l < 2; ++l)
                if ((c.patched >> l & 1) && !Executed(c, l))
                    verified = false;
            w.verified += verified;
            w.written = c.written;
            w.written_mask = c.patched & 3;
        }
        const auto &lists = FrameLists(c, last);
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
        if (c.logged & LogResolveStats)
            w.stats.Add(StatsCover(c, 0) && StatsCover(c, 1));
        if (c.logged & LogApplyScene253) {
            w.scene253 = c.log[28];
            w.have_scene253 = true;
        }
        for (unsigned l = 0; l < 2; ++l) {
            w.raw_rays_max[l] = (std::max)(w.raw_rays_max[l], f[RawRays + l]);
            w.margin_exceeded[l] += (raised >> l & 1) && f[RawRays + l] > c.margin[l];
            if (const int t = int(f[Limits + 2 * l]); t != INT_MIN)
                w.threshold[l].Add(t);
            if (lists.why != Why::None || !lists.w || !lists.h || !lists.Entries(l))
                continue;
            w.fill[l] =
                (std::max)(w.fill[l], 100.0 * double(Scheduled(f, l)) / double(lists.Entries(l)));
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
