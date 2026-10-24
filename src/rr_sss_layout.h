#pragma once
#include <d3d12.h>
#include "rr_resolution_limits.h"
namespace rrsss {
// The native producer packs four parity samples into an exact 2x2 input block.
// Extents come from validated frame resources, never the display or a preset.
struct Layout {
    UINT width{}, height{};
    bool Valid() const {
        return rrresolution::Parity(width, height);
    }
    UINT ProducerWidth() const {
        return width / 2;
    }
    UINT ProducerHeight() const {
        return height / 2;
    }
    D3D12_DISPATCH_ARGUMENTS Dispatch() const {
        return Valid() ? D3D12_DISPATCH_ARGUMENTS{(width + 7) / 8, (height + 7) / 8, 1}
                       : D3D12_DISPATCH_ARGUMENTS{};
    }
};
} // namespace rrsss
