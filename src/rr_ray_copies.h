#pragma once
// The ray budget test's GPU copy plan (std-only), the one source of truth for every byte offset the
// controller's copies use: GpuFrame (rr_live_gpu.h) records exactly these lists and restates none
// of their offsets. The ray_log layout is the table next to the Log bits in rr_ray_budget.h.
#include "rr_ray_budget.h"
#include <array>
#include <cstddef>
#include <cstdint>
namespace rrraycopy {
// B1/B3: offset is relative to the CBV start; Stats: relative to the t0 view start.
enum class Source : unsigned { B1, B3, Stats, Values, Cb };
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
// Controller-only: the gate never reads either word.
inline constexpr Copy ResolveSerial{Target::Log, 96, Source::Values, ValuesSerial, 4},
    ApplySerial{Target::Log, 108, Source::Values, ValuesSerial, 4};
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
constexpr bool Fits(const Copy &c, std::uint64_t avail) noexcept {
    return avail >= c.from && avail - c.from >= c.bytes;
}
constexpr void Add(List &l, const Copy &c) noexcept {
    if (l.count < l.copies.size())
        l.copies[l.count++] = c;
}
// The serial first, then the copies of every bit whose source holds all of them; empty when none.
// `avail` is indexed by Source (B1, B3, Stats).
template <std::size_t N>
constexpr List Pick(const Copy &serial, const std::array<Item, N> &items,
                    const std::array<std::uint64_t, 3> &avail) noexcept {
    static_assert(N + 1 <= List{}.copies.size(), "a list holds the serial and every item");
    unsigned wanted{}, short_bits{};
    for (const auto &i : items) {
        const auto s = unsigned(i.copy.source);
        wanted |= i.bit;
        if (s >= avail.size() || !Fits(i.copy, avail[s]))
            short_bits |= i.bit;
    }
    List l;
    l.bits = wanted & ~short_bits;
    if (!l.bits)
        return l;
    Add(l, serial);
    for (const auto &i : items)
        if (l.bits & i.bit)
            Add(l, i.copy);
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
// ray_cb contents before the barrier, for raised lobes `mask` (1..3, otherwise an empty list): the
// game's b1 bytes in place, a raised lobe's register 2 word from ray_values, then the governor's
// constants after the game's bytes. Nothing reads b1 at or past GameBytes. LogExecuted is set by
// the caller after recording Executed, not here.
constexpr List PatchFill(unsigned mask) noexcept {
    List l;
    if (mask < 1 || mask > 3)
        return l;
    Add(l, {Target::Cb, 0, Source::B1, 0, 32});
    for (std::uint32_t lobe = 0; lobe < 2; ++lobe)
        Add(l, (mask >> lobe & 1)
                   ? Copy{Target::Cb, 32 + 4 * lobe, Source::Values, ValuesTargets + 4 * lobe, 4}
                   : Copy{Target::Cb, 32 + 4 * lobe, Source::B1, 32 + 4 * lobe, 4});
    Add(l, {Target::Cb, 40, Source::B1, 40, GameBytes - 40});
    Add(l, {Target::Cb, GameBytes, Source::Values, ValuesGovernor, GovernorBytes});
    return l;
}
// After the barrier: register 2 as the patched resolve executes it, from ray_cb into the log.
inline constexpr Copy Executed{Target::Log, 16, Source::Cb, 32, 16};
} // namespace rrraycopy
