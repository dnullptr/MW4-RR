#pragma once
#include <cmath>
namespace rrresponsivity {
// NVIDIA's RR integration guide supports the responsivity mask on preset F only.
// Negative values make RR less responsive (steadier, less boiling) but can ghost.
inline constexpr unsigned SupportedPreset = 6;
inline constexpr float DefaultStrength = -0.5f;
inline float Clamp(float strength) {
    if (!std::isfinite(strength))
        return 0;
    return strength < -1 ? -1.0f : strength > 0 ? 0.0f : strength;
}
// Send the mask only when this frame's SSS resolve wrote it and RR can use it.
inline bool Use(bool enabled, float strength, bool sss_used, bool preset_known, unsigned preset) {
    return enabled && Clamp(strength) != 0 && sss_used && preset_known && preset == SupportedPreset;
}
} // namespace rrresponsivity
