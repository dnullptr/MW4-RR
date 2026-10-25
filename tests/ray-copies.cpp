// CPU-only checks of the ray budget test's copy plan (src/rr_ray_copies.h): what the controller's
// GPU copies read, and where they land in its private constant buffer and ray log.
#include "../src/rr_ray_copies.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {
using namespace rrraycopy;
namespace rb = rrraybudget;

constexpr bool Same(const Copy &a, const Copy &b) {
    return a.target == b.target && a.at == b.at && a.source == b.source && a.from == b.from &&
           a.bytes == b.bytes;
}
constexpr bool Contains(const List &l, const Copy &c) {
    for (unsigned i = 0; i < l.count; ++i)
        if (Same(l.copies[i], c))
            return true;
    return false;
}
// The bytes the caller says each native source holds from the copy's start.
struct Avail {
    std::uint64_t b1{}, b3{}, stats{};
};
constexpr Avail AllOn{CbBytes, CbBytes, 2 * std::uint64_t(CbBytes)}, AllOff{};
// Every copy reads inside its source: the available native bytes, ray_values or ray_cb.
constexpr bool Reads(const List &l, const Avail &a) {
    if (l.count > l.copies.size())
        return false;
    for (unsigned i = 0; i < l.count; ++i) {
        const auto &c = l.copies[i];
        const std::uint64_t limit = c.source == Source::B1       ? a.b1
                                    : c.source == Source::B3     ? a.b3
                                    : c.source == Source::Stats  ? a.stats
                                    : c.source == Source::Values ? ValuesBytes
                                                                 : CbBytes;
        if (!c.bytes || c.from > limit || c.bytes > limit - c.from)
            return false;
    }
    return true;
}

// ---- ray_cb: the private b1 the patched resolve and the governor run with ----
static_assert(GameBytes == 38 * 16 && GovernorBytes == 4 * 4 && CbBytes == 65536,
              "the governor declares Game[38] then Mask, Headroom0, Headroom1, Pad");
static_assert(ValuesTargets == 0 && ValuesSerial == 16 && ValuesGovernor == 32 &&
                  ValuesGovernor + GovernorBytes <= ValuesBytes && ValuesBytes == 256,
              "ray_values: targets 0..7, serial 16..19, governor constants 32..47 of 256 bytes");
// The copy that writes ray_cb byte `at`, or nullptr.
constexpr const Copy *Writer(const List &l, std::uint32_t at) {
    for (unsigned i = 0; i < l.count; ++i)
        if (l.copies[i].at <= at && at - l.copies[i].at < l.copies[i].bytes)
            return &l.copies[i];
    return nullptr;
}
// Bytes [0, 624) are written exactly once; b1 bytes keep their place and none at or past the game's
// 608 is read; lobe l's target word comes from ray_values iff l is raised, and the governor's
// constants follow the game's bytes.
constexpr bool PatchFillCovers(unsigned mask) {
    const auto l = PatchFill(mask);
    if (!l.count || l.bits || !Reads(l, {GameBytes, 0, 0}))
        return false;
    std::array<unsigned char, GameBytes + GovernorBytes> writes{};
    for (unsigned i = 0; i < l.count; ++i) {
        const auto &c = l.copies[i];
        if (c.target != Target::Cb || c.at > writes.size() || c.bytes > writes.size() - c.at ||
            (c.source != Source::B1 && c.source != Source::Values) ||
            (c.source == Source::B1 && c.from != c.at))
            return false;
        for (std::uint32_t b = 0; b < c.bytes; ++b)
            ++writes[c.at + b];
    }
    for (const auto w : writes)
        if (w != 1)
            return false;
    for (unsigned lobe = 0; lobe < 2; ++lobe) {
        const std::uint32_t at = 32 + 4 * lobe;
        const auto *c = Writer(l, at);
        const Copy raised{Target::Cb, at, Source::Values, ValuesTargets + 4 * lobe, 4},
            native{Target::Cb, at, Source::B1, at, 4};
        if (!c || !Same(*c, (mask >> lobe & 1) ? raised : native))
            return false;
    }
    const auto *governor = Writer(l, GameBytes);
    return governor &&
           Same(*governor, {Target::Cb, GameBytes, Source::Values, ValuesGovernor, GovernorBytes});
}
static_assert(PatchFillCovers(1) && PatchFillCovers(2) && PatchFillCovers(3));
static_assert(!PatchFill(0).count && !PatchFill(4).count && !PatchFill(~0u).count &&
              !PatchFill(0).bits && !PatchFill(4).bits);
// The executed copy reads ray_cb's register 2 after the patch (the fill writes it) into the log.
static_assert(Same(Executed, {Target::Log, 16, Source::Cb, 32, 16}));

