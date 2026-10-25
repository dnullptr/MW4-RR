#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
namespace rrdropout {
// Development test for window glass (RR quality audit, refract-resample-1). The
// game's screen-space refraction resamples a noisy half-resolution copy of the
// scene, so RR's color carries neighbouring copies of the same noise there and
// RR keeps it as texture. NVIDIA's remedy (RR guide 3.5, "Resampling a noisy
// image"): inside the refraction coverage, set a pixel to 0 with probability P
// and divide the survivors by 1 - P, so the average radiance is unchanged; blue
// noise picks the survivors (shaders/rr_refraction_dropout.hlsl). RR reads a
// copy; the game's color is only read. Session-only and off by default.
inline constexpr float DefaultStrength = 0.5f;
// Survivors are scaled by up to 1 / (1 - MaxStrength) = 8.
inline constexpr float MaxStrength = 0.875f;
inline std::atomic<bool> enabled{};
inline std::atomic<float> strength{DefaultStrength};
// Drop the same pixels of the color before transparency (RR compares the two).
inline std::atomic<bool> before_transparency{};
// Show the refraction coverage as a red tint instead of dropping pixels.
inline std::atomic<bool> show_mask{};
// Bumped by the panel on every change; a test blocked for GPU memory retries
// once this differs from the count it was blocked at.
inline std::atomic<unsigned> changes{};

inline float ClampStrength(float value) {
    if (!std::isfinite(value))
        return 0;
    return value < 0 ? 0.0f : value > MaxStrength ? MaxStrength : value;
}
struct Tuning {
    bool enabled{};
    float strength = DefaultStrength;
    bool before_transparency{}, show_mask{};
};
inline Tuning Read() {
    return {enabled.load(std::memory_order_relaxed),
            ClampStrength(strength.load(std::memory_order_relaxed)),
            before_transparency.load(std::memory_order_relaxed),
            show_mask.load(std::memory_order_relaxed)};
}

// Blue-noise threshold tile (shaders/rr_blue_noise.inc, tools/blue_noise.py).
inline constexpr uint32_t Tile = 128, TileMask = Tile - 1, Ranks = Tile * Tile;
// Shader flags.
inline constexpr uint32_t CoverageFlag = 1, BeforeTransparencyFlag = 2, MaskFlag = 4;
// Largest finite value of the copy's format: survivors scaled up to 8x are
// clamped to it rather than overflowing to infinity. The color before
// transparency is always R11G11B10.
inline constexpr float R11G11B10Max = 65024.0f, HalfMax = 65504.0f, FloatMax = 3.4028235e38f;
inline constexpr float FormatMax(bool r11g11b10, bool half) {
    return r11g11b10 ? R11G11B10Max : half ? HalfMax : FloatMax;
}
// The pass's 8 root constants (b0).
struct Constants {
    uint32_t width, height, color_x, color_y;
    uint32_t offset; // blue-noise tile shift: x in bits 0-15, y in bits 16-31
    uint32_t flags;
    float strength;
    float limit; // FormatMax of the copy
};
static_assert(sizeof(Constants) == 32);
inline constexpr unsigned ConstantWords = sizeof(Constants) / 4;

// PCG-RXS-M-XS 32 (Jarzynski and Olano 2020): a new, unrelated tile shift every
// evaluation, so the dropped pixels are independent from frame to frame.
inline constexpr uint32_t Hash(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
inline constexpr uint32_t Offset(uint32_t serial) {
    const uint32_t h = Hash(serial);
    return (h & TileMask) | (((h >> 16) & TileMask) << 16);
}

// The copy replaces RR's color; the color brightness and contrast tests, which
// share another copy, take precedence.
inline bool Wanted(const Tuning &t, bool color_test) {
    return t.enabled && !color_test;
}
// The glass capture cleared and wrote its coverage target in this frame;
// otherwise the target holds an older frame. Its coverage is ready when at
// least one refraction composite also carried its transmittance in alpha.
// Without it the color is copied unchanged.
inline bool CoverageSource(bool target, bool cleared, bool live) {
    return target && cleared && live;
}
inline bool CoverageReady(bool target, bool cleared, bool live, unsigned refraction_alpha) {
    return CoverageSource(target, cleared, live) && refraction_alpha > 0;
}
// The color before transparency is changed only when RR receives it this frame
// and the GPU can load its R11G11B10 texels through a UAV.
inline bool DropBeforeTransparency(const Tuning &t, bool transparency_used, bool typed_load) {
    return t.before_transparency && !t.show_mask && transparency_used && typed_load;
}
inline Constants Make(const Tuning &t, uint32_t width, uint32_t height, uint32_t color_x,
                      uint32_t color_y, uint32_t serial, bool coverage, bool before,
                      float limit = FloatMax) {
    Constants c{};
    c.width = width;
    c.height = height;
    c.color_x = color_x;
    c.color_y = color_y;
    c.offset = Offset(serial);
    c.flags = (coverage ? CoverageFlag : 0u) | (before ? BeforeTransparencyFlag : 0u) |
              (t.show_mask ? MaskFlag : 0u);
    c.strength = ClampStrength(t.strength);
    c.limit = limit;
    return c;
}

// CPU mirror of the shader, per render pixel.
// Coverage alpha is the refraction transmittance; opacity is 1 - saturate(alpha)
// (saturate takes NaN to 0, as on the GPU; UNORM texels are never NaN).
inline float Opacity(float alpha) {
    return 1 - (alpha >= 0 ? (alpha <= 1 ? alpha : 1.0f) : 0.0f);
}
// 3x3 tent (1-2-1 x 1-2-1) of opacity(x, y) over the in-bounds neighbours of (x, y).
template <class F>
float Filtered(int x, int y, int width, int height, F &&opacity) {
    float sum = 0, weight = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= width || qy >= height)
                continue;
            const float k = float((2 - (dx < 0 ? -dx : dx)) * (2 - (dy < 0 ? -dy : dy)));
            sum += k * opacity(qx, qy);
            weight += k;
        }
    return weight > 0 ? sum / weight : 0.0f;
}
inline float Probability(float opacity, float strength) {
    return opacity * ClampStrength(strength);
}
// Threshold in (0, 1) for a blue-noise rank.
inline float Threshold(uint32_t rank) {
    return (float(rank) + 0.5f) / float(Ranks);
}
// Factor for the pixel's color (and color before transparency): 1 outside the
// coverage, 0 when dropped, 1 / (1 - p) when kept.
inline float Scale(float p, uint32_t rank) {
    if (!(p > 0))
        return 1;
    return Threshold(rank) >= p ? 1 / (1 - p) : 0.0f;
}
// A covered pixel's stored value: 0 when dropped (also for an infinite input),
// otherwise the scaled value clamped to the format's largest finite value.
inline float Store(float value, float scale, float limit) {
    if (!(scale > 0))
        return 0;
    const float v = value * scale;
    return v < limit ? v : limit;
}
struct Rgb {
    float r, g, b;
};
// The mask view: red in proportion to coverage, at the pixel's own luminance.
inline Rgb MaskView(Rgb c, float opacity) {
    const float y = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    return {c.r + (2 * y - c.r) * opacity, c.g - c.g * opacity, c.b - c.b * opacity};
}

// Panel line for the test.
enum class Report { Off, ColorTest, Waiting, Blocked, Unavailable, NoSource, NoRefraction, Active };
inline const char *Label(Report r) {
    switch (r) {
    case Report::Off:
        return "off";
    case Report::ColorTest:
        return "off while the color brightness or contrast test is on";
    case Report::Waiting:
        return "waiting for RR";
    case Report::Blocked:
        return "off: not enough GPU memory (change a dropout setting to retry)";
    case Report::Unavailable:
        return "unavailable (see ReShade.log)";
    case Report::NoSource:
        return "on, no glass capture this frame (no glass in view, or the transparency albedo blend is off)";
    case Report::NoRefraction:
        return "on, no refraction captured this frame";
    case Report::Active:
        return "active (refraction captured; the blend's submission check still applies)";
    }
    return "off";
}
} // namespace rrdropout
