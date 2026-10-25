#pragma once
#include <d3d12.h>
#include "rr_resolution_limits.h"
#include "feed_fg_identity.h"
#include "rr_ray_budget.h"
#include <array>
#include <algorithm>
namespace rrvrrt {
inline constexpr UINT64 ChunkBytes = 131072, WindowBytes = 65536;
inline constexpr UINT StripRows = 32;
inline bool Dimensions(UINT64 w, UINT h) {
    return rrresolution::Texture(w, h) && !(w & 1);
}
// Each strip keeps the original two 64-KiB CBV windows and 16-byte header.
// Shorten its row count as width grows; never let pixel codes cross the chunk.
inline UINT Rows(UINT w) {
    return w > 0 && w <= RR_TEXTURE2D_LIMIT && !(w & 1)
               ? (std::min)(StripRows, UINT((ChunkBytes - 16) / (UINT64(w) * 2)))
               : 0;
}
inline UINT64 Bytes(UINT w, UINT h) {
    const auto rows = Rows(w);
    return Dimensions(w, h) && rows ? UINT64((h + rows - 1) / rows) * ChunkBytes : 0;
}
struct Inputs {
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> resources;
    std::array<UINT64, 2> cbv{};
    UINT64 budget_offset{}, budget_bytes{};
    UINT w{}, h{};
};
struct Data {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    UINT w{}, h{};
};
struct CBVs {
    std::array<UINT, 2> parameters{};
    std::array<UINT64, 2> addresses{};
    bool valid{};
};
// The ray budget test's view of one native root constant buffer: `avail` = resource Width - offset,
// `upload` when the resource lives in an upload heap (always GENERIC_READ, so copies need no
// barrier).
struct RayConstant {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    UINT64 offset{}, address{}, avail{};
    bool upload{};
};
// The resolve's t0 (raw view): offset and avail are the view's first byte and size; state is the
// tracked state of the buffer in this list.
struct RayStats {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    UINT64 offset{}, avail{};
    D3D12_RESOURCE_STATES state{};
    bool known{};
};
struct RayResolve {
    rrraybudget::Why why = rrraybudget::Why::Context;
    bool constants{};
    UINT parameter{};
    RayConstant settings, scene;
    RayStats stats;
    ID3D12PipelineState *pipeline{};
    ID3D12RootSignature *root{};
    UINT64 budget_resource{}, budget_offset{};
};
struct RayApply {
    RayConstant settings, scene;
};
} // namespace rrvrrt
