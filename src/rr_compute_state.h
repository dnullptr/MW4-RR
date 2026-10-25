#pragma once
#include "feed_fg_identity.h"
#include "rr_vrrt_data.h"
#include "rr_sss_root_state.h"
#include "rr_sss_graphics_state.h"
#include <d3d12.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace rrcompute {
using Microsoft::WRL::ComPtr;
// Raw native arguments: ReShade's CBV callback can omit unresolved GPUVAs.
// Recording these setters directly avoids restoring an older argument then.
enum class WriteKind { Indirect, Bundle, ClearState };
struct OutputBindings {
    ID3D12GraphicsCommandList *command{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> original{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    UINT count{};
    UINT64 generation{}, serial{};
    bool known{}, covered{};
};
// One native ExecuteIndirect as the tracker forwarded it. replay re-issues it
// through the same trampoline, so this detour, Update and the observer never see
// the replay. Replay only the unmodified call, synchronously, from inside the
// observer, while holding Tracker::Internal (the observer runs under it), and add
// rrinternal::Scope as for any other add-on command. replay returns false, and
// records nothing, for any other call, for a second replay or without a trampoline.
struct IndirectCall {
    ID3D12GraphicsCommandList *cl{};
    ID3D12CommandSignature *signature{};
    UINT max{};
    ID3D12Resource *args{};
    UINT64 args_offset{};
    ID3D12Resource *count{};
    UINT64 count_offset{};
    bool (*replay)(const IndirectCall &){};
};
enum class Kind { Empty, Table, Constants, CBV, SRV, UAV };
struct Argument {
    Kind kind{};
    UINT64 address{}, mask{};
    std::array<UINT, 64> words{};
};
struct Shape {
    Kind kind{};
    UINT words{};
    UINT shader_register = UINT_MAX, space = UINT_MAX, flags = 8;
    bool compute{}, ignored{};
};
using CBVPair = rrvrrt::CBVs;
struct Snapshot {
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> root;
    std::array<Argument, 64> args{}, graphics_tables{};
    std::array<ComPtr<ID3D12DescriptorHeap>, 2> heaps{};
    bool heap_restore_valid{}, complete{};
    ID3D12GraphicsCommandList *command{};
    bool valid{};
    const char *status = "not-captured";
    void Restore(ID3D12GraphicsCommandList *cl, bool restore_heaps = false) const {
        if (restore_heaps) {
            ID3D12DescriptorHeap *saved[2]{};
            UINT n = 0;
            for (const auto &h : heaps)
                if (h)
                    saved[n++] = h.Get();
            cl->SetDescriptorHeaps(n, saved);
            for (UINT p = 0; p < graphics_tables.size(); ++p)
                if (graphics_tables[p].kind == Kind::Table)
                    cl->SetGraphicsRootDescriptorTable(p, {graphics_tables[p].address});
        }
        cl->SetPipelineState(pipeline.Get());
        cl->SetComputeRootSignature(root.Get());
        for (UINT p = 0; p < args.size(); ++p) {
            const auto &a = args[p];
            switch (a.kind) {
            case Kind::Table:
                cl->SetComputeRootDescriptorTable(p, {a.address});
                break;
            case Kind::CBV:
                cl->SetComputeRootConstantBufferView(p, a.address);
                break;
            case Kind::SRV:
                cl->SetComputeRootShaderResourceView(p, a.address);
                break;
            case Kind::UAV:
                cl->SetComputeRootUnorderedAccessView(p, a.address);
                break;
            case Kind::Constants:
                for (UINT k = 0; k < 64; ++k)
                    if (a.mask & (1ull << k))
                        cl->SetComputeRoot32BitConstant(p, a.words[k], k);
                break;
            default:
                break;
            }
        }
    }
};
class Tracker {
    friend struct TrackerTestAccess;
    enum Method {
        PSO,
        Root,
        Table,
        Word,
        Words,
        CBV,
        SRV,
        UAV,
        Heaps,
        Indirect,
        Bundle,
        Clear,
        RayPSO,
        Program,
        GraphicsRoot,
        GraphicsTable,
        GraphicsTarget,
        GraphicsCBV,
        Viewport,
        Scissor,
        Predication,
        BeginPass,
        EndPass,
        StencilRef,
        StencilRefs,
        BeginQuery,
        EndQuery,
        Count
    };
    struct HookSlot {
        void *target{};
        std::atomic<void *> original{};
    };
    std::array<std::array<HookSlot, 8>, Count> hooks;
    struct State {
        OutputBindings bindings;
        UINT64 binding_serial{};
        bool write_coverage_lost{};
        ID3D12PipelineState *pipeline{};
        ID3D12RootSignature *root{};
        ID3D12RootSignature *graphics_root{};
        std::array<Argument, 64> args{}, graphics_tables{};
        rrsss::RootCBVs sss_cbvs;
        UINT64 graphics_target{};
        bool graphics_unknown{};
        rrsss::GraphicsOutputProof output_proof;
        std::shared_ptr<rrsss::GraphicsOutputMirror> output_mirror;
        bool output_bound{}, output_coverage_lost{}, stencil_coverage_lost{};
        std::array<ID3D12DescriptorHeap *, 2> heaps{};
        bool uncertain{}, root_unknown{}, creation_pending = true, coverage_lost{};
        DWORD attach_thread = GetCurrentThreadId();
        UINT64 graphics_target_serial{};
        std::array<void *, Count> targets{};
        UINT64 resets{};
    };
    struct Layout {
        std::array<Shape, 64> params{};
        UINT count{};
    };
    std::mutex install_mutex, mutex;
    std::unordered_map<ID3D12GraphicsCommandList *, State> states;
    std::unordered_map<ID3D12RootSignature *, Layout> layouts;
    std::atomic<bool> healthy{true}, target_healthy{true}, sss_healthy{true}, outputs_healthy{true},
        stencil_healthy{true};
    std::atomic<bool> write_observers_healthy{true};
    void (*before_write)(ID3D12GraphicsCommandList *, WriteKind){};
    void (*bound_outputs)(const OutputBindings &){};
    // Both written once, at the first registration, and never changed after. A refused
    // registration or an observer exception clears the latch for good.
    std::atomic<void (*)(const IndirectCall &)> after_indirect{};
    void (*indirect_log)(const char *){};
    std::atomic<bool> after_indirect_healthy{true};
    bool (*shared_output_context)(ID3D12GraphicsCommandList *){};
    static inline Tracker *active{};
    struct Error {
        DWORD value = GetLastError();
        ~Error() {
            SetLastError(value);
        }
    };
    void BeforeWrite(ID3D12GraphicsCommandList *cl, WriteKind kind) noexcept {
        if (internal || !write_observers_healthy || !before_write)
            return;
        Error preserve;
        try {
            before_write(cl, kind);
        } catch (...) {
            write_observers_healthy = false;
        }
    }
    void BoundOutputs(ID3D12GraphicsCommandList *cl) noexcept {
        if (internal || !write_observers_healthy || !bound_outputs)
            return;
        Error preserve;
        try {
            bound_outputs(CaptureOutputBindings(cl));
        } catch (...) {
            write_observers_healthy = false;
        }
    }
    void AfterIndirect(const IndirectCall &call) noexcept {
        if (internal || !after_indirect_healthy)
            return;
        auto *observer = after_indirect.load(std::memory_order_acquire);
        if (!observer)
            return;
        Error preserve;
        // Everything the observer records, the replay included, is the add-on's own.
        Internal recording;
        const auto *previous = observed;
        observed = &call;
        try {
            observer(call);
        } catch (...) {
            // Only the first failure logs, even when two lists throw at once.
            if (after_indirect_healthy.exchange(false) && indirect_log)
                try {
                    indirect_log("RR indirect draw observer exception; observer disabled, "
                                 "native call already forwarded");
                } catch (...) {
                }
        }
        observed = previous;
    }
    bool WriteCovered(State &s, const std::array<void *, Count> &current) const {
        bool covered =
            write_observers_healthy && healthy && target_healthy && before_write && bound_outputs;
        for (auto method : {Indirect, Bundle, Clear, GraphicsTarget})
            covered = covered && current[method] && current[method] == s.targets[method];
        if (!covered)
            s.write_coverage_lost = true;
        return covered && !s.write_coverage_lost && !s.coverage_lost;
    }
    template <Method M, class F> void Update(ID3D12GraphicsCommandList *cl, F &&f) noexcept {
        if (internal || !healthy)
            return;
        Error e;
        try {
            std::lock_guard lock(mutex);
            auto i = states.find(cl);
            if (i != states.end()) {
                auto &s = i->second;
                const auto graphics_serial = s.graphics_target_serial;
                auto targets = s.targets;
                const bool coverage_lost = s.coverage_lost;
                const auto resets = s.resets;
                f(s);
                s.graphics_target_serial = graphics_serial;
                if constexpr (M == GraphicsTarget) {
                    if (s.graphics_target_serial == UINT64_MAX)
                        s.coverage_lost = true;
                    else
                        ++s.graphics_target_serial;
                }
                s.targets = targets;
                s.coverage_lost = s.coverage_lost || coverage_lost;
                s.resets = resets;
                s.creation_pending = false;
            }
        } catch (...) {
            if constexpr (M == GraphicsTarget)
                target_healthy = false;
            else if constexpr (M == GraphicsCBV)
                sss_healthy = false;
            else if constexpr (M >= StencilRef)
                stencil_healthy = false;
            else if constexpr (M >= Viewport)
                outputs_healthy = false;
            else
                healthy = false;
        }
    }
    template <Method M, size_t N, class... A>
    static void Forward(ID3D12GraphicsCommandList *cl, A... a) {
        using Fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, A...);
        reinterpret_cast<Fn>(active->hooks[M][N].original.load())(cl, a...);
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnPSO(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *p) {
        Forward<PSO, N>(cl, p);
        active->Update<PSO>(cl, [&](State &s) { s.pipeline = p; });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnRoot(ID3D12GraphicsCommandList *cl, ID3D12RootSignature *p) {
        Forward<Root, N>(cl, p);
        active->Update<Root>(cl, [&](State &s) {
            if (s.root != p || s.root_unknown) {
                s.args = {};
                if (!s.root_unknown)
                    s.uncertain = false;
            }
            s.root = p;
            s.root_unknown = false;
        });
    }
    template <Method M, size_t N>
    static void STDMETHODCALLTYPE OnAddress(ID3D12GraphicsCommandList *cl, UINT p, UINT64 value) {
        Forward<M, N>(cl, p, value);
        active->Update<M>(cl, [&](State &s) {
            if (p >= 64) {
                active->healthy = false;
                return;
            }
            auto &a = s.args[p];
            a = {};
            a.kind = M == CBV ? Kind::CBV : M == SRV ? Kind::SRV : Kind::UAV;
            a.address = value;
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnTable(ID3D12GraphicsCommandList *cl, UINT p,
                                          D3D12_GPU_DESCRIPTOR_HANDLE value) {
        Forward<Table, N>(cl, p, value);
        active->Update<Table>(cl, [&](State &s) {
            if (p >= 64) {
                active->healthy = false;
                return;
            }
            s.args[p] = {};
            s.args[p].kind = Kind::Table;
            s.args[p].address = value.ptr;
        });
    }
    static void SetWords(State &s, UINT p, UINT n, const void *values, UINT first) {
        if (p >= 64 || first > 64 || n > 64 - first || (n && !values)) {
            active->healthy = false;
            return;
        }
        auto &a = s.args[p];
        if (a.kind != Kind::Constants)
            a = {};
        a.kind = Kind::Constants;
        for (UINT k = 0; k < n; ++k) {
            a.words[first + k] = static_cast<const UINT *>(values)[k];
            a.mask |= 1ull << (first + k);
        }
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnWord(ID3D12GraphicsCommandList *cl, UINT p, UINT value,
                                         UINT first) {
        Forward<Word, N>(cl, p, value, first);
        active->Update<Word>(cl, [&](State &s) { SetWords(s, p, 1, &value, first); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnWords(ID3D12GraphicsCommandList *cl, UINT p, UINT n,
                                          const void *values, UINT first) {
        Forward<Words, N>(cl, p, n, values, first);
        active->Update<Words>(cl, [&](State &s) { SetWords(s, p, n, values, first); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnHeaps(ID3D12GraphicsCommandList *cl, UINT n,
                                          ID3D12DescriptorHeap *const *heaps) {
        Forward<Heaps, N>(cl, n, heaps);
        active->Update<Heaps>(cl, [&](State &s) {
            if (n > 2 || (n && !heaps)) {
                active->healthy = false;
                return;
            }
            std::array<ID3D12DescriptorHeap *, 2> next{};
            for (UINT k = 0; k < n; ++k) {
                const auto type = heaps[k]->GetDesc().Type;
                if (type > D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
                    active->healthy = false;
                    return;
                }
                next[unsigned(type)] = heaps[k];
            }
            if (s.heaps != next) {
                for (auto &a : s.args)
                    if (a.kind == Kind::Table)
                        a = {};
                s.graphics_tables = {};
                s.heaps = next;
            }
        });
    }
    // A hooked layer can forward into another hooked layer (a runtime or debug layer
    // into the next list). Only the outermost one, the caller's own call, is observed.
    static inline thread_local unsigned indirect_depth{};
    struct IndirectDepth {
        IndirectDepth() {
            ++indirect_depth;
        }
        ~IndirectDepth() {
            --indirect_depth;
        }
    };
    template <size_t N>
    static void STDMETHODCALLTYPE OnIndirect(ID3D12GraphicsCommandList *cl,
                                             ID3D12CommandSignature *sig, UINT n,
                                             ID3D12Resource *args, UINT64 offset,
                                             ID3D12Resource *count, UINT64 count_offset) {
        // SDK D3D12_INDIRECT_ARGUMENT_TYPE can change draw/dispatch, VB/IB and
        // root arguments, but cannot change PSO/OM/RS/predication/renderpass state.
        active->BeforeWrite(cl, WriteKind::Indirect);
        const bool outermost = !indirect_depth;
        {
            IndirectDepth nested;
            Forward<Indirect, N>(cl, sig, n, args, offset, count, count_offset);
        }
        active->Update<Indirect>(cl, [](State &s) {
            s.args = {};
            s.sss_cbvs.Invalidate();
            s.uncertain = true;
        });
        if (outermost)
            active->AfterIndirect({cl, sig, n, args, offset, count, count_offset, &Replay<N>});
    }
    // The call the observer on this thread is handling, until its one replay.
    static inline thread_local const IndirectCall *observed{};
    static bool SameCall(const IndirectCall &a, const IndirectCall &b) noexcept {
        return a.cl == b.cl && a.signature == b.signature && a.max == b.max &&
               a.args == b.args && a.args_offset == b.args_offset && a.count == b.count &&
               a.count_offset == b.count_offset && a.replay == b.replay;
    }
    template <size_t N> static bool Replay(const IndirectCall &c) {
        using Fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *,
                                             UINT, ID3D12Resource *, UINT64, ID3D12Resource *,
                                             UINT64);
        auto *self = active;
        void *original = self ? self->hooks[Indirect][N].original.load() : nullptr;
        if (!observed || !SameCall(*observed, c) || c.replay != &Replay<N> || !original)
            return false;
        observed = nullptr;
        reinterpret_cast<Fn>(original)(c.cl, c.signature, c.max, c.args, c.args_offset, c.count,
                                       c.count_offset);
        return true;
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnBundle(ID3D12GraphicsCommandList *cl,
                                           ID3D12GraphicsCommandList *bundle) {
        active->BeforeWrite(cl, WriteKind::Bundle);
        Forward<Bundle, N>(cl, bundle);
        active->Update<Bundle>(cl, [](State &s) {
            s.args = {};
            s.pipeline = nullptr;
            s.root = nullptr;
            s.root_unknown = true;
            s.uncertain = true;
            s.graphics_unknown = true;
            s.graphics_tables = {};
            s.output_proof.Invalidate();
            s.bindings.known = false;
            if (s.binding_serial == UINT64_MAX)
                s.write_coverage_lost = true;
            else
                ++s.binding_serial;
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnClear(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *p) {
        active->BeforeWrite(cl, WriteKind::ClearState);
        Forward<Clear, N>(cl, p);
        active->Update<Clear>(cl, [&](State &s) {
            auto proof = s.output_proof;
            auto mirror = std::move(s.output_mirror);
            const bool lost = s.output_coverage_lost, write_lost = s.write_coverage_lost;
            const auto binding_serial = s.binding_serial;
            s = {};
            s.write_coverage_lost = write_lost || binding_serial == UINT64_MAX;
            s.binding_serial = binding_serial == UINT64_MAX ? binding_serial : binding_serial + 1;
            s.bindings.known = true;
            s.output_proof = proof;
            s.output_proof.Invalidate();
            s.output_mirror = std::move(mirror);
            s.output_coverage_lost = lost;
            s.pipeline = p;
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnRay(ID3D12GraphicsCommandList *cl, ID3D12StateObject *p) {
        Forward<RayPSO, N>(cl, p);
        active->Update<RayPSO>(cl, [](State &s) { s.pipeline = nullptr; });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnProgram(ID3D12GraphicsCommandList *cl,
                                            const D3D12_SET_PROGRAM_DESC *p) {
        Forward<Program, N>(cl, p);
        active->Update<Program>(cl, [](State &s) { s.pipeline = nullptr; });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnGraphicsRoot(ID3D12GraphicsCommandList *cl,
                                                 ID3D12RootSignature *root) {
        Forward<GraphicsRoot, N>(cl, root);
        active->Update<GraphicsRoot>(cl, [&](State &s) {
            if (s.graphics_root != root) {
                s.graphics_tables = {};
                s.sss_cbvs.Invalidate();
            }
            s.graphics_root = root; /* A bundle may have left unobserved tables on the same root. */
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnGraphicsTable(ID3D12GraphicsCommandList *cl, UINT p,
                                                  D3D12_GPU_DESCRIPTOR_HANDLE value) {
        Forward<GraphicsTable, N>(cl, p, value);
        active->Update<GraphicsTable>(cl, [&](State &s) {
            if (p >= 64) {
                active->healthy = false;
                return;
            }
            s.graphics_tables[p] = {};
            s.graphics_tables[p].kind = Kind::Table;
            s.graphics_tables[p].address = value.ptr;
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnGraphicsCBV(ID3D12GraphicsCommandList *cl, UINT parameter,
                                                UINT64 address) {
        Forward<GraphicsCBV, N>(cl, parameter, address);
        active->Update<GraphicsCBV>(cl, [&](State &s) {
            if (!s.sss_cbvs.Bind(parameter, address))
                active->sss_healthy = false;
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnGraphicsTarget(ID3D12GraphicsCommandList *cl, UINT n,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE *handles,
                                                   BOOL contiguous,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE *depth) {
        Forward<GraphicsTarget, N>(cl, n, handles, contiguous, depth);
        active->Update<GraphicsTarget>(cl, [&](State &s) {
            if (s.binding_serial == UINT64_MAX)
                s.write_coverage_lost = true;
            else
                ++s.binding_serial;
            s.graphics_target = n == 1 && handles ? handles[0].ptr : 0;
            s.output_bound = false;
            s.bindings = {};
            if (!active->outputs_healthy)
                return;
            try {
                if (!s.output_mirror || s.output_mirror.use_count() != 1)
                    s.output_mirror = std::make_shared<rrsss::GraphicsOutputMirror>();
                s.output_bound = s.output_mirror->Capture(cl, n, handles, contiguous, depth);
                s.bindings = {};
                if (s.output_bound) {
                    s.bindings.known = true;
                    s.bindings.count = s.output_mirror->count;
                    s.bindings.original = s.output_mirror->original_handles;
                    s.bindings.depth = s.output_mirror->original_depth;
                }
            } catch (...) {
                active->outputs_healthy = false;
            }
        });
        active->BoundOutputs(cl);
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnViewport(ID3D12GraphicsCommandList *cl, UINT n,
                                             const D3D12_VIEWPORT *values) {
        Forward<Viewport, N>(cl, n, values);
        active->Update<Viewport>(cl, [&](State &s) { s.output_proof.Viewports(n, values); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnScissor(ID3D12GraphicsCommandList *cl, UINT n,
                                            const D3D12_RECT *values) {
        Forward<Scissor, N>(cl, n, values);
        active->Update<Scissor>(cl, [&](State &s) { s.output_proof.Scissors(n, values); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnPredication(ID3D12GraphicsCommandList *cl,
                                                ID3D12Resource *resource, UINT64 offset,
                                                D3D12_PREDICATION_OP op) {
        Forward<Predication, N>(cl, resource, offset, op);
        active->Update<Predication>(
            cl, [&](State &s) { s.output_proof.Predication(resource != nullptr); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnBeginPass(ID3D12GraphicsCommandList *cl, UINT n,
                                              const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets,
                                              const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth,
                                              D3D12_RENDER_PASS_FLAGS flags) {
        Forward<BeginPass, N>(cl, n, targets, depth, flags);
        active->Update<BeginPass>(cl, [&](State &s) {
            s.output_proof.BeginPass(flags != D3D12_RENDER_PASS_FLAG_NONE);
            s.output_bound = false;
        });
    }
    template <size_t N> static void STDMETHODCALLTYPE OnEndPass(ID3D12GraphicsCommandList *cl) {
        Forward<EndPass, N>(cl);
        active->Update<EndPass>(cl, [](State &s) {
            s.output_proof.EndPass();
            s.output_bound = false;
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnStencilRef(ID3D12GraphicsCommandList *cl, UINT value) {
        Forward<StencilRef, N>(cl, value);
        active->Update<StencilRef>(cl, [&](State &s) { s.output_proof.StencilRefs(value, value); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnStencilRefs(ID3D12GraphicsCommandList *cl, UINT front,
                                                UINT back) {
        Forward<StencilRefs, N>(cl, front, back);
        active->Update<StencilRefs>(cl, [&](State &s) { s.output_proof.StencilRefs(front, back); });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnBeginQuery(ID3D12GraphicsCommandList *cl, ID3D12QueryHeap *heap,
                                               D3D12_QUERY_TYPE type, UINT index) {
        Forward<BeginQuery, N>(cl, heap, type, index);
        active->Update<BeginQuery>(cl, [&](State &s) {
            s.output_proof.QueryBegin(reinterpret_cast<uintptr_t>(heap), type, index);
        });
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnEndQuery(ID3D12GraphicsCommandList *cl, ID3D12QueryHeap *heap,
                                             D3D12_QUERY_TYPE type, UINT index) {
        Forward<EndQuery, N>(cl, heap, type, index);
        active->Update<EndQuery>(cl, [&](State &s) {
            s.output_proof.QueryEnd(reinterpret_cast<uintptr_t>(heap), type, index);
        });
    }
    template <Method M, size_t... N> static auto Addresses(std::index_sequence<N...>) {
        if constexpr (M == GraphicsRoot)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnGraphicsRoot<N>)...};
        if constexpr (M == GraphicsTable)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnGraphicsTable<N>)...};
        if constexpr (M == GraphicsCBV)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnGraphicsCBV<N>)...};
        if constexpr (M == GraphicsTarget)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnGraphicsTarget<N>)...};
        if constexpr (M == Viewport)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnViewport<N>)...};
        if constexpr (M == Scissor)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnScissor<N>)...};
        if constexpr (M == Predication)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnPredication<N>)...};
        if constexpr (M == BeginPass)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnBeginPass<N>)...};
        if constexpr (M == EndPass)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnEndPass<N>)...};
        if constexpr (M == StencilRef)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnStencilRef<N>)...};
        if constexpr (M == StencilRefs)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnStencilRefs<N>)...};
        if constexpr (M == BeginQuery)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnBeginQuery<N>)...};
        if constexpr (M == EndQuery)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnEndQuery<N>)...};
        if constexpr (M == PSO)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnPSO<N>)...};
        if constexpr (M == Root)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnRoot<N>)...};
        if constexpr (M == Table)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnTable<N>)...};
        if constexpr (M == Word)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnWord<N>)...};
        if constexpr (M == Words)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnWords<N>)...};
        if constexpr (M == CBV || M == SRV || M == UAV)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnAddress<M, N>)...};
        if constexpr (M == Heaps)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnHeaps<N>)...};
        if constexpr (M == Indirect)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnIndirect<N>)...};
        if constexpr (M == Bundle)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnBundle<N>)...};
        if constexpr (M == Clear)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnClear<N>)...};
        if constexpr (M == RayPSO)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnRay<N>)...};
        if constexpr (M == Program)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnProgram<N>)...};
    }
    template <Method M> bool Hook(void *target) {
        auto &family = hooks[M];
        for (auto &s : family)
            if (s.target == target)
                return s.original != nullptr;
        for (size_t k = 0; k < family.size(); ++k)
            if (!family[k].target) {
                void *original{};
                if (MH_CreateHook(target, Addresses<M>(std::make_index_sequence<8>{})[k],
                                  &original) != MH_OK)
                    return false;
                family[k].target = target;
                family[k].original = original;
                if (MH_EnableHook(target) != MH_OK) {
                    MH_RemoveHook(target);
                    family[k].original = nullptr;
                    return false;
                }
                return true;
            }
        return false;
    }
    static std::array<void *, Count> Targets(ID3D12GraphicsCommandList *cl) {
        std::array<void *, Count> targets{};
        auto **v = *reinterpret_cast<void ***>(cl);
        constexpr unsigned indices[] = {25, 29, 31, 33, 35, 37, 39, 41, 28, 59, 27, 11};
        for (unsigned k = 0; k < 12; ++k)
            targets[k] = v[indices[k]];
        targets[GraphicsRoot] = v[30];
        targets[GraphicsTable] = v[32];
        targets[GraphicsTarget] = v[46];
        targets[GraphicsCBV] = v[38];
        // SDK ID3D12GraphicsCommandList4Vtbl offsets are independently compiled in
        // tests/rr-sss-graphics-slots.cpp. No inferred driver-vtable layout.
        targets[Viewport] = v[21];
        targets[Scissor] = v[22];
        targets[Predication] = v[55];
        targets[StencilRef] = v[24];
        targets[BeginQuery] = v[52];
        targets[EndQuery] = v[53];
        ComPtr<ID3D12GraphicsCommandList8> cl8;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl8))) && cl8.Get() == cl)
            targets[StencilRefs] = (*reinterpret_cast<void ***>(cl8.Get()))[81];
        ComPtr<ID3D12GraphicsCommandList4> cl4;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl4)))) {
            auto **v4 = *reinterpret_cast<void ***>(cl4.Get());
            targets[RayPSO] = v4[75];
            if (cl4.Get() == cl) {
                targets[BeginPass] = v4[68];
                targets[EndPass] = v4[69];
            }
        }
        ComPtr<ID3D12GraphicsCommandList10> cl10;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl10))))
            targets[Program] = (*reinterpret_cast<void ***>(cl10.Get()))[84];
        return targets;
    }
    static bool CoreTargetsEqual(const std::array<void *, Count> &a,
                                 const std::array<void *, Count> &b) {
        for (unsigned i = 0; i < GraphicsTarget; ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }
    void OptionalTarget(void *target) noexcept {
        if (!target_healthy)
            return;
        try {
            if (!target || !Hook<GraphicsTarget>(target))
                target_healthy = false;
        } catch (...) {
            target_healthy = false;
        }
    }
    void OptionalSSS(void *target) noexcept {
        if (!sss_healthy)
            return;
        try {
            if (!target || !Hook<GraphicsCBV>(target))
                sss_healthy = false;
        } catch (...) {
            sss_healthy = false;
        }
    }
    void OptionalOutputs(const std::array<void *, Count> &targets) noexcept {
        if (!outputs_healthy)
            return;
        try {
            if (!targets[Viewport] || !targets[Scissor] || !targets[Predication] ||
                !targets[BeginPass] || !targets[EndPass] || !Hook<Viewport>(targets[Viewport]) ||
                !Hook<Scissor>(targets[Scissor]) ||
                (!shared_output_context &&
                 (!Hook<Predication>(targets[Predication]) ||
                  !Hook<BeginPass>(targets[BeginPass]) || !Hook<EndPass>(targets[EndPass]))))
                outputs_healthy = false;
        } catch (...) {
            outputs_healthy = false;
        }
    }
    void OptionalStencil(const std::array<void *, Count> &t) noexcept {
        if (!stencil_healthy)
            return;
        try {
            if (!t[StencilRef] || !t[StencilRefs] || !t[BeginQuery] || !t[EndQuery] ||
                !Hook<StencilRef>(t[StencilRef]) || !Hook<StencilRefs>(t[StencilRefs]) ||
                !Hook<BeginQuery>(t[BeginQuery]) || !Hook<EndQuery>(t[EndQuery]))
                stencil_healthy = false;
        } catch (...) {
            stencil_healthy = false;
        }
    }
    template <size_t... M>
    bool HookTargets(const std::array<void *, Count> &targets, std::index_sequence<M...>) {
        return ((!targets[M] || Hook<Method(M)>(targets[M])) && ...);
    }

  public:
    // Optional content observer registration must precede first Attach. Changing it
    // invalidates only the optional observer, never baseline compute restoration.
    bool SetWriteObservers(void (*before)(ID3D12GraphicsCommandList *, WriteKind),
                           void (*bound)(const OutputBindings &)) noexcept {
        try {
            std::lock_guard install(install_mutex);
            std::lock_guard lock(mutex);
            if (!before || !bound ||
                (!states.empty() && (before_write != before || bound_outputs != bound)) ||
                (before_write && before_write != before) ||
                (bound_outputs && bound_outputs != bound)) {
                write_observers_healthy = false;
                return false;
            }
            before_write = before;
            bound_outputs = bound;
            return write_observers_healthy.load();
        } catch (...) {
            write_observers_healthy = false;
            return false;
        }
    }
    // Called after each forwarded native ExecuteIndirect that is not Internal, outside
    // the tracker lock. Same registration rule as above; a refusal or an observer
    // exception turns off only this observer, for good.
    bool SetAfterIndirect(void (*observer)(const IndirectCall &),
                          void (*log)(const char *)) noexcept {
        try {
            std::lock_guard install(install_mutex);
            std::lock_guard lock(mutex);
            const auto current = after_indirect.load();
            if (!after_indirect_healthy || !observer || !log || (!current && !states.empty()) ||
                (current && (current != observer || indirect_log != log))) {
                after_indirect_healthy = false;
                return false;
            }
            if (!current) {
                indirect_log = log; // published by the release store below
                after_indirect.store(observer, std::memory_order_release);
            }
            return true;
        } catch (...) {
            after_indirect_healthy = false;
            return false;
        }
    }
    bool WriteObserverCoverage(ID3D12GraphicsCommandList *cl,
                               void (*before)(ID3D12GraphicsCommandList *, WriteKind),
                               void (*bound)(const OutputBindings &)) noexcept {
        try {
            if (!cl)
                return false;
            std::lock_guard install(install_mutex);
            std::lock_guard lock(mutex);
            auto i = states.find(cl);
            if (i == states.end())
                return false;
            if (before_write != before || bound_outputs != bound) {
                i->second.write_coverage_lost = true;
                return false;
            }
            return WriteCovered(i->second, Targets(cl));
        } catch (...) {
            write_observers_healthy = false;
            return false;
        }
    }
    OutputBindings CaptureOutputBindings(ID3D12GraphicsCommandList *cl) noexcept {
        OutputBindings result;
        result.command = cl;
        try {
            if (!cl)
                return result;
            std::lock_guard install(install_mutex);
            std::lock_guard lock(mutex);
            auto i = states.find(cl);
            if (i == states.end())
                return result;
            auto &s = i->second;
            result = s.bindings;
            result.command = cl;
            result.generation = s.resets;
            result.serial = s.binding_serial;
            result.covered = WriteCovered(s, Targets(cl)) && s.resets != 0;
            return result;
        } catch (...) {
            write_observers_healthy = false;
            return result;
        }
    }
    bool UseSharedOutputContext(bool (*coverage)(ID3D12GraphicsCommandList *)) noexcept {
        try {
            std::lock_guard install(install_mutex);
            std::lock_guard state(mutex);
            if (!coverage || (!states.empty() && shared_output_context != coverage) ||
                (shared_output_context && shared_output_context != coverage)) {
                outputs_healthy = false;
                return false;
            }
            shared_output_context = coverage;
            return outputs_healthy.load();
        } catch (...) {
            outputs_healthy = false;
            return false;
        }
    }
    void OutputPredication(ID3D12GraphicsCommandList *cl, bool enabled) noexcept {
        Update<Predication>(cl, [&](State &s) { s.output_proof.Predication(enabled); });
    }
    void OutputBeginPass(ID3D12GraphicsCommandList *cl, D3D12_RENDER_PASS_FLAGS flags) noexcept {
        Update<BeginPass>(cl, [&](State &s) {
            s.output_proof.BeginPass(flags != D3D12_RENDER_PASS_FLAG_NONE);
            s.output_bound = false;
        });
    }
    void OutputEndPass(ID3D12GraphicsCommandList *cl) noexcept {
        Update<EndPass>(cl, [](State &s) {
            s.output_proof.EndPass();
            s.output_bound = false;
        });
    }
    static inline thread_local bool internal{};
    struct Internal {
        bool previous = internal;
        Internal() {
            internal = true;
        }
        ~Internal() {
            internal = previous;
        }
    };
    bool Attach(ID3D12GraphicsCommandList *list) {
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(list);
        if (!native)
            return false;
        std::lock_guard lock(install_mutex);
        auto mh = MH_Initialize();
        if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        active = this;
        const auto targets = Targets(native.Get());
        if (!HookTargets(targets, std::make_index_sequence<GraphicsTarget>{})) {
            healthy = false;
            return false;
        }
        OptionalTarget(targets[GraphicsTarget]);
        OptionalSSS(targets[GraphicsCBV]);
        OptionalOutputs(targets);
        OptionalStencil(targets);
        std::lock_guard state_lock(mutex);
        if (states.size() >= 2048 && !states.contains(native.Get())) {
            healthy = false;
            return false;
        }
        auto [i, inserted] = states.try_emplace(native.Get());
        if (inserted)
            i->second.targets = targets;
        return true;
    }
    // ReShade emits the creation initial-PSO event after init_command_list. Any
    // observed native setter/reset closes this one-time initialization window;
    // later wrapper events must never repair invalidated or missing native state.
    void CreationPipeline(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso) {
        std::lock_guard lock(mutex);
        auto i = states.find(cl);
        if (i != states.end() && i->second.creation_pending &&
            i->second.attach_thread == GetCurrentThreadId()) {
            i->second.creation_pending = false;
            i->second.pipeline = pso;
        }
    }
    void Reset(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso = nullptr) noexcept {
        Error error;
        try {
            std::lock_guard install_lock(install_mutex);
            {
                std::lock_guard lock(mutex);
                if (!states.contains(cl))
                    return;
            }
            // The real Reset may select a driver-direct vtable. Refresh at this clean
            // recording boundary, before the caller can issue any new root/PSO setters.
            const auto targets = Targets(cl);
            if (!HookTargets(targets, std::make_index_sequence<GraphicsTarget>{})) {
                healthy = false;
                return;
            }
            OptionalTarget(targets[GraphicsTarget]);
            OptionalSSS(targets[GraphicsCBV]);
            OptionalOutputs(targets);
            OptionalStencil(targets);
            std::lock_guard lock(mutex);
            auto i = states.find(cl);
            if (i != states.end()) {
                auto &s = i->second;
                const auto graphics_serial = s.graphics_target_serial;
                const auto resets = s.resets;
                auto mirror = std::move(s.output_mirror);
                s = {};
                s.output_mirror = std::move(mirror);
                s.graphics_target_serial = graphics_serial;
                s.resets = resets + 1;
                s.targets = targets;
                s.creation_pending = false;
                s.pipeline = pso;
                s.bindings.known = true;
                s.output_proof.Reset(s.resets, cl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
            }
        } catch (...) {
            healthy = false;
        }
    }
    void Invalidate(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        auto at = states.find(cl);
        if (at != states.end()) {
            auto &s = at->second;
            s.args = {};
            s.pipeline = nullptr;
            s.root = nullptr;
            s.root_unknown = true;
            s.uncertain = true;
            s.graphics_unknown = true;
            s.graphics_tables = {};
            s.output_proof.Invalidate();
        }
    }
    void Forget(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        states.erase(cl);
    }
    void Define(ID3D12RootSignature *root, UINT count, const Shape *params) {
        std::lock_guard lock(mutex);
        if (count > 64 || !root || (count && !params) || layouts.size() >= 4096) {
            healthy = false;
            return;
        }
        Layout l;
        l.count = count;
        for (UINT i = 0; i < count; ++i)
            l.params[i] = params[i];
        layouts[root] = l;
    }
    void ForgetRoot(ID3D12RootSignature *root) {
        std::lock_guard lock(mutex);
        layouts.erase(root);
    }
    // The 32-bit value count of a root-constants parameter, or -1 when the root
    // signature or the parameter is not known to hold root constants.
    int RootConstantWords(ID3D12RootSignature *root, UINT parameter) noexcept {
        try {
            std::lock_guard lock(mutex);
            const auto i = layouts.find(root);
            if (!root || i == layouts.end() || parameter >= i->second.count)
                return -1;
            const auto &shape = i->second.params[parameter];
            return shape.kind == Kind::Constants && !shape.ignored ? int(shape.words) : -1;
        } catch (...) {
            return -1;
        }
    }
    bool TableBound(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pipeline,
                    ID3D12RootSignature *root, UINT parameter, UINT64 address) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        if (!healthy || parameter >= 64 || !address)
            return false;
        auto at = states.find(cl);
        if (at == states.end())
            return false;
        auto &s = at->second;
        if (!CoreTargetsEqual(Targets(cl), s.targets))
            s.coverage_lost = true;
        return !s.coverage_lost && !s.root_unknown && s.root == root && s.pipeline == pipeline &&
               s.args[parameter].kind == Kind::Table && s.args[parameter].address == address;
    }
    bool GraphicsTableBound(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pipeline,
                            ID3D12RootSignature *root, UINT parameter, UINT64 address) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        if (!healthy || parameter >= 64 || !address)
            return false;
        auto at = states.find(cl);
        if (at == states.end())
            return false;
        auto &s = at->second;
        if (!CoreTargetsEqual(Targets(cl), s.targets))
            s.coverage_lost = true;
        return !s.coverage_lost && !s.graphics_unknown && s.graphics_root == root &&
               s.pipeline == pipeline && s.heaps[0] &&
               s.graphics_tables[parameter].kind == Kind::Table &&
               s.graphics_tables[parameter].address == address;
    }
    // Optional SSS-only observation. Failure never changes core compute health.
    bool GraphicsCBVBound(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pipeline,
                          ID3D12RootSignature *root, UINT parameter, UINT64 address) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        if (!healthy || !sss_healthy || !cl)
            return false;
        auto i = states.find(cl);
        if (i == states.end())
            return false;
        auto &s = i->second;
        const auto current = Targets(cl);
        if (!CoreTargetsEqual(current, s.targets) || current[GraphicsCBV] != s.targets[GraphicsCBV])
            return false;
        return !s.coverage_lost && !s.graphics_unknown && s.pipeline == pipeline &&
               s.graphics_root == root && s.sss_cbvs.Matches(parameter, address);
    }
    UINT64 GraphicsTargetSerial(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        auto i = states.find(cl);
        return !target_healthy || i == states.end() ? 0 : i->second.graphics_target_serial;
    }
    ComPtr<ID3D12RootSignature> TransparencyGraphicsRoot(ID3D12GraphicsCommandList *cl) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        auto i = states.find(cl);
        if (!healthy || !cl || i == states.end())
            return {};
        auto &s = i->second;
        if (!CoreTargetsEqual(Targets(cl), s.targets) || s.coverage_lost || s.graphics_unknown ||
            !s.graphics_root)
            return {};
        return s.graphics_root;
    }
    bool GraphicsTargetBound(ID3D12GraphicsCommandList *cl, UINT64 handle, UINT64 serial) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        auto i = states.find(cl);
        if (!healthy || !target_healthy || !handle || !serial || i == states.end())
            return false;
        auto &s = i->second;
        const auto current = Targets(cl);
        if (!CoreTargetsEqual(current, s.targets) ||
            current[GraphicsTarget] != s.targets[GraphicsTarget])
            return false;
        return !s.coverage_lost && !s.graphics_unknown && s.graphics_target == handle &&
               s.graphics_target_serial == serial;
    }
    // SSS-only, immutable output descriptors. Failure must not disable baseline
    // compute restoration or target observation.
    rrsss::GraphicsOutputSnapshot CaptureGraphicsOutputs(ID3D12GraphicsCommandList *cl,
                                                         ID3D12PipelineState *expected = nullptr) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        rrsss::GraphicsOutputSnapshot result;
        result.command = cl;
        result.status = "native-output-hooks-unavailable";
        if (!healthy || !target_healthy || !outputs_healthy || !cl)
            return result;
        auto i = states.find(cl);
        result.status = "native-output-list-untracked";
        if (i == states.end())
            return result;
        auto &s = i->second;
        result.proof = s.output_proof;
        result.generation = s.output_proof.generation;
        const auto current = Targets(cl);
        bool covered = CoreTargetsEqual(current, s.targets) &&
                       current[GraphicsTarget] == s.targets[GraphicsTarget];
        for (unsigned k = Viewport; k < StencilRef; ++k)
            covered = covered && current[k] && current[k] == s.targets[k];
        if (shared_output_context)
            try {
                covered = shared_output_context(cl) && covered;
            } catch (...) {
                covered = false;
                outputs_healthy = false;
            }
        bool stencil_covered = stencil_healthy.load();
        for (unsigned k = StencilRef; k < Count; ++k)
            stencil_covered = stencil_covered && current[k] && current[k] == s.targets[k];
        if (!stencil_covered)
            s.stencil_coverage_lost = true;
        result.proof.StencilTracking(stencil_covered && !s.stencil_coverage_lost);
        if (!covered)
            s.output_coverage_lost = true;
        result.status = "native-output-hook-target-changed-requires-reset";
        if (s.coverage_lost || s.output_coverage_lost)
            return result;
        result.status = "native-output-recording-or-dynamic-state-unproven";
        if (!s.output_proof.Available() || s.graphics_unknown)
            return result;
        result.status = "native-output-pipeline-mismatch";
        if (!s.pipeline || (expected && expected != s.pipeline))
            return result;
        result.status = "native-output-descriptors-unavailable";
        if (!s.output_bound || !s.output_mirror || !s.output_mirror->valid)
            return result;
        result.outputs = s.output_mirror;
        result.status = "native-output-layout-unsupported";
        if (s.output_mirror->count != 1)
            return result;
        result.pipeline = s.pipeline;
        result.valid = true;
        result.status = "available";
        return result;
    }
    // This proof deliberately leaves unrelated unknown arguments untouched.
    // DATA_STATIC (8) cannot reference a buffer produced later on the GPU.
    CBVPair RateCBVs(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pipeline,
                     ID3D12RootSignature *root) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        CBVPair result;
        if (!healthy)
            return result;
        auto at = states.find(cl);
        if (at == states.end())
            return result;
        auto &s = at->second;
        if (!CoreTargetsEqual(Targets(cl), s.targets))
            s.coverage_lost = true;
        if (s.coverage_lost || s.root_unknown || s.root != root || s.pipeline != pipeline)
            return result;
        auto layout = layouts.find(root);
        if (layout == layouts.end())
            return result;
        for (unsigned k = 0; k < 2; ++k) {
            unsigned matches = 0;
            for (UINT p = 0; p < layout->second.count; ++p) {
                const auto &shape = layout->second.params[p];
                if (shape.kind != Kind::CBV || !shape.compute || shape.space ||
                    shape.shader_register != (k ? 3u : 1u))
                    continue;
                ++matches;
                const auto &a = s.args[p];
                if ((shape.flags != 0 && shape.flags != 2 && shape.flags != 4) ||
                    a.kind != Kind::CBV || !a.address || (a.address & 255))
                    return result;
                result.parameters[k] = p;
                result.addresses[k] = a.address;
            }
            if (matches != 1)
                return result;
        }
        result.valid = true;
        return result;
    }
    Snapshot Capture(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *expected = nullptr) {
        std::lock_guard install_lock(install_mutex);
        std::lock_guard lock(mutex);
        Snapshot result;
        result.status = "native-tracker-unhealthy";
        if (!healthy)
            return result;
        auto i = states.find(cl);
        result.status = "native-list-untracked";
        if (i == states.end())
            return result;
        auto &s = i->second;
        const auto current = Targets(cl);
        if (!CoreTargetsEqual(current, s.targets))
            s.coverage_lost = true;
        result.status = "native-hook-target-changed-requires-reset";
        if (s.coverage_lost)
            return result;
        result.status = "native-pipeline-or-root-unknown";
        if (!s.pipeline || !s.root || s.root_unknown)
            return result;
        // ReShade exposes native PSO handles. Treat the expected handle as an opaque
        // value: a stale shadow may refer to an already released previous pipeline.
        result.status = "recognized-prepass-pipeline-mismatch";
        if (expected && expected != s.pipeline)
            return result;
        result.status = "arguments-incomplete-after-indirect-or-bundle";
        auto l = layouts.find(s.root);
        result.complete = l != layouts.end();
        if (result.complete)
            for (UINT p = 0; p < l->second.count; ++p) {
                const auto &shape = l->second.params[p];
                const auto &a = s.args[p];
                if (shape.ignored)
                    continue;
                if (shape.kind == Kind::Empty || shape.kind != a.kind) {
                    result.complete = false;
                    break;
                }
                if (shape.kind == Kind::Constants) {
                    if (shape.words > 64 || !shape.words) {
                        result.complete = false;
                        break;
                    }
                    const auto mask = shape.words == 64 ? ~0ull : (1ull << shape.words) - 1;
                    if ((a.mask & mask) != mask) {
                        result.complete = false;
                        break;
                    }
                }
            }
        if (s.uncertain && !result.complete)
            return result;
        result.heap_restore_valid = !s.graphics_unknown;
        result.graphics_tables = s.graphics_tables;
        for (unsigned k = 0; k < 2; ++k)
            result.heaps[k] = s.heaps[k];
        result.pipeline = s.pipeline;
        result.root = s.root;
        result.args = s.args;
        result.command = cl;
        result.valid = true;
        result.status = "available";
        return result;
    }
#ifdef RRGLASS_TESTING
    void DisableTargetProofForTest() {
        target_healthy = false;
    }
#endif
    // Hooks are production process-lifetime; fixtures may remove them after GPU idle.
    void RemoveForTest() {
        std::lock_guard lock(install_mutex);
        for (auto &family : hooks)
            for (auto &s : family)
                if (s.target) {
                    MH_DisableHook(s.target);
                    MH_RemoveHook(s.target);
                    s.target = nullptr;
                    s.original = nullptr;
                }
        active = nullptr;
    }
};
} // namespace rrcompute
