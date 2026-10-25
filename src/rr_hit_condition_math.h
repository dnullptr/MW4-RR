#pragma once
// Specular hit distance conditioning (shaders/rr_hit_condition.hlsl), shared with
// the CPU tests. The game's cs_raygen_sum_up distance is the found distance, or 0
// for no ray and for a miss. NVIDIA's RR guide 3.4.9 asks for FP16_MAX at sky.
#ifdef __cplusplus
#define RR_HIT_CONST inline constexpr
#else
#define RR_HIT_CONST static const
#endif
namespace rrhitcond {
#ifdef __cplusplus
using UInt = unsigned;
#else
typedef uint UInt;
#endif
RR_HIT_CONST float Far = 65504.0f;
// Mode bits, one per panel test.
RR_HIT_CONST UInt SkyMiss = 1u, FillHoles = 2u;
// PackedSurface.w: viewmodel range (its own projection) and no RT specular ray.
RR_HIT_CONST UInt Alternate = 1u, NoRtSpecular = 32u;
// Pixel kinds: left raw, primary sky, a hit, a fresh specular ray that missed,
// and a hole (no specular ray this frame).
RR_HIT_CONST UInt Leave = 0u, Sky = 1u, Hit = 2u, Miss = 3u, Hole = 4u;
// Holes are filled from the (2 Radius + 1)^2 neighbourhood.
RR_HIT_CONST int Radius = 3;
// A neighbour stands in for the centre within this share of its device depth
// (about the same share of view depth) and within about 25 degrees of its normal.
RR_HIT_CONST float DepthTolerance = 0.04f, MinCosine = 0.9f;
// Statistics after the gate's 16 bytes in the slot's stats buffer, which only
// this pass writes; it zeroes them first.
RR_HIT_CONST UInt StatsOffset = 16u, StatCount = 12u;
RR_HIT_CONST UInt CountSky = 0u, CountViewmodel = 1u, CountNoRtSpecular = 2u, CountHit = 3u,
                  CountMiss = 4u, CountHole = 5u, CountFilledHit = 6u, CountFilledMiss = 7u,
                  CountHitNoLobe1 = 8u, CountHitNoLobe2 = 9u, CountNoRtSpecularHit = 10u,
                  CountOther = 11u;

// Raw lighting weights: bits 20-31 count lobe-1 samples, 8-19 lobe-2 samples
// (shaders/rr_sparse_scale_vrrt.hlsl). Lobe 1 behaves as specular in game.
inline bool SpecularRay(UInt weights) {
    return (weights >> 20) != 0u;
}
inline bool DiffuseRay(UInt weights) {
    return ((weights >> 8) & 4095u) != 0u;
}
inline bool SurfaceDepth(float depth) {
    return depth > 0.0f && depth <= 1.0f;
}
// The guide conversion's status for a surface (1 ordinary, 3 special).
inline bool SurfaceStatus(UInt status) {
    return status == 1u || status == 3u;
}
// status: the guide conversion's (1 ordinary, 2 sky, 3 special). NaN or a
// negative raw distance has no hit; infinity is clamped by Resolve.
inline UInt Classify(UInt status, UInt w, float depth, float raw, bool ray) {
    if (status == 2u)
        return Sky;
    if (!SurfaceStatus(status) || !SurfaceDepth(depth) || (w & (Alternate | NoRtSpecular)) != 0u)
        return Leave;
    if (raw > 0.0f)
        return Hit;
    return ray ? Miss : Hole;
}
// The centre's per-pixel depth step along one axis. Two sides that agree lie on
// one plane (device depth is affine across it), however steep. Otherwise the
// smaller step is used only while it is itself within the depth tolerance, so
// neither an edge on one side nor both edges of a 1-px feature tilt the plane
// onto the background.
inline float Slope(float centre, float before, bool has_before, float after, bool has_after) {
    const float a = centre - before, b = after - centre, limit = DepthTolerance * centre;
    const float abs_a = a < 0.0f ? -a : a, abs_b = b < 0.0f ? -b : b, d = a - b;
    if (has_before && has_after && (d < 0.0f ? -d : d) <= limit)
        return abs_a < abs_b ? a : b;
    const float s = has_before && (!has_after || abs_a < abs_b) ? a : has_after ? b : 0.0f;
    return (s < 0.0f ? -s : s) <= limit ? s : 0.0f;
}
// Device depth is affine in screen space across a plane, so predicted is the
// centre's depth extrapolated with its slopes to the neighbour.
inline bool SameSurface(UInt centre_w, UInt w, UInt status, float centre_depth, float depth,
                        float predicted, float cosine) {
    const float error = depth - predicted;
    return w == centre_w && SurfaceStatus(status) && SurfaceDepth(depth) &&
           (error < 0.0f ? -error : error) <= DepthTolerance * centre_depth &&
           cosine >= MinCosine;
}
inline float Weight(int dx, int dy) {
    return 1.0f / (1.0f + float(dx * dx + dy * dy));
}
inline float Clamp(float distance) {
    return distance < Far ? distance : Far;
}
// A hole takes the miss value if fresh misses outweigh hits around it, else the
// weighted mean of the hits; with neither it keeps its raw 0.
inline bool FilledAsMiss(float hit_weight, float miss_weight) {
    return miss_weight > hit_weight;
}
inline float Resolve(UInt kind, float raw, UInt mode, float hit_weight, float hit_sum,
                     float miss_weight) {
    const bool sky_far = (mode & SkyMiss) != 0u;
    if (kind == Sky || kind == Miss)
        return sky_far ? Far : raw;
    if (kind == Hit)
        return Clamp(raw);
    if (kind == Hole && (mode & FillHoles) != 0u) {
        if (FilledAsMiss(hit_weight, miss_weight))
            return sky_far ? Far : 0.0f;
        if (hit_weight > 0.0f)
            return Clamp(hit_sum / hit_weight);
    }
    return raw;
}
} // namespace rrhitcond
#undef RR_HIT_CONST
