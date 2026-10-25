#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
namespace rrsheet {
// Development test for near-opaque transparent surfaces (the plastic sheets). The game
// draws them in its transparency (OIT) pass after it has written depth and motion, so
// RR gets the background's depth and motion there and smears the sheets while moving.
// Where the glass capture's coverage says the transparency is at least Threshold
// opaque, this test gives RR the depth and motion of a surface at an assumed view
// depth (Distance, in game units) instead: the motion is rebuilt from this frame's and
// the previous frame's camera (shaders/rr_sheet_motion.hlsl). A quick check of the
// approach before replaying the sheets' real depth. Needs linear depth (weapon depth,
// on by default); RR reads a copy of its motion and the linear depth copy is changed in
// place. Session-only and off by default.
inline constexpr float DefaultDistance = 200.0f, MinDistance = 10.0f, MaxDistance = 5000.0f;
inline constexpr float DefaultThreshold = 0.9f, MinThreshold = 0.5f, MaxThreshold = 1.0f;
inline std::atomic<bool> enabled{};
inline std::atomic<float> distance{DefaultDistance};
inline std::atomic<float> threshold{DefaultThreshold};
// Bumped by the panel on every change; a test blocked for GPU memory or a failed
// setup retries once this differs from the count it stopped at.
inline std::atomic<unsigned> changes{};

inline float Clamp(float value, float low, float high, float fallback) {
    if (!std::isfinite(value))
        return fallback;
    return value < low ? low : value > high ? high : value;
}
inline float ClampDistance(float v) {
    return Clamp(v, MinDistance, MaxDistance, DefaultDistance);
}
inline float ClampThreshold(float v) {
    return Clamp(v, MinThreshold, MaxThreshold, DefaultThreshold);
}
struct Tuning {
    bool enabled{};
    float distance = DefaultDistance, threshold = DefaultThreshold;
};
inline Tuning Read() {
    return {enabled.load(std::memory_order_relaxed),
            ClampDistance(distance.load(std::memory_order_relaxed)),
            ClampThreshold(threshold.load(std::memory_order_relaxed))};
}

// A frame's camera as RR is given it: world-to-view and view-to-clip, both row-vector
// (clip = [x y z 1] * view * projection), the projection infinite reverse-Z with clip
// w = view z.
struct Camera {
    std::array<float, 16> view{}, projection{};
    unsigned frame{}, viewport{};
    bool valid{};
};
// The previous camera is usable when it is the frame just before, on the same
// viewport, and RR's history was not reset.
inline bool Consecutive(const Camera &previous, unsigned frame, unsigned viewport, bool reset) {
    return previous.valid && !reset && previous.viewport == viewport &&
           previous.frame + 1 == frame;
}

inline constexpr uint32_t CoverageFlag = 1;
// The pass's 32 root constants (b0). Pixel (x, y) at view depth z is at view position
// (z * (ndc.x * ax + bx), z * (ndc.y * ay + by), z); its previous clip position is
// [x y z 1] * reproject (rows r0..r3).
struct Constants {
    uint32_t width, height, motion_x, motion_y, flags, pad[3];
    float distance, threshold, inv_scale_x, inv_scale_y, ax, bx, ay, by;
    float reproject[16];
};
static_assert(sizeof(Constants) == 128);
inline constexpr unsigned ConstantWords = sizeof(Constants) / 4;

using Matrix = std::array<double, 16>;
inline Matrix Widen(const std::array<float, 16> &m) {
    Matrix r{};
    for (unsigned i = 0; i < 16; ++i)
        r[i] = m[i];
    return r;
}
inline Matrix Multiply(const Matrix &a, const Matrix &b) {
    Matrix r{};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            double v = 0;
            for (unsigned k = 0; k < 4; ++k)
                v += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = v;
        }
    return r;
}
// General 4x4 inverse (cofactors); false when singular or not finite.
inline bool Invert(const Matrix &m, Matrix &out) {
    Matrix inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::abs(det) < 1e-30)
        return false;
    for (unsigned i = 0; i < 16; ++i) {
        out[i] = inv[i] / det;
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}
// The current projection must map view (x, y) to clip (x, y) without shear and give
// clip w = view z: then ndc = (x * p0 + z * p8) / z, (y * p5 + z * p9) / z.
inline bool Projectable(const std::array<float, 16> &p) {
    for (unsigned i : {1u, 3u, 4u, 7u, 12u, 13u, 15u})
        if (std::abs(p[i]) > 1e-6f)
            return false;
    return p[11] == 1 && std::isfinite(p[0]) && std::isfinite(p[5]) && std::abs(p[0]) > 1e-6f &&
           std::abs(p[5]) > 1e-6f && std::isfinite(p[8]) && std::isfinite(p[9]);
}
// Fills the camera part of c: false when the cameras cannot be used (the test then
// leaves RR's inputs unchanged for the frame).
inline bool Reprojection(const Camera &current, const Camera &previous, Constants &c) {
    if (!Projectable(current.projection) || !Projectable(previous.projection))
        return false;
    Matrix inverse_view{};
    if (!Invert(Widen(current.view), inverse_view))
        return false;
    const auto m = Multiply(Multiply(inverse_view, Widen(previous.view)), Widen(previous.projection));
    for (unsigned i = 0; i < 16; ++i) {
        c.reproject[i] = float(m[i]);
        if (!std::isfinite(c.reproject[i]))
            return false;
    }
    const auto &p = current.projection;
    c.ax = 1 / p[0];
    c.bx = -p[8] / p[0];
    c.ay = 1 / p[5];
    c.by = -p[9] / p[5];
    return true;
}
inline bool Make(const Tuning &t, uint32_t width, uint32_t height, uint32_t motion_x,
                 uint32_t motion_y, float motion_scale_x, float motion_scale_y,
                 const Camera &current, const Camera &previous, bool coverage, Constants &c) {
    c = {};
    c.width = width;
    c.height = height;
    c.motion_x = motion_x;
    c.motion_y = motion_y;
    c.distance = ClampDistance(t.distance);
    c.threshold = ClampThreshold(t.threshold);
    if (!std::isfinite(motion_scale_x) || !std::isfinite(motion_scale_y) || motion_scale_x == 0 ||
        motion_scale_y == 0)
        return false;
    c.inv_scale_x = 1 / motion_scale_x;
    c.inv_scale_y = 1 / motion_scale_y;
    const bool ok = Reprojection(current, previous, c);
    c.flags = ok && coverage ? CoverageFlag : 0u;
    return ok;
}

