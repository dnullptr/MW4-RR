#pragma once
#include "feed_fg_identity.h"
#include "rr_resolution_limits.h"
#include "rr_sparse_budget.h"
#include <dxgi1_4.h>
#include "rr_vrrt_data.h"
#include "rr_compute_state.h"
#include "../build/rr_vrrt_native.generated.h"
#include "rr_lighting_source.h"
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
namespace rrsparsecopy {
using Microsoft::WRL::ComPtr;
inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                                         D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return v;
}
// Opaque identities only. Holding COM references for every observed transition
// would retain the game's entire transient texture allocator.
class Layouts {
    inline static constexpr GUID IdentityId = {
        0x028c9157, 0x4821, 0x490b, {0xa4, 0xc8, 0x59, 0x34, 0x72, 0x1a, 0x17, 0x01}};
    inline static std::atomic<UINT64> nextIdentity{};
    struct State {
        D3D12_RESOURCE_STATES layout{};
        UINT64 identity{};
        bool aliased{};
    };
    struct Recording {
        std::unordered_map<ID3D12Resource *, State> states;
        bool capped{};
    };
    std::mutex mutex;
    std::unordered_map<ID3D12GraphicsCommandList *, Recording> lists;
    static UINT64 Identity(ID3D12Resource *r, bool assign) {
        UINT64 value{};
        UINT size = sizeof(value);
        if (SUCCEEDED(r->GetPrivateData(IdentityId, &size, &value)) && size == sizeof(value) &&
            value)
            return value;
        if (!assign)
            return 0;
        auto previous = nextIdentity.load();
        do {
            if (previous == UINT64_MAX)
                return 0;
        } while (!nextIdentity.compare_exchange_weak(previous, previous + 1));
        value = previous + 1;
        if (FAILED(r->SetPrivateData(IdentityId, sizeof(value), &value)))
            return 0;
        return value;
    }

