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
#include "../build/rr_convert_guides_test7.generated.h"
#include "../build/rr_convert_guides_test8.generated.h"
#include "../build/rr_convert_guides_test9.generated.h"
#include "../build/rr_sky_depth_floor.generated.h"
#include "../build/rr_color_scale.generated.h"
#include "rr_guide_test.h"
#include "../build/rr_live_begin.generated.h"
#include "../build/rr_live_validate.generated.h"
#include "../build/rr_live_display.generated.h"
#include "../build/rr_vrrt_native.generated.h"
#include "../build/rr_sky_edge_mask.generated.h"
#include "../build/rr_water_masks.generated.h"
#include "rr_vrrt_data.h"
#include "rr_sss_gpu.h"
#include "rr_transparency_guide.h"
#include <dxgi1_4.h>
#include "rr_sparse_copy.h"
#include "rr_specular_distance.h"
#include "rr_ray_density.h"
#include "rr_water.h"
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
    ComPtr<ID3D12RootSignature> converter_root, gate_root, probability_root, sky_edge_root,
        test_root, water_root;
    ComPtr<ID3D12PipelineState> converter, probability, sky_edge, depth_floor, color_scale,
        water;
    // Guide test variants, indexed by rrguidetest::Mode - 1.
    std::array<ComPtr<ID3D12PipelineState>, rrguidetest::Count - 1> converter_tests;
    std::array<ComPtr<ID3D12PipelineState>, 3> gate;
    UINT stride{};
    void Initialize(ID3D12Device *d) {
        Require(d && !device, "live pipelines already initialized");
        device = d;
        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 0}};
        // u4 water classification (heap WaterSlot) and b2 water guide mode.
        D3D12_DESCRIPTOR_RANGE water_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, 0};
        D3D12_ROOT_PARAMETER p[6]{};
        for (unsigned i = 0; i < 2; ++i) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[i].DescriptorTable = {1, &ranges[i]};
            p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[i + 2].Descriptor.ShaderRegister = i;
        }
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[4].DescriptorTable = {1, &water_range};
        p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[5].Constants = {2, 0, 4};
        D3D12_ROOT_SIGNATURE_DESC rd{6, p};
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
        Pipeline(converter_root.Get(), RRGuideShaderTest7, sizeof(RRGuideShaderTest7),
                 converter_tests[6]);
        Pipeline(converter_root.Get(), RRGuideShaderTest8, sizeof(RRGuideShaderTest8),
                 converter_tests[7]);
        Pipeline(converter_root.Get(), RRGuideShaderTest9, sizeof(RRGuideShaderTest9),
                 converter_tests[8]);
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
        // Sky edge mask: Status SRV table, mask UAV table, 4 root constants.
        ranges[0].NumDescriptors = 1;
        ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER e[3]{};
        for (unsigned i = 0; i < 2; ++i) {
            e[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            e[i].DescriptorTable = {1, &ranges[i]};
        }
        e[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        e[2].Constants = {0, 0, 4};
        rd = {3, e};
        Root(rd, sky_edge_root);
        Pipeline(sky_edge_root.Get(), RRSkyEdgeMask, sizeof(RRSkyEdgeMask), sky_edge);
        // Color scale and sky depth floor tests: source SRV table, target UAV
        // table, 8 root constants.
        e[2].Constants = {0, 0, 8};
        Root(rd, test_root);
        Pipeline(test_root.Get(), RRColorScale, sizeof(RRColorScale), color_scale);
        Pipeline(test_root.Get(), RRSkyDepthFloor, sizeof(RRSkyDepthFloor), depth_floor);
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
    void WaterPipeline() {
        if (water)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0}};
        D3D12_ROOT_PARAMETER p[4]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (unsigned i = 0; i < 2; ++i) {
            p[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[i + 1].DescriptorTable = {1, &ranges[i]};
        }
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[3].Descriptor.ShaderRegister = 2;
        D3D12_ROOT_SIGNATURE_DESC desc{4, p};
        Root(desc, water_root);
        Pipeline(water_root.Get(), RRWaterMasks, sizeof(RRWaterMasks), water);
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
    // Heap slots for the development tests (TestPass): the depth floor's source,
    // then one target per test, so tests in the same frame never share one.
    static constexpr UINT DepthSource = 12, SkyEdgeTarget = 13, ColorTarget = 14,
                          DepthTarget = 15;
    // The guide shader's u4 water classification (WaterTarget).
    static constexpr UINT WaterSlot = 16;
    // Guide test mode Convert used for this frame (rrguidetest::Mode).
    unsigned guide_test{};
    // Water guide mode Convert used for this frame (rrwater::Guide).
    unsigned water_guide{};
    // Water tests (rr_water.h), allocated only once a test wants them: the
    // guide shader's classification and the masks rr_water_masks.hlsl writes.
    // bytes is their own optional term in the pool reservations, like SSS.
    struct Water {
        ComPtr<ID3D12Resource> mask, responsivity, disocclusion;
        ComPtr<ID3D12DescriptorHeap> heap;
        UINT64 bytes{};
        bool ready{};
    } water;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_fp{};
    UINT64 bytes{};
    UINT w{}, h{}, ow{}, oh{};
    UINT *header_map{};
    bool use_depth{}, hit_ready{}, water_written{};
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
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 17,
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
        // stats (gate, rr_live_gate.hlsl): bytes 0-15 ReadStats, 16-23 ReadFinite, 64+ ReadRates.
        auto bd = rrprivate::Buffer(256);
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        stats = Allocate(p, b, bd);
        readback = Allocate(p, b, rrprivate::Buffer(256), D3D12_HEAP_TYPE_READBACK);
    }
    // Water test storage for a w x h guide: the classification and two masks.
    static UINT64 WaterBytes(ID3D12Device *device, UINT w, UINT h) {
        if (!device)
            return 0;
        const D3D12_RESOURCE_DESC descs[] = {TextureDesc(w, h, DXGI_FORMAT_R8_UNORM),
                                             TextureDesc(w, h, DXGI_FORMAT_R16_FLOAT),
                                             TextureDesc(w, h, DXGI_FORMAT_R16_FLOAT)};
        UINT64 total{};
        for (const auto &d : descs) {
            const auto n = device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
                return 0;
            total += n;
        }
        return total;
    }
    // Optional water storage, after Prepare. The caller reserves the pool-wide
    // budget first; this rechecks this frame's share and returns false on a
    // shortage, or throws AllocationDeferred under GPU memory pressure. The
    // controller then keeps the water tests waiting and retries later.
    bool PrepareWater(Pipelines &p, Budget &b) {
        if (water.ready)
            return true;
        const auto mask = TextureDesc(w, h, DXGI_FORMAT_R8_UNORM),
                   value = TextureDesc(w, h, DXGI_FORMAT_R16_FLOAT);
        UINT64 need{};
        auto add = [&](const ComPtr<ID3D12Resource> &r, const D3D12_RESOURCE_DESC &d) {
            if (r)
                return true;
            const auto n = p.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            if (!n || n == UINT64_MAX || need > UINT64_MAX - n)
                return false;
            need += n;
            return true;
        };
        if (!add(water.mask, mask) || !add(water.responsivity, value) ||
            !add(water.disocclusion, value) || need > UINT64_MAX - b.bytes ||
            !b.Reserve(p.device.Get(), b.bytes + need))
            return false;
        p.WaterPipeline();
        auto take = [&](ComPtr<ID3D12Resource> &r, const D3D12_RESOURCE_DESC &d) {
            if (r)
                return;
            r = Allocate(p, b, d);
            water.bytes += p.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
        };
        take(water.mask, mask);
        take(water.responsivity, value);
        take(water.disocclusion, value);
        if (!water.heap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            rrprivate::CheckAllocation(
                p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&water.heap)),
                "water descriptor heap failed");
        }
        water.ready = true;
        return true;
    }
    // The guide shader's u4 (heap WaterSlot): the classification, or a null view that
    // drops its writes when no water test wants it this frame.
    void WaterTarget(Pipelines &p, bool wanted) {
        water_written = wanted && water.ready;
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R8_UNORM;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(water_written ? water.mask.Get() : nullptr, nullptr,
                                            &v, Cpu(p, WaterSlot));
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
    // guide: rrwater::Guide for water pixels.
    void Convert(ID3D12GraphicsCommandList *cl, Pipelines &p, unsigned guide = 0) {
        Order(cl);
        for (unsigned i = 2; i < 4; ++i)
            Change(cl, sources[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto &r : guides)
            Change(cl, r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (water_written)
            Change(cl, water.mask.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.converter_root.Get());
        const auto test = rrguidetest::Valid(rrguidetest::mode.load(std::memory_order_relaxed));
        guide_test = test;
        cl->SetPipelineState(test == rrguidetest::Off ? p.converter.Get()
                                                      : p.converter_tests[test - 1].Get());
        cl->SetComputeRootDescriptorTable(0, Gpu(p, 0));
        cl->SetComputeRootDescriptorTable(1, Gpu(p, 4));
        for (unsigned i = 0; i < 2; ++i)
            cl->SetComputeRootConstantBufferView(2 + i,
                                                 constants[i]->GetGPUVirtualAddress() + offsets[i]);
        cl->SetComputeRootDescriptorTable(4, Gpu(p, WaterSlot));
        water_guide = rrwater::ValidGuide(guide);
        const UINT water_constants[4] = {water_guide, 0, 0, 0};
        cl->SetComputeRoot32BitConstants(5, 4, water_constants, 0);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        for (auto &r : guides)
            Change(cl, r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (water_written)
            Change(cl, water.mask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COMMON);
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
    // Development tests (rr_live.h): each fills one shared test texture from this
    // frame's inputs between GateBegin and GateEnd, where Status (slot 8), the
    // color (slot 10) and RR's depth are shader resources. The descriptors are
    // written on this slot's own heap before the dispatch that reads them; each
    // test has its own target slot. Leaves the texture readable by RR;
    // TestFinish returns it to COMMON.
    void TestPass(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12RootSignature *root,
                  ID3D12PipelineState *pso, UINT source, ID3D12Resource *target, UINT slot,
                  const UINT *constants, UINT count) {
        UAV(p, target, slot);
        Change(cl, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(root);
        cl->SetPipelineState(pso);
        cl->SetComputeRootDescriptorTable(0, Gpu(p, source));
        cl->SetComputeRootDescriptorTable(1, Gpu(p, slot));
        cl->SetComputeRoot32BitConstants(2, count, constants, 0);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Order(cl);
        Change(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void TestFinish(ID3D12GraphicsCommandList *cl, ID3D12Resource *target) {
        Change(cl, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    }
    // Sky edge responsivity mask (R16_FLOAT, w x h) from Status.
    void SkyEdge(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *mask,
                 float strength, UINT radius) {
        UINT c[] = {w, h, radius, 0};
        memcpy(&c[3], &strength, sizeof(strength));
        TestPass(cl, p, p.sky_edge_root.Get(), p.sky_edge.Get(), 8, mask, SkyEdgeTarget, c, 4);
    }
    // RR's color scaled by a power of two (R32G32B32A32_FLOAT, w x h), read from
    // the color subrect at (x, y). RR then reads it at subrect origin 0.
    void ScaleColor(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *scaled, UINT x,
                    UINT y, float scale) {
        UINT c[8] = {w, h, x, y};
        memcpy(&c[4], &scale, sizeof(scale));
        TestPass(cl, p, p.test_root.Get(), p.color_scale.Get(), 10, scaled, ColorTarget, c, 8);
    }
    // RR's depth (the private copy, or the game's R32_FLOAT depth) with sky (0)
    // lifted to floor (R32_FLOAT, w x h), read from the depth subrect at (x, y).
    void FloorDepth(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *floored, UINT x,
                    UINT y, float floor) {
        SRV(p, use_depth ? depth.Get() : sr[1].Get(), DepthSource);
        UINT c[8] = {w, h, x, y};
        memcpy(&c[4], &floor, sizeof(floor));
        TestPass(cl, p, p.test_root.Get(), p.depth_floor.Get(), DepthSource, floored, DepthTarget,
                 c, 8);
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
    // SR time, after the SSS resolve. Reads this frame's classification and,
    // when merged, the SSS responsivity mask (left NON_PIXEL_SHADER_RESOURCE by
    // that resolve); leaves both masks readable for RR until WaterFinish.
    void WaterPass(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *sss_mask) {
        Require(water.ready && water_written && p.water, "water masks unavailable");
        auto cpu = water.heap->GetCPUDescriptorHandleForHeapStart();
        const auto gpu = water.heap->GetGPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p.device->CreateShaderResourceView(water.mask.Get(), &srv, cpu);
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        p.device->CreateShaderResourceView(sss_mask, &srv, {cpu.ptr + p.stride});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(water.responsivity.Get(), nullptr, &uav,
                                            {cpu.ptr + 2 * p.stride});
        p.device->CreateUnorderedAccessView(water.disocclusion.Get(), nullptr, &uav,
                                            {cpu.ptr + 3 * p.stride});
        Change(cl, water.mask.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto *r : {water.responsivity.Get(), water.disocclusion.Get()})
            Change(cl, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Zero the water pixel count (stats byte 16) from the header's never-written tail.
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyBufferRegion(stats.Get(), 16, header.Get(), 64, 4);
        Change(cl, stats.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {water.heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(p.water_root.Get());
        cl->SetPipelineState(p.water.Get());
        cl->SetComputeRootConstantBufferView(0, header->GetGPUVirtualAddress());
        cl->SetComputeRootDescriptorTable(1, gpu);
        cl->SetComputeRootDescriptorTable(2, {gpu.ptr + 2 * p.stride});
        cl->SetComputeRootUnorderedAccessView(3, stats->GetGPUVirtualAddress());
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Order(cl);
        for (auto *r : {water.responsivity.Get(), water.disocclusion.Get()})
            Change(cl, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void WaterFinish(ID3D12GraphicsCommandList *cl) {
        for (auto *r : {water.mask.Get(), water.responsivity.Get(), water.disocclusion.Get()})
            Change(cl, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
    }
    // Header words 14-15 for rr_water_masks.hlsl; Header() clears them each frame.
    void WaterHeader(float strength, UINT flags) {
        memcpy(&header_map[14], &strength, sizeof(strength));
        header_map[15] = flags;
    }
    // Water pixels counted by the latest WaterPass on this frame.
    UINT ReadWaterPixels() {
        UINT v{};
        void *m{};
        D3D12_RANGE r{16, 20};
        Check(readback->Map(0, &r, &m), "live water statistics map failed");
        memcpy(&v, static_cast<const char *>(m) + 16, sizeof(v));
        r = {0, 0};
        readback->Unmap(0, &r);
        return v;
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
    // RR's output the gate's Display reads (heap slot 9): rr_output, or the color
    // brightness test's FP32 output. Rewritten for every evaluation.
    void Reconstructed(Pipelines &p, ID3D12Resource *output) {
        SRV(p, output, 9);
    }
    // Factor the gate's Display applies to RR's output (color scale test); 0 = none.
    void OutputScale(float factor) {
        memcpy(&header_map[13], &factor, sizeof(factor));
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
    // Pixels with a non-finite channel: in the color RR receives, in RR's output.
    std::array<UINT, 2> ReadFinite() {
        std::array<UINT, 2> v{};
        void *m{};
        D3D12_RANGE r{16, 16 + sizeof(v)};
        Check(readback->Map(0, &r, &m), "live finite statistics map failed");
        memcpy(v.data(), static_cast<const char *>(m) + 16, sizeof(v));
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
    void Retire() {
        sources = {};
        constants = {};
        sr = {};
        probability_inputs = {};
        hit_source.Reset();
        hit_ready = false;
        water_written = false;
    }
    void Release(Budget &b) {
        if (header_map)
            header->Unmap(0, nullptr);
        Require(b.bytes >= bytes, "live allocation ledger underflow");
        b.bytes -= bytes;
        *this = {};
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
