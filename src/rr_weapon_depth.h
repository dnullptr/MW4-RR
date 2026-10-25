#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include "rr_sky_depth_test.h"
namespace rrweapondepth {
// Development test: real depth on the first-person weapon. The game draws the weapon and
// arms with their own camera into the nearest depth slice, so with hardware depth RR sees
// them as a flat card at the near plane. This test creates RR with linear (view-space)
// depth instead (shaders/rr_weapon_depth.hlsl): world depth is exactly what the projection
// RR is given implies, and the weapon keeps its own depth order, entirely in front of the
// near plane, so it never lands behind world geometry it is drawn over. RR's depth type is
// fixed when RR is created, so a change restarts RR. Session-only.
inline std::atomic<bool> enabled{};
// Bumped by the panel on every change (retries a test held off for GPU memory, an
// unsupported projection or RR refusing linear depth).
inline std::atomic<unsigned> changes{};
// The weapon slice (shaders/rr_convert_guides.hlsl: the alternate camera): depth in
// (Slice, 1] is Slice + (1 - Slice) * t, with t the weapon camera's own reverse-Z depth.
inline constexpr float Slice = 63.0f / 64.0f;
// Weapon depth sent to RR: near / (1 + t / Knee), which keeps the weapon's own depth order
// and, where t >> Knee, its depth ratios (a weapon in view spans t of about 0.004 to 0.03,
// where the knee bends ratios by about 12% to 2%). Every t > 0 stays in front of the near
// plane, and world depth starts at near * 64 / 63.
inline constexpr float Knee = 1.0f / 2048;
// World linear depth = scale / (depth + offset); sky (depth 0) = sky.
struct Mapping {
    float scale{}, offset{}, sky{};
};
// game: the game's projection, which must be infinite reverse-Z (rrskydepth::Detect).
// passed: the projection RR is given, the game's or the sky depth test's (finite far
// plane), so linear depth matches it. sky_depth: the hardware depth sky stands for (0, or
// the guide test's floor). False when the depth cannot be expressed this way.
inline bool Map(const std::array<float, 16> &game, const std::array<float, 16> &passed,
                float sky_depth, Mapping &m) {
    const auto layout = rrskydepth::Detect(game);
    if (layout == rrskydepth::Layout::Unknown)
        return false;
    // Row-vector: clip z = x m2 + y m6 + z m10 + m14, clip w = x m3 + y m7 + z (m11 = 1).
    // Column-vector is the transpose. Only m10 and the near term may differ in passed.
    const bool row = layout == rrskydepth::Layout::RowVector;
    const unsigned near_term = row ? 14 : 11;
    const unsigned planar[] = {row ? 2u : 8u, row ? 6u : 9u, row ? 3u : 12u, row ? 7u : 13u};
    for (unsigned i : planar)
        if (std::abs(game[i]) > 1e-6f)
            return false;
    for (unsigned i = 0; i < 16; ++i)
        if (i != 10 && i != near_term && passed[i] != game[i])
            return false;
    const double scale = passed[near_term], offset = 0.0 - double(passed[10]); // Never -0.
    if (!std::isfinite(scale) || !std::isfinite(offset) || !(scale > 0) || !(offset >= 0) ||
        !(sky_depth >= 0) || !std::isfinite(sky_depth))
        return false;
    const double sky = sky_depth > 0 ? scale / (sky_depth + offset)
                       : offset > 0  ? scale / offset
                                     : scale * rrskydepth::FarScale;
    m = {float(scale), float(offset), float(sky)};
    return std::isfinite(m.sky) && m.sky > 0;
}
// CPU reference of shaders/rr_weapon_depth.hlsl, same operations in the same order.
inline float Linear(float depth, const Mapping &m) {
    if (!(depth > 0))
        return m.sky;
    if (depth > Slice) {
        const float t = std::fmin((depth - Slice) * 64, 1.0f);
        return m.scale / (1 + m.offset) / (1 + t / Knee);
    }
    return std::fmin(m.scale / (depth + m.offset), m.sky);
}
} // namespace rrweapondepth