  public:
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        lists.erase(cl);
    }
    void Observe(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
        std::lock_guard lock(mutex);
        if (!cl || n > 4096 || (n && !b)) {
            lists.erase(cl);
            return;
        }
        if (!lists.contains(cl) && lists.size() >= 256)
            return;
        auto &l = lists[cl];
        if (l.capped)
            return;
        for (UINT i = 0; i < n; ++i) {
            // Aliasing barriers synchronize overlapping memory usage; they do not
            // transition either resource object's state. Keep those states, but
            // separately withhold snapshot eligibility until a new transition.
            // State-only callers must independently prove current native use.
            // https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                const auto &a = b[i].Aliasing;
                if (!a.pResourceBefore || !a.pResourceAfter) {
                    for (auto &[resource, state] : l.states)
                        state.aliased = true;
                } else {
                    for (auto *resource : {a.pResourceBefore, a.pResourceAfter}) {
                        auto at = l.states.find(resource);
                        if (at != l.states.end())
                            at->second.aliased = true;
                    }
                }
            }
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const auto &t = b[i].Transition;
                if (!t.pResource)
                    continue;
                if (b[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                    t.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                    l.states.erase(t.pResource);
                    continue;
                }
                if (l.states.size() >= 4096 && !l.states.contains(t.pResource)) {
                    l.states.clear();
                    l.capped = true;
                    return;
                }
                const auto id = Identity(t.pResource, true);
                if (id)
                    l.states[t.pResource] = {t.StateAfter, id};
                else
                    l.states.erase(t.pResource);
            }
        }
    }
  private:
    bool Lookup(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES &value, bool require_unaliased) {
        std::lock_guard lock(mutex);
        auto l = lists.find(cl);
        if (!r || l == lists.end() || l->second.capped)
            return false;
        auto at = l->second.states.find(r);
        if (at == l->second.states.end() || Identity(r, false) != at->second.identity ||
            (require_unaliased && at->second.aliased))
            return false;
        value = at->second.layout;
        return true;
    }
  public:
    bool Get(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES &value) {
        return Lookup(cl, r, value, false);
    }
    // Additional conservative eligibility, not a guarantee of initialized contents.
    // The caller still needs its producer and command-order checks.
    bool GetUnaliased(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                      D3D12_RESOURCE_STATES &value) {
        return Lookup(cl, r, value, true);
    }
    // Fixture simulates allocation lifetime rollover at the same opaque address.
    static void RetireIdentityForTest(ID3D12Resource *r) {
        r->SetPrivateData(IdentityId, 0, nullptr);
    }
};
struct Pair {
    std::array<ComPtr<ID3D12Resource>, 2> r;
    std::array<D3D12_RESOURCE_STATES, 2> state{};
};
inline bool Shape(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    auto valid = [](const auto &d) {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               rrresolution::Texture(d.Width, d.Height) && d.MipLevels == 1 &&
               d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1;
    };
    return valid(a) && valid(b) && a.Width == b.Width && a.Height == b.Height &&
           (a.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
            a.Format == DXGI_FORMAT_R32G32B32A32_UINT) &&
           (b.Format == DXGI_FORMAT_R32_FLOAT || b.Format == DXGI_FORMAT_R32_UINT);
}
inline bool Shape(const Pair &p) {
    return p.r[0] && p.r[1] && p.r[0] != p.r[1] && Shape(p.r[0]->GetDesc(), p.r[1]->GetDesc());
}
inline bool SameShape(ID3D12Resource *a, ID3D12Resource *b) {
    auto x = a->GetDesc(), y = b->GetDesc();
    return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format &&
           x.DepthOrArraySize == y.DepthOrArraySize && x.MipLevels == y.MipLevels &&
           x.SampleDesc.Count == y.SampleDesc.Count;
}
inline bool Readable(D3D12_RESOURCE_STATES s) {
    return (s & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
           !(s & ~(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE));
}
inline bool SameDevice(ID3D12Device *d, ID3D12Resource *r) {
    ComPtr<ID3D12Device> actual;
    return r && SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&actual))) &&
           fg::SameNativeObject(actual.Get(), d);
}
class Pool {
#ifdef RR_SPARSE_RETIREMENT_TEST
    friend struct RetirementTest;
#endif
    struct Slot {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        Pair privatePair, source, history, destination;
        ComPtr<ID3D12PipelineState> scale;
        ComPtr<ID3D12Resource> rates;
        ComPtr<ID3D12RootSignature> privateRoot;
        // Indexed by rrlighting::Source.
        std::array<ComPtr<ID3D12PipelineState>, rrlighting::Count> privateScale;
        unsigned privateLobe{};
        ComPtr<ID3D12DescriptorHeap> privateHeap;
        ID3D12GraphicsCommandList *command{};
        UINT64 serial{}, signaled{}, bytes{};
        bool live{}, consumed{}, pending{}, poisoned{};
    };
    std::mutex mutex;
    std::array<Slot, 8> slots;
    std::unordered_map<unsigned, std::vector<unsigned>> calls;
    unsigned next{};
    UINT64 allocated{};
    bool Complete(const Slot &s) const {
        if (s.poisoned || s.pending)
            return false;
        if (!s.serial)
            return true;
        auto v = s.fence->GetCompletedValue();
        return v != UINT64_MAX && v >= s.signaled && s.signaled == s.serial;
    }
    void RetireCompleted() {
        for (auto &s : slots)
            if (!s.live && Complete(s)) {
                s.source = {};
                s.history = {};
                s.destination = {};
                s.scale.Reset();
                s.rates.Reset();
                s.command = nullptr;
            }
    }
    static void Copy(ID3D12GraphicsCommandList *cl, ID3D12Resource *to, D3D12_RESOURCE_STATES ts,
                     ID3D12Resource *from, D3D12_RESOURCE_STATES fs) {
        std::array<D3D12_RESOURCE_BARRIER, 2> before = {
            Transition(from, fs, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(to, ts, D3D12_RESOURCE_STATE_COPY_DEST)};
        cl->ResourceBarrier(2, before.data());
        cl->CopyResource(to, from);
        std::array<D3D12_RESOURCE_BARRIER, 2> after = {
            Transition(to, D3D12_RESOURCE_STATE_COPY_DEST, ts),
            Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE, fs)};
        cl->ResourceBarrier(2, after.data());
    }
    static D3D12_SHADER_BYTECODE PrivateScaleCode(bool packed_float, unsigned lobe) {
        using rrlighting::Source;
        switch (rrlighting::Valid(lobe)) {
        case Source::NativeLobe1:
            return packed_float ? D3D12_SHADER_BYTECODE{RRVrrtScaleNative1Float,
                                                        sizeof(RRVrrtScaleNative1Float)}
                                : D3D12_SHADER_BYTECODE{RRVrrtScaleNative1,
                                                        sizeof(RRVrrtScaleNative1)};
        case Source::NativeLobe2:
            return packed_float ? D3D12_SHADER_BYTECODE{RRVrrtScaleNative2Float,
                                                        sizeof(RRVrrtScaleNative2Float)}
                                : D3D12_SHADER_BYTECODE{RRVrrtScaleNative2,
                                                        sizeof(RRVrrtScaleNative2)};
        case Source::FillLobe2:
            return packed_float
                       ? D3D12_SHADER_BYTECODE{RRVrrtScaleFill2Float, sizeof(RRVrrtScaleFill2Float)}
                       : D3D12_SHADER_BYTECODE{RRVrrtScaleFill2, sizeof(RRVrrtScaleFill2)};
        case Source::FillBoth:
            return packed_float
                       ? D3D12_SHADER_BYTECODE{RRVrrtScaleFill3Float, sizeof(RRVrrtScaleFill3Float)}
                       : D3D12_SHADER_BYTECODE{RRVrrtScaleFill3, sizeof(RRVrrtScaleFill3)};
        case Source::CapGentle:
            return packed_float
                       ? D3D12_SHADER_BYTECODE{RRVrrtScaleCap8Float, sizeof(RRVrrtScaleCap8Float)}
                       : D3D12_SHADER_BYTECODE{RRVrrtScaleCap8, sizeof(RRVrrtScaleCap8)};
        case Source::CapStrong:
            return packed_float
                       ? D3D12_SHADER_BYTECODE{RRVrrtScaleCap4Float, sizeof(RRVrrtScaleCap4Float)}
                       : D3D12_SHADER_BYTECODE{RRVrrtScaleCap4, sizeof(RRVrrtScaleCap4)};
        default:
            return packed_float
                       ? D3D12_SHADER_BYTECODE{RRVrrtScaleFloat, sizeof(RRVrrtScaleFloat)}
                       : D3D12_SHADER_BYTECODE{RRVrrtScale, sizeof(RRVrrtScale)};
        }
    }
    static bool PreparePrivateScale(Slot &s, const Pair &input, const Pair &output,
                                    unsigned lobe = 0) {
        lobe = rrlighting::Valid(lobe);
        if (!s.privateRoot) {
            D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0}};
            D3D12_ROOT_PARAMETER p[4]{};
            for (unsigned i = 0; i < 2; ++i) {
                p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                p[i].DescriptorTable = {1, &ranges[i]};
                p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                p[i + 2].Descriptor.ShaderRegister = i ? 3 : 1;
            }
            D3D12_ROOT_SIGNATURE_DESC rd{4, p};
            ComPtr<ID3DBlob> code, error;
            ComPtr<ID3D12RootSignature> root;
            if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &code,
                                                   &error)) ||
                FAILED(s.device->CreateRootSignature(0, code->GetBufferPointer(),
                                                     code->GetBufferSize(), IID_PPV_ARGS(&root))))
                return false;
            s.privateRoot = root;
        }
        const bool packed_float = output.r[0]->GetDesc().Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        auto create = [&](unsigned variant) {
            if (s.privateScale[variant])
                return true;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = s.privateRoot.Get();
            pd.CS = PrivateScaleCode(packed_float, variant);
            return SUCCEEDED(s.device->CreateComputePipelineState(
                &pd, IID_PPV_ARGS(&s.privateScale[variant])));
        };
        // A lobe variant that cannot be created falls back to raw preparation.
        if (!create(lobe))
            lobe = 0;
        if (!create(lobe))
            return false;
        s.privateLobe = lobe;
        if (!s.privateHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.privateHeap))))
                return false;
        }
        // A slot is selected only after both recording retirement and GPU completion.
        // Its four descriptors stay immutable from Apply through every submission.
        auto cpu = s.privateHeap->GetCPUDescriptorHandleForHeapStart();
        auto stride =
            s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (unsigned i = 0; i < 2; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = input.r[i]->GetDesc().Format;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.device->CreateShaderResourceView(input.r[i].Get(), &v, {cpu.ptr + i * stride});
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = output.r[i]->GetDesc().Format;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            s.device->CreateUnorderedAccessView(output.r[i].Get(), nullptr, &u,
                                                {cpu.ptr + (2 + i) * stride});
        }
        return true;
    }

  public:
    void (*failure)(const char *){};
    void Fail(const char *why) {
        if (failure)
            failure(why);
    }
    bool Snapshot(ID3D12GraphicsCommandList *cl, const Pair &source, const Pair &history) {
        std::lock_guard lock(mutex);
        if (!cl || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !Shape(source) ||
            !Shape(history))
            return false;
        ComPtr<ID3D12Device> d;
        if (FAILED(cl->GetDevice(IID_PPV_ARGS(&d))))
            return false;
        d = fg::NativeObject<ID3D12Device>(d.Get());
        if (!d)
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!SameDevice(d.Get(), source.r[i].Get()) ||
                !SameDevice(d.Get(), history.r[i].Get()) ||
                !SameShape(source.r[i].Get(), history.r[i].Get()) || source.r[i] == history.r[i] ||
                !Readable(source.state[i]))
                return false;
        }
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed)
                return false;
        Slot *slot = nullptr;
        for (auto &s : slots)
            if (!s.live && Complete(s)) {
                if (s.device == d && s.privatePair.r[0] &&
                    SameShape(s.privatePair.r[0].Get(), source.r[0].Get()) &&
                    SameShape(s.privatePair.r[1].Get(), source.r[1].Get())) {
                    slot = &s;
                    break;
                }
                if (!slot)
                    slot = &s;
            }
        if (!slot)
            return false;
        auto &s = *slot;
        if (!s.privatePair.r[0] || s.device != d ||
            !SameShape(s.privatePair.r[0].Get(), source.r[0].Get()) ||
            !SameShape(s.privatePair.r[1].Get(), source.r[1].Get())) {
            Pair replacement;
            UINT64 bytes{};
            std::array<D3D12_RESOURCE_DESC, 2> descriptions;
            for (unsigned i = 0; i < 2; ++i) {
                auto &desc = descriptions[i];
                desc = source.r[i]->GetDesc();
                desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                desc.Alignment = 0;
                auto a = d->GetResourceAllocationInfo(0, 1, &desc);
                if (!a.SizeInBytes || a.SizeInBytes == UINT64_MAX ||
                    a.SizeInBytes > UINT64_MAX - bytes)
                    return false;
                bytes += a.SizeInBytes;
            }
            // Query only for a new/replacement allocation, never matching-slot reuse.
            // The local usage already includes game resources and other private pools.
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter3> adapter;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            UINT64 replacement_total{};
            const auto target_luid = d->GetAdapterLuid(),
                       previous_luid = s.device ? s.device->GetAdapterLuid() : LUID{};
            const bool same_adapter = s.device && target_luid.LowPart == previous_luid.LowPart &&
                                      target_luid.HighPart == previous_luid.HighPart;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
                FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ||
                !rrsparsebudget::Replacement(allocated, s.bytes, bytes, same_adapter, info.Budget,
                                             info.CurrentUsage, replacement_total))
                return false;
            // This slot is reset/disposed AND complete. Release its previous private
            // textures before replacement, so transient allocation also stays bounded.
            allocated -= s.bytes;
            s = {};
            ComPtr<ID3D12Fence> fence;
            if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                return false;
            for (unsigned i = 0; i < 2; ++i) {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &descriptions[i],
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&replacement.r[i]))))
                    return false;
            }
            allocated = replacement_total;
            s.device = d;
            s.privatePair = std::move(replacement);
            s.bytes = bytes;
            s.fence = fence;
        }
        s.source = source;
        s.history = history;
        s.destination = {};
        s.scale.Reset();
        s.command = cl;
        s.live = true;
        s.consumed = false;
        for (unsigned i = 0; i < 2; ++i)
            Copy(cl, s.privatePair.r[i].Get(), D3D12_RESOURCE_STATE_COMMON, source.r[i].Get(),
                 source.state[i]);
        return true;
    }
    bool Pending(ID3D12GraphicsCommandList *cl, const Pair &input) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed)
                return s.history.r == input.r;
        return false;
    }
    bool Apply(ID3D12GraphicsCommandList *cl, const Pair &input, const Pair &output,
               ID3D12PipelineState *scale, ID3D12PipelineState *restore, UINT x, UINT y, UINT z,
               const rrvrrt::Data *rates = nullptr, const rrvrrt::CBVs *cbvs = nullptr,
               const rrcompute::Snapshot *privateRestore = nullptr, unsigned lobe = 0) {
        std::lock_guard lock(mutex);
        if (!Shape(input) || !Shape(output) || !scale || !restore || z != 1)
            return false;
        Slot *slot = nullptr;
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed && s.history.r == input.r) {
                slot = &s;
                break;
            }
        if (!slot)
            return false;
        auto &s = *slot;
        const auto desc = output.r[0]->GetDesc();
        if (x != (desc.Width + 15) / 16 || y != (desc.Height + 15) / 16)
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!Readable(input.state[i]) ||
                output.state[i] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
                !SameDevice(s.device.Get(), output.r[i].Get()) ||
                !SameShape(s.privatePair.r[i].Get(), output.r[i].Get()))
                return false;
            for (auto &h : s.history.r)
                if (output.r[i] == h)
                    return false;
            for (auto &r : s.source.r)
                if (output.r[i] == r)
                    return false;
        }
        if (privateRestore &&
            (!rates || cbvs || !privateRestore->valid || !privateRestore->complete ||
             !privateRestore->heap_restore_valid || privateRestore->command != cl ||
             privateRestore->pipeline.Get() != restore || !privateRestore->root))
            return false;
        if (privateRestore)
            for (unsigned i = 0; i < 2; ++i) {
                const bool packedFloat =
                    output.r[0]->GetDesc().Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
                const auto format = i ? (packedFloat ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT)
                                      : (packedFloat ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                                     : DXGI_FORMAT_R32G32B32A32_UINT);
                if (input.r[i]->GetDesc().Format != format ||
                    output.r[i]->GetDesc().Format != format)
                    return false;
            }
        if (rates) {
            if ((!privateRestore && (!cbvs || !cbvs->valid)) || !rates->buffer ||
                rates->w != desc.Width || rates->h != desc.Height ||
                !SameDevice(s.device.Get(), rates->buffer.Get()))
                return false;
            const auto buffer = rates->buffer->GetDesc();
            if (buffer.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
                !rrvrrt::Dimensions(rates->w, rates->h) ||
                buffer.Width < rrvrrt::Bytes(rates->w, rates->h) ||
                !rates->buffer->GetGPUVirtualAddress())
                return false;
        }
        if (privateRestore && !PreparePrivateScale(s, input, output, lobe))
            return false;
        // All proofs and references acquired before the first output mutation.
        s.destination = output;
        s.scale = scale;
        s.rates = rates ? rates->buffer : nullptr;
        s.consumed = true;
        for (unsigned i = 0; i < 2; ++i)
            Copy(cl, output.r[i].Get(), output.state[i], s.privatePair.r[i].Get(),
                 D3D12_RESOURCE_STATE_COMMON);
        if (privateRestore) {
            auto gpu = s.privateHeap->GetGPUDescriptorHandleForHeapStart();
            auto stride =
                s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ID3D12DescriptorHeap *heaps[] = {s.privateHeap.Get()};
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootSignature(s.privateRoot.Get());
            cl->SetPipelineState(s.privateScale[s.privateLobe].Get());
            cl->SetComputeRootDescriptorTable(0, gpu);
            cl->SetComputeRootDescriptorTable(1, {gpu.ptr + 2 * stride});
        } else
            cl->SetPipelineState(scale);
        if (rates) {
            auto before = Transition(s.rates.Get(), D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            cl->ResourceBarrier(1, &before);
            const UINT strip_rows = rrvrrt::Rows(rates->w);
            for (UINT row = 0; row < rates->h; row += strip_rows) {
                const auto va =
                    s.rates->GetGPUVirtualAddress() + UINT64(row / strip_rows) * rrvrrt::ChunkBytes;
                cl->SetComputeRootConstantBufferView(privateRestore ? 2 : cbvs->parameters[0], va);
                cl->SetComputeRootConstantBufferView(privateRestore ? 3 : cbvs->parameters[1],
                                                     va + rrvrrt::WindowBytes);
                cl->Dispatch(x, (strip_rows + 15) / 16, 1);
            }
            if (!privateRestore)
                for (unsigned i = 0; i < 2; ++i)
                    cl->SetComputeRootConstantBufferView(cbvs->parameters[i], cbvs->addresses[i]);
            std::swap(before.Transition.StateBefore, before.Transition.StateAfter);
            cl->ResourceBarrier(1, &before);
        } else
            cl->Dispatch(x, y, z);
        if (privateRestore)
            privateRestore->Restore(cl, true);
        else
            cl->SetPipelineState(restore);
        return true;
    }
    // Retirement requires BOTH recording retirement and observed GPU completion.
    // Keep owned allocations cached; release only the completed native inputs.
    void Poll() {
        std::lock_guard lock(mutex);
        RetireCompleted();
    }
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (s.command == cl)
                s.live = false;
        RetireCompleted();
    }
    unsigned Before(ID3D12CommandQueue *q, UINT count, ID3D12CommandList *const *lists) {
        std::lock_guard lock(mutex);
        if (!q || count > 1024 || (count && !lists)) {
            Fail("RR invalid native submission");
            return 0;
        }
        std::vector<unsigned> matched;
        for (unsigned k = 0; k < slots.size(); ++k) {
            auto &s = slots[k];
            if (!s.live)
                continue;
            unsigned occurrences = 0;
            for (unsigned i = 0; i < count; ++i)
                occurrences += lists[i] == s.command;
            if (!occurrences)
                continue;
            ComPtr<ID3D12Device> d;
            const bool valid = occurrences == 1 && s.consumed && !s.pending && !s.poisoned &&
                               q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                               SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) &&
                               fg::SameNativeObject(d.Get(), s.device.Get()) &&
                               (!s.queue || s.queue.Get() == q);
            if (!valid) {
                s.poisoned = true;
                Fail("RR unmatched first spatial, ambiguous queue/replay; resources retained and "
                     "RR blocked");
                continue;
            }
            matched.push_back(k);
        }
        if (matched.empty())
            return 0;
        unsigned tag{};
        for (unsigned i = 0; i < 65535; ++i) {
            next = next % 65535 + 1;
            if (!calls.contains(next)) {
                tag = next;
                break;
            }
        }
        if (!tag) {
            for (auto k : matched)
                slots[k].poisoned = true;
            Fail("RR submission token capacity");
            return 0;
        }
        for (auto k : matched) {
            auto &s = slots[k];
            s.pending = true;
            s.queue = q;
            ++s.serial;
        }
        calls.emplace(tag, std::move(matched));
        return tag;
    }
    void After(ID3D12CommandQueue *q, unsigned tag) {
        std::lock_guard lock(mutex);
        auto at = calls.find(tag);
        if (!tag || at == calls.end())
            return;
        for (auto k : at->second) {
            auto &s = slots[k];
            if (s.queue.Get() != q || FAILED(q->Signal(s.fence.Get(), s.serial))) {
                s.poisoned = true;
                Fail("RR native GPU fence failed; resources retained and RR blocked");
            } else
                s.signaled = s.serial;
            s.pending = false;
        }
        calls.erase(at);
    }
    unsigned Live() {
        std::lock_guard lock(mutex);
        unsigned n = 0;
        for (auto &s : slots)
            n += s.live;
        return n;
    }
    UINT64 Bytes() {
        std::lock_guard lock(mutex);
        return allocated;
    }
};
} // namespace rrsparsecopy
