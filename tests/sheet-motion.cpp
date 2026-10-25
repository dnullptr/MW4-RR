#include "../src/rr_sheet_motion.h"
#include <cassert>
#include <cmath>
#include <cstdio>

// CPU mirror checks for the sheet motion test. No GPU is used.
namespace {
using namespace rrsheet;
bool Near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}
// Frames 2 and 3 of the strafing capture 20261025-181459 (plastic room), as RR got them.
Camera Frame(bool current) {
    Camera c;
    c.view = current ? std::array<float, 16>{-0.6374321579933167f, 0.0304717980325222f, 0.7699041366577148f, 0,
                                            -0.7705068588256836f, -0.025536073371767998f, -0.6369205117225647f, 0,
                                            -0.00025221449322998524f, 0.9992093443870544f, -0.03975621983408928f, 0,
                                            -21089.29296875f, -3674.0791015625f, -16766.29296875f, 1}
                     : std::array<float, 16>{-0.6374349594116211f, 0.03037012182176113f, 0.7699058055877686f, 0,
                                            -0.7705044746398926f, -0.02551218867301941f, -0.6369243264198303f, 0,
                                            -0.00029851324507035315f, 0.9992130398750305f, -0.03966265171766281f, 0,
                                            -21091.3671875f, -3673.493896484375f, -16766.673828125f, 1};
    c.projection = {1, 0, 0, 0, 0, 1.77777779f, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    c.frame = current ? 45255 : 45254;
    c.valid = true;
    return c;
}

void Settings() {
    assert(!Read().enabled);
    assert(Read().distance == DefaultDistance && Read().threshold == DefaultThreshold);
    assert(ClampDistance(std::nanf("")) == DefaultDistance && ClampDistance(1) == MinDistance &&
           ClampDistance(1e9f) == MaxDistance && ClampDistance(300) == 300);
    assert(ClampThreshold(INFINITY) == DefaultThreshold && ClampThreshold(0) == MinThreshold &&
           ClampThreshold(2) == MaxThreshold && ClampThreshold(0.95f) == 0.95f);
    distance = -5.0f;
    assert(Read().distance == MinDistance);
    distance = DefaultDistance;
}

void Previous() {
    const auto previous = Frame(false);
    assert(Consecutive(previous, 45255, 0, false));
    assert(!Consecutive(previous, 45255, 0, true));  // reset
    assert(!Consecutive(previous, 45256, 0, false)); // a frame skipped
    assert(!Consecutive(previous, 45255, 1, false)); // another viewport
    auto invalid = previous;
    invalid.valid = false;
    assert(!Consecutive(invalid, 45255, 0, false));
}

void Matrices() {
    Matrix a{2, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 3, 5, 7, 1}, inverse{};
    assert(Invert(a, inverse));
    const auto identity = Multiply(a, inverse);
    for (unsigned i = 0; i < 16; ++i)
        assert(std::fabs(identity[i] - (i % 5 == 0 ? 1 : 0)) < 1e-12);
    Matrix singular{};
    assert(!Invert(singular, inverse));
    auto p = Frame(true).projection;
    assert(Projectable(p));
    p[4] = 0.5f; // shear
    assert(!Projectable(p));
    p = Frame(true).projection;
    p[11] = 0; // orthographic-style w
    assert(!Projectable(p));
}

// Real data: rebuilding motion from depth and the two cameras reproduces the game's own
// motion on a wall (pixel 1800, 1000 of frame 3: linear depth 78.83962, game motion
// 32.59375, -0.15857 with RR's motion scale -1) and matches the offline rebuild
// (sheet-depth-motion/reproj.py: 32.08898, -0.15973) closely.
void RealCapture() {
    Tuning t{true, 78.83962f, 0.9f};
    Constants c{};
    assert(Make(t, 2224, 1248, 0, 0, -1, -1, Frame(true), Frame(false), true, c));
    assert(c.flags == CoverageFlag);
    float mx = 0, my = 0;
    assert(Motion(c, 1800, 1000, mx, my));
    std::printf("wall pixel: rebuilt %.4f %.4f, game 32.59375 -0.15857\n", mx, my);
    assert(Near(mx, 32.08898f, 0.02f) && Near(my, -0.15973f, 0.01f));
    assert(Near(mx, 32.59375f, 0.6f) && Near(my, -0.15857f, 0.05f));
    // Background behind the sheets at (2000, 300), depth 488.70367: game 5.30469, -0.08807.
    t.distance = 488.70367f;
    assert(Make(t, 2224, 1248, 0, 0, -1, -1, Frame(true), Frame(false), true, c));
    assert(Motion(c, 2000, 300, mx, my));
    std::printf("background pixel: rebuilt %.4f %.4f, game 5.30469 -0.08807\n", mx, my);
    assert(Near(mx, 5.30469f, 0.2f) && Near(my, -0.08807f, 0.05f));
    // The same pixel with a sheet assumed at 200 units moves faster than its background.
    t.distance = 200;
    assert(Make(t, 2224, 1248, 0, 0, -1, -1, Frame(true), Frame(false), true, c));
    float sx = 0, sy = 0;
    assert(Motion(c, 2000, 300, sx, sy));
    assert(sx > mx * 2);
    // The motion scale: RR multiplies by it, so a scale of -1/W stores motion in
    // units of -W pixels.
    assert(Make(t, 2224, 1248, 0, 0, -1.0f / 2224, -1.0f / 1248, Frame(true), Frame(false), true, c));
    float ux = 0, uy = 0;
    assert(Motion(c, 2000, 300, ux, uy));
    assert(Near(ux * (-1.0f / 2224), -sx, 1e-4f) && Near(uy * (-1.0f / 1248), -sy, 1e-4f));
}

void Gating() {
    Tuning t{true, 200, 0.9f};
    Constants c{};
    assert(Make(t, 64, 64, 0, 0, -1, -1, Frame(true), Frame(false), true, c));
    assert(Near(Opacity(0, 0, 0), 1, 0) && Near(Opacity(1, 1, 1), 0, 0));
    assert(Near(Opacity(2, -1, std::nanf("")), 2.0f / 3, 1e-6f)); // saturate, NaN -> 0
    assert(Replaced(c, 0.95f, 400));
    assert(!Replaced(c, 0.85f, 400));  // below the threshold
    assert(!Replaced(c, 0.95f, 150));  // the background is nearer than the sheet
    assert(!Replaced(c, 0.95f, 0.04f)); // the first-person weapon
    // Without coverage nothing is replaced.
    assert(Make(t, 64, 64, 0, 0, -1, -1, Frame(true), Frame(false), false, c));
    assert(!c.flags && !Replaced(c, 1, 400));
    // Unusable cameras or motion scale: the frame passes through.
    auto bad = Frame(true);
    bad.projection[4] = 1;
    assert(!Make(t, 64, 64, 0, 0, -1, -1, bad, Frame(false), true, c) && !c.flags);
    assert(!Make(t, 64, 64, 0, 0, 0, -1, Frame(true), Frame(false), true, c) && !c.flags);
    // A point behind the previous camera keeps the game's motion.
    Camera behind = Frame(false);
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            behind.view[i * 4 + j] = Frame(true).view[i * 4 + j] * (j == 2 ? -1.0f : 1.0f);
    assert(Make(t, 64, 64, 0, 0, -1, -1, Frame(true), behind, true, c));
    float mx, my;
    assert(!Motion(c, 32, 32, mx, my));
}

void Labels() {
    for (auto r : {Report::Off, Report::Waiting, Report::Blocked, Report::Unavailable,
                   Report::NoLinearDepth, Report::NoCamera, Report::NoSource, Report::Active})
        assert(Label(r) && *Label(r));
}
} // namespace

int main() {
    Settings();
    Previous();
    Matrices();
    RealCapture();
    Gating();
    Labels();
    std::puts("sheet-motion: OK");
    return 0;
}
