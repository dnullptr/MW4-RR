#pragma once
#include <cstdint>
namespace rrsettings {
struct Preferences {
    bool rr = true;
    bool sss = true;
    bool glass = true;
    bool hit = true;
    unsigned preset = 6;
    // SSS noise experiments.
    bool tolerate_isolated = true; // count isolated bad guide pixels, keep the frame
    unsigned native_lobe = 0;      // rrlighting::Source, 0 = raw lighting
    bool responsivity = false;            // RR responsivity mask on SSS pixels (preset F)
    float responsivity_strength = -0.5f;  // -1..0, negative = steadier
};
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
