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
// constants follow the game's bytes. ray_values supplies nothing else: exactly the raised lobes'
// words and the governor block.
constexpr bool PatchFillCovers(unsigned mask) {
    const auto l = PatchFill(mask);
    if (!l.count || l.bits || !Reads(l, {GameBytes, 0, 0}))
        return false;
    std::array<unsigned char, GameBytes + GovernorBytes> writes{};
    std::uint32_t values{};
    for (unsigned i = 0; i < l.count; ++i) {
        const auto &c = l.copies[i];
        if (c.target != Target::Cb || c.at > writes.size() || c.bytes > writes.size() - c.at ||
            (c.source != Source::B1 && c.source != Source::Values) ||
            (c.source == Source::B1 && c.from != c.at))
            return false;
        for (std::uint32_t b = 0; b < c.bytes; ++b)
            ++writes[c.at + b];
        if (c.source == Source::Values)
            values += c.bytes;
    }
    if (values != 4 * ((mask & 1) + (mask >> 1 & 1)) + GovernorBytes)
        return false;
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
// A full list takes no ninth copy and says so (Pick and PatchFill then return an empty list).
constexpr bool AddRefusesWhenFull() {
    List l;
    for (std::uint32_t i = 0; i < l.copies.size(); ++i)
        if (!Add(l, {Target::Log, 4 * i, Source::Values, 0, 4}))
            return false;
    const auto full = l;
    return !Add(l, Executed) && l.count == full.count && Same(l.copies.back(), full.copies.back());
}
static_assert(AddRefusesWhenFull());
// The game's buffers come first: GpuFrame retains one slot per native source and site.
static_assert(Native(Source::B1) && Native(Source::B3) && Native(Source::Stats) &&
              !Native(Source::Values) && !Native(Source::Cb) && unsigned(Source::Count) == 5);

// ---- byte images: what the patched resolve and the governor read from ray_cb ----
template <std::size_t N>
constexpr std::uint32_t WordAt(const std::array<unsigned char, N> &m, std::size_t at) {
    return std::uint32_t(m[at]) | std::uint32_t(m[at + 1]) << 8 | std::uint32_t(m[at + 2]) << 16 |
           std::uint32_t(m[at + 3]) << 24;
}
template <std::size_t N>
constexpr void PutWord(std::array<unsigned char, N> &m, std::size_t at, std::uint32_t v) {
    for (unsigned b = 0; b < 4; ++b)
        m[at + b] = static_cast<unsigned char>(v >> (8 * b));
}
// The game's b1 from the CBV start (as far as PatchFill reads), ray_values, ray_cb, ray_log.
struct Images {
    std::array<unsigned char, GameBytes> b1{};
    std::array<unsigned char, ValuesBytes> values{};
    std::array<unsigned char, GameBytes + GovernorBytes> cb{};
    std::array<unsigned char, LogBytes> log{};
};
// Executes copies from b1 / ray_values / ray_cb into ray_cb / ray_log (every copy is in bounds:
// PatchFillCovers and Reads check that).
constexpr void Run(const List &l, Images &m) {
    for (unsigned i = 0; i < l.count; ++i) {
        const auto &c = l.copies[i];
        for (std::uint32_t b = 0; b < c.bytes; ++b) {
            const auto from = c.from + b;
            const unsigned char v = c.source == Source::B1       ? m.b1[from]
                                    : c.source == Source::Values ? m.values[from]
                                                                 : m.cb[from];
            (c.target == Target::Cb ? m.cb[c.at + b] : m.log[c.at + b]) = v;
        }
    }
}
// The governor's cbuffer (shaders/rr_vrrt_governor.hlsl): uint4 Game[38], then Mask, Headroom0,
// Headroom1, Pad.
constexpr std::uint32_t GovernorMask = 38 * 16, GovernorHeadroom0 = GovernorMask + 4,
                        GovernorHeadroom1 = GovernorMask + 8, GovernorPad = GovernorMask + 12;
// ray_values as WriteRayPlan and WriteRaySerial leave it, then the patch: ray_cb has the game's
// bytes except the raised lobes' register 2 words, which are the plan's bits, and the governor
// reads the plan's mask and headrooms; the executed copy logs the register 2 the resolve runs with.
constexpr bool PatchImage(unsigned mask) {
    rb::Plan plan;
    plan.mask = mask;
    plan.bits = {0x3f000000u, 0x3e99999au};
    plan.headroom = {0x00012345u, 0x00054321u};
    Images m;
    for (std::size_t i = 0; i < m.b1.size(); ++i)
        m.b1[i] = static_cast<unsigned char>(i * 7 + 3);
    for (auto &v : m.values)
        v = 0xEE; // whatever an earlier recording left
    const auto image = ValuesImage(plan);
    for (const auto &r : PlanRanges) {
        if (r.at < ValuesSerial + 4 && ValuesSerial < r.at + r.bytes)
            return false; // the plan never overwrites the serial
        for (std::uint32_t w = r.at / 4; w < (r.at + r.bytes) / 4; ++w)
            PutWord(m.values, 4 * w, image[w]);
    }
    PutWord(m.values, ValuesSerial, 77);
    Run(PatchFill(mask), m);
    List executed;
    Add(executed, Executed);
    Run(executed, m);
    for (std::uint32_t at = 0; at < GameBytes; ++at)
        if ((at < 32 || at >= 40) && m.cb[at] != m.b1[at])
            return false;
    for (unsigned lobe = 0; lobe < 2; ++lobe) {
        const auto word = WordAt(m.cb, 32 + 4 * lobe);
        if (word != ((mask >> lobe & 1) ? plan.bits[lobe] : WordAt(m.b1, 32 + 4 * lobe)) ||
            WordAt(m.log, Executed.at + 4 * lobe) != word)
            return false;
    }
    return WordAt(m.cb, GovernorMask) == mask && WordAt(m.cb, GovernorHeadroom0) == 0x00012345u &&
           WordAt(m.cb, GovernorHeadroom1) == 0x00054321u && WordAt(m.cb, GovernorPad) == 0 &&
           WordAt(m.log, Executed.at + 8) == WordAt(m.b1, 40) &&
           WordAt(m.log, Executed.at + 12) == WordAt(m.b1, 44);
}
static_assert(PatchImage(1) && PatchImage(2) && PatchImage(3));

// ---- the pool ceiling for new ray bytes ----
static_assert(RayCeiling(1u << 30, 3 * 256) == (1u << 30) + 3 * 256 &&
              RayCeiling(1u << 30, 0) == 0 && RayCeiling(UINT64_MAX - 6, 6) == UINT64_MAX &&
              RayCeiling(UINT64_MAX - 5, 6) == 0 && RayCeiling(UINT64_MAX, 2) == 0 &&
              RayCeiling(0, 1) == 1);
// A model of Budget::Reserve (without its DXGI check) and of the next mandatory projection: the
// ray bytes enter each projection once (PersistentBytes) and leave every slot's base unchanged.
// With the limit exactly at the last projection and headroom kept for allocations still to come,
// the projection after a ray allocation fits under the raised ceiling without growing it.
constexpr bool CeilingKeepsHeadroom(std::uint64_t projection, std::uint64_t allocated,
                                    std::uint64_t ray) {
    std::uint64_t limit = projection, bytes = allocated;
    const auto reserve = [&](std::uint64_t required) {
        if (required > limit)
            limit = required; // the real Reserve asks DXGI for required - bytes here
    };
    const auto ceiling = RayCeiling(limit, ray);
    if (!ceiling)
        return false;
    reserve(ceiling);
    bytes += ray;
    const auto before = limit, next = projection + ray;
    reserve(next);
    return limit == before && next <= limit && bytes <= limit &&
           limit - bytes == projection - allocated; // the mandatory headroom is untouched
}
static_assert(CeilingKeepsHeadroom(1212416000, 1202000000, 131072) &&
              CeilingKeepsHeadroom(1073741824, 600000000, 196608) &&
              CeilingKeepsHeadroom(1073741824, 1073741824, 65536));

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
    assert(AddRefusesWhenFull());
    for (unsigned mask = 1; mask < 4; ++mask)
        assert(PatchImage(mask));
    assert(CeilingKeepsHeadroom(1212416000, 1202000000, 131072));
    assert(LogLayout());
    assert(Thresholds());
    assert(Uploads());
    std::printf("ray copy plan checks passed\n");
    return 0;
}
