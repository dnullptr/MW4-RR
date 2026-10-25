#pragma once
#include "rr_gpu_common.h"
#include "rr_evaluation_params.h"
#include "feed_fg_identity.h"
#include "../build/rr_convert_guides.generated.h"
#include "../build/rr_convert_guides_test1.generated.h"
#include "../build/rr_convert_guides_test2.generated.h"
#include "../build/rr_convert_guides_test3.generated.h"
#include "../build/rr_convert_guides_test4.generated.h"
#include "../build/rr_convert_guides_test5.generated.h"
#include "../build/rr_convert_guides_test6.generated.h"
#include "rr_guide_test.h"
#include "../build/rr_live_begin.generated.h"
#include "../build/rr_live_validate.generated.h"
#include "../build/rr_live_display.generated.h"
#include "../build/rr_vrrt_native.generated.h"
#include "rr_vrrt_data.h"
#include "rr_sss_gpu.h"
#include "rr_transparency_guide.h"
#include <dxgi1_4.h>
#include "rr_sparse_copy.h"
#include "rr_specular_distance.h"
#include "rr_ray_density.h"
#include "rr_ray_copies.h"
namespace rrlive {
using Microsoft::WRL::ComPtr;
using rrprivate::Check;
using rrprivate::Require;
inline D3D12_RESOURCE_DESC TextureDesc(UINT w, UINT h, DXGI_FORMAT f) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}
inline void Change(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                   D3D12_RESOURCE_STATES b, UINT plane = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    if (a == b)
        return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, plane, a, b};
    cl->ResourceBarrier(1, &x);
}
inline void Order(ID3D12GraphicsCommandList *cl) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &b);
}
struct Budget {
    static constexpr UINT64 Minimum = 1024ull * 1024 * 1024;
    UINT64 limit = Minimum, bytes{};
    // A pool projection is not a DXGI reservation. Recheck each actual new
    // allocation; matching retained resources never enter this path.
    template <class Query> bool AdmitAllocation(UINT64 needed, Query query) const {
        if (!needed || needed == UINT64_MAX || needed > limit || bytes > limit - needed)
            return false;
        UINT64 available_budget{}, usage{};
        return query(available_budget, usage) && usage <= available_budget &&
               needed <= available_budget - usage;
    }
    static DXGI_MEMORY_SEGMENT_GROUP Segment(bool uma, D3D12_HEAP_TYPE type) {
        return uma || type == D3D12_HEAP_TYPE_DEFAULT ? DXGI_MEMORY_SEGMENT_GROUP_LOCAL
                                                      : DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL;
    }
    bool AdmitAllocation(ID3D12Device *device, UINT64 needed, D3D12_HEAP_TYPE type) const {
        if (!device || (type != D3D12_HEAP_TYPE_DEFAULT && type != D3D12_HEAP_TYPE_UPLOAD &&
                        type != D3D12_HEAP_TYPE_READBACK))
            return false;
        return AdmitAllocation(needed, [&](UINT64 &available_budget, UINT64 &usage) {
            D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter3> adapter;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                                   sizeof(architecture))) ||
                FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(
                    factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
                FAILED(adapter->QueryVideoMemoryInfo(0, Segment(architecture.UMA != FALSE, type),
                                                     &info)))
                return false;
            available_budget = info.Budget;
            usage = info.CurrentUsage;
            return true;
        });
    }
    bool Reserve(ID3D12Device *device, UINT64 required) {
        if (required <= limit)
            return true;
        if (!device || required < bytes)
            return false;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter3> adapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ||
            info.CurrentUsage > info.Budget || required - bytes > info.Budget - info.CurrentUsage)
            return false;
        limit = required;
        return true;
    }
};
struct Pipelines {
    rrsss::ResolvePipeline sss;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> converter_root, gate_root, probability_root;
    ComPtr<ID3D12PipelineState> converter, probability;
    // Guide test variants, indexed by rrguidetest::Mode - 1.
    std::array<ComPtr<ID3D12PipelineState>, rrguidetest::Count - 1> converter_tests;
    std::array<ComPtr<ID3D12PipelineState>, 3> gate;
    // The ray budget governor (shaders/rr_vrrt_governor.hlsl) on a game root signature, one PSO per
    // root. An entry keeps its root alive, so a cached address cannot be reused by another root;
    // `pipelines = {}` (after full retirement) releases them.
    struct GovernorEntry {
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        HRESULT failed = S_OK;
    };
    std::array<GovernorEntry, 4> governors;
    UINT stride{};
    void Initialize(ID3D12Device *d) {
        Require(d && !device, "live pipelines already initialized");
        device = d;
        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p[4]{};
        for (unsigned i = 0; i < 2; ++i) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[i].DescriptorTable = {1, &ranges[i]};
            p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[i + 2].Descriptor.ShaderRegister = i;
        }
        D3D12_ROOT_SIGNATURE_DESC rd{4, p};
        Root(rd, converter_root);
        Pipeline(converter_root.Get(), RRGuideShader, sizeof(RRGuideShader), converter);
        Pipeline(converter_root.Get(), RRGuideShaderTest1, sizeof(RRGuideShaderTest1),
                 converter_tests[0]);
        Pipeline(converter_root.Get(), RRGuideShaderTest2, sizeof(RRGuideShaderTest2),
                 converter_tests[1]);
        Pipeline(converter_root.Get(), RRGuideShaderTest3, sizeof(RRGuideShaderTest3),
                 converter_tests[2]);
        Pipeline(converter_root.Get(), RRGuideShaderTest4, sizeof(RRGuideShaderTest4),
                 converter_tests[3]);
        Pipeline(converter_root.Get(), RRGuideShaderTest5, sizeof(RRGuideShaderTest5),
                 converter_tests[4]);
        Pipeline(converter_root.Get(), RRGuideShaderTest6, sizeof(RRGuideShaderTest6),
                 converter_tests[5]);
        ranges[0].NumDescriptors = 3;
        ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER q[5]{};
        q[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (unsigned i = 0; i < 2; ++i) {
            q[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            q[i + 1].DescriptorTable = {1, &ranges[i]};
            q[i + 3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            q[i + 3].Descriptor.ShaderRegister = 1 + i;
        }
        rd = {5, q};
        Root(rd, gate_root);
        Pipeline(gate_root.Get(), RRLiveBegin, sizeof(RRLiveBegin), gate[0]);
        Pipeline(gate_root.Get(), RRLiveValidate, sizeof(RRLiveValidate), gate[1]);
        Pipeline(gate_root.Get(), RRLiveDisplay, sizeof(RRLiveDisplay), gate[2]);
    }
    void Root(const D3D12_ROOT_SIGNATURE_DESC &rd, ComPtr<ID3D12RootSignature> &r) {
        ComPtr<ID3DBlob> b, e;
        rrprivate::CheckAllocation(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e),
              "live root serialization failed");
        rrprivate::CheckAllocation(device->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(),
                                          IID_PPV_ARGS(&r)),
              "live root creation failed");
    }
    void ProbabilityPipeline() {
        if (probability)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p[4]{};
        for (unsigned i = 0; i < 2; ++i) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[i].DescriptorTable = {1, &ranges[i]};
            p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[i + 2].Descriptor.ShaderRegister = i;
        }
        D3D12_ROOT_SIGNATURE_DESC desc{4, p};
        Root(desc, probability_root);
        Pipeline(probability_root.Get(), RRVrrtProbability, sizeof(RRVrrtProbability), probability);
    }
    // pso, else why not: hr E_INVALIDARG (no root or device, or a root of another device; nothing
    // cached), E_OUTOFMEMORY (nothing cached, worth a retry), another failure of the creation
    // (cached: every later call answers the same), or `full` (every entry holds another root).
    struct GovernorPipeline {
        ID3D12PipelineState *pso{};
        HRESULT hr = S_OK;
        bool full{};
    };
    GovernorPipeline Governor(ID3D12RootSignature *root) noexcept {
        ComPtr<ID3D12Device> owner;
        if (!root || !device || FAILED(root->GetDevice(IID_PPV_ARGS(&owner))) ||
            !fg::SameNativeObject(owner.Get(), device.Get()))
            return {nullptr, E_INVALIDARG, false};
        GovernorEntry *empty{};
        for (auto &e : governors) {
            if (e.root.Get() == root)
                return {e.pso.Get(), e.pso ? S_OK : e.failed, false};
            if (!e.root && !empty)
                empty = &e;
        }
        if (!empty)
            return {nullptr, S_OK, true};
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root;
        d.CS = {RRVrrtGovernor, sizeof(RRVrrtGovernor)};
        empty->root = root;
        const HRESULT hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&empty->pso));
        if (SUCCEEDED(hr) && empty->pso)
            return {empty->pso.Get(), S_OK, false};
        if (hr == E_OUTOFMEMORY) {
            *empty = {};
            return {nullptr, E_OUTOFMEMORY, false};
        }
        empty->pso.Reset();
        empty->failed = FAILED(hr) ? hr : E_FAIL;
        return {nullptr, empty->failed, false};
    }
    void Pipeline(ID3D12RootSignature *r, const void *code, size_t bytes,
                  ComPtr<ID3D12PipelineState> &p) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = r;
        d.CS = {code, bytes};
        rrprivate::CheckAllocation(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&p)),
              "live pipeline creation failed");
    }
};
// Caller owns synchronization. All descriptors/resources are immutable from
// recording until BOTH recording retirement and GPU completion.
struct GpuFrame {
    // Reserve all mandatory prepass storage before the first sized allocation.
    // The query overload keeps overflow/accounting checks CPU-testable.
    template <class Query> static UINT64 RequiredPrepassBytes(UINT w, UINT h, Query query) {
        if (!rrvrrt::Dimensions(w, h))
            return 0;
        UINT64 total{};
        auto add = [&](const D3D12_RESOURCE_DESC &d) {
            const UINT64 n = query(d);
            if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
                return false;
            total += n;
            return true;
        };
        for (unsigned i = 0; i < 3; ++i)
            if (!add(TextureDesc(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT)))
                return 0;
        if (!add(TextureDesc(w, h, DXGI_FORMAT_R32_UINT)))
            return 0;
        auto distance = TextureDesc(w, h, DXGI_FORMAT_R16_FLOAT);
        distance.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (!add(distance))
            return 0;
        auto buffer = rrprivate::Buffer(256);
        if (!add(buffer))
            return 0;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!add(buffer))
            return 0;
        buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (!add(buffer))
            return 0;
        buffer = rrprivate::Buffer(rrvrrt::Bytes(w, h));
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return add(buffer) ? total : 0;
    }
    static UINT64 RequiredPrepassBytes(ID3D12Device *device, UINT w, UINT h) {
        return device ? RequiredPrepassBytes(
                            w, h,
                            [&](const D3D12_RESOURCE_DESC &d) {
                                return device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
                            })
                      : 0;
    }
    rrsss::Gpu sss;
    rrtrans::Gpu transparency;
    std::array<ComPtr<ID3D12Resource>, 4> sources, guides;
    std::array<ComPtr<ID3D12Resource>, 2> constants;
    std::array<UINT64, 2> offsets{};
    ComPtr<ID3D12Resource> probability;
    ComPtr<ID3D12DescriptorHeap> probability_heap;
    rrvrrt::Inputs probability_inputs;
    std::array<ComPtr<ID3D12Resource>, 4> sr;
    ComPtr<ID3D12Resource> rr_output, depth, depth_buffer, header, stats, readback;
    // Owned copy of the native specular hit distance; hit_source is retained
    // until the recording that copied it retires.
    ComPtr<ID3D12Resource> specular_hit, hit_source;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_fp{};
    UINT64 bytes{};
    UINT w{}, h{}, ow{}, oh{};
    UINT *header_map{};
    bool use_depth{}, hit_ready{};
    // The ray budget test (every offset is in rr_ray_copies.h): ray_cb (DEFAULT 64 KiB, COMMON
    // outside a patch) is the patched resolve's b1, ray_values (UPLOAD 256 B) stays mapped at
    // ray_map, ray_log (READBACK 256 B) collects the copies. ray_bytes is their share of `bytes`:
    // optional, so kept out of the pool projection. ray_sources holds the native buffers this
    // recording copied until Retire: resolve b1, b3, t0; apply b1, b3; patch b1.
    ComPtr<ID3D12Resource> ray_cb, ray_values, ray_log;
    UINT *ray_map{};
    UINT64 ray_bytes{};
    std::array<ComPtr<ID3D12Resource>, 6> ray_sources;
    // A site's ray_sources slots: from `first`, one per native rrraycopy::Source it reads (B1, B3,
    // Stats in that order).
    struct RaySite {
        unsigned first, slots;
    };
    static constexpr RaySite RayResolveSite{0, 3}, RayApplySite{3, 2}, RayPatchSite{5, 1};
    ComPtr<ID3D12Resource> Allocate(Pipelines &p, Budget &b, D3D12_RESOURCE_DESC d,
                                    D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
        auto info = p.device->GetResourceAllocationInfo(0, 1, &d);
        Require(info.SizeInBytes && info.SizeInBytes <= b.limit &&
                    b.bytes <= b.limit - info.SizeInBytes,
                "live frame-pool memory reserve exceeded");
        if (!b.AdmitAllocation(p.device.Get(), info.SizeInBytes, type))
            throw rrprivate::AllocationDeferred();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        ComPtr<ID3D12Resource> r;
        rrprivate::CheckAllocation(p.device->CreateCommittedResource(
                  &hp, D3D12_HEAP_FLAG_NONE, &d,
                  type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                  : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                     : D3D12_RESOURCE_STATE_COMMON,
                  nullptr, IID_PPV_ARGS(&r)),
              "live resource allocation failed");
        b.bytes += info.SizeInBytes;
        bytes += info.SizeInBytes;
        return r;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(const Pipelines &p, UINT at) const {
        return {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(at) * p.stride};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(const Pipelines &p, UINT at) const {
        return {heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(at) * p.stride};
    }
    void SRV(Pipelines &p, ID3D12Resource *r, UINT at, bool array = false) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = r->GetDesc().Format;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (array) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray.MipLevels = 1;
            v.Texture2DArray.ArraySize = 4;
        } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
        }
        p.device->CreateShaderResourceView(r, &v, Cpu(p, at));
    }
    void UAV(Pipelines &p, ID3D12Resource *r, UINT at) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = r->GetDesc().Format;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(r, nullptr, &v, Cpu(p, at));
    }
    void Prepare(Pipelines &p, Budget &b, const std::array<ComPtr<ID3D12Resource>, 4> &in,
                 const std::array<ComPtr<ID3D12Resource>, 2> &cb,
                 const std::array<UINT64, 2> &off) {
        hit_ready = false;
        hit_source.Reset();
        if (heap) {
            Require(w == in[3]->GetDesc().Width && h == in[3]->GetDesc().Height,
                    "live guide generation was not retired before size change");
            sources = in;
            constants = cb;
            offsets = off;
            for (unsigned i = 0; i < 4; ++i)
                SRV(p, sources[i].Get(), i, i < 3);
            return;
        }
        sources = in;
        constants = cb;
        offsets = off;
        w = UINT(in[3]->GetDesc().Width);
        h = in[3]->GetDesc().Height;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Check(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
              "live immutable descriptor heap failed");
        for (unsigned i = 0; i < 4; ++i) {
            SRV(p, sources[i].Get(), i, i < 3);
            guides[i] = Allocate(
                p, b,
                TextureDesc(w, h, i == 3 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT));
            UAV(p, guides[i].Get(), 4 + i);
        }
        SRV(p, guides[3].Get(), 8);
        auto distance = TextureDesc(w, h, DXGI_FORMAT_R16_FLOAT);
        distance.Flags = D3D12_RESOURCE_FLAG_NONE;
        specular_hit = Allocate(p, b, distance);
        header = Allocate(p, b, rrprivate::Buffer(256), D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE empty{};
        void *m{};
        Check(header->Map(0, &empty, &m), "live association upload map failed");
        header_map = (UINT *)m;
        memset(m, 0, 256);
        auto bd = rrprivate::Buffer(256);
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        stats = Allocate(p, b, bd);
        readback = Allocate(p, b, rrprivate::Buffer(256), D3D12_HEAP_TYPE_READBACK);
    }

    // Copy-only: records no shader state. Caller proves the native dispatch,
    // its bindings and the recording; this checks extent, format and state.
    bool CopySpecularDistance(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *source,
                              D3D12_RESOURCE_STATES source_state) {
        if (!cl || !source || !specular_hit || hit_ready ||
            !rrspecular::ValidSource(source->GetDesc(), w, h, source_state))
            return false;
        ComPtr<ID3D12Device> device;
        if (FAILED(source->GetDevice(IID_PPV_ARGS(&device))) ||
            !fg::SameNativeObject(device.Get(), p.device.Get()))
            return false;
        hit_source = source;
        hit_ready = true;
        Change(cl, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Change(cl, specular_hit.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(specular_hit.Get(), source);
        Change(cl, specular_hit.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Change(cl, source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
        return true;
    }
    void Convert(ID3D12GraphicsCommandList *cl, Pipelines &p) {
        Order(cl);
        for (unsigned i = 2; i < 4; ++i)
            Change(cl, sources[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto &r : guides)
            Change(cl, r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.converter_root.Get());
        const auto test = rrguidetest::Valid(rrguidetest::mode.load(std::memory_order_relaxed));
        cl->SetPipelineState(test == rrguidetest::Off ? p.converter.Get()
                                                      : p.converter_tests[test - 1].Get());
        cl->SetComputeRootDescriptorTable(0, Gpu(p, 0));
        cl->SetComputeRootDescriptorTable(1, Gpu(p, 4));
        for (unsigned i = 0; i < 2; ++i)
            cl->SetComputeRootConstantBufferView(2 + i,
                                                 constants[i]->GetGPUVirtualAddress() + offsets[i]);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        for (auto &r : guides)
            Change(cl, r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        for (unsigned i = 2; i < 4; ++i)
            Change(cl, sources[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void Probability(ID3D12GraphicsCommandList *cl, Pipelines &p, Budget &b,
                     const rrvrrt::Inputs &in) {
        Require(in.w == w && in.h == h && rrvrrt::Dimensions(w, h),
                "native VRRT guide dimensions unsupported");
        p.ProbabilityPipeline();
        if (!probability) {
            auto desc = rrprivate::Buffer(rrvrrt::Bytes(w, h));
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            probability = Allocate(p, b, desc);
        }
        if (!probability_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            Check(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&probability_heap)),
                  "native VRRT immutable descriptor heap");
        }
        probability_inputs = in;
        auto cpu = probability_heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = probability_heap->GetGPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p.device->CreateShaderResourceView(in.resources[0].Get(), &srv, cpu);
        srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = in.budget_offset / 4;
        srv.Buffer.NumElements = UINT(in.budget_bytes / 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        p.device->CreateShaderResourceView(in.resources[1].Get(), &srv, {cpu.ptr + p.stride});
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = UINT(probability->GetDesc().Width / 4);
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        p.device->CreateUnorderedAccessView(probability.Get(), nullptr, &u,
                                            {cpu.ptr + 2 * p.stride});
        u = {};
        u.Format = DXGI_FORMAT_R32_UINT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(guides[3].Get(), nullptr, &u, {cpu.ptr + 3 * p.stride});
        Require(stats && header, "native VRRT rate statistics unavailable");
        u = {};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = UINT(stats->GetDesc().Width / 4);
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        p.device->CreateUnorderedAccessView(stats.Get(), nullptr, &u, {cpu.ptr + 4 * p.stride});
        // All allocations/descriptors/references exist before recording any change.
        Change(cl, probability.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Change(cl, guides[3].Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Zero the rate statistics from the header's never-written tail.
        constexpr UINT64 rates = rrdensity::Offset, rate_bytes = 256 - rrdensity::Offset;
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyBufferRegion(stats.Get(), rates, header.Get(), rates, rate_bytes);
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {probability_heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.probability_root.Get());
        cl->SetPipelineState(p.probability.Get());
        cl->SetComputeRootDescriptorTable(0, gpu);
        cl->SetComputeRootDescriptorTable(1, {gpu.ptr + 2 * p.stride});
        for (unsigned i = 0; i < 2; ++i)
            cl->SetComputeRootConstantBufferView(2 + i, in.cbv[i]);
        cl->Dispatch((w + 31) / 32, (h + 15) / 16, 1);
        Change(cl, probability.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
        Change(cl, guides[3].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    }
    void PrepareSR(Pipelines &p, Budget &b, const rreval::Options &o,
                   const std::array<ComPtr<ID3D12Resource>, 4> &in) {
        sr = in;
        use_depth = sr[1]->GetDesc().Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        if (rr_output)
            Require(ow == o.out_width && oh == o.out_height,
                    "live output generation was not retired before size change");
        else {
            ow = o.out_width;
            oh = o.out_height;
            rr_output = Allocate(p, b, TextureDesc(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT));
            SRV(p, rr_output.Get(), 9);
        }
        SRV(p, sr[0].Get(), 10);
        UAV(p, sr[3].Get(), 11);
        if (use_depth && depth) {
            const auto cached = depth->GetDesc(), actual = sr[1]->GetDesc();
            Require(cached.Width == actual.Width && cached.Height == actual.Height,
                    "live depth generation was not retired before size change");
        }
        if (use_depth && !depth) {
            auto native = sr[1]->GetDesc();
            UINT rows{};
            UINT64 width{}, size{};
            p.device->GetCopyableFootprints(&native, 0, 1, 0, &depth_fp, &rows, &width, &size);
            Require(depth_fp.Footprint.Format == DXGI_FORMAT_R32_TYPELESS &&
                        rows == native.Height && width == native.Width * 4 && size,
                    "live depth plane footprint invalid");
            depth_buffer = Allocate(p, b, rrprivate::Buffer(size));
            auto d = TextureDesc(UINT(native.Width), native.Height, DXGI_FORMAT_R32_FLOAT);
            d.Flags = D3D12_RESOURCE_FLAG_NONE;
            depth = Allocate(p, b, d);
        }
    }
    void Depth(ID3D12GraphicsCommandList *cl, D3D12_RESOURCE_STATES source_state) {
        if (!use_depth)
            return;
        Change(cl, sr[1].Get(), source_state, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
        Change(cl, depth_buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = depth_buffer.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = depth_fp;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = sr[1].Get();
        cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Change(cl, sr[1].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source_state, 0);
        Change(cl, depth_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
        Change(cl, depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        from = to;
        to = {};
        to.pResource = depth.Get();
        cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Change(cl, depth_buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);
        Change(cl, depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void BindGate(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *history) {
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.gate_root.Get());
        cl->SetComputeRootConstantBufferView(0, header->GetGPUVirtualAddress());
        cl->SetComputeRootDescriptorTable(1, Gpu(p, 8));
        cl->SetComputeRootDescriptorTable(2, Gpu(p, 11));
        cl->SetComputeRootUnorderedAccessView(3, stats->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(4, history->GetGPUVirtualAddress());
    }
    void GateBegin(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *history) {
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Change(cl, history, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto &r : guides)
            Change(cl, r.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        BindGate(cl, p, history);
        cl->SetPipelineState(p.gate[0].Get());
        cl->Dispatch(1, 1, 1);
        Order(cl);
        cl->SetPipelineState(p.gate[1].Get());
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Order(cl);
        Change(cl, rr_output.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void GateEnd(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *history,
                 bool inference_ok) {
        if (inference_ok) {
            Change(cl, rr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            BindGate(cl, p, history);
            cl->SetPipelineState(p.gate[2].Get());
            cl->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
            Order(cl);
            Change(cl, rr_output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        } else
            Change(cl, rr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COMMON);
        for (auto &r : guides)
            Change(cl, r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        if (use_depth)
            Change(cl, depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        Change(cl, history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(readback.Get(), 0, stats.Get(), 0, 256);
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    // tolerate: count isolated unusable guide pixels instead of rejecting the frame.
    // responsivity: value the SSS resolve writes to the responsivity mask on SSS pixels.
    void Header(UINT serial, bool reset, bool replacement = false, UINT color_x = 0,
                UINT color_y = 0, bool tolerate = false, float responsivity = 0) {
        UINT strength{};
        memcpy(&strength, &responsivity, sizeof(strength));
        UINT c[] = {w,       h,       ow, oh, serial, reset ? 1u : 0u, 0, 0, replacement ? 1u : 0u,
                    color_x, color_y, tolerate ? 1u : 0u, strength, 0, 0, 0};
        memcpy(header_map, c, sizeof(c));
    }
    void Accept(bool yes) {
        header_map[6] = yes ? 1u : 0u;
    }
    // bad frame, copied frame, isolated guide pixels, sanitized SSS samples
    std::array<UINT, 4> ReadStats() {
        std::array<UINT, 4> v{};
        void *m{};
        D3D12_RANGE r{0, 16};
        Check(readback->Map(0, &r, &m), "live stats map failed");
        memcpy(v.data(), m, 16);
        r = {0, 0};
        readback->Unmap(0, &r);
        return v;
    }
    // Valid only for a frame whose native VRRT reader ran (see Probability).
    rrdensity::Frame ReadRates() {
        rrdensity::Frame v{};
        void *m{};
        D3D12_RANGE r{rrdensity::Offset, rrdensity::Offset + sizeof(v)};
        Check(readback->Map(0, &r, &m), "live rate statistics map failed");
        memcpy(v.data(), static_cast<const char *>(m) + rrdensity::Offset, sizeof(v));
        r = {0, 0};
        readback->Unmap(0, &r);
        return v;
    }
    // Ensures ray_log and ray_values (mapped once). False, with nothing allocated, when a size is
    // unusable or the ledger cannot grow; Allocate may still throw AllocationDeferred or
    // runtime_error. Idempotent.
    bool PrepareRayLog(Pipelines &p, Budget &b) {
        using rrprivate::Buffer;
        if (!AllocateRay(p, b,
                         {{&ray_log, Buffer(rrraycopy::LogBytes), D3D12_HEAP_TYPE_READBACK},
                          {&ray_values, Buffer(rrraycopy::ValuesBytes), D3D12_HEAP_TYPE_UPLOAD}}))
            return false;
        if (!ray_map) {
            D3D12_RANGE empty{};
            void *m{};
            Check(ray_values->Map(0, &empty, &m), "ray budget upload map failed");
            memset(m, 0, rrraycopy::ValuesBytes);
            ray_map = static_cast<UINT *>(m);
        }
        return true;
    }
    // PrepareRayLog, then ray_cb the same way. Idempotent.
    bool PrepareRayPatch(Pipelines &p, Budget &b) {
        const RayBuffer cb{&ray_cb, rrprivate::Buffer(rrraycopy::CbBytes), D3D12_HEAP_TYPE_DEFAULT};
        return PrepareRayLog(p, b) && AllocateRay(p, b, {cb});
    }
    // CPU writes into ray_values while this slot records (not in flight), like header_map.
    void WriteRaySerial(UINT serial) noexcept {
        if (ray_map)
            ray_map[rrraycopy::ValuesSerial / 4] = serial;
    }
    void WriteRayPlan(const rrraybudget::Plan &plan) noexcept {
        if (!ray_map)
            return;
        const std::array<UINT, 4> governor{plan.mask, plan.headroom[0], plan.headroom[1], 0};
        static_assert(rrraycopy::ValuesTargets + sizeof(plan.bits) <= rrraycopy::ValuesSerial &&
                          sizeof(governor) == rrraycopy::GovernorBytes,
                      "rr_ray_copies.h's ray_values layout");
        memcpy(reinterpret_cast<char *>(ray_map) + rrraycopy::ValuesTargets, plan.bits.data(),
               sizeof(plan.bits));
        memcpy(reinterpret_cast<char *>(ray_map) + rrraycopy::ValuesGovernor, governor.data(),
               sizeof(governor));
    }
    // The logging copies at the native resolve and at the native VRRT apply: the recorded Log bits,
    // 0 when nothing was recorded (no ray_log/ray_map, this site already recorded in this
    // recording, or nothing to copy). A source outside its buffer is dropped; t0 is copied only
    // when its tracked state is a known read-only shader-resource state, inside a transition pair
    // to state | COPY_SOURCE unless it already has COPY_SOURCE.
    unsigned LogResolve(ID3D12GraphicsCommandList *cl, const rrvrrt::RayConstant &b1,
                        const rrvrrt::RayConstant &b3, const rrvrrt::RayStats &stats) noexcept {
        if (!cl || !ray_log || !ray_map || RaySiteUsed(RayResolveSite))
            return 0;
        auto from = RayOwn();
        from.Set(rrraycopy::Source::B1, b1.resource.Get(), b1.offset);
        from.Set(rrraycopy::Source::B3, b3.resource.Get(), b3.offset);
        from.Set(rrraycopy::Source::Stats, stats.resource.Get(), stats.offset);
        const bool readable = stats.known && rrsparsecopy::Readable(stats.state);
        const auto list = rrraycopy::ResolveLog(
            RayAvail(b1.resource.Get(), b1.offset, b1.avail), b1.upload,
            RayAvail(b3.resource.Get(), b3.offset, b3.avail), b3.upload,
            readable ? RayAvail(stats.resource.Get(), stats.offset, stats.avail) : 0);
        return RecordRayLog(cl, list, from, RayResolveSite, stats.state);
    }
    unsigned LogApply(ID3D12GraphicsCommandList *cl, const rrvrrt::RayConstant &b1,
                      const rrvrrt::RayConstant &b3) noexcept {
        if (!cl || !ray_log || !ray_map || RaySiteUsed(RayApplySite))
            return 0;
        auto from = RayOwn();
        from.Set(rrraycopy::Source::B1, b1.resource.Get(), b1.offset);
        from.Set(rrraycopy::Source::B3, b3.resource.Get(), b3.offset);
        const auto list =
            rrraycopy::ApplyLog(RayAvail(b1.resource.Get(), b1.offset, b1.avail), b1.upload,
                                RayAvail(b3.resource.Get(), b3.offset, b3.avail), b3.upload);
        return RecordRayLog(cl, list, from, RayApplySite, {});
    }
    // ray_cb's GPU VA after recording the copies that fill it (the game's first 608 bytes of b1
    // with the plan's targets and governor constants), its barrier to VERTEX_AND_CONSTANT_BUFFER |
    // COPY_SOURCE and the Executed copy; 0 with nothing recorded. The caller binds the VA,
    // dispatches, runs the governor, restores, then calls EndPatch.
    UINT64 PatchResolve(ID3D12GraphicsCommandList *cl, const rrvrrt::RayConstant &b1,
                        const rrraybudget::Plan &plan) noexcept {
        if (!cl || !ray_cb || !ray_values || !ray_map || !ray_log || !b1.resource || !b1.upload ||
            b1.avail < rrraycopy::GameBytes ||
            RayAvail(b1.resource.Get(), b1.offset, b1.avail) < rrraycopy::GameBytes ||
            plan.mask < 1 || plan.mask > 3 || RaySiteUsed(RayPatchSite))
            return 0;
        auto from = RayOwn();
        from.Set(rrraycopy::Source::B1, b1.resource.Get(), b1.offset);
        const auto fill = rrraycopy::PatchFill(plan.mask);
        const UINT64 va = ray_cb->GetGPUVirtualAddress();
        if (!fill.count || !from.Covers(fill) || !from.Covers(rrraycopy::Executed) || !va)
            return 0;
        ray_sources[RayPatchSite.first] = b1.resource;
        WriteRayPlan(plan);
        Change(cl, ray_cb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        for (unsigned i = 0; i < fill.count; ++i)
            RecordRay(cl, fill.copies[i], from);
        Change(cl, ray_cb.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_COPY_SOURCE);
        RecordRay(cl, rrraycopy::Executed, from);
        return va;
    }
    // After a PatchResolve that returned a VA, once the patched dispatch and the governor are
    // recorded.
    void EndPatch(ID3D12GraphicsCommandList *cl) noexcept {
        if (cl && ray_cb)
            Change(cl, ray_cb.Get(),
                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                       D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
    }
    // The gate's words of ray_log. Valid only after the recording's GPU work completed.
    bool ReadRayLog(std::array<std::uint32_t, rrraybudget::LogWords> &out) noexcept {
        static_assert(sizeof(out) == rrraycopy::LogRead, "the gate's words of the ray log");
        out = {};
        if (!ray_log)
            return false;
        void *m{};
        D3D12_RANGE r{0, rrraycopy::LogRead};
        if (FAILED(ray_log->Map(0, &r, &m)))
            return false;
        if (m)
            memcpy(out.data(), m, sizeof(out));
        r = {0, 0};
        ray_log->Unmap(0, &r);
        return m != nullptr;
    }
    void Retire() {
        sources = {};
        constants = {};
        sr = {};
        probability_inputs = {};
        hit_source.Reset();
        hit_ready = false;
        ray_sources = {};
    }
    void Release(Budget &b) {
        if (header_map)
            header->Unmap(0, nullptr);
        if (ray_map)
            ray_values->Unmap(0, nullptr);
        Require(b.bytes >= bytes, "live allocation ledger underflow");
        b.bytes -= bytes;
        *this = {};
    }

  private:
    // What each rrraycopy::Source reads: a buffer and the byte its offsets count from.
    struct RayFrom {
        std::array<ID3D12Resource *, 5> resource{};
        std::array<UINT64, 5> start{};
        void Set(rrraycopy::Source s, ID3D12Resource *r, UINT64 offset) noexcept {
            resource[unsigned(s)] = r;
            start[unsigned(s)] = offset;
        }
        bool Covers(const rrraycopy::Copy &c) const noexcept {
            return unsigned(c.source) < resource.size() && resource[unsigned(c.source)];
        }
        bool Covers(const rrraycopy::List &l) const noexcept {
            if (l.count > l.copies.size())
                return false;
            for (unsigned i = 0; i < l.count; ++i)
                if (!Covers(l.copies[i]))
                    return false;
            return true;
        }
    };
    struct RayBuffer {
        ComPtr<ID3D12Resource> *resource;
        D3D12_RESOURCE_DESC desc;
        D3D12_HEAP_TYPE type;
    };
    // Allocates the missing `buffers` after reserving their sum in the ledger (overflow-checked).
    bool AllocateRay(Pipelines &p, Budget &b, std::initializer_list<RayBuffer> buffers) {
        if (!p.device)
            return false;
        UINT64 needed{};
        for (const auto &x : buffers)
            if (!*x.resource) {
                const UINT64 n = p.device->GetResourceAllocationInfo(0, 1, &x.desc).SizeInBytes;
                if (!n || n == UINT64_MAX || needed > UINT64_MAX - n)
                    return false;
                needed += n;
            }
        if (!needed)
            return true;
        if (b.bytes > UINT64_MAX - needed || !b.Reserve(p.device.Get(), b.bytes + needed))
            return false;
        for (const auto &x : buffers)
            if (!*x.resource) {
                const UINT64 before = bytes;
                *x.resource = Allocate(p, b, x.desc, x.type);
                ray_bytes += bytes - before;
            }
        return true;
    }
    RayFrom RayOwn() const noexcept {
        RayFrom f;
        f.Set(rrraycopy::Source::Values, ray_values.Get(), 0);
        f.Set(rrraycopy::Source::Cb, ray_cb.Get(), 0);
        return f;
    }
    // `avail` when [offset, offset + avail) lies inside the buffer `r`, else 0 (no copy reads it).
    static UINT64 RayAvail(ID3D12Resource *r, UINT64 offset, UINT64 avail) noexcept {
        if (!r)
            return 0;
        const auto d = r->GetDesc();
        return d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && offset <= d.Width &&
                       avail <= d.Width - offset
                   ? avail
                   : 0;
    }
    bool RaySiteUsed(const RaySite &site) const noexcept {
        for (unsigned i = site.first; i < site.first + site.slots && i < ray_sources.size(); ++i)
            if (ray_sources[i])
                return true;
        return false;
    }
    // The ray_sources slot of a native source at `site`, or nullptr when the site has none for it.
    ComPtr<ID3D12Resource> *RaySlot(const RaySite &site, rrraycopy::Source s) noexcept {
        const auto at = site.first + unsigned(s);
        return unsigned(s) <= unsigned(rrraycopy::Source::Stats) && unsigned(s) < site.slots &&
                       at < ray_sources.size()
                   ? &ray_sources[at]
                   : nullptr;
    }
    // One copy of the plan; the caller has checked it (RayFrom::Covers) before the first command.
    void RecordRay(ID3D12GraphicsCommandList *cl, const rrraycopy::Copy &c,
                   const RayFrom &from) noexcept {
        const auto s = unsigned(c.source);
        cl->CopyBufferRegion(c.target == rrraycopy::Target::Log ? ray_log.Get() : ray_cb.Get(),
                             c.at, from.resource[s], from.start[s] + c.from, c.bytes);
    }
    static bool Native(rrraycopy::Source s) noexcept {
        return s == rrraycopy::Source::B1 || s == rrraycopy::Source::B3 ||
               s == rrraycopy::Source::Stats;
    }
    // LogResolve/LogApply after their checks: retains the native sources in the site's
    // ray_sources slots, then records the list; the t0 copies (Source::Stats) go last, inside the
    // transition pair unless `stats_state` already has COPY_SOURCE.
    unsigned RecordRayLog(ID3D12GraphicsCommandList *cl, const rrraycopy::List &list,
                          const RayFrom &from, const RaySite &site,
                          D3D12_RESOURCE_STATES stats_state) noexcept {
        if (!list.count || !from.Covers(list))
            return 0;
        bool stats = false;
        for (unsigned i = 0; i < list.count; ++i) {
            const auto s = list.copies[i].source;
            stats = stats || s == rrraycopy::Source::Stats;
            if (Native(s) && !RaySlot(site, s))
                return 0;
        }
        for (unsigned i = 0; i < list.count; ++i)
            if (const auto s = list.copies[i].source; Native(s))
                *RaySlot(site, s) = from.resource[unsigned(s)];
        for (unsigned i = 0; i < list.count; ++i)
            if (list.copies[i].source != rrraycopy::Source::Stats)
                RecordRay(cl, list.copies[i], from);
        if (stats) {
            auto *t0 = from.resource[unsigned(rrraycopy::Source::Stats)];
            const auto read = stats_state | D3D12_RESOURCE_STATE_COPY_SOURCE;
            Change(cl, t0, stats_state, read);
            for (unsigned i = 0; i < list.count; ++i)
                if (list.copies[i].source == rrraycopy::Source::Stats)
                    RecordRay(cl, list.copies[i], from);
            Change(cl, t0, read, stats_state);
        }
        return list.bits;
    }
};
inline bool SlotReservationParts(ID3D12Device *device, const rrlive::GpuFrame &frame, UINT64 limit,
                                 UINT64 &base, UINT64 &depth) {
    base = depth = 0;
    if (!device || frame.bytes > limit)
        return false;
    base = frame.bytes;
    if (!frame.depth && !frame.depth_buffer)
        return true;
    if (!frame.depth || !frame.depth_buffer || !frame.w || !frame.h)
        return false;
    if (!rrsparsecopy::SameDevice(device, frame.depth.Get()) ||
        !rrsparsecopy::SameDevice(device, frame.depth_buffer.Get()))
        return false;
    const auto texture = frame.depth->GetDesc(), buffer = frame.depth_buffer->GetDesc();
    const auto &footprint = frame.depth_fp.Footprint;
    const bool has_footprint = footprint.Width || footprint.Height;
    const UINT64 depth_width = has_footprint ? footprint.Width : frame.w;
    const UINT depth_height = has_footprint ? footprint.Height : frame.h;
    if (has_footprint &&
        (!footprint.Width || !footprint.Height || footprint.Depth != 1 ||
         footprint.Format != DXGI_FORMAT_R32_TYPELESS || frame.w > footprint.Width ||
         frame.h > footprint.Height || UINT64(footprint.Width) * 4 > footprint.RowPitch ||
         footprint.RowPitch % 256))
        return false;

    if (texture.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || texture.Width != depth_width ||
        texture.Height != depth_height || texture.DepthOrArraySize != 1 || texture.MipLevels != 1 ||
        texture.SampleDesc.Count != 1 || texture.SampleDesc.Quality ||
        texture.Format != DXGI_FORMAT_R32_FLOAT || texture.Flags != D3D12_RESOURCE_FLAG_NONE)
        return false;
    if (buffer.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !buffer.Width ||
        buffer.Height != 1 || buffer.DepthOrArraySize != 1 || buffer.MipLevels != 1 ||
        buffer.SampleDesc.Count != 1 || buffer.SampleDesc.Quality ||
        buffer.Format != DXGI_FORMAT_UNKNOWN || buffer.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
        buffer.Flags != D3D12_RESOURCE_FLAG_NONE)
        return false;
    if (has_footprint) {
        const UINT64 last_row =
            UINT64(footprint.RowPitch) * (footprint.Height - 1) + UINT64(footprint.Width) * 4;
        if (frame.depth_fp.Offset > buffer.Width || last_row > buffer.Width - frame.depth_fp.Offset)
            return false;
    }
    for (const auto &desc : {texture, buffer}) {
        const auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (!size || size == UINT64_MAX || size > base || size > limit - depth)
            return false;
        base -= size;
        depth += size;
    }
    return true;
}
} // namespace rrlive
