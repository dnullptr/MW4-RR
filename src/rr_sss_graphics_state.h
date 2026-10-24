#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <memory>

namespace rrsss {
// Native dynamic-state evidence belongs to one observed direct recording.
// Geometry is reported verbatim; the producer decides its supported crop.
struct GraphicsOutputProof {
    uint64_t generation{};
    bool recording_known{}, direct{}, tainted{};
    bool predication_known{}, predicated{}, renderpass_known{}, renderpass{};
    bool stencil_tracking{}, stencil_refs_known{}, queries_known{};
    UINT front_ref{}, back_ref{}, query_count{};
    struct Query {
        uintptr_t heap{};
        D3D12_QUERY_TYPE type{};
        UINT index{};
    };
    std::array<Query, 16> queries{};
    UINT viewport_count{}, scissor_count{};
    std::array<D3D12_VIEWPORT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports{};
    std::array<D3D12_RECT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    void Reset(uint64_t next, bool is_direct) {
        *this = {};
        generation = next;
        recording_known = true;
        direct = is_direct;
        predication_known = renderpass_known = stencil_refs_known = queries_known = true;
    }
    void Invalidate() {
        tainted = true;
    }
    void Viewports(UINT count, const D3D12_VIEWPORT *values) {
        viewport_count = 0;
        if (count > viewports.size() || (count && !values))
            return;
        for (UINT i = 0; i < count; ++i)
            viewports[i] = values[i];
        viewport_count = count;
    }
    void Scissors(UINT count, const D3D12_RECT *values) {
        scissor_count = 0;
        if (count > scissors.size() || (count && !values))
            return;
        for (UINT i = 0; i < count; ++i)
            scissors[i] = values[i];
        scissor_count = count;
    }
    void Predication(bool enabled) {
        predication_known = true;
        predicated = enabled;
    }
    void BeginPass(bool unsupported = false) {
        if (renderpass || unsupported)
            tainted = true;
        renderpass_known = true;
        renderpass = true;
    }
    void EndPass() {
        if (!renderpass_known || !renderpass)
            tainted = true;
        renderpass_known = true;
        renderpass = false;
    }
    void StencilTracking(bool covered) {
        stencil_tracking = covered;
    }
    void StencilRefs(UINT front, UINT back) {
        front_ref = front;
        back_ref = back;
        stencil_refs_known = true;
    }
    void QueryBegin(uintptr_t heap, D3D12_QUERY_TYPE type, UINT index) {
        if (!queries_known)
            return;
        if (!heap || type == D3D12_QUERY_TYPE_TIMESTAMP || query_count >= queries.size()) {
            queries_known = false;
            return;
        }
        for (UINT i = 0; i < query_count; ++i)
            if (queries[i].heap == heap && queries[i].index == index) {
                queries_known = false;
                return;
            }
        queries[query_count++] = {heap, type, index};
    }
    void QueryEnd(uintptr_t heap, D3D12_QUERY_TYPE type, UINT index) {
        if (type == D3D12_QUERY_TYPE_TIMESTAMP || !queries_known)
            return;
        for (UINT i = 0; i < query_count; ++i)
            if (queries[i].heap == heap && queries[i].type == type && queries[i].index == index) {
                queries[i] = queries[--query_count];
                return;
            }
        queries_known = false;
    }
    const char *StencilStatus(UINT mask, bool per_face = false) const {
        if (!stencil_tracking)
            return "native-stencil-query-hooks-unproven";
        if (!recording_known || !direct || tainted || !stencil_refs_known)
            return "native-stencil-reference-unproven";
        if (!per_face && ((front_ref ^ back_ref) & mask & 255) != 0)
            return "native-stencil-references-asymmetric";
        if (!queries_known)
            return "native-query-state-unproven";
        if (query_count)
            return "native-counting-query-active";
        return "available";
    }
    bool StencilReady(UINT mask, bool per_face = false) const {
        return stencil_tracking && recording_known && direct && !tainted && stencil_refs_known &&
               (per_face || ((front_ref ^ back_ref) & mask & 255) == 0) && queries_known &&
               !query_count;
    }
    bool Available() const {
        return recording_known && direct && !tainted && viewport_count == 1 && scissor_count == 1 &&
               predication_known && !predicated && renderpass_known && !renderpass;
    }
};

// The original handles are audit metadata only. D3D12 consumes CPU OM
// descriptors at recording time; the game may immediately overwrite them.
// CPU-only mirrors are reused unless a snapshot still shares this generation.
struct GraphicsOutputMirror {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> handles{},
        original_handles{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{}, original_depth{};
    UINT count{};
    bool original_contiguous{}, depth_bound{}, valid{};
    bool Capture(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *source,
                 BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
        valid = false;
        count = 0;
        depth_bound = false;
        handles = {};
        original_handles = {};
        depth = {};
        original_depth = {};
        original_contiguous = contiguous != FALSE;
        if (n > handles.size() || (n && !source) || (dsv && !dsv->ptr))
            return false;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(cl->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        if (n && !rtv_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
            if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap))))
                return false;
        }
        if (dsv && !dsv_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            desc.NumDescriptors = 1;
            if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsv_heap))))
                return false;
        }
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto start =
            n ? rtv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        for (UINT i = 0; i < n; ++i) {
            auto from = contiguous ? D3D12_CPU_DESCRIPTOR_HANDLE{source[0].ptr + SIZE_T(i) * step}
                                   : source[i];
            if (!from.ptr)
                return false;
            original_handles[i] = from;
            handles[i] = {start.ptr + SIZE_T(i) * step};
            device->CopyDescriptorsSimple(1, handles[i], from, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (dsv) {
            original_depth = *dsv;
            depth = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            device->CopyDescriptorsSimple(1, depth, *dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            depth_bound = true;
        }
        count = n;
        valid = true;
        return true;
    }
};

struct GraphicsOutputSnapshot {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    std::shared_ptr<const GraphicsOutputMirror> outputs;
    GraphicsOutputProof proof;
    ID3D12GraphicsCommandList *command{};
    uint64_t generation{};
    bool valid{};
    const char *status = "not-captured";
    D3D12_CPU_DESCRIPTOR_HANDLE OriginalTarget() const {
        return outputs && outputs->count ? outputs->handles[0] : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE DepthTarget() const {
        return outputs && outputs->depth_bound ? outputs->depth : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    void Restore(ID3D12GraphicsCommandList *cl) const {
        if (!valid || !cl || cl != command)
            return;
        cl->SetPipelineState(pipeline.Get());
        cl->OMSetRenderTargets(outputs->count, outputs->handles.data(), FALSE,
                               outputs->depth_bound ? &outputs->depth : nullptr);
    }
};
} // namespace rrsss
