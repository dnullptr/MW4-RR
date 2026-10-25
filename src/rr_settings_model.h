#pragma once
#include <cstdint>
namespace rrsettings {
struct Preferences {
    bool rr = true;
    bool sss = true;
    bool glass = true;
    bool blend = true;
    bool hit = true;
    bool weapon_depth = true; // linear depth for RR, the weapon kept in front (rr_weapon_depth.h)
    unsigned preset = 6;
    // SSS noise experiments.
    bool tolerate_isolated = true; // count isolated bad guide pixels, keep the frame
    unsigned native_lobe = 17;     // rrlighting::Source: SssHistory2 (0 = raw lighting)
    bool responsivity = false;            // RR responsivity mask on SSS pixels (preset F)
    float responsivity_strength = -0.5f;  // -1..0, negative = steadier
};
// NVIDIA's development RR DLL binds Ctrl+Alt+F9/F11, so modified presses must not toggle guides.
inline constexpr bool HotkeyToggles(bool pressed, bool was_down, bool ctrl_or_alt, bool focused) {
    return pressed && !was_down && !ctrl_or_alt && focused;
}
inline constexpr const char *QualityLabel(int quality) {
    switch (quality) {
    case 0:
        return "Performance";
    case 1:
        return "Balanced";
    case 2:
        return "Quality";
    case 3:
        return "Ultra Performance";
    case 4:
        return "Ultra Quality";
    case 5:
        return "Native / DLAA";
    default:
        return "Not observed yet";
    }
}
} // namespace rrsettings
