#include "../src/rr_refraction_dropout.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

// CPU mirror checks for the refraction dropout test. No GPU is used.
namespace {
using namespace rrdropout;
// The shader's threshold tile.
constexpr uint32_t BlueNoise[Ranks / 2] = {
#include "../shaders/rr_blue_noise.inc"
};
uint32_t Rank(uint32_t x, uint32_t y) {
    const uint32_t i = (y & TileMask) * Tile + (x & TileMask);
    return (BlueNoise[i >> 1] >> ((i & 1) * 16)) & 0xffff;
}
// The rank the shader uses for render pixel (x, y) with this evaluation's offset.
uint32_t PixelRank(uint32_t x, uint32_t y, uint32_t offset) {
    return Rank(x + (offset & 0xffff), y + (offset >> 16));
}
bool Near(float a, float b, float tolerance = 1e-6f) {
    return std::fabs(a - b) <= tolerance;
}

void Settings() {
    assert(ClampStrength(std::nanf("")) == 0 && ClampStrength(INFINITY) == 0);
    assert(ClampStrength(-1) == 0 && ClampStrength(2) == MaxStrength);
    assert(ClampStrength(0.3f) == 0.3f && ClampStrength(DefaultStrength) == DefaultStrength);
    assert(MaxStrength < 1 && Near(1 / (1 - MaxStrength), 8));
    // Off by default.
    assert(!Read().enabled && !Read().before_transparency && !Read().show_mask);
    assert(Read().strength == DefaultStrength);
    strength = 5.0f;
    assert(Read().strength == MaxStrength);
    strength = DefaultStrength;
}

void Selection() {
    Tuning on{true, 0.5f, false, false}, off{};
    assert(Wanted(on, false) && !Wanted(on, true) && !Wanted(off, false));
    // Coverage only from a target cleared and written this frame with refraction alpha.
    assert(CoverageReady(true, true, true, 1));
    assert(!CoverageReady(false, true, true, 1) && !CoverageReady(true, false, true, 1) &&
           !CoverageReady(true, true, false, 1) && !CoverageReady(true, true, true, 0));
    Tuning before = on;
    before.before_transparency = true;
    assert(DropBeforeTransparency(before, true, true));
    assert(!DropBeforeTransparency(on, true, true));      // option off
    assert(!DropBeforeTransparency(before, false, true)); // RR does not get it this frame
    assert(!DropBeforeTransparency(before, true, false)); // no typed UAV load
    Tuning mask = before;
    mask.show_mask = true;
    assert(!DropBeforeTransparency(mask, true, true)); // the mask view leaves it alone
}

void MakeConstants() {
    Tuning t{true, 0.7f, true, false};
    auto c = Make(t, 1920, 1088, 3, 5, 42, true, true);
    assert(c.width == 1920 && c.height == 1088 && c.color_x == 3 && c.color_y == 5);
    assert(c.offset == Offset(42) && c.flags == (CoverageFlag | BeforeTransparencyFlag));
    assert(c.strength == 0.7f && c.reserved == 0);
    t.strength = 3;
    t.show_mask = true;
    c = Make(t, 8, 8, 0, 0, 0, false, false);
    assert(c.flags == MaskFlag && c.strength == MaxStrength);
    static_assert(ConstantWords == 8);
}

void Offsets() {
    std::set<uint32_t> seen;
    std::vector<unsigned> xs(Tile), ys(Tile);
    uint32_t previous = Offset(0);
    unsigned repeats = 0;
    for (uint32_t serial = 1; serial <= 16384; ++serial) {
        const uint32_t o = Offset(serial);
        assert((o & 0xffff) < Tile && (o >> 16) < Tile);
        seen.insert(o);
        ++xs[o & 0xffff];
        ++ys[o >> 16];
        repeats += o == previous;
        previous = o;
    }
    // A shift is close to uniform over the tile and rarely repeats back to back.
    assert(seen.size() > 9000 && repeats < 8);
    for (unsigned i = 0; i < Tile; ++i)
        assert(xs[i] > 64 && xs[i] < 192 && ys[i] > 64 && ys[i] < 192);
}

void Table() {
    std::vector<bool> used(Ranks);
    for (uint32_t y = 0; y < Tile; ++y)
        for (uint32_t x = 0; x < Tile; ++x) {
            const uint32_t r = Rank(x, y);
            assert(r < Ranks && !used[r]);
            used[r] = true;
        }
    // Blue noise: at 50%, neighbours differ more often than white noise's 0.5
    // and 8x8 blocks hold close to 32 survivors (white noise: std-dev 4).
    unsigned differ = 0, pairs = 0;
    double variance = 0;
    for (uint32_t y = 0; y < Tile; ++y)
        for (uint32_t x = 0; x < Tile; ++x) {
            const bool a = Rank(x, y) < Ranks / 2;
            differ += a != (Rank(x + 1, y) < Ranks / 2);
            differ += a != (Rank(x, y + 1) < Ranks / 2);
            pairs += 2;
        }
    for (uint32_t by = 0; by < Tile; by += 8)
        for (uint32_t bx = 0; bx < Tile; bx += 8) {
            int n = 0;
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x)
                    n += Rank(bx + x, by + y) < Ranks / 2;
            variance += (n - 32.0) * (n - 32.0);
        }
    variance /= (Tile / 8) * (Tile / 8);
    assert(double(differ) / pairs > 0.6 && std::sqrt(variance) < 2);
}

