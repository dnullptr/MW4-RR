#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
namespace rrwater {
// Development tests for running water, which RR blurs. Water pixels are the
// game prepass's water marker: PackedSurface.w bit 2 (value 4) without the
// depth-override bit 3 (value 8). The guide shader classifies them, and
// shaders/rr_water_masks.hlsl turns that into RR's optional masks.
enum Guide : unsigned { Mirror, MirrorSmooth, Material, Physical, GuideCount };
// Order matches the WaterGuide values in shaders/rr_convert_guides.hlsl.
inline constexpr const char *guide_labels[GuideCount] = {
    "Game mirror (today)", "Game mirror, smoothed normal", "Water material (F0 and diffuse)",
    "Physical water constants, smoothed normal"};
inline constexpr unsigned ValidGuide(unsigned guide) {
    return guide < GuideCount ? guide : Mirror;
}
// Positive responsivity makes RR lean on the current frame; NVIDIA's nominal
// range is -1..1, so water uses 0..1.
inline constexpr float DefaultStrength = 1.0f;
inline float Clamp(float strength) {
    if (!std::isfinite(strength))
        return 0;
    return strength < 0 ? 0.0f : strength > 1 ? 1.0f : strength;
}
// Session-only panel state: a test mode never sticks across restarts.
struct Settings {
    bool responsivity{}, discard{}, animated{};
    float strength = DefaultStrength;
    unsigned guide = Mirror;
};
// Any RR mask that needs the water classification at SR time.
inline constexpr bool Masks(const Settings &s) {
    return s.responsivity || s.discard || s.animated;
}
// The classification is also kept for guide modes, so water coverage is logged.
inline constexpr bool Wanted(const Settings &s) {
    return Masks(s) || ValidGuide(s.guide) != Mirror;
}
// Like the SSS mask, the responsivity mask is supported on preset F (6) only.
inline bool UseResponsivity(const Settings &s, bool water_used, bool preset_known,
                            unsigned preset) {
    return s.responsivity && Clamp(s.strength) != 0 && water_used && preset_known && preset == 6;
}
// Flags read by shaders/rr_water_masks.hlsl (header word 15).
inline constexpr unsigned FlagResponsivity = 1, FlagDiscard = 2, FlagSssMask = 4;
inline constexpr unsigned Flags(bool responsivity, bool discard, bool sss_mask) {
    return (responsivity ? FlagResponsivity : 0u) | (discard ? FlagDiscard : 0u) |
           (sss_mask ? FlagSssMask : 0u);
}
// What the latest evaluation sent, for the panel.
struct Status {
    bool wanted{}, ready{}, unavailable{}, waiting_memory{}, responsivity{}, discard{}, animated{};
    float coverage = -1; // share of the latest frame's pixels classified as water, 0..1
};
// CPU-only coverage summary, logged periodically so water detection can be
// checked in ReShade.log without a capture.
class Tracker {
    unsigned frames{}, empty{};
    double share_sum{}, share_max{}, last = -1;
    std::uint64_t since{};
    bool started{};

  public:
    static constexpr std::uint64_t Interval = 10000;
    void Add(std::uint32_t water_pixels, std::uint64_t pixels) {
        if (!pixels)
            return;
        const double share = double(water_pixels) / double(pixels);
        ++frames;
        empty += water_pixels == 0;
        share_sum += share;
        if (share > share_max)
            share_max = share;
        last = share;
    }
    double Last() const {
        return last;
    }
    bool Report(std::uint64_t now, std::string &out, const Settings &s) {
        if (!started) {
            started = true;
            since = now;
            return false;
        }
        if (now < since || now - since < Interval)
            return false;
        const bool active = frames != 0;
        if (active) {
            char text[320];
            std::snprintf(text, sizeof(text),
                          "Water (%llus): frames=%u waterPixels avg=%.1f%% max=%.1f%% "
                          "framesWithoutWater=%u responsivity=%s(%.2f) discard=%s "
                          "animatedMask=%s guides=%u",
                          static_cast<unsigned long long>((now - since) / 1000), frames,
                          100.0 * share_sum / frames, 100.0 * share_max, empty,
                          s.responsivity ? "on" : "off", Clamp(s.strength),
                          s.discard ? "on" : "off", s.animated ? "on" : "off",
                          ValidGuide(s.guide));
            out = text;
        }
        const double keep = last;
        *this = {};
        last = keep;
        started = true;
        since = now;
        return active;
    }
};
} // namespace rrwater
