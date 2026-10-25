#pragma once
#include <atomic>
namespace rrguidetest {
// Development tests for noise on thin geometry. Each mode swaps the guide
// conversion shader for a variant (shaders/rr_convert_guides.hlsl RR_GUIDE_TEST).
enum Mode : unsigned {
    Off, FlipBackFaces, FaceCamera, FlatDiffuse, NoSpecular, FlatRoughness, AllFlat,
    SkyNormals, SkySurface, SkyRoughness, Count
};
inline constexpr const char *labels[Count] = {
    "Off (normal guides)", "Flip back-facing normals", "All normals face the camera",
    "Flat diffuse albedo (0.5)", "No specular albedo", "Flat roughness (0.5)",
    "All guides flat", "Sky normals face the camera", "Sky normals + non-zero sky depth",
    "Sky roughness 0 (NVIDIA default)"};
// SkySurface also lifts sky depth from 0 to DepthFloor in RR's private depth copy
// (shaders/rr_sky_depth_floor.hlsl), so RR cannot single sky out by depth either.
inline constexpr float DepthFloor = 1e-7f;
inline constexpr unsigned Valid(unsigned mode) {
    return mode < Count ? mode : Off;
}
inline std::atomic<unsigned> mode{};
// Bumped by the panel on every change (retries a test blocked for GPU memory).
inline std::atomic<unsigned> changes{};
} // namespace rrguidetest
