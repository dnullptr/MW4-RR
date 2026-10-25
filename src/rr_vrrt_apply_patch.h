#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <string>
namespace rrapply {
// Development test, session only: which noise value the game's cs_vrrt_apply compares each
// lobe's ray probability against. The game fetches four noise taps per lobe but compares both
// lobes against tap 0 of the same pixel, so lobe 2's rays land on lobe 1's pixels. Lobe 1 is
// never changed; each lobe keeps its native probability, ray counts and lists.
enum Noise : unsigned { Shared, Separate, Opposite, NoiseCount };
inline constexpr const char *noise_labels[NoiseCount] = {
    "Off (both lobes share one draw)", "Separate draws", "Opposite draws"};
inline std::atomic<unsigned> noise{};
// Shifts the 128x128 noise texture by a fixed hashed offset per 128-pixel screen tile, so tiles
// no longer repeat one pattern. Each pixel keeps the native per-texel sequence over time.
inline std::atomic<bool> untile{};
inline constexpr unsigned Variants = 2 * NoiseCount; // Variant 0 is the native shader.
inline constexpr unsigned Variant(unsigned n, bool u) {
    return (n < NoiseCount ? n : 0u) + (u ? unsigned(NoiseCount) : 0u);
}
inline unsigned Selected() {
    return Variant(noise.load(std::memory_order_relaxed), untile.load(std::memory_order_relaxed));
}
inline constexpr unsigned NoiseOf(unsigned v) {
    return v % NoiseCount;
}
inline constexpr bool UntileOf(unsigned v) {
    return v / NoiseCount != 0;
}
inline const char *Name(unsigned v) {
    constexpr const char *names[Variants] = {"native",
                                             "separate draws",
                                             "opposite draws",
                                             "128-px repeat break",
                                             "separate draws + 128-px repeat break",
                                             "opposite draws + 128-px repeat break"};
    return v < Variants ? names[v] : "unknown";
}
// Overlay bookkeeping, written by pipeline creation and the apply dispatch.
inline std::array<std::atomic<const char *>, Variants> variant_status{};
inline std::atomic<unsigned> last_variant{};
inline std::atomic<long long> last_patched{-1000000}, last_fallback{-1000000};
inline long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// variant: what was selected for this apply dispatch; patched: whether its copy was bound.
inline void Dispatched(unsigned variant, bool patched) {
    last_variant.store(patched ? variant : 0, std::memory_order_relaxed);
    (patched ? last_patched : last_fallback).store(NowMs(), std::memory_order_relaxed);
}
enum class State { Off, Active, Unavailable, Waiting };
inline State Current(unsigned selected) {
    if (!selected)
        return State::Off;
    const auto now = NowMs();
    if (now - last_patched.load(std::memory_order_relaxed) < 2000 &&
        last_variant.load(std::memory_order_relaxed) == selected)
        return State::Active;
    if (now - last_fallback.load(std::memory_order_relaxed) < 2000)
        return State::Unavailable;
    return State::Waiting;
}

// Exact lines of the native disassembly that the edits rely on, each present once.
inline constexpr const char *Entry = "define void @cs_vrrt_apply() {";
inline constexpr const char *Guards[] = {
    ("  %9 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 17, i1 false)  ; "
     "CreateHandle(resourceClass,rangeId,index,nonUniformIndex)"),
    "  %12 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)",
    "  %13 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)",
    "  %15 = alloca [4 x float], align 4",
    // Lobe loop: %34 is the lobe index, 0 then 1.
    "  %34 = phi i32 [ 0, %28 ], [ %305, %302 ]",
    "  %305 = add nuw nsw i32 %34, 1",
    "  %306 = icmp eq i32 %305, 2",
    // %106 is the ray rate: the probability 1/(16-code) on the Bernoulli path below.
    "  %104 = fdiv fast float 1.000000e+00, %103",
    "  %106 = phi float [ %100, %98 ], [ %104, %101 ]",
    "  %109 = fcmp fast olt float %106, 1.000000e+00",
    // Tap loop: all four taps are stored before the draw reads one.
    "  %116 = phi i32 [ 0, %110 ], [ %140, %115 ]",
    "  %129 = frem fast float %127, 1.280000e+02",
    "  %130 = frem fast float %128, 1.280000e+02",
    "  %131 = fptosi float %129 to i32",
    "  %132 = fptosi float %130 to i32",
    "  %138 = extractvalue %dx.types.ResRet.f32 %137, 0",
    "  %139 = getelementptr [4 x float], [4 x float]* %15, i32 0, i32 %116",
    "  %140 = add nuw nsw i32 %116, 1",
    "  %141 = icmp eq i32 %140, 4",
    "  %144 = load float, float* %143, align 4",
    "  %146 = select i1 %145, float 1.000000e+00, float 0.000000e+00",
};
inline constexpr const char *Draw = "  %145 = fcmp fast ogt float %106, %144";
inline constexpr const char *FirstTap =
    "  %143 = getelementptr inbounds [4 x float], [4 x float]* %15, i32 0, i32 0";
inline constexpr const char *TapLoad =
    "  %137 = call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32 66, %dx.types.Handle %9, i32 0, "
    "i32 %131, i32 %132, i32 %136, i32 undef, i32 undef, i32 undef)  ; "
    "TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,offset1,offset2)";
inline constexpr const char *EntryAnchor = "  %18 = uitofp i32 %13 to float";

inline size_t FindLine(const std::string &text, const std::string &line) {
    const auto key = '\n' + line + '\n';
    const auto at = text.find(key);
    if (at == std::string::npos || text.find(key, at + 1) != std::string::npos)
        return std::string::npos;
    return at + 1;
}
inline bool ReplaceLine(std::string &text, const std::string &line, const std::string &with) {
    const auto at = FindLine(text, line);
    if (at == std::string::npos)
        return false;
    text.replace(at, line.size(), with);
    return true;
}
// The first line the edits rely on that is not present exactly once, or nullptr.
inline const char *MissingAnchor(const std::string &text) {
    if (FindLine(text, Entry) == std::string::npos)
        return Entry;
    for (const char *g : Guards)
        if (FindLine(text, g) == std::string::npos)
            return g;
    for (const char *a : {Draw, FirstTap, TapLoad, EntryAnchor})
        if (FindLine(text, a) == std::string::npos)
            return a;
    return nullptr;
}
// text: the dxcompiler disassembly with '\r' removed. Variant 0 is rejected: the native shader
// needs no copy.
inline bool Transform(std::string &text, unsigned variant) {
    if (!variant || variant >= Variants || text.find("%rrsel.") != std::string::npos ||
        MissingAnchor(text))
        return false;
    const auto n = NoiseOf(variant);
    // Lobe 2 takes tap 1, which the game already fetches at the next R2 offset in the same slice.
    if (n == Separate &&
        !ReplaceLine(text, FirstTap,
                     "  %143 = getelementptr inbounds [4 x float], [4 x float]* %15, i32 0, "
                     "i32 %34"))
        return false;
    // Lobe 2 draws with 1 - u, so its rays avoid lobe 1's: every Bernoulli rate is at most 1/2,
    // so the two hit sets are disjoint. Its rate is shrunk by 2^-16 so that a noise level lying
    // exactly at 1 - p counts as a miss, as level p does for the native draw; that keeps lobe 2's
    // hit count per rate equal to the native one for the 8-bit noise.
    if (n == Opposite &&
        !ReplaceLine(text, Draw,
                     "  %rrsel.second = icmp ne i32 %34, 0\n"
                     "  %rrsel.flipped = fsub float 1.000000e+00, %144\n"
                     "  %rrsel.noise = select i1 %rrsel.second, float %rrsel.flipped, float %144\n"
                     "  %rrsel.shrunk = fmul float %106, 0x3FEFFFE000000000\n"
                     "  %rrsel.rate = select i1 %rrsel.second, float %rrsel.shrunk, float %106\n"
                     "  %145 = fcmp fast ogt float %rrsel.rate, %rrsel.noise"))
        return false;
    if (!UntileOf(variant))
        return true;
    // Per 128-pixel screen tile, a fixed toroidal offset hashed from the tile coordinate
    // (lowbias32), added to every tap's texel. The slice, and so each pixel's sequence over
    // time, stays native.
    return ReplaceLine(text, EntryAnchor,
                       std::string(EntryAnchor) +
                           "\n"
                           "  %rrsel.tilex = lshr i32 %12, 7\n"
                           "  %rrsel.tiley = lshr i32 %13, 7\n"
                           "  %rrsel.tilerow = shl i32 %rrsel.tiley, 16\n"
                           "  %rrsel.tile = or i32 %rrsel.tilerow, %rrsel.tilex\n"
                           "  %rrsel.mix0 = lshr i32 %rrsel.tile, 16\n"
                           "  %rrsel.hash0 = xor i32 %rrsel.tile, %rrsel.mix0\n"
                           "  %rrsel.hash1 = mul i32 %rrsel.hash0, 2146121005\n"
                           "  %rrsel.mix1 = lshr i32 %rrsel.hash1, 15\n"
                           "  %rrsel.hash2 = xor i32 %rrsel.hash1, %rrsel.mix1\n"
                           "  %rrsel.hash3 = mul i32 %rrsel.hash2, -2073254261\n"
                           "  %rrsel.mix3 = lshr i32 %rrsel.hash3, 16\n"
                           "  %rrsel.hash = xor i32 %rrsel.hash3, %rrsel.mix3\n"
                           "  %rrsel.offsetx = and i32 %rrsel.hash, 127\n"
                           "  %rrsel.hashy = lshr i32 %rrsel.hash, 7\n"
                           "  %rrsel.offsety = and i32 %rrsel.hashy, 127") &&
           ReplaceLine(text, TapLoad,
                       "  %rrsel.shiftx = add i32 %131, %rrsel.offsetx\n"
                       "  %rrsel.texelx = and i32 %rrsel.shiftx, 127\n"
                       "  %rrsel.shifty = add i32 %132, %rrsel.offsety\n"
                       "  %rrsel.texely = and i32 %rrsel.shifty, 127\n"
                       "  %137 = call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32 66, "
                       "%dx.types.Handle %9, i32 0, i32 %rrsel.texelx, i32 %rrsel.texely, "
                       "i32 %136, i32 undef, i32 undef, i32 undef)  ; "
                       "TextureLoad(srv,mipLevelOrSampleCount,coord0,coord1,coord2,offset0,"
                       "offset1,offset2)");
}
} // namespace rrapply
