#pragma once
#include <d3d12.h>
#include <climits>
#include <cstdint>
namespace rrspecular {
// Native cs_ray_hit_distance_blur: t10 is the cs_raygen_sum_up specular hit
// distance scalar, t13 is the prepass PackedSurface of the same frame.
inline constexpr uint64_t DistanceBlur = 0xc9ca6daa80593f3aull;
// The copy is sized from the current prepass guides, not a fixed resolution.
inline bool ValidSource(const D3D12_RESOURCE_DESC &d, UINT w, UINT h,
                        D3D12_RESOURCE_STATES state) {
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_COPY_SOURCE;
    return w && h && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == w &&
           d.Height == h && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
           d.SampleDesc.Count == 1 && d.Format == DXGI_FORMAT_R16_FLOAT &&
           !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) &&
           (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && !(state & ~read);
}
// Dispatch shape of the native blur: one 16x16 group per tile of the guide.
inline bool DispatchShape(UINT64 w, UINT h, UINT x, UINT y, UINT z) {
    return w && h && w <= UINT_MAX && x == (UINT(w) + 15) / 16 && y == (h + 15) / 16 && z == 1;
}
} // namespace rrspecular
