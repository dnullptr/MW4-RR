#pragma once
#include <atomic>
#include <cmath>
namespace rrcontrast {
// Development test: RR's color with its highlights compressed, and RR's output expanded
// back, so the picture looks the same unless RR treats contrast differently. The sky
// behind thin branches is about 28x brighter than the needles; this shows whether RR's
// flicker there follows the contrast it is given. The brightest channel m becomes
// Knee * ln(1 + m / Knee) and the other channels keep their ratio to it (the same hue);
// a lower knee compresses more. Shaders: rr_color_scale.hlsl compresses (after the color
// scale test's factor), rr_live_gate.hlsl Display expands (before that test's 1/factor).
enum Mode : unsigned { Off, Gentle, Medium, Strong, Count };
inline constexpr const char *labels[Count] = {"Off", "Gentle (knee 1)", "Medium (knee 0.25)",
                                              "Strong (knee 0.05)"};
inline constexpr float knees[Count] = {0.0f, 1.0f, 0.25f, 0.05f};
inline constexpr unsigned Valid(unsigned mode) {
    return mode < Count ? mode : Off;
}
// 0 = off.
inline constexpr float Knee(unsigned mode) {
    return knees[Valid(mode)];
}
// Below this m / Knee, ln(1 + x) / x and (exp(y) - 1) / y use their series to x^2 (error
// under 3e-7), where 1 + x would lose digits.
inline constexpr float SeriesLimit = 0.01f;
// Expansion input y = t / Knee is capped here; the result is capped at FP16's maximum.
inline constexpr float MaxExponent = 18.0f; // 0.05 * e^18 > 65504 * 16
inline constexpr float MaxOutput = 65504.0f;
// CPU references of the shaders, same operations in the same order. Factor multiplies
// every channel of a color whose brightest channel is m.
inline float CompressFactor(float m, float knee) {
    if (!(knee > 0) || !(m > 0))
        return 1.0f;
    const float x = m / knee;
    return x < SeriesLimit ? 1 + x * (x / 3 - 0.5f) : std::log(1 + x) / x;
}
inline float ExpandFactor(float t, float knee) {
    if (!(knee > 0) || !(t > 0))
        return 1.0f;
    const float y = std::fmin(t / knee, MaxExponent);
    return y < SeriesLimit ? 1 + y * (0.5f + y / 6) : (std::exp(y) - 1) / y;
}
inline std::atomic<unsigned> mode{};
// Bumped by the panel on every change (retries a test blocked for GPU memory).
inline std::atomic<unsigned> changes{};
} // namespace rrcontrast
