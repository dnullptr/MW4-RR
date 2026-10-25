#pragma once
#include <atomic>
namespace rrguidetest {
// Development tests for noise on thin geometry. Each mode swaps the guide
// conversion shader for a variant (shaders/rr_convert_guides.hlsl RR_GUIDE_TEST).
enum Mode : unsigned {
    Off, FlipBackFaces, FaceCamera, FlatDiffuse, NoSpecular, FlatRoughness, AllFlat, Count
};
inline constexpr const char *labels[Count] = {
    "Off (normal guides)", "Flip back-facing normals", "All normals face the camera",
    "Flat diffuse albedo (0.5)", "No specular albedo", "Flat roughness (0.5)",
    "All guides flat"};
inline constexpr unsigned Valid(unsigned mode) {
    return mode < Count ? mode : Off;
}
inline std::atomic<unsigned> mode{};
} // namespace rrguidetest
