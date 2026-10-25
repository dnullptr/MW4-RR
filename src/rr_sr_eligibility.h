#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
namespace rrsr {
struct StateProof {
    std::array<bool, 4> known{};
    std::array<D3D12_RESOURCE_STATES, 4> states{};
    std::array<uintptr_t, 4> resources{};
};
inline bool Eligible(const StateProof &p) {
    constexpr auto reads =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (unsigned i = 0; i < 4; ++i) {
        if (!p.known[i] || !p.resources[i])
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (p.resources[i] == p.resources[j])
                return false;
        if (i < 3 && (!(p.states[i] & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
                      (p.states[i] & ~reads)))
            return false;
    }
    return p.states[3] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}
} // namespace rrsr
