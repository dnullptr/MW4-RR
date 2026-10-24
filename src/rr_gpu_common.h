#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include "rr_resolution_limits.h"
namespace rrprivate {
using Microsoft::WRL::ComPtr;
inline D3D12_RESOURCE_DESC Buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
inline void Require(bool valid, const char *error) {
    if (!valid)
        throw std::runtime_error(error);
}
inline void Check(HRESULT hr, const char *error) {
    Require(SUCCEEDED(hr), error);
}
// Immutable submission snapshot. A shutdown observer can wait independently
// of Gpu::Wait without touching its event, serial or pending state.
struct FencePoint {
    ComPtr<ID3D12Fence> fence;
    UINT64 value{};
    bool Wait(DWORD timeout) const {
        if (!fence)
            return value == 0;
        const auto start = GetTickCount64();
        for (;;) {
            const auto done = fence->GetCompletedValue();
            if (done == UINT64_MAX)
                return false;
            if (done >= value)
                return true;
            if (GetTickCount64() - start >= timeout)
                return false;
            Sleep(1);
        }
    }
};
} // namespace rrprivate
