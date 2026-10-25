#pragma once
#include <atomic>
#include <cmath>
#include "rr_responsivity.h"
namespace rrskyedge {
// Development test: a steadier RR responsivity mask on pixels where geometry
// meets sky (shaders/rr_sky_edge_mask.hlsl). Session-only; while it is in use
// it replaces the SSS responsivity mask, since RR takes a single mask.
inline constexpr float DefaultStrength = -0.5f;
inline constexpr unsigned DefaultRadius = 2, MaxRadius = 4;
inline std::atomic<bool> enabled{};
inline std::atomic<float> strength{DefaultStrength};
inline std::atomic<unsigned> radius{DefaultRadius};
// Bumped by the panel on every change; a test blocked for GPU memory retries
// once this differs from the count it was blocked at.
inline std::atomic<unsigned> changes{};
inline unsigned ClampRadius(unsigned value) {
    return value < 1 ? 1 : value > MaxRadius ? MaxRadius : value;
}
// Same preset rule as the SSS mask: NVIDIA supports the mask on preset F only.
inline bool Use(bool on, float value, bool preset_known, unsigned preset) {
    return on && rrresponsivity::Clamp(value) != 0 && preset_known &&
           preset == rrresponsivity::SupportedPreset;
}
} // namespace rrskyedge
