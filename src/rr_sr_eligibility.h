#pragma once
#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
namespace rrsr {
struct StateProof {
    std::array<bool, 4> known{};
    std::array<D3D12_RESOURCE_STATES, 4> states{};
    std::array<uintptr_t, 4> resources{};
    std::array<bool, 4> aliased{}; // Known, but past an aliasing barrier (only Describe reads it).
};
// Whether SR resource i (color, depth, motion, output) is in a state replacement takes.
inline bool Accepts(unsigned i, D3D12_RESOURCE_STATES s) {
    constexpr auto reads =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (i == 3)
        return s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return (s & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && !(s & ~reads);
}
inline bool Eligible(const StateProof &p) {
    for (unsigned i = 0; i < 4; ++i) {
        if (!p.known[i] || !p.resources[i])
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (p.resources[i] == p.resources[j])
                return false;
        if (!Accepts(i, p.states[i]))
            return false;
    }
    return true;
}
// One clause per SR resource, in Eligible's order of checks, e.g. "color 0x80 (wants NPSR, reads
// only), depth unknown, motion same as color, output 0x8 ok". Truncated to fit size.
inline void Describe(const StateProof &p, char *out, size_t size) {
    static constexpr const char *names[4] = {"color", "depth", "motion", "output"};
    if (!out || !size)
        return;
    out[0] = '\0';
    size_t used = 0;
    for (unsigned i = 0; i < 4 && used < size; ++i) {
        unsigned same = i;
        for (unsigned j = i; j-- > 0;)
            if (p.resources[j] == p.resources[i])
                same = j;
        char text[64];
        if (!p.resources[i])
            std::snprintf(text, sizeof(text), "missing");
        else if (!p.known[i])
            std::snprintf(text, sizeof(text), "unknown");
        else if (same != i)
            std::snprintf(text, sizeof(text), "same as %s", names[same]);
        else
            std::snprintf(text, sizeof(text), "%s0x%x %s", p.aliased[i] ? "aliased " : "",
                          unsigned(p.states[i]),
                          Accepts(i, p.states[i]) ? "ok"
                          : i < 3                 ? "(wants NPSR, reads only)"
                                                  : "(wants UAV)");
        const int n =
            std::snprintf(out + used, size - used, "%s%s %s", i ? ", " : "", names[i], text);
        if (n < 0)
            return;
        used += size_t(n);
    }
}
} // namespace rrsr
