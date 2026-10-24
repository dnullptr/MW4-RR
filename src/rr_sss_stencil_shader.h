#pragma once
#include "rr_sss_stencil.h"
#include "../build/sss-owned-shader/rr_sss_stencil.generated.h"
namespace rrsss {
inline HRESULT CreateStencilInitializer(ID3D12Device *device, ID3D12RootSignature *root,
                                        const D3D12_DEPTH_STENCIL_DESC2 &native, DXGI_FORMAT format,
                                        StencilSetup &setup) {
    setup = {};
    if (!device || !root || !StencilCompatible(native, format))
        return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root;
    d.VS = {RRSSSStencilVS, sizeof(RRSSSStencilVS)};
    d.PS = {RRSSSStencilPS, sizeof(RRSSSStencilPS)};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState = InverseStencilState(native);
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
    d.DSVFormat = format;
    d.SampleDesc = {4, 0};
    const auto hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&setup.pipeline));
    if (SUCCEEDED(hr)) {
        setup.required = true;
        setup.read_mask = native.FrontFace.StencilReadMask;
    }
    return hr;
}
// Direct native fixture path. Runtime creation clones the original API
// description under the scoped native-quality patch and validates VS replay.
inline HRESULT CreateGeometryStencilInitializer(ID3D12Device *device,
                                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC &original,
                                                const D3D12_DEPTH_STENCIL_DESC2 &native,
                                                StencilSetup &setup) {
    setup = {};
    if (!device || !original.pRootSignature || !original.VS.pShaderBytecode ||
        !original.VS.BytecodeLength || original.SampleDesc.Count != 4 ||
        (original.SampleDesc.Quality != 0 &&
         original.SampleDesc.Quality != UINT(D3D12_STANDARD_MULTISAMPLE_PATTERN)) ||
        !StencilCompatible(native, original.DSVFormat, true))
        return E_INVALIDARG;
    auto d = original;
    d.PS = {RRSSSStencilPS, sizeof(RRSSSStencilPS)};
    d.DepthStencilState = InverseStencilState(native);
    d.NumRenderTargets = 1;
    for (auto &f : d.RTVFormats)
        f = DXGI_FORMAT_UNKNOWN;
    d.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
    d.BlendState = {};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    d.CachedPSO = {};
    const auto hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&setup.pipeline));
    if (SUCCEEDED(hr)) {
        setup.required = true;
        setup.read_mask = native.FrontFace.StencilReadMask;
        setup.samples = original.SampleDesc;
        setup.replay_geometry = true;
    }
    return hr;
}
} // namespace rrsss
