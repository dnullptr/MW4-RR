#pragma once
// D3D12 texture limit, shared with HLSL. This is not a display-mode limit.
#define RR_TEXTURE2D_LIMIT 16384u
#ifdef __cplusplus
#include <d3d12.h>
#include <cstdint>
static_assert(RR_TEXTURE2D_LIMIT == D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
namespace rrresolution {
inline constexpr bool Texture(uint64_t width, uint64_t height) {
    return width && height && width <= RR_TEXTURE2D_LIMIT && height <= RR_TEXTURE2D_LIMIT;
}
inline constexpr bool HalfInput(uint64_t width, uint64_t height) {
    return width && height && width <= RR_TEXTURE2D_LIMIT / 2 && height <= RR_TEXTURE2D_LIMIT / 2;
}
inline constexpr bool Parity(uint64_t width, uint64_t height) {
    return Texture(width, height) && !(width & 1) && !(height & 1);
}
// Subtract only after checking origins; never add potentially overflowing input.
inline constexpr bool Fits(uint64_t backing_width, uint64_t backing_height, uint64_t x, uint64_t y,
                           uint64_t width, uint64_t height) {
    return Texture(backing_width, backing_height) && width && height && x <= backing_width &&
           y <= backing_height && width <= backing_width - x && height <= backing_height - y;
}
} // namespace rrresolution
#endif