void Math() {
    // Opacity mirrors 1 - saturate(alpha).
    assert(Opacity(1) == 0 && Opacity(0) == 1 && Opacity(2) == 0 && Opacity(-1) == 1);
    assert(Near(Opacity(1 / 3.0f), 2 / 3.0f) && Opacity(std::nanf("")) == 1);
    // No coverage: the color passes through bit for bit.
    for (uint32_t r : {0u, 100u, Ranks - 1})
        assert(Scale(0, r) == 1 && Scale(std::nanf(""), r) == 1 && Scale(-0.5f, r) == 1);
    assert(Probability(0, 0.5f) == 0 && Probability(1, 0.5f) == 0.5f &&
           Probability(1, 4) == MaxStrength && Probability(0.5f, 0.5f) == 0.25f);
    // Kept pixels are divided by 1 - p, dropped ones are 0.
    assert(Near(Scale(0.5f, Ranks - 1), 2) && Scale(0.5f, 0) == 0);
    // Unbiased: over the tile, the mean factor is 1 within the rank quantization.
    for (float p : {0.01f, 0.1f, 0.25f, 1 / 3.0f, 0.5f, 0.75f, MaxStrength}) {
        double sum = 0;
        for (uint32_t r = 0; r < Ranks; ++r)
            sum += Scale(p, r);
        const double mean = sum / Ranks;
        assert(std::fabs(mean - 1) < 1.0 / (Ranks * (1 - p)) + 1e-6);
    }
    // The mask view: identity without coverage, red at the pixel's luminance with.
    const Rgb c{0.2f, 0.4f, 0.8f};
    const auto same = MaskView(c, 0);
    assert(same.r == c.r && same.g == c.g && same.b == c.b);
    const auto red = MaskView(c, 1);
    assert(Near(red.r, 2 * (0.2126f * 0.2f + 0.7152f * 0.4f + 0.0722f * 0.8f)) && red.g == 0 &&
           red.b == 0);
}

void Filter() {
    const int w = 6, h = 5;
    auto all = [](int, int) { return 1.0f; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            assert(Near(Filtered(x, y, w, h, all), 1)); // corners renormalize
    // One covered pixel at (2, 2): the tent's 4/16, 2/16 and 1/16 around it.
    auto one = [](int x, int y) { return x == 2 && y == 2 ? 1.0f : 0.0f; };
    assert(Near(Filtered(2, 2, w, h, one), 4 / 16.0f));
    assert(Near(Filtered(3, 2, w, h, one), 2 / 16.0f) && Near(Filtered(2, 1, w, h, one), 2 / 16.0f));
    assert(Near(Filtered(1, 1, w, h, one), 1 / 16.0f) && Filtered(4, 2, w, h, one) == 0);
    // At a corner only the in-bounds taps count: weights 4, 2, 2, 1.
    auto corner = [](int x, int y) { return x == 0 && y == 0 ? 1.0f : 0.0f; };
    assert(Near(Filtered(0, 0, w, h, corner), 4 / 9.0f));
    // Never reads outside the image.
    Filtered(0, 0, 1, 1, [&](int x, int y) {
        assert(x == 0 && y == 0);
        return 1.0f;
    });
}

// The audit's measurement: in the window, rows (2k+1, 2k+2) are copies. At
// strength 0.5 blue noise keeps exactly one of such a pair more often than
// independent coin flips (0.5), every frame picks new pixels, and a pixel's
// survival in one frame says little about the next.
void Pairs() {
    const float p = 0.5f;
    unsigned one = 0, pairs = 0;
    for (uint32_t y = 1; y + 1 < 256; y += 2)
        for (uint32_t x = 0; x < 256; ++x) {
            const bool a = Scale(p, PixelRank(x, y, Offset(7))) > 0;
            const bool b = Scale(p, PixelRank(x, y + 1, Offset(7))) > 0;
            one += a != b;
            ++pairs;
        }
    assert(double(one) / pairs > 0.6);
    const uint32_t frames = 2000;
    unsigned kept = 0, both = 0, previous_kept = 0;
    bool previous = false;
    for (uint32_t f = 0; f < frames; ++f) {
        const bool k = Scale(p, PixelRank(10, 20, Offset(f))) > 0;
        kept += k;
        if (f) {
            both += k && previous;
            previous_kept += previous;
        }
        previous = k;
    }
    const double rate = double(kept) / frames;
    assert(rate > 0.45 && rate < 0.55);
    // P(kept | kept last frame) close to the unconditional rate.
    assert(std::fabs(double(both) / previous_kept - rate) < 0.06);
}

void Labels() {
    for (auto r : {Report::Off, Report::ColorTest, Report::Waiting, Report::Blocked,
                   Report::Unavailable, Report::NoRefraction, Report::Active})
        assert(Label(r) && *Label(r));
}
} // namespace

int main() {
    Settings();
    Selection();
    MakeConstants();
    Offsets();
    Table();
    Math();
    Filter();
    Pairs();
    Labels();
    std::puts("Refraction dropout CPU tests passed.");
    return 0;
}
