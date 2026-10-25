#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include "../src/rr_weapon_depth.h"
#include "../src/rr_evaluation_params.h"
#include "ngx-recording-params.h"
#include <cassert>
#include <cstdio>
#include <limits>

using Matrix = std::array<float, 16>;
// The game's projection: infinite reverse-Z, row-vector (pos * M).
static Matrix Infinite(float near_plane) {
    Matrix m{};
    m[0] = 0.75f;
    m[5] = 1.3333f;
    m[8] = 0.0005f; // Jitter offsets stay in the x/y columns.
    m[9] = -0.0005f;
    m[11] = 1;
    m[14] = near_plane;
    return m;
}
static bool Near(double a, double b, double relative) {
    return std::abs(a - b) <= relative * std::abs(b);
}
// Weapon slice: t is the weapon camera's own reverse-Z depth.
static float Weapon(float t) {
    return rrweapondepth::Slice + (1 - rrweapondepth::Slice) * t;
}

static void GameProjection() {
    using namespace rrweapondepth;
    const Matrix game = Infinite(1);
    Mapping m;
    assert(Map(game, game, 0, m));
    assert(m.scale == 1 && m.offset == 0 && m.sky == rrskydepth::FarScale);
    // World depth is exactly near / depth, what RR's projection says.
    for (float d : {Slice, 0.5f, 0.1f, 1e-3f, 1e-5f, 2e-6f})
        assert(Linear(d, m) == 1 / d);
    // Sky, and anything farther, is the far end; nothing is infinite or NaN.
    assert(Linear(0, m) == m.sky && Linear(1e-7f, m) == m.sky);
    assert(Linear(-0.0f, m) == m.sky && Linear(std::numeric_limits<float>::quiet_NaN(), m) == m.sky);
    // The weapon is in front of the near plane and of every world depth.
    const float world_nearest = Linear(Slice, m);
    assert(Near(world_nearest, 64.0 / 63, 1e-6));
    float previous = 1;
    // t = 1e-6 rounds to Slice itself: world depth, as the game's own split has it.
    assert(Linear(Weapon(1e-6f), m) == world_nearest);
    for (float t : {1e-5f, 1e-4f, 0.001f, 0.004f, 0.01f, 0.03f, 0.25f, 0.5f, 1.0f}) {
        const float z = Linear(Weapon(t), m);
        assert(z > 0 && z < previous && z < world_nearest); // Nearer weapon depth, smaller z.
        previous = z;
    }
    assert(Near(Linear(1, m), 1.0 / 2049, 1e-6));
    assert(Linear(std::nextafter(Slice, 1.0f), m) < 1);
    // Away from the knee the weapon keeps its own depth ratios (t = near / z).
    assert(Near(Linear(Weapon(0.25f), m) / Linear(Weapon(0.5f), m), 2, 0.002));
    // Other near planes scale every depth.
    const Matrix close = Infinite(0.1f);
    Mapping c;
    assert(Map(close, close, 0, c) && c.scale == 0.1f && c.offset == 0);
    assert(Near(Linear(0.5f, c), 0.2, 1e-6) && Near(Linear(1, c), 0.1 / 2049, 1e-6));
    // Column-vector projections are the transpose (jitter moves to m2 and m6).
    Matrix column{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            column[col * 4 + row] = close[row * 4 + col];
    assert(rrskydepth::Detect(column) == rrskydepth::Layout::ColumnVector);
    assert(Map(column, column, 0, c) && c.scale == 0.1f && c.offset == 0);
    assert(Linear(0.5f, c) == Linear(0.5f, Mapping{0.1f, 0, 0.1f * rrskydepth::FarScale}));
    Matrix column_sky = column;
    assert(rrskydepth::Apply(column_sky) && Map(column, column_sky, 0, c));
    assert(Near(c.sky, 0.1 * rrskydepth::FarScale, 1e-5) && Near(c.scale / (1 + c.offset), 0.1, 1e-6));
    Matrix column_jitter = column;
    column_jitter[8] = 0.01f; // A planar term in the column layout: refused.
    assert(!Map(column_jitter, column_jitter, 0, c));
}

static void SkyDepthTest() {
    // The sky depth test gives RR a finite far plane; linear depth follows that projection
    // (what RR computes from the game's depth with it), and sky is the far plane.
    using namespace rrweapondepth;
    const Matrix game = Infinite(1);
    Matrix passed = game;
    assert(rrskydepth::Apply(passed));
    Mapping m;
    assert(Map(game, passed, 0, m));
    assert(Near(m.sky, rrskydepth::FarScale, 1e-5));
    for (float d : {Slice, 0.5f, 0.01f, 1e-4f, 1e-6f}) {
        const double z = double(passed[14]) / (double(d) - double(passed[10]));
        assert(Near(Linear(d, m), z, 1e-6));
    }
    assert(Linear(0, m) == m.sky);
    // The weapon is still in front of the near plane.
    assert(Near(m.scale / (1 + m.offset), 1, 1e-6) && Linear(Weapon(1e-5f), m) < 1);
}

static void SkySurfaceGuideTest() {
    // The sky guide test lifts sky depth to its floor; sky is then that depth's distance.
    using namespace rrweapondepth;
    const Matrix game = Infinite(1);
    Mapping m;
    assert(Map(game, game, 1e-7f, m));
    assert(Near(m.sky, 1e7, 1e-6) && Linear(0, m) == m.sky && Linear(5e-8f, m) == m.sky);
    assert(Linear(0.5f, m) == 2);
    Matrix passed = game;
    rrskydepth::Apply(passed);
    assert(Map(game, passed, 1e-7f, m));
    assert(Near(m.sky, double(passed[14]) / (1e-7 - double(passed[10])), 1e-6));
}

static void Unsupported() {
    using namespace rrweapondepth;
    Mapping m;
    const Matrix game = Infinite(1);
    Matrix finite = game;
    rrskydepth::Apply(finite);
    assert(!Map(finite, finite, 0, m)); // The game's own projection must be infinite.
    Matrix ortho = game;
    ortho[15] = 1;
    assert(!Map(ortho, ortho, 0, m));
    Matrix oblique = game;
    oblique[2] = 0.01f; // Depth that varies across the screen is not near / z.
    assert(!Map(oblique, oblique, 0, m));
    Matrix skewed = game;
    skewed[7] = 0.01f;
    assert(!Map(skewed, skewed, 0, m));
    Matrix other = game;
    other[0] = 0.5f; // RR must be given the game's projection apart from depth.
    assert(!Map(game, other, 0, m));
    assert(!Map(game, game, -1e-7f, m));
    assert(!Map(game, game, std::numeric_limits<float>::infinity(), m));
    assert(!Map(game, game, std::numeric_limits<float>::quiet_NaN(), m));
    Matrix negative = game;
    negative[10] = 0.5f; // Depth offset below zero: not a projection RR could be given.
    assert(!Map(game, negative, 0, m));
}

static void Creation() {
    // RR is told the depth type at creation; linear depth is not inverted, so the game's
    // flag is cleared and every other flag kept.
    rreval::Options v;
    v.width = 1280;
    v.height = 720;
    v.out_width = 2560;
    v.out_height = 1440;
    v.quality = 1;
    v.flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    RecordingParams hw, linear;
    rreval::CreateParameters(hw, v);
    rreval::CreateParameters(linear, v, 6, true);
    int flags{};
    unsigned depth{};
    assert(NVSDK_NGX_SUCCEED(hw.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)) &&
           flags == v.flags);
    assert(NVSDK_NGX_SUCCEED(hw.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth)) &&
           depth == NVSDK_NGX_DLSS_Depth_Type_HW);
    assert(NVSDK_NGX_SUCCEED(linear.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)) &&
           flags == (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    assert(NVSDK_NGX_SUCCEED(linear.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth)) &&
           depth == NVSDK_NGX_DLSS_Depth_Type_Linear);
    // Nothing else changes.
    assert(hw.values.size() == linear.values.size());
    for (const auto &[key, value] : hw.values)
        if (key != NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags &&
            key != NVSDK_NGX_Parameter_Use_HW_Depth)
            assert(linear.values.at(key) == value);
}

int main() {
    assert(!rrweapondepth::enabled && !rrweapondepth::changes); // Off until the panel says so.
    GameProjection();
    SkyDepthTest();
    SkySurfaceGuideTest();
    Unsupported();
    Creation();
    std::puts("weapon depth tests passed");
}
