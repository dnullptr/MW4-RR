#pragma once
#include <cstdint>
#include "rr_sss_vertex_proof.h"
// The coverage replay runs a composite's pixel stage a second time, so the
// stage must only read. Pixel-stage sibling of rrsss::VertexReplayProof, on
// the same reflection machinery.
namespace rrblend {
// [earlydepthstencil] is allowed too: the replay's depth state never writes.
inline constexpr UINT64 PixelReplayRequires =
    rrsss::ReplayRequires | D3D_SHADER_REQUIRES_EARLY_DEPTH_STENCIL;

// A pixel stage that binds only CBVs, SRVs and samplers and writes only
// SV_Target. detail is the refused requirement bits, binding type or output
// system value, or ~0 when the refusal has none.
inline bool PixelReplayGate(ID3D12ShaderReflection *reflection, const char *&status,
                            uint64_t &detail) noexcept {
    detail = ~0ull;
    D3D12_SHADER_DESC desc{};
    status = "pixel-stage-or-description-unsupported";
    if (!reflection || FAILED(reflection->GetDesc(&desc)) ||
        D3D12_SHVER_GET_TYPE(desc.Version) != D3D12_SHVER_PIXEL_SHADER ||
        desc.BoundResources > 128 || desc.OutputParameters > 32)
        return false;
    // No waves, UAV features, bindless heaps or stencil reference output.
    status = "pixel-replay-requires-unsupported-capabilities";
    if (const auto extra = reflection->GetRequiresFlags() & ~PixelReplayRequires) {
        detail = extra;
        return false;
    }
    status = "pixel-replay-writable-or-unknown-binding";
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding)))
            return false;
        if (!rrsss::ReplayBinding(binding.Type)) {
            detail = uint64_t(binding.Type);
            return false;
        }
    }
    // No depth, coverage or stencil reference output: only the render targets.
    status = "pixel-replay-output-not-render-target";
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC output{};
        if (FAILED(reflection->GetOutputParameterDesc(i, &output)))
            return false;
        if (output.SystemValueType != D3D_NAME_TARGET) {
            detail = uint64_t(output.SystemValueType);
            return false;
        }
    }
    status = "pixel-read-only-reflection-proven";
    return true;
}

inline bool PixelReplayProof(const void *code, size_t size, const char *&status,
                             uint64_t &detail) noexcept {
    detail = ~0ull;
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    if (!rrsss::ReplayReflection(code, size,
                                 {"pixel-bytecode-invalid", "pixel-dxc-companion-unavailable",
                                  "pixel-dxil-reflection-unavailable"},
                                 reflection, status))
        return false;
    return PixelReplayGate(reflection.Get(), status, detail);
}
} // namespace rrblend
