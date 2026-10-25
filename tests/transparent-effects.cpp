#include "../src/rr_transparent_effects.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

// CPU mirror checks for the transparent effects responsivity test. No GPU is used.
namespace {
using namespace rrfx;
bool Near(float a, float b, float tolerance = 1e-6f) {
    return std::fabs(a - b) <= tolerance;
}
void Settings() {
    assert(!Read().enabled && Read().strength == DefaultStrength); // off by default
    assert(ClampStrength(std::nanf("")) == 0 && ClampStrength(-1) == 0 && ClampStrength(3) == 1);
    strength = 7.0f;
    assert(Read().strength == 1);
    strength = DefaultStrength;
}
void Holds() {
    const Tuning on{true, 1}, off{}, zero{true, 0};
    assert(Check(on, false, true, SupportedPreset, true, false) == Hold::None);
    assert(Check(off, false, true, SupportedPreset, true, false) == Hold::Off);
    assert(Check(zero, false, true, SupportedPreset, true, false) == Hold::Off);
    assert(Check(on, true, true, SupportedPreset, true, false) == Hold::SkyEdge);
    assert(Check(on, false, false, SupportedPreset, true, false) == Hold::Preset);
    assert(Check(on, false, true, 4, true, false) == Hold::Preset);
    assert(Check(on, false, true, SupportedPreset, false, false) == Hold::NoTransparency);
    assert(Check(on, false, true, SupportedPreset, true, true) == Hold::ColorTest);
    for (auto h : {Hold::Off, Hold::SkyEdge, Hold::Preset, Hold::NoTransparency, Hold::ColorTest})
        assert(ReportOf(h) != Report::Active && *Label(ReportOf(h)));
    for (auto r : {Report::Off, Report::SkyEdge, Report::Preset, Report::NoTransparency,
                   Report::ColorTest, Report::Waiting, Report::Blocked, Report::Unavailable,
                   Report::Active})
        assert(Label(r) && *Label(r));
}
void MakeConstants() {
    const auto c = Make({true, 0.6f}, 1920, 1080, 4, 2, true);
    assert(c.width == 1920 && c.height == 1080 && c.color_x == 4 && c.color_y == 2);
    assert(c.flags == ExistingFlag && c.strength == 0.6f && c.low == Low && c.high == High);
    assert(Make({true, 9}, 8, 8, 0, 0, false).flags == 0 && Make({true, 9}, 8, 8, 0, 0, false).strength == 1);
    static_assert(ConstantWords == 8);
}
void MaskValues() {
    // No transparent layer: color equals the color before transparency.
    assert(Mask(0.3f, 0.3f, 1) == 0);
    // Glass that only dims the scene adds no light: no mask, whatever the dimming.
    assert(Mask(0.1f, 0.3f, 1) == 0 && Mask(0.0f, 1.0f, 1) == 0);
    // Smoke over a dark room (the capture: about 7x brighter) gets the full strength.
    assert(Near(Mask(0.7f, 0.1f, 1), 1) && Near(Mask(0.7f, 0.1f, 0.4f), 0.4f));
    // The ramp: added share 0.1 -> 0, 0.3 -> half, 0.5 -> full.
    auto pair = [](float added) { return Mask(1 + added, 1 - added, 1); };
    assert(Near(pair(0.1f), 0, 1e-5f) && Near(pair(0.3f), 0.5f, 1e-5f) && Near(pair(0.5f), 1, 1e-5f));
    assert(pair(0.05f) == 0 && pair(0.9f) == 1);
    // Dark noise below the floor is ignored.
    assert(Mask(4e-4f, 1e-4f, 1) == 0);
    // The mask RR would otherwise get is kept where it is larger, including the
    // SSS mask's negative values where no layer adds light.
    assert(Mask(0.3f, 0.3f, 1, 0.8f) == 0.8f && Mask(0.3f, 0.3f, 1, -0.5f) == -0.5f);
    assert(Near(Mask(0.7f, 0.1f, 1, 0.8f), 1));
    // Non-finite input never produces a mask.
    assert(Mask(std::nanf(""), 0.1f, 1) == 0 && Luma(1, 1, 1) > 0.999f);
}
} // namespace

int main() {
    Settings();
    Holds();
    MakeConstants();
    MaskValues();
    std::puts("Transparent effects CPU tests passed.");
    return 0;
}