// CPU mirror of the shader for one pixel.
// Coverage rgb is the transparency's transmittance; opacity is 1 - its mean.
inline float Opacity(float r, float g, float b) {
    auto s = [](float v) { return v >= 0 ? (v <= 1 ? v : 1.0f) : 0.0f; };
    return 1 - (s(r) + s(g) + s(b)) / 3;
}
// Whether the pixel is replaced: covered enough and the assumed surface in front of
// the depth RR has there (the first-person weapon and anything nearer stay).
inline bool Replaced(const Constants &c, float opacity, float depth) {
    return (c.flags & CoverageFlag) && opacity >= c.threshold && c.distance < depth;
}
// The motion RR reads for a replaced pixel (in the game's motion units: RR multiplies
// by the motion scale it is given to get previous minus current pixel position).
// False when the point is behind the previous camera.
inline bool Motion(const Constants &c, uint32_t x, uint32_t y, float &mx, float &my) {
    const float nx = (float(x) + 0.5f) / float(c.width) * 2 - 1;
    const float ny = 1 - (float(y) + 0.5f) / float(c.height) * 2;
    const float z = c.distance;
    const float v[3] = {z * (nx * c.ax + c.bx), z * (ny * c.ay + c.by), z};
    float clip[4];
    for (unsigned j = 0; j < 4; ++j)
        clip[j] = v[0] * c.reproject[j] + v[1] * c.reproject[4 + j] + v[2] * c.reproject[8 + j] +
                  c.reproject[12 + j];
    if (!(clip[3] > 0))
        return false;
    const float px = (clip[0] / clip[3] * 0.5f + 0.5f) * float(c.width);
    const float py = (0.5f - clip[1] / clip[3] * 0.5f) * float(c.height);
    mx = (px - (float(x) + 0.5f)) * c.inv_scale_x;
    my = (py - (float(y) + 0.5f)) * c.inv_scale_y;
    return true;
}

// Panel line for the test.
enum class Report { Off, Waiting, Blocked, Unavailable, NoLinearDepth, NoCamera, NoSource, Active };
inline const char *Label(Report r) {
    switch (r) {
    case Report::Off:
        return "off";
    case Report::Waiting:
        return "waiting for RR";
    case Report::Blocked:
        return "off: not enough GPU memory (change a sheet setting to retry)";
    case Report::Unavailable:
        return "unavailable (see ReShade.log)";
    case Report::NoLinearDepth:
        return "off: needs Weapon depth on";
    case Report::NoCamera:
        return "on, no usable previous camera this frame";
    case Report::NoSource:
        return "on, no transparency capture this frame (none in view, or the transparency albedo blend is off)";
    case Report::Active:
        return "active (transparency captured; the blend's submission check still applies)";
    }
    return "off";
}
} // namespace rrsheet
