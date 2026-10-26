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
    assert(Map(game, m));
    assert(m.scale == 1 && m.offset == 0 && m.sky == FarScale);
    // RR keeps hardware depth: world depth is the game's, bit for bit, and sky stays 0.
    for (float d : {Slice, 0.5f, 0.1f, 1e-3f, 1e-5f, 2e-6f, 1e-7f})
        assert(Hardware(d, m) == d);
    assert(Hardware(0, m) == 0 && Hardware(-0.0f, m) == 0 &&
           Hardware(std::numeric_limits<float>::quiet_NaN(), m) == 0);
    // The weapon slice lies past the near plane (hardware depth above 1), in its own order.
    assert(Hardware(std::nextafter(Slice, 1.0f), m) > 1 && Near(Hardware(1, m), 2049, 1e-6));
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
    assert(Map(close, c) && c.scale == 0.1f && c.offset == 0);
    assert(Near(Linear(0.5f, c), 0.2, 1e-6) && Near(Linear(1, c), 0.1 / 2049, 1e-6));
    // Column-vector projections are the transpose (jitter moves to m2 and m6).
    Matrix column{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            column[col * 4 + row] = close[row * 4 + col];
    assert(Detect(column) == Layout::ColumnVector);
    assert(Map(column, c) && c.scale == 0.1f && c.offset == 0);
    assert(Linear(0.5f, c) == Linear(0.5f, Mapping{0.1f, 0, 0.1f * FarScale}));
    Matrix column_jitter = column;
    column_jitter[8] = 0.01f; // A planar term in the column layout: refused.
    assert(!Map(column_jitter, c));
}

static void Unsupported() {
    using namespace rrweapondepth;
    Mapping m;
    const Matrix game = Infinite(1);
    Matrix finite = game;
    finite[10] = -0.001f;
    assert(!Map(finite, m)); // The game's own projection must be infinite.
    Matrix ortho = game;
    ortho[15] = 1;
    assert(!Map(ortho, m));
    Matrix oblique = game;
    oblique[2] = 0.01f; // Depth that varies across the screen is not near / z.
    assert(!Map(oblique, m));
    Matrix skewed = game;
    skewed[7] = 0.01f;
    assert(!Map(skewed, m));
}

static void Choice() {
    // AfterSR's depth for a frame: RR's depth type is fixed at creation, so a frame follows
    // the RR it goes to, and only a new RR (feature false) takes the setting.
    using namespace rrweapondepth;
    auto same = [](FrameDepth a, FrameDepth b) {
        return a.linear == b.linear && a.restart == b.restart && a.hold == b.hold;
    };
    const FrameDepth hardware{}, linear{true};
    for (bool wanted : {false, true}) {
        // RR with hardware depth: hardware depth, whatever the setting and the frame.
        for (bool mapped : {false, true})
            for (bool texture : {false, true})
                for (bool blocked : {false, true})
                    assert(same(Choose(true, false, wanted, mapped, texture, blocked), hardware));
        // RR with linear depth: linear depth when the frame has it, else restart RR, and hold
        // linear depth off when the cause would stay.
        assert(same(Choose(true, true, wanted, true, true, false), linear));
        assert(same(Choose(true, true, wanted, false, false, false), {true, true, Hold::Projection}));
        assert(same(Choose(true, true, wanted, true, false, true), {true, true, Hold::Memory}));
        // A texture released for another size: restart, nothing held.
        assert(same(Choose(true, true, wanted, true, false, false), {true, true, Hold::None}));
    }
    // A new RR: the setting, unless the frame cannot have linear depth.
    assert(same(Choose(false, false, false, false, false, false), hardware));
    assert(same(Choose(false, true, false, true, true, false), hardware));
    assert(same(Choose(false, false, true, true, true, false), linear));
    assert(same(Choose(false, false, true, false, false, false), {false, false, Hold::Projection}));
    assert(same(Choose(false, false, true, true, false, true), {false, false, Hold::Memory}));
    assert(same(Choose(false, false, true, true, false, false), hardware));
    // Every case: linear depth is sent only with a mapping and its texture, a restart only for
    // an RR with linear depth, and a hold names its cause.
    for (unsigned bits = 0; bits < 64; ++bits) {
        const bool b[] = {bool(bits & 1), bool(bits & 2), bool(bits & 4),
                          bool(bits & 8), bool(bits & 16), bool(bits & 32)};
        const auto f = Choose(b[0], b[1], b[2], b[3], b[4], b[5]);
        const bool sent = f.linear && !f.restart;
        assert(!sent || (b[3] && b[4]));
        assert(f.linear == (b[0] ? b[1] : b[2] && b[3] && b[4]) || f.restart);
        assert(!f.restart || (b[0] && b[1] && !(b[3] && b[4])));
        assert(f.hold != Hold::Projection || !b[3]);
        assert(f.hold != Hold::Memory || (b[3] && b[5] && !b[4]));
        assert(f.hold != Hold::Refused);
        assert(f.hold == Hold::None || (Wants(b[0], b[1], b[2]) && !(b[3] && b[4])));
    }
}

static void Creation() {
    // RR is always created with hardware depth and the game's flags (AutoExposure aside):
    // the DLSS5 NR add-on declines RR frames created with linear depth.
    rreval::Options v;
    v.width = 1280;
    v.height = 720;
    v.out_width = 2560;
    v.out_height = 1440;
    v.quality = 1;
    v.flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    RecordingParams hw;
    rreval::CreateParameters(hw, v);
    int flags{};
    unsigned depth{};
    assert(NVSDK_NGX_SUCCEED(hw.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)) &&
           flags == v.flags);
    assert(NVSDK_NGX_SUCCEED(hw.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth)) &&
           depth == NVSDK_NGX_DLSS_Depth_Type_HW);
    // CreateFlags is what RR is created with, and what the "RR created" line reports: v.flags
    // is IsHDR | MVLowRes | DepthInverted (11). The game's AutoExposure (64) is never sent.
    assert(rreval::CreateFlags(v.flags) == 11);
    assert(rreval::CreateFlags(v.flags | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) == 11);
}

int main() {
    // Off until the settings are applied (rr_settings.inl ApplyEnabled).
    assert(!rrweapondepth::enabled && !rrweapondepth::changes);
    GameProjection();
    Unsupported();
    Choice();
    Creation();
    std::puts("weapon depth tests passed");
}