// ---- ray_log: what the gate reads (the table next to the Log bits in rr_ray_budget.h) ----
struct Words {
    unsigned bit, first, count;
};
constexpr Words gate[] = {{rb::LogResolveTarget, 0, 4},    {rb::LogExecuted, 4, 4},
                          {rb::LogResolveScene, 8, 4},     {rb::LogApplyTarget, 12, 4},
                          {rb::LogApplySettings36, 16, 4}, {rb::LogApplyScene251, 20, 4},
                          {rb::LogResolveStats, 25, 2},    {rb::LogApplyScene253, 28, 4}};
// The serials only the controller reads: one per half, so a skipped half is told apart.
constexpr unsigned ResolveSerialWord = 24, ApplySerialWord = 27;
constexpr Copy ResolveSerialCopy{Target::Log, ResolveSerialWord * 4, Source::Values, ValuesSerial,
                                 4},
    ApplySerialCopy{Target::Log, ApplySerialWord * 4, Source::Values, ValuesSerial, 4};
constexpr std::uint64_t Span(unsigned first, unsigned count) {
    return ((std::uint64_t(1) << count) - 1) << first;
}
constexpr std::uint64_t GateWords(unsigned bit) {
    std::uint64_t words{};
    for (const auto &g : gate)
        if (g.bit == bit)
            words |= Span(g.first, g.count);
    return words;
}
// Adds the log words `c` writes (bit w = word w): false unless it is a word-aligned copy inside
// [0, LogRead) that writes no word already in `words`.
constexpr bool AddWords(const Copy &c, std::uint64_t &words) {
    if (c.target != Target::Log || !c.bytes || c.at % 4 || c.bytes % 4 || c.at > LogRead ||
        c.bytes > LogRead - c.at)
        return false;
    for (std::uint32_t w = c.at / 4; w < (c.at + c.bytes) / 4; ++w) {
        if (words >> w & 1)
            return false;
        words |= std::uint64_t(1) << w;
    }
    return true;
}
static_assert(LogRead == rb::LogWords * 4 && LogRead <= LogBytes && rb::LogWords <= 64);
enum Site { Resolve, Apply };
constexpr List Make(Site site, const Avail &a, bool b1_upload = true, bool b3_upload = true) {
    return site == Resolve ? ResolveLog(a.b1, b1_upload, a.b3, b3_upload, a.stats)
                           : ApplyLog(a.b1, b1_upload, a.b3, b3_upload);
}
constexpr unsigned ResolveBits = rb::LogResolveTarget | rb::LogResolveScene | rb::LogResolveStats,
                   ApplyBits = rb::LogApplyTarget | rb::LogApplySettings36 | rb::LogApplyScene251 |
                               rb::LogApplyScene253;
// Everything on: the serial first, then every copy; the two halves and the executed copy write
// pairwise disjoint words inside [0, LogRead), and together exactly the gate's and serial words.
constexpr bool LogLayout() {
    const auto resolve = Make(Resolve, AllOn), apply = Make(Apply, AllOn);
    if (resolve.bits != ResolveBits || apply.bits != ApplyBits || resolve.count != 5 ||
        apply.count != 5 || !Same(resolve.copies[0], ResolveSerialCopy) ||
        !Same(apply.copies[0], ApplySerialCopy) || !Reads(resolve, AllOn) || !Reads(apply, AllOn))
        return false;
    std::uint64_t words{};
    for (const auto *l : {&resolve, &apply})
        for (unsigned i = 0; i < l->count; ++i)
            if (!AddWords(l->copies[i], words))
                return false;
    if (!AddWords(Executed, words))
        return false;
    std::uint64_t expected = Span(ResolveSerialWord, 1) | Span(ApplySerialWord, 1);
    for (const auto &g : gate)
        expected |= Span(g.first, g.count);
    return words == expected && GateWords(rb::LogExecuted) == Span(Executed.at / 4, 4);
}
static_assert(LogLayout());

// ---- thresholds: each bit's copies, the bytes they need, and where they land ----
struct Bit {
    Site site;
    Source source;
    std::uint64_t need; // the source's bytes from its start that the bit's copies read
    unsigned bit;
    std::array<Copy, 2> copies;
    unsigned count;
};
// A log copy of the native register `reg` (16 bytes) or of one 4-byte t0 word.
constexpr Copy Register(std::uint32_t at, Source s, std::uint32_t reg) {
    return {Target::Log, at, s, std::uint64_t(reg) * 16, 16};
}
constexpr Copy Word(std::uint32_t at, std::uint64_t from) {
    return {Target::Log, at, Source::Stats, from, 4};
}
constexpr Bit bits[] = {
    {Resolve, Source::B1, 48, rb::LogResolveTarget, {Register(0, Source::B1, 2)}, 1},
    {Resolve, Source::B3, 1392, rb::LogResolveScene, {Register(32, Source::B3, 86)}, 1},
    // the governor's x per lobe, at lobe << 16
    {Resolve, Source::Stats, 65540, rb::LogResolveStats, {Word(100, 0), Word(104, 1 << 16)}, 2},
    {Apply, Source::B1, 48, rb::LogApplyTarget, {Register(48, Source::B1, 2)}, 1},
    {Apply, Source::B1, 592, rb::LogApplySettings36, {Register(64, Source::B1, 36)}, 1},
    {Apply, Source::B3, 4032, rb::LogApplyScene251, {Register(80, Source::B3, 251)}, 1},
    {Apply, Source::B3, 4064, rb::LogApplyScene253, {Register(112, Source::B3, 253)}, 1}};
