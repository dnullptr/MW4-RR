#pragma once
#include <array>
#include <cstdint>
#include <d3d12.h>
namespace rrfilter {
enum class Mode { Raw, TemporalSpatial, TemporalOnly };
inline bool Valid(Mode value) {
    return value == Mode::Raw || value == Mode::TemporalSpatial || value == Mode::TemporalOnly;
}
inline bool Native(Mode value) {
    return value == Mode::TemporalSpatial || value == Mode::TemporalOnly;
}
inline const char *Label(Mode value) {
    switch (value) {
    case Mode::Raw: return "Raw (normal RR path)";
    case Mode::TemporalSpatial: return "Native temporal + spatial";
    case Mode::TemporalOnly: return "Native temporal only";
    default: return "Invalid";
    }
}
// CPU recording proof only. The caller owns resources and validates native
// pipeline, binding/state/shape, same-recording identity and submission order.
class Chain {
    std::array<uint64_t, 2> history{}, next{};
    bool seen{}, consumed{}, invalid{}, bypassed{};
public:
    bool Temporal(std::array<uint64_t, 2> pair) {
        if (seen || invalid || !pair[0] || !pair[1] || pair[0] == pair[1]) {
            invalid = true;
            return false;
        }
        seen = true;
        history = pair;
        return true;
    }
    bool Matches(std::array<uint64_t, 2> pair) const {
        return seen && !invalid && !consumed && history == pair;
    }
    bool Spatial(std::array<uint64_t, 2> pair) {
        if (!Matches(pair)) return false;
        consumed = true;
        next = pair;
        return true;
    }
    bool Accepted() const { return seen && consumed && !invalid; }
    bool Next(std::array<uint64_t,2> pair) const { return Accepted() && next == pair; }
    void Invalidate() { invalid = true; }
    bool Forward(std::array<uint64_t,2> input, std::array<uint64_t,2> output) {
        if (!Next(input) || !output[0] || !output[1] || output[0] == output[1] ||
            output[0] == input[0] || output[0] == input[1] ||
            output[1] == input[0] || output[1] == input[1]) {
            Invalidate();
            return false;
        }
        next = output;
        bypassed = true;
        return true;
    }
    bool Bypassed() const { return Accepted() && bypassed; }
};
// Pure CPU validation shared by the dispatch path and regression fixtures.
struct Surface { uint64_t identity{}; D3D12_RESOURCE_DESC desc{}; D3D12_RESOURCE_STATES state{}; };
inline bool SpatialPassValid(const std::array<Surface,2> &input,
                             const std::array<Surface,2> &output,
                             uint32_t x, uint32_t y, uint32_t z, bool packed_uint) {
    const auto &extent = output[0].desc;
    if (z != 1 || x != (extent.Width + 15) / 16 || y != (extent.Height + 15) / 16)
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        const auto &a = input[i]; const auto &b = output[i];
        if (!a.identity || !b.identity || input[0].identity == input[1].identity ||
            output[0].identity == output[1].identity ||
            b.identity == input[0].identity || b.identity == input[1].identity ||
            !(a.state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
            (a.state & ~(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE)) ||
            b.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            return false;
        const auto uint_format = i ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
        const auto float_format = i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (a.desc.Format != b.desc.Format ||
            (a.desc.Format != uint_format && (packed_uint || a.desc.Format != float_format)))
            return false;
        for (const auto *d : {&a.desc, &b.desc})
            if (d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d->Width || !d->Height ||
                d->Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                d->Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                d->Width != extent.Width || d->Height != extent.Height || d->MipLevels != 1 ||
                d->DepthOrArraySize != 1 || d->SampleDesc.Count != 1 || d->SampleDesc.Quality)
                return false;
    }
    return true;
}
struct Status { Mode desired{}, effective{}; bool pending{}; };
}
