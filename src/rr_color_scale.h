#pragma once
#include <atomic>
namespace rrcolorscale {
// Development test: RR's color input brighter or darker by a power of two, with
// RR's output scaled back (shaders/rr_color_scale.hlsl). RR ignores exposure,
// unlike DLSS SR, so this shows whether the input brightness level matters.
enum Mode : unsigned { Off, Quarter, Sixteenth, Four, Sixteen, Count };
inline constexpr const char *labels[Count] = {"Off", "1/4 (darker)", "1/16 (darker)",
                                              "4x (brighter)", "16x (brighter)"};
inline constexpr float factors[Count] = {1.0f, 0.25f, 0.0625f, 4.0f, 16.0f};
inline constexpr unsigned Valid(unsigned mode) {
    return mode < Count ? mode : Off;
}
inline constexpr float Factor(unsigned mode) {
    return factors[Valid(mode)];
}
inline std::atomic<unsigned> mode{};
// Bumped by the panel on every change (retries a test blocked for GPU memory).
inline std::atomic<unsigned> changes{};
} // namespace rrcolorscale
