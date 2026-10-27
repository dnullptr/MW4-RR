#pragma once
#include <array>
#include <atomic>
#include <cmath>
namespace rrweapondepth {
// Weapon depth (a saved setting, rrsettings::Preferences::weapon_depth): real depth on the
// first-person weapon. The game draws the weapon and arms with their own camera into the
// nearest depth slice, so RR sees them as a flat card at the near plane. With this on, RR
// reads a copy of the game's hardware depth (shaders/rr_weapon_depth.hlsl): world depth is
// unchanged, and the weapon slice is remapped so the weapon keeps its own depth order,
// entirely in front of the near plane, so it never lands behind world geometry it is drawn
// over. RR stays on hardware depth: the third-party DLSS5 NR add-on declines RR frames
// created with linear depth. A change restarts RR.
// The setting as applied (rr_settings.inl ApplyEnabled); off until the settings are loaded.
inline std::atomic<bool> enabled{};
// Bumped on every change of the setting (retries linear depth held off for GPU memory, an
// unsupported projection or RR refusing it).
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
// The game's projection is infinite reverse-Z (depth = near / z, sky at depth 0).
// Row-vector (pos * M): [11] = 1, [14] = near. Column-vector (M * pos): transposed.
enum class Layout { Unknown, RowVector, ColumnVector };
inline Layout Detect(const std::array<float, 16> &m) {
    if (std::abs(m[10]) > 1e-6f || m[15] != 0)
        return Layout::Unknown;
    if (m[11] == 1 && m[14] > 0)
        return Layout::RowVector;
    if (m[14] == 1 && m[11] > 0)
        return Layout::ColumnVector;
    return Layout::Unknown;
}
// Linear depth of sky, in near planes: very far instead of infinitely far.
inline constexpr float FarScale = 1e6f;
// projection: the game's, which RR is also given, so linear depth matches it. False when
// the depth cannot be expressed this way.
inline bool Map(const std::array<float, 16> &projection, Mapping &m) {
    const auto layout = Detect(projection);
    if (layout == Layout::Unknown)
        return false;
    // Row-vector: clip z = x m2 + y m6 + z m10 + m14, clip w = x m3 + y m7 + z (m11 = 1).
    // Column-vector is the transpose.
    const bool row = layout == Layout::RowVector;
    const unsigned near_term = row ? 14 : 11;
    const unsigned planar[] = {row ? 2u : 8u, row ? 6u : 9u, row ? 3u : 12u, row ? 7u : 13u};
    for (unsigned i : planar)
        if (std::abs(projection[i]) > 1e-6f)
            return false;
    const double scale = projection[near_term],
                 offset = 0.0 - double(projection[10]); // Never -0.
    if (!std::isfinite(scale) || !std::isfinite(offset) || !(scale > 0) || !(offset >= 0))
        return false;
    const double sky = offset > 0 ? scale / offset : scale * FarScale;
    m = {float(scale), float(offset), float(sky)};
    return std::isfinite(m.sky) && m.sky > 0;
}
// Why linear depth stays off until the setting next changes (Controller::HoldWeaponDepth).
enum class Hold { None, Memory, Projection, Refused };
// A frame's depth (Controller::AfterSR). RR's depth type is fixed when RR is created, so a
// frame follows its RR's type and a new RR takes the setting's.
inline constexpr bool Wants(bool feature, bool feature_linear, bool wanted) {
    return feature ? feature_linear : wanted;
}
struct FrameDepth {
    bool linear{};  // This frame gives RR linear depth.
    bool restart{}; // Skip this frame and restart RR: its RR has linear depth it cannot get.
    Hold hold{};    // Hold linear depth off (None: no change).
};
// mapped: the frame's projection maps to linear depth (Map). texture: the linear depth
// texture is ready for this frame (Controller::UseTest); blocked: it had no GPU memory.
inline constexpr FrameDepth Choose(bool feature, bool feature_linear, bool wanted, bool mapped,
                                   bool texture, bool blocked) {
    FrameDepth f{Wants(feature, feature_linear, wanted)};
    if (!f.linear || (mapped && texture))
        return f;
    // No GPU memory or a projection that does not map holds linear depth off. A texture left
    // at another size is only released: the restart allocates it again.
    f.hold = !mapped ? Hold::Projection : blocked ? Hold::Memory : Hold::None;
    // An RR with linear depth is never given hardware depth.
    if (feature)
        f.restart = true;
    else
        f.linear = false;
    return f;
}
// CPU reference of shaders/rr_weapon_depth.hlsl, same operations in the same order: the
// hardware depth RR reads.
inline float Hardware(float depth, const Mapping &m) {
    if (!(depth > 0))
        return 0;
    if (depth > Slice) {
        const float t = std::fmin((depth - Slice) * 64, 1.0f);
        return (1 + m.offset) * (1 + t / Knee) - m.offset;
    }
    return depth;
}
// The linear view depth RR's projection implies for a hardware depth, sky at m.sky.
inline float Linear(float depth, const Mapping &m) {
    const float d = Hardware(depth, m);
    return d > 0 ? std::fmin(m.scale / (d + m.offset), m.sky) : m.sky;
}
} // namespace rrweapondepth
