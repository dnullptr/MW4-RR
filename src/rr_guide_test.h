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
// SkySurface also lifts sky depth from 0 to a floor in RR's private depth copy
// (shaders/rr_sky_depth_floor.hlsl), so RR cannot single sky out by depth either. With the
// game's infinite reverse-Z, depth = near / distance, so the floor sets how far away the
// sky sits for RR, in near-plane units (sp26's near plane is 1). In the capture this was
// chosen from, trees against the sky sat at about 600 and the farthest hills at about
// 170,000; hills sit a median 6x farther than what is in front of them. Geometry farther
// than the floor's distance ends up behind the sky.
enum Floor : unsigned { VeryFar, PastHills, Far, BehindTrees, FloorCount };
inline constexpr const char *floor_labels[FloorCount] = {
    "10,000,000 (very far, as before)", "200,000 (past the farthest hills)", "10,000",
    "2,000 (just behind nearby trees)"};
inline constexpr float floors[FloorCount] = {1e-7f, 5e-6f, 1e-4f, 5e-4f};
inline constexpr unsigned ValidFloor(unsigned f) {
    return f < FloorCount ? f : VeryFar;
}
inline constexpr float DepthFloor(unsigned f) {
    return floors[ValidFloor(f)];
}
inline constexpr unsigned Valid(unsigned mode) {
    return mode < Count ? mode : Off;
}
inline std::atomic<unsigned> mode{};
// Sky distance for SkySurface (Floor), session-only like the mode.
inline std::atomic<unsigned> floor{};
// Bumped by the panel on every change (retries a test blocked for GPU memory).
inline std::atomic<unsigned> changes{};
} // namespace rrguidetest