constexpr Avail With(Avail a, Source s, std::uint64_t n) {
    (s == Source::B1 ? a.b1 : s == Source::B3 ? a.b3 : a.stats) = n;
    return a;
}
// With the source at `need` bytes the bit's copies are in the list and land on the gate's words for
// the bit; one byte less drops exactly those copies and the bit (and the serial when nothing else
// is left). The other sources are all on or all absent.
constexpr bool Threshold(const Bit &t, const Avail &others) {
    const Avail at = With(others, t.source, t.need), below = With(others, t.source, t.need - 1);
    const auto in = Make(t.site, at), out = Make(t.site, below);
    const auto &serial = t.site == Resolve ? ResolveSerialCopy : ApplySerialCopy;
    if (!Reads(in, at) || !Reads(out, below) || !(in.bits & t.bit) ||
        out.bits != (in.bits & ~t.bit))
        return false;
    for (unsigned i = 0; i < out.count; ++i)
        if (!Contains(in, out.copies[i]))
            return false;
    for (unsigned i = 0; i < t.count; ++i)
        if (!Contains(in, t.copies[i]) || Contains(out, t.copies[i]))
            return false;
    std::uint64_t words{};
    for (unsigned i = 0; i < in.count; ++i)
        if (!Contains(out, in.copies[i]) && !Same(in.copies[i], serial) &&
            !AddWords(in.copies[i], words))
            return false;
    const unsigned serial_dropped = out.count ? 0 : 1;
    return in.count == out.count + t.count + serial_dropped && Same(in.copies[0], serial) &&
           (!out.count || Same(out.copies[0], serial)) && words == GateWords(t.bit);
}
constexpr bool Thresholds() {
    for (const auto &t : bits)
        if (!Threshold(t, AllOn) || !Threshold(t, AllOff))
            return false;
    return true;
}
static_assert(Thresholds());
// Nothing included means an empty list (no serial); upload false drops the b1/b3 copies.
constexpr bool Uploads() {
    const Avail short_by_one{47, 1391, 65539};
    for (const auto site : {Resolve, Apply})
        for (const auto &a : {AllOff, short_by_one})
            if (Make(site, a).count || Make(site, a).bits)
                return false;
    if (Make(Apply, AllOn, false, false).count ||
        Make(Resolve, AllOn, false, false).bits != rb::LogResolveStats ||
        Make(Resolve, AllOn, false, true).bits != (rb::LogResolveScene | rb::LogResolveStats) ||
        Make(Resolve, AllOn, true, false).bits != (rb::LogResolveTarget | rb::LogResolveStats) ||
        Make(Apply, AllOn, false, true).bits != (rb::LogApplyScene251 | rb::LogApplyScene253) ||
        Make(Apply, AllOn, true, false).bits != (rb::LogApplyTarget | rb::LogApplySettings36))
        return false;
    for (const auto site : {Resolve, Apply})
        for (unsigned upload = 0; upload < 4; ++upload) {
            const bool b1 = (upload & 1) != 0, b3 = (upload & 2) != 0;
            const auto l = Make(site, AllOn, b1, b3);
            for (unsigned i = 0; i < l.count; ++i)
                if ((l.copies[i].source == Source::B1 && !b1) ||
                    (l.copies[i].source == Source::B3 && !b3))
                    return false;
        }
    return true;
}
static_assert(Uploads());
} // namespace

int main() {
    // The same checks at run time, so the sanitizer builds execute them too.
    for (unsigned mask = 1; mask < 4; ++mask)
        assert(PatchFillCovers(mask));
    for (const unsigned mask : {0u, 4u, ~0u})
        assert(!PatchFill(mask).count && !PatchFill(mask).bits);
    assert(Same(Executed, {Target::Log, 16, Source::Cb, 32, 16}));
    assert(LogLayout());
    assert(Thresholds());
    assert(Uploads());
    std::printf("ray copy plan checks passed\n");
    return 0;
}
