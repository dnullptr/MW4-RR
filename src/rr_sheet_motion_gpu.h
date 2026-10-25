#pragma once
#include "rr_gpu_common.h"
#include "rr_sheet_motion.h"
#include "../build/rr_sheet_motion.generated.h"
namespace rrsheet {
using Microsoft::WRL::ComPtr;
using rrprivate::Check;
using rrprivate::Require;
inline void Change(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                   D3D12_RESOURCE_STATES to) {
    if (!r || from == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    cl->ResourceBarrier(1, &b);
}
// Root: SRV table (t0 motion, t1 transparency coverage), UAV table (u0 the motion copy
// RR reads, u1 RR's linear depth), 32 root constants (b0), root CBV (b1, coverage
// admission).
struct Pipeline {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state;
    UINT stride{};
    void Initialize(ID3D12Device *d) {
        if (state) {
            Require(device.Get() == d, "sheet motion pipeline device changed");
            return;
        }
        Require(d, "sheet motion device missing");
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p[4]{};
        for (UINT i = 0; i < 2; ++i) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[i].DescriptorTable = {1, &ranges[i]};
        }
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[2].Constants = {0, 0, ConstantWords};
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[3].Descriptor = {1, 0};
        D3D12_ROOT_SIGNATURE_DESC desc{4, p};
        ComPtr<ID3DBlob> signature, error;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
              "sheet motion root serialization failed");
        ComPtr<ID3D12RootSignature> r;
        Check(d->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                     IID_PPV_ARGS(&r)),
              "sheet motion root creation failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = r.Get();
        pd.CS = {RRSheetMotion, sizeof(RRSheetMotion)};
        ComPtr<ID3D12PipelineState> s;
        Check(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s)),
              "sheet motion pipeline creation failed");
        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device = d;
        root = r;
        state = s;
    }
    // The copy has the motion's format and must take typed UAV stores.
    bool Writable(DXGI_FORMAT format) const {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        return device &&
               SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                     sizeof(support))) &&
               (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
               (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }
};
// One frame's resources. motion: the game's motion, NON_PIXEL_SHADER_RESOURCE.
// output: the copy RR reads, COMMON at rest. depth: RR's linear depth copy,
// NON_PIXEL_SHADER_RESOURCE. coverage: the glass capture's target, COMMON at rest, or
// null. admitted: a valid CBV address even without coverage (read only with
// CoverageFlag).
struct Inputs {
    ID3D12Resource *motion{}, *output{}, *depth{}, *coverage{};
    D3D12_GPU_VIRTUAL_ADDRESS admitted{};
};
// Per slot: its own descriptors, rewritten only while the slot is not in flight.
struct Gpu {
    ComPtr<ID3D12DescriptorHeap> heap;
    // Creates the heap; the only step that can fail. Call before recording.
    void Prepare(Pipeline &p) {
        Require(p.state, "sheet motion pipeline missing");
        if (heap)
            return;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Check(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
              "sheet motion descriptor heap failed");
    }
    // Leaves output readable by RR (NON_PIXEL_SHADER_RESOURCE; the caller returns it to
    // COMMON after evaluation) and every other resource in its entry state.
    void Record(ID3D12GraphicsCommandList *cl, Pipeline &p, const Inputs &in,
                const Constants &c) {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + SIZE_T(i) * p.stride}; };
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = in.motion->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        p.device->CreateShaderResourceView(in.motion, &srv, at(0));
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        p.device->CreateShaderResourceView(in.coverage, &srv, at(1)); // null: a null view
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = in.output->GetDesc().Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(in.output, nullptr, &uav, at(2));
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        p.device->CreateUnorderedAccessView(in.depth, nullptr, &uav, at(3));
        Change(cl, in.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Change(cl, in.coverage, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Change(cl, in.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.root.Get());
        cl->SetPipelineState(p.state.Get());
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, gpu);
        cl->SetComputeRootDescriptorTable(1, {gpu.ptr + 2 * UINT64(p.stride)});
        cl->SetComputeRoot32BitConstants(2, ConstantWords, &c, 0);
        cl->SetComputeRootConstantBufferView(3, in.admitted);
        cl->Dispatch((c.width + 7) / 8, (c.height + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER order{};
        order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &order);
        Change(cl, in.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Change(cl, in.coverage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
        Change(cl, in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
};
} // namespace rrsheet
