#pragma once
#include "rr_bindings.h"
#include <d3d12.h>
namespace rrlive {
// For ordinary texture_2d views ReShade leaves texture.layers at zero.
// The native resource description proves the actual single-layer shape.
// Array views still have to explicitly cover all four material/sample layers.
inline bool GuideBindingMatches(const rrbindings::Value &v, const D3D12_RESOURCE_DESC &d,
                                unsigned slot) {
    if (slot >= 4)
        return false;
    const bool array = slot < 3;
    const auto format = slot < 2    ? DXGI_FORMAT_R11G11B10_FLOAT
                        : slot == 2 ? DXGI_FORMAT_R32_UINT
                                    : DXGI_FORMAT_R32G32B32A32_UINT;
    return v.known && v.resource &&
           v.dimension == unsigned(reshade::api::resource_type::texture_2d) &&
           d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Format == format &&
           v.format == unsigned(format) && v.view_format == unsigned(format) &&
           v.width == d.Width && v.height == d.Height &&
           unsigned(d.DepthOrArraySize) == (array ? 4u : 1u) && d.MipLevels == 1 &&
           d.SampleDesc.Count == 1 && v.first_layer == 0 && v.first_level == 0 && v.levels == 1 &&
           (array ? v.layers == 4 : v.layers <= 1) &&
           v.view_type == unsigned(array ? reshade::api::resource_view_type::texture_2d_array
                                         : reshade::api::resource_view_type::texture_2d) &&
           (slot < 2 || (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
}
} // namespace rrlive
