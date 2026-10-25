#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
namespace rrfx {
// Development test for bright see-through effects (smoke, fog, light shafts):
// the game draws them over the scene without motion vectors of their own, so
// RR accumulates them with the background's motion and they smear. A
// responsivity mask tells RR to lean on the current frame where the color is
// brighter than the color before transparency, that is where a layer added
// light (shaders/rr_transparent_effects.hlsl). Glass that only dims the scene
// (frosted or tinted) adds none, so it keeps RR's history. The mask is merged
// with the water or SSS mask RR would otherwise get: where a layer added light
// the larger value wins, elsewhere that mask is kept.
// Session-only and off by default; NVIDIA supports the mask on preset F only.
inline constexpr float DefaultStrength = 1.0f;
// The added share (color - before) / (color + before) ramps the mask from 0 at
// Low to the full strength at High.
inline constexpr float Low = 0.1f, High = 0.5f;
// Below this luminance sum the two colors are treated as equal (dark noise).
inline constexpr float Floor = 1e-3f;
inline std::atomic<bool> enabled{};
inline std::atomic<float> strength{DefaultStrength};
// Bumped by the panel on every change; a test blocked for GPU memory retries
// once this differs from the count it was blocked at.
inline std::atomic<unsigned> changes{};
inline constexpr unsigned SupportedPreset = 6;

inline float ClampStrength(float value) {
    if (!std::isfinite(value))
        return 0;
    return value < 0 ? 0.0f : value > 1 ? 1.0f : value;
}
struct Tuning {
    bool enabled{};
    float strength = DefaultStrength;
};
inline Tuning Read() {
    return {enabled.load(std::memory_order_relaxed),
            ClampStrength(strength.load(std::memory_order_relaxed))};
}
// Why the mask is not sent this frame, checked in this order.
enum class Hold { None, Off, SkyEdge, Preset, NoTransparency, ColorTest };
// sky_edge: the sky edge test replaces every other mask. transparency: RR gets
// the color before transparency this frame. color_test: the color brightness
// or contrast test scales the color, and the guide is withheld then.
inline Hold Check(const Tuning &t, bool sky_edge, bool preset_known, unsigned preset,
                  bool transparency, bool color_test) {
    if (!t.enabled || ClampStrength(t.strength) == 0)
        return Hold::Off;
    if (sky_edge)
        return Hold::SkyEdge;
    if (!preset_known || preset != SupportedPreset)
        return Hold::Preset;
    if (!transparency)
        return Hold::NoTransparency;
    if (color_test)
        return Hold::ColorTest;
    return Hold::None;
}

// Shader flags and the pass's 8 root constants (b0).
inline constexpr uint32_t ExistingFlag = 1;
struct Constants {
    uint32_t width, height, color_x, color_y, flags;
    float strength, low, high;
};
static_assert(sizeof(Constants) == 32);
inline constexpr unsigned ConstantWords = sizeof(Constants) / 4;
inline Constants Make(const Tuning &t, uint32_t width, uint32_t height, uint32_t color_x,
                      uint32_t color_y, bool existing) {
    return {width, height, color_x, color_y, existing ? ExistingFlag : 0u,
            ClampStrength(t.strength), Low, High};
}

// CPU mirror of the shader, per render pixel: luminances of the color and the
// color before transparency, and the mask RR would get without this test.
inline float Luma(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}
inline float Saturate(float v) {
    return v >= 0 ? (v <= 1 ? v : 1.0f) : 0.0f; // NaN -> 0
}
inline float Mask(float color, float before, float strength, float existing = 0) {
    const float sum = std::fabs(color) + std::fabs(before);
    const float added = sum > Floor ? (color - before) / sum : 0.0f;
    const float m = ClampStrength(strength) * Saturate((added - Low) / (High - Low));
    // Only where a layer added light: elsewhere the existing mask (the SSS one is
    // negative) is kept as it is.
    return m > 0 && m > existing ? m : existing;
}

// Panel line for the test.
enum class Report { Off, SkyEdge, Preset, NoTransparency, ColorTest, Waiting, Blocked, Unavailable, Active };
inline const char *Label(Report r) {
    switch (r) {
    case Report::Off:
        return "off";
    case Report::SkyEdge:
        return "off while the sky edge test is on (RR takes one mask)";
    case Report::Preset:
        return "waiting (needs RR preset F)";
    case Report::NoTransparency:
        return "waiting (no color before transparency this frame; needs the Transparency guide, F9)";
    case Report::ColorTest:
        return "off while the color brightness or contrast test is on";
    case Report::Waiting:
        return "waiting for RR";
    case Report::Blocked:
        return "off: not enough GPU memory (change a setting to retry)";
    case Report::Unavailable:
        return "unavailable (see ReShade.log)";
    case Report::Active:
        return "active";
    }
    return "off";
}
inline Report ReportOf(Hold h) {
    switch (h) {
    case Hold::Off:
        return Report::Off;
    case Hold::SkyEdge:
        return Report::SkyEdge;
    case Hold::Preset:
        return Report::Preset;
    case Hold::NoTransparency:
        return Report::NoTransparency;
    case Hold::ColorTest:
        return Report::ColorTest;
    case Hold::None:
        break;
    }
    return Report::Waiting;
}
} // namespace rrfx
