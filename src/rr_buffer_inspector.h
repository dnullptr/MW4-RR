#pragma once
#include "feed_fg_identity.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <MinHook.h>

namespace rrbuffer {
using Microsoft::WRL::ComPtr;
using BarrierFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT,
                                            const D3D12_RESOURCE_BARRIER *);
using ExecuteFn = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
class Inspector {
    friend struct InspectorTestAccess;

  public:
    using OutputPredicate = void (*)(ID3D12GraphicsCommandList *, bool);
    using OutputBegin = void (*)(ID3D12GraphicsCommandList *, D3D12_RENDER_PASS_FLAGS);
    using OutputEnd = void (*)(ID3D12GraphicsCommandList *);

  private:
    OutputPredicate output_predicate{};
    OutputBegin output_begin{};
    OutputEnd output_end{};
    std::atomic<bool> output_observer_ready{}, output_observer_failed{};
    enum Method { Barrier, Execute, Reset, BeginPass, EndPass, Predicate, Enhanced, MethodCount };
    struct Slot {
        void *target{};
        std::atomic<void *> original{};
    };
    std::array<std::array<Slot, 8>, MethodCount> slots;
    std::mutex install_mutex, context_mutex;
    struct Binding {
        bool in_pass{}, predicated{};
    };
    std::unordered_map<ID3D12GraphicsCommandList *, Binding> bindings;
    std::atomic<bool> execution_ready{};
    static inline thread_local unsigned nesting{};
    struct Nested {
        Nested() {
            ++nesting;
        }
        ~Nested() {
            --nesting;
        }
    };
    struct ErrorGuard {
        DWORD error = GetLastError();
        ~ErrorGuard() {
            SetLastError(error);
        }
    };
    using EnhancedFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList7 *, UINT,
                                                 const D3D12_BARRIER_GROUP *);
    using ResetFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *,
                                                 ID3D12CommandAllocator *, ID3D12PipelineState *);
    using BeginFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *, UINT,
                                              const D3D12_RENDER_PASS_RENDER_TARGET_DESC *,
                                              const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *,
                                              D3D12_RENDER_PASS_FLAGS);
    using EndFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *);
    using PredicateFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12Resource *,
                                                  UINT64, D3D12_PREDICATION_OP);
    template <size_t N>
    static void STDMETHODCALLTYPE OnBarrier(ID3D12GraphicsCommandList *cl, UINT n,
                                            const D3D12_RESOURCE_BARRIER *barriers) {
        auto &self = *active;
        auto original = reinterpret_cast<BarrierFn>(self.slots[Barrier][N].original.load());
        if (!nesting && self.on_barriers) {
            ErrorGuard error;
            try {
                self.on_barriers(cl, n, barriers);
            } catch (...) {
                self.Log("CPU barrier observer failed; original call preserved");
            }
        }
        if (!nesting && self.execution_ready && self.observe_transitions && self.after_transition &&
            n && n <= 4096 && barriers) {
            Binding binding;
            bool known = false;
            {
                std::lock_guard lock(self.context_mutex);
                auto i = self.bindings.find(cl);
                if (i != self.bindings.end()) {
                    known = true;
                    binding = i->second;
                }
            }
            if (known && !binding.in_pass && !binding.predicated) {
                // A hooked debug/runtime layer may forward into another hooked layer.
                // Observe this transition once on the outer tracked recording that the
                // queue submits, before invoking its post-original copy callback.
                for (UINT i = 0; i < n; ++i) {
                    {
                        Nested nested;
                        original(cl, 1, barriers + i);
                    }
                    ErrorGuard error;
                    if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                        Nested nested;
                        try {
                            self.after_transition(cl, barriers[i], original);
                        } catch (...) {
                            self.Log(
                                "post-transition observer failed; following barriers preserved");
                        }
                    }
                }
                return;
            }
        }
        original(cl, n, barriers);
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnEnhanced(ID3D12GraphicsCommandList7 *cl, UINT n,
                                             const D3D12_BARRIER_GROUP *groups) {
        auto &self = *active;
        if (!nesting && self.on_unknown_states) {
            ErrorGuard error;
            try {
                self.on_unknown_states(cl);
            } catch (...) {
                self.Log("Enhanced barrier observer failed; original preserved");
            }
        }
        reinterpret_cast<EnhancedFn>(self.slots[Enhanced][N].original.load())(cl, n, groups);
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnExecute(ID3D12CommandQueue *queue, UINT n,
                                            ID3D12CommandList *const *lists) {
        auto &self = *active;
        auto original = reinterpret_cast<ExecuteFn>(self.slots[Execute][N].original.load());
        if (nesting) {
            original(queue, n, lists);
            return;
        }
        uint64_t observation{};
        if (self.on_execute) {
            ErrorGuard error;
            try {
                observation = self.on_execute(queue, n, lists);
            } catch (...) {
                self.Log("Submission observer failed; original call preserved");
            }
        }
        struct EndObservation {
            Inspector &self;
            ID3D12CommandQueue *queue;
            uint64_t token;
            ~EndObservation() {
                ErrorGuard error;
                if (self.on_execute_end)
                    try {
                        self.on_execute_end(queue, token);
                    } catch (...) {
                        self.Log("Submission return observer failed; original result preserved");
                    }
            }
        } end_observation{self, queue, observation};
        // Preserve nested wrapper observation: a wrapper may unwrap list identities.
        original(queue, n, lists);
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnReset(ID3D12GraphicsCommandList *cl,
                                             ID3D12CommandAllocator *allocator,
                                             ID3D12PipelineState *pso) {
        auto &self = *active;
        auto original = reinterpret_cast<ResetFn>(self.slots[Reset][N].original.load());
        auto hr = original(cl, allocator, pso);
        ErrorGuard error;
        if (SUCCEEDED(hr)) {
            self.AttachList(cl);
            if (self.on_reset)
                try {
                    self.on_reset(cl);
                } catch (...) {
                    self.Log("CPU reset observer failed; original result preserved");
                }
            if (self.on_reset_compute)
                try {
                    self.on_reset_compute(cl, pso);
                } catch (...) {
                    self.Log("Compute reset observer failed; original result preserved");
                }
            std::lock_guard lock(self.context_mutex);
            auto it = self.bindings.find(cl);
            if (it != self.bindings.end())
                it->second = {};
        }
        return hr;
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnBegin(ID3D12GraphicsCommandList4 *cl, UINT count,
                                          const D3D12_RENDER_PASS_RENDER_TARGET_DESC *rt,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *ds,
                                          D3D12_RENDER_PASS_FLAGS flags) {
        auto &self = *active;
        reinterpret_cast<BeginFn>(self.slots[BeginPass][N].original.load())(cl, count, rt, ds,
                                                                            flags);
        ErrorGuard error;
        {
            std::lock_guard lock(self.context_mutex);
            auto it = self.bindings.find(cl);
            if (it != self.bindings.end())
                it->second.in_pass = true;
        }
        if (self.output_observer_ready.load(std::memory_order_acquire))
            try {
                self.output_begin(cl, flags);
            } catch (...) {
                self.output_observer_failed = true;
            }
    }
    template <size_t N> static void STDMETHODCALLTYPE OnEnd(ID3D12GraphicsCommandList4 *cl) {
        auto &self = *active;
        reinterpret_cast<EndFn>(self.slots[EndPass][N].original.load())(cl);
        ErrorGuard error;
        {
            std::lock_guard lock(self.context_mutex);
            auto it = self.bindings.find(cl);
            if (it != self.bindings.end())
                it->second.in_pass = false;
        }
        if (self.output_observer_ready.load(std::memory_order_acquire))
            try {
                self.output_end(cl);
            } catch (...) {
                self.output_observer_failed = true;
            }
    }
    template <size_t N>
    static void STDMETHODCALLTYPE OnPredicate(ID3D12GraphicsCommandList *cl,
                                              ID3D12Resource *resource, UINT64 offset,
                                              D3D12_PREDICATION_OP op) {
        auto &self = *active;
        reinterpret_cast<PredicateFn>(self.slots[Predicate][N].original.load())(cl, resource,
                                                                                offset, op);
        ErrorGuard error;
        {
            std::lock_guard lock(self.context_mutex);
            auto it = self.bindings.find(cl);
            if (it != self.bindings.end())
                it->second.predicated = resource != nullptr;
        }
        if (self.output_observer_ready.load(std::memory_order_acquire))
            try {
                self.output_predicate(cl, resource != nullptr);
            } catch (...) {
                self.output_observer_failed = true;
            }
    }
    template <Method M, size_t... N> static auto Addresses(std::index_sequence<N...>) {
        if constexpr (M == Enhanced)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnEnhanced<N>)...};
        if constexpr (M == Barrier)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnBarrier<N>)...};
        if constexpr (M == Execute)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnExecute<N>)...};
        if constexpr (M == Reset)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnReset<N>)...};
        if constexpr (M == BeginPass)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnBegin<N>)...};
        if constexpr (M == EndPass)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnEnd<N>)...};
        if constexpr (M == Predicate)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnPredicate<N>)...};
    }
    template <Method M> bool Hook(void *target) {
        auto &family = slots[M];
        for (auto &s : family)
            if (s.target == target)
                return s.original != nullptr;
        for (size_t i = 0; i < family.size(); ++i)
            if (!family[i].target) {
                void *original{};
                auto status = MH_CreateHook(target, Addresses<M>(std::make_index_sequence<8>{})[i],
                                            &original);
                if (status != MH_OK) {
                    Log("buffer native hook creation failed");
                    return false;
                }
                family[i].target = target;
                family[i].original = original;
                if (MH_EnableHook(target) != MH_OK) {
                    MH_RemoveHook(target);
                    family[i].original = nullptr;
                    Log("buffer native hook enable failed");
                    return false;
                }
                return true;
            }
        Log("buffer native hook slot limit reached");
        return false;
    }
    void Log(const char *message) {
        if (logger)
            logger(message);
    }

  public:
    // One observer, configured before attachment. A changed registration can hide
    // native events, so it permanently loses only this optional coverage proof.
    bool SetOutputObserver(OutputPredicate predicate, OutputBegin begin, OutputEnd end) noexcept {
        try {
            std::lock_guard lock(install_mutex);
            if (!predicate || !begin || !end) {
                output_observer_failed = true;
                return false;
            }
            if (output_observer_ready.load()) {
                const bool same =
                    predicate == output_predicate && begin == output_begin && end == output_end;
                if (!same)
                    output_observer_failed = true;
                return same && !output_observer_failed;
            }
            output_predicate = predicate;
            output_begin = begin;
            output_end = end;
            output_observer_ready.store(true, std::memory_order_release);
            return !output_observer_failed;
        } catch (...) {
            output_observer_failed = true;
            return false;
        }
    }
    bool OutputContextCoverage(ID3D12GraphicsCommandList *cl, OutputPredicate predicate,
                               OutputBegin begin, OutputEnd end) {
        if (!cl || !output_observer_ready.load(std::memory_order_acquire) || output_observer_failed)
            return false;
        std::lock_guard lock(install_mutex);
        if (predicate != output_predicate || begin != output_begin || end != output_end)
            return false;
        auto covered = [&](Method m, void *target) {
            for (const auto &s : slots[m])
                if (s.target == target && s.original.load())
                    return true;
            return false;
        };
        auto **v = *reinterpret_cast<void ***>(cl);
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl4;
        if (FAILED(cl->QueryInterface(IID_PPV_ARGS(&cl4))) || cl4.Get() != cl)
            return false;
        auto **v4 = *reinterpret_cast<void ***>(cl4.Get());
        return covered(Predicate, v[55]) && covered(BeginPass, v4[68]) && covered(EndPass, v4[69]);
    }
    std::atomic<bool> observe_transitions{};
    void (*after_transition)(ID3D12GraphicsCommandList *, const D3D12_RESOURCE_BARRIER &,
                             BarrierFn){};
    bool SafeCopies(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(context_mutex);
        auto i = bindings.find(cl);
        return i != bindings.end() && !i->second.in_pass && !i->second.predicated;
    }
    void (*on_unknown_states)(ID3D12GraphicsCommandList *){};
    void (*on_barriers)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *){};
    uint64_t (*on_execute)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *){};
    void (*on_execute_end)(ID3D12CommandQueue *, uint64_t){};
    void (*on_reset)(ID3D12GraphicsCommandList *){};
    void (*on_reset_compute)(ID3D12GraphicsCommandList *, ID3D12PipelineState *){};
    void (*logger)(const char *){};
    static inline Inspector *active{};
    bool AttachQueue(ID3D12CommandQueue *queue) {
        auto native = fg::NativeObject<ID3D12CommandQueue>(queue);
        if (!native)
            return false;
        std::lock_guard lock(install_mutex);
        auto mh = MH_Initialize();
        if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        active = this;
        auto **vt = *reinterpret_cast<void ***>(native.Get());
        bool ok = Hook<Execute>(vt[10]);
        if (ok)
            execution_ready = true;
        return ok;
    }
    bool AttachList(ID3D12GraphicsCommandList *list) {
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(list);
        if (!native)
            return false;
        std::lock_guard lock(install_mutex);
        auto mh = MH_Initialize();
        if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        active = this;
        auto **vt = *reinterpret_cast<void ***>(native.Get());
        bool ok = Hook<Reset>(vt[10]) && Hook<Predicate>(vt[55]);
        ComPtr<ID3D12GraphicsCommandList4> cl4;
        if (SUCCEEDED(native.As(&cl4))) {
            auto **vt4 = *reinterpret_cast<void ***>(cl4.Get());
            ok = ok && Hook<BeginPass>(vt4[68]) && Hook<EndPass>(vt4[69]);
        }
        ComPtr<ID3D12GraphicsCommandList7> cl7;
        if (SUCCEEDED(native.As(&cl7))) {
            auto **vt7 = *reinterpret_cast<void ***>(cl7.Get());
            ok = ok && Hook<Enhanced>(vt7[80]);
        }
        ok = ok && Hook<Barrier>(vt[26]);
        if (ok) {
            std::lock_guard context_lock(context_mutex);
            if (bindings.size() < 8192) {
                bindings.try_emplace(native.Get());
            }
        }
        return ok;
    }
    bool StateCoverage(ID3D12GraphicsCommandList *cl) {
        if (!cl || !execution_ready)
            return false;
        std::lock_guard lock(install_mutex);
        auto covered = [&](Method method, void *target) {
            for (const auto &s : slots[method])
                if (s.target == target && s.original.load())
                    return true;
            return false;
        };
        auto **vt = *reinterpret_cast<void ***>(cl);
        bool ok = covered(Barrier, vt[26]) && covered(Reset, vt[10]) && covered(Predicate, vt[55]);
        ComPtr<ID3D12GraphicsCommandList4> cl4;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl4)))) {
            auto **v = *reinterpret_cast<void ***>(cl4.Get());
            ok = ok && covered(BeginPass, v[68]) && covered(EndPass, v[69]);
        }
        ComPtr<ID3D12GraphicsCommandList7> cl7;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl7)))) {
            auto **v = *reinterpret_cast<void ***>(cl7.Get());
            ok = ok && covered(Enhanced, v[80]);
        }
        return ok;
    }
    void Forget(ID3D12GraphicsCommandList *list) {
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(list);
        if (!native)
            return;
        std::lock_guard lock(context_mutex);
        bindings.erase(native.Get());
    }
    void RemoveForTest() {
        std::lock_guard lock(install_mutex);
        for (auto &family : slots)
            for (auto &slot : family)
                if (slot.target) {
                    MH_DisableHook(slot.target);
                    MH_RemoveHook(slot.target);
                    slot.target = nullptr;
                    slot.original = nullptr;
                }
        active = nullptr;
    }
};
} // namespace rrbuffer
