#pragma once
#include "rr_gpu_common.h"
#include "rr_transparent_effects.h"
#include "../build/rr_transparent_effects.generated.h"
namespace rrfx {
using Microsoft::WRL::ComPtr;
using rrprivate::Check;
using rrprivate::Require;
// Root: SRV table (t0 color, t1 color before transparency, t2 the mask RR would
// otherwise get), UAV table (u0 the mask RR reads), 8 root constants (b0).
struct Pipeline {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state;
    UINT stride{};
    void Initialize(ID3D12Device *d) {
        if (state) {
            Require(device.Get() == d, "transparent effects pipeline device changed");
            return;
        }
        Require(d, "transparent effects device missing");
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p[3]{};
        for (UINT i = 0; i < 2; ++i) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[i].DescriptorTable = {1, &ranges[i]};
        }
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[2].Constants = {0, 0, ConstantWords};
        D3D12_ROOT_SIGNATURE_DESC desc{3, p};
        ComPtr<ID3DBlob> signature, error;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
              "transparent effects root serialization failed");
        ComPtr<ID3D12RootSignature> r;
        Check(d->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                     IID_PPV_ARGS(&r)),
              "transparent effects root creation failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = r.Get();
        pd.CS = {RRTransparentEffects, sizeof(RRTransparentEffects)};
        ComPtr<ID3D12PipelineState> s;
        Check(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s)),
              "transparent effects pipeline creation failed");
        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device = d;
        root = r;
        state = s;
    }
};
// One frame's resources, all NON_PIXEL_SHADER_RESOURCE on entry except output
// (COMMON at rest): color, before (RR's color before transparency,
// R11G11B10_FLOAT), existing (R16_FLOAT, or null).
struct Inputs {
    ID3D12Resource *color{}, *before{}, *existing{}, *output{};
};
// Per slot: its own descriptors, rewritten only while the slot is not in flight.
struct Gpu {
    ComPtr<ID3D12DescriptorHeap> heap;
    // Creates the heap; the only step that can fail. Call before recording.
    void Prepare(Pipeline &p) {
        Require(p.state, "transparent effects pipeline missing");
        if (heap)
            return;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Check(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
              "transparent effects descriptor heap failed");
    }
    // Leaves output readable by RR (NON_PIXEL_SHADER_RESOURCE; the caller returns
    // it to COMMON after evaluation) and the inputs in their entry state.
    void Record(ID3D12GraphicsCommandList *cl, Pipeline &p, const Inputs &in,
                const Constants &c) {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + SIZE_T(i) * p.stride}; };
        auto srv = [&](ID3D12Resource *r, DXGI_FORMAT f, UINT i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Format = f;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            p.device->CreateShaderResourceView(r, &v, at(i)); // null: a null view
        };
        srv(in.color, in.color->GetDesc().Format, 0);
        srv(in.before, DXGI_FORMAT_R11G11B10_FLOAT, 1);
        srv(in.existing, DXGI_FORMAT_R16_FLOAT, 2);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(in.output, nullptr, &uav, at(3));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {in.output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cl->ResourceBarrier(1, &b);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.root.Get());
        cl->SetPipelineState(p.state.Get());
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, gpu);
        cl->SetComputeRootDescriptorTable(1, {gpu.ptr + 3 * UINT64(p.stride)});
        cl->SetComputeRoot32BitConstants(2, ConstantWords, &c, 0);
        cl->Dispatch((c.width + 7) / 8, (c.height + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER order{};
        order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &order);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
    }
};
} // namespace rrfx
