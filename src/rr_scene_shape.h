#pragma once
#include "rr_resolution_limits.h"
namespace rrglass {
// Producer shape only. The consumer separately verifies current render extent.
inline bool Shape(const D3D12_RESOURCE_DESC &d, unsigned slot) {
    return slot == 1 && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           rrresolution::Texture(d.Width, d.Height) && d.DepthOrArraySize == 1 &&
           d.MipLevels == 1 && d.SampleDesc.Count == 4 &&
           (d.SampleDesc.Quality == 0 ||
            d.SampleDesc.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN) &&
           d.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
           !(d.Flags &
             (D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
}
} // namespace rrglass
