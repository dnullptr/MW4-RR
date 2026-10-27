#pragma once
#include <cmath>
namespace rrwater {
// Water responsivity mask: running water, which RR blurs. Water pixels are the
// game prepass's special surfaces, PackedSurface.w bit 1 (value 2): its water
// flag (value 4) or its class override (value 8). The guide shader classifies
// them, and shaders/rr_water_masks.hlsl turns that into RR's responsivity mask.
// Positive responsivity makes RR lean on the current frame; NVIDIA's nominal
// range is -1..1, so water uses 0..1.
inline constexpr float DefaultStrength = 1.0f;
inline float Clamp(float strength) {
    if (!std::isfinite(strength))
        return 0;
    return strength < 0 ? 0.0f : strength > 1 ? 1.0f : strength;
}
// The responsivity mask is on by default whenever RR runs preset F.
struct Settings {
    bool responsivity = true;
    float strength = DefaultStrength;
};
// The classification is wanted only on preset F (preset_f), the one preset that
// reads the mask.
inline constexpr bool Wanted(const Settings &s, bool preset_f) {
    return s.responsivity && preset_f;
}
// Like the SSS mask, the responsivity mask is supported on preset F (6) only.
inline bool UseResponsivity(const Settings &s, bool water_used, bool preset_known,
                            unsigned preset) {
    return s.responsivity && Clamp(s.strength) != 0 && water_used && preset_known && preset == 6;
}
// Flags read by shaders/rr_water_masks.hlsl (header word 15).
inline constexpr unsigned FlagResponsivity = 1;
inline constexpr unsigned Flags(bool responsivity) {
    return responsivity ? FlagResponsivity : 0u;
}
// What the latest evaluation sent, for the panel.
struct Status {
    bool wanted{}, ready{}, unavailable{}, waiting_memory{}, responsivity{};
};
} // namespace rrwater
