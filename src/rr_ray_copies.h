#pragma once
// The ray budget test's GPU copy plan (std-only), the one source of truth for every byte offset the
// controller's copies use: GpuFrame (rr_live_gpu.h) records exactly these lists and restates none
// of their offsets. The ray_log layout is the table next to the Log bits in rr_ray_budget.h.
#include "rr_ray_budget.h"
#include <array>
#include <cstddef>
#include <cstdint>
namespace rrraycopy {
// B1/B3: offset is relative to the CBV start; Stats: relative to the t0 view start. The first
// NativeSources are the game's buffers (GpuFrame keeps one retained slot per site for each).
enum class Source : unsigned { B1, B3, Stats, Values, Cb, Count };
inline constexpr unsigned NativeSources = 3;
static_assert(unsigned(Source::B1) == 0 && unsigned(Source::B3) == 1 &&
                  unsigned(Source::Stats) == 2 && unsigned(Source::Values) == NativeSources,
              "native sources first, in this order");
constexpr bool Native(Source s) noexcept {
    return unsigned(s) < NativeSources;
}
enum class Target : unsigned { Cb, Log };
// Copy `bytes` from `source` + `from` to `target` + `at`.
struct Copy {
    Target target;
    std::uint32_t at;
    Source source;
    std::uint64_t from;
    std::uint32_t bytes;
};
// `bits` holds exactly the Log bits whose copies are in the list.
struct List {
    std::array<Copy, 8> copies{};
    unsigned count{};
    unsigned bits{};
};
// ray_values (UPLOAD 256 B): 0..7 target bits per lobe, 16..19 serial, 32..47 governor constants
// (mask, headroom lobe 0, headroom lobe 1, 0)
inline constexpr std::uint32_t ValuesTargets = 0, ValuesSerial = 16, ValuesGovernor = 32,
                               ValuesBytes = 256;
// ray_cb (DEFAULT) holds the game's first GameBytes of b1 then the governor's constants; ray_log
// (READBACK) is LogBytes, of which the controller reads the gate's LogRead.
inline constexpr std::uint32_t GameBytes = 608, GovernorBytes = 16, CbBytes = 65536, LogBytes = 256,
                               LogRead = rrraybudget::LogWords * 4;
// The recording's serial, copied from ray_values by each half that copies anything: ray_log bytes
// 96..99 for the resolve's copies and 108..111 for the apply's. A readback whose half serial is not
// the recording's means that half was predicated away or skipped, so its bits are dropped per half.
// Serials are nonzero: ray_log and ray_values start zeroed, so 0 would match a half that never ran
// (GpuFrame records no logging copy until a nonzero serial is written in the recording).
// Controller-only: the gate never reads either word.
inline constexpr Copy ResolveSerial{Target::Log, 96, Source::Values, ValuesSerial, 4},
    ApplySerial{Target::Log, 108, Source::Values, ValuesSerial, 4};
// The ray_values words a plan owns (the serial word is written on its own): register 2 .x/.y as
// the patch writes it, and the governor's constants it reads after the game's bytes of its b1.
inline constexpr unsigned ValuesWords = (ValuesGovernor + GovernorBytes) / 4;
inline constexpr std::uint32_t TargetBytes = 8;
static_assert(sizeof(rrraybudget::Plan{}.bits) == TargetBytes &&
                  ValuesTargets + TargetBytes <= ValuesSerial && ValuesSerial + 4 <= ValuesGovernor,
              "the plan's words and the serial do not overlap");
struct Range {
    std::uint32_t at, bytes;
};
inline constexpr std::array<Range, 2> PlanRanges{
    {{ValuesTargets, TargetBytes}, {ValuesGovernor, GovernorBytes}}};
constexpr std::array<std::uint32_t, ValuesWords>
ValuesImage(const rrraybudget::Plan &plan) noexcept {
    std::array<std::uint32_t, ValuesWords> v{};
    v[ValuesTargets / 4] = plan.bits[0];
    v[ValuesTargets / 4 + 1] = plan.bits[1];
    v[ValuesGovernor / 4] = plan.mask;
    v[ValuesGovernor / 4 + 1] = plan.headroom[0];
    v[ValuesGovernor / 4 + 2] = plan.headroom[1];
    v[ValuesGovernor / 4 + 3] = 0;
    return v;
}
// The copies behind each Log bit. A bit is copied only when its source holds every byte its copies
// read: the stats pair needs both lobes' x (the governor's input, t0 at lobe << 16).
struct Item {
    unsigned bit;
    Copy copy;
};
inline constexpr std::array<Item, 4> ResolveItems{{
    {rrraybudget::LogResolveTarget, {Target::Log, 0, Source::B1, 32, 16}},       // b1 register 2
    {rrraybudget::LogResolveScene, {Target::Log, 32, Source::B3, 1376, 16}},     // b3 register 86
    {rrraybudget::LogResolveStats, {Target::Log, 100, Source::Stats, 0, 4}},     // lobe 0 x
    {rrraybudget::LogResolveStats, {Target::Log, 104, Source::Stats, 65536, 4}}, // lobe 1 x
}};
inline constexpr std::array<Item, 4> ApplyItems{{
    {rrraybudget::LogApplyTarget, {Target::Log, 48, Source::B1, 32, 16}},      // b1 register 2
    {rrraybudget::LogApplySettings36, {Target::Log, 64, Source::B1, 576, 16}}, // b1 register 36
    {rrraybudget::LogApplyScene251, {Target::Log, 80, Source::B3, 4016, 16}},  // b3 register 251
    {rrraybudget::LogApplyScene253, {Target::Log, 112, Source::B3, 4048, 16}}, // b3 register 253
}};
// Each copy lands on the log words the gate reads (rr_ray_budget.h); register 36's is its .w.
static_assert(ResolveSerial.at == 4 * rrraybudget::LogWordResolveSerial &&
                  ApplySerial.at == 4 * rrraybudget::LogWordApplySerial &&
                  ResolveItems[0].copy.at == 4 * rrraybudget::LogWordResolveTarget &&
                  ResolveItems[1].copy.at == 4 * rrraybudget::LogWordResolveScene &&
                  ResolveItems[2].copy.at == 4 * rrraybudget::LogWordStats &&
                  ResolveItems[3].copy.at == 4 * (rrraybudget::LogWordStats + 1) &&
                  ApplyItems[0].copy.at == 4 * rrraybudget::LogWordApplyTarget &&
                  ApplyItems[1].copy.at + 12 == 4 * rrraybudget::LogWordSettings36W &&
                  ApplyItems[2].copy.at == 4 * rrraybudget::LogWordScene251 &&
                  ApplyItems[3].copy.at == 4 * rrraybudget::LogWordScene253,
              "every item at its log word");
// The halves' bits are the items' (the resolve half also holds the patch's LogExecuted).
template <std::size_t N> constexpr unsigned Bits(const std::array<Item, N> &items) noexcept {
    unsigned bits{};
    for (const auto &i : items)
        bits |= i.bit;
    return bits;
}
static_assert(Bits(ResolveItems) == (rrraybudget::LogResolveHalf & ~rrraybudget::LogExecuted) &&
                  Bits(ApplyItems) == rrraybudget::LogApplyHalf,
              "the halves are the items' bits");
// GpuFrame's retained source slots for a site: one per native Source up to the last it reads, in
// Source order.
constexpr unsigned NativeSlotsOf(unsigned used) noexcept {
    unsigned slots{};
    for (unsigned s = 0; s < NativeSources; ++s)
        if (used >> s & 1)
            slots = s + 1;
    return slots;
}
template <std::size_t N> constexpr unsigned NativeSlots(const std::array<Item, N> &items) noexcept {
    unsigned used{};
    for (const auto &i : items)
        if (Native(i.copy.source))
            used |= 1u << unsigned(i.copy.source);
    return NativeSlotsOf(used);
}
// Writes the words of ray_values a plan owns (ValuesImage over PlanRanges) through
// put(byte offset, word), leaving the serial word and everything else as it was.
template <class Put> constexpr void ApplyPlan(const rrraybudget::Plan &plan, Put put) {
    const auto image = ValuesImage(plan);
    for (const auto &r : PlanRanges)
        for (std::uint32_t w = r.at / 4; w < (r.at + r.bytes) / 4; ++w)
            put(4 * w, image[w]);
}
constexpr bool Fits(const Copy &c, std::uint64_t avail) noexcept {
    return avail >= c.from && avail - c.from >= c.bytes;
}
// False, leaving `l` as it was, when it is full: the caller then returns an empty list, so a plan
// that outgrew List records nothing rather than a list whose bits claim a dropped copy.
constexpr bool Add(List &l, const Copy &c) noexcept {
    if (l.count >= l.copies.size())
        return false;
    l.copies[l.count++] = c;
    return true;
}
// The serial first, then the copies of every bit whose source holds all of them; empty when none.
// `avail` is indexed by Source (the native ones).
template <std::size_t N>
constexpr List Pick(const Copy &serial, const std::array<Item, N> &items,
                    const std::array<std::uint64_t, NativeSources> &avail) noexcept {
    static_assert(N + 1 <= List{}.copies.size(), "a list holds the serial and every item");
    unsigned wanted{}, short_bits{};
    for (const auto &i : items) {
        wanted |= i.bit;
        if (!Native(i.copy.source) || !Fits(i.copy, avail[unsigned(i.copy.source)]))
            short_bits |= i.bit;
    }
    List l;
    l.bits = wanted & ~short_bits;
    if (!l.bits)
        return l;
    if (!Add(l, serial))
        return {};
    for (const auto &i : items)
        if ((l.bits & i.bit) && !Add(l, i.copy))
            return {};
    return l;
}
// At the native resolve: b1/b3 only from an upload heap; stats_avail 0 = t0 not available.
constexpr List ResolveLog(std::uint64_t b1_avail, bool b1_upload, std::uint64_t b3_avail,
                          bool b3_upload, std::uint64_t stats_avail) noexcept {
    return Pick(ResolveSerial, ResolveItems,
                {b1_upload ? b1_avail : 0, b3_upload ? b3_avail : 0, stats_avail});
}
// At the native VRRT apply.
constexpr List ApplyLog(std::uint64_t b1_avail, bool b1_upload, std::uint64_t b3_avail,
                        bool b3_upload) noexcept {
    return Pick(ApplySerial, ApplyItems, {b1_upload ? b1_avail : 0, b3_upload ? b3_avail : 0, 0});
}
// Register 2 word of `lobe` in ray_cb: from ray_values when the lobe is raised, else the game's.
constexpr Copy TargetWord(unsigned mask, std::uint32_t lobe) noexcept {
    return (mask >> lobe & 1)
               ? Copy{Target::Cb, 32 + 4 * lobe, Source::Values, ValuesTargets + 4 * lobe, 4}
               : Copy{Target::Cb, 32 + 4 * lobe, Source::B1, 32 + 4 * lobe, 4};
}
// ray_cb contents before the barrier, for raised lobes `mask` (1..3, otherwise an empty list): the
// game's b1 bytes in place, a raised lobe's register 2 word from ray_values, then the governor's
// constants after the game's bytes. Nothing reads b1 at or past GameBytes. LogExecuted is set by
// the caller after recording Executed, not here.
constexpr List PatchFill(unsigned mask) noexcept {
    if (mask < 1 || mask > 3)
        return {};
    const std::array<Copy, 5> fill{{
        {Target::Cb, 0, Source::B1, 0, 32},
        TargetWord(mask, 0),
        TargetWord(mask, 1),
        {Target::Cb, 40, Source::B1, 40, GameBytes - 40},
        {Target::Cb, GameBytes, Source::Values, ValuesGovernor, GovernorBytes},
    }};
    List l;
    for (const auto &c : fill)
        if (!Add(l, c))
            return {};
    return l;
}
// The patch's slots: those of every native source a fill reads (the same for every valid mask).
constexpr unsigned NativeSlots(const List &l) noexcept {
    unsigned used{};
    for (unsigned i = 0; i < l.count; ++i)
        if (Native(l.copies[i].source))
            used |= 1u << unsigned(l.copies[i].source);
    return NativeSlotsOf(used);
}
static_assert(NativeSlots(PatchFill(1)) == NativeSlots(PatchFill(3)) &&
                  NativeSlots(PatchFill(2)) == NativeSlots(PatchFill(3)),
              "one patch site for every mask");
// After the barrier: register 2 as the patched resolve executes it, from ray_cb into the log.
inline constexpr Copy Executed{Target::Log, 16, Source::Cb, 32, 16};
static_assert(Executed.at == 4 * rrraybudget::LogWordExecuted, "the executed words");
// The pool ceiling to reserve before allocating `needed` new ray bytes: on top of the current
// limit, never out of the headroom below it that an earlier pool projection kept for mandatory
// allocations. Every later projection counts ray bytes once in its persistent term
// (Controller::PersistentBytes) and leaves each slot's base unchanged, so it grows by exactly the
// ray bytes allocated: one that fit under `limit` fits under the result. 0 when nothing is needed
// or the sum overflows (no reservation).
constexpr std::uint64_t RayCeiling(std::uint64_t limit, std::uint64_t needed) noexcept {
    return needed && needed <= UINT64_MAX - limit ? limit + needed : 0;
}
} // namespace rrraycopy
