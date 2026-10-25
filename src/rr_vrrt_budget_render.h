#pragma once
// The ray budget test's hooks (rr_ray_budget.h; the controller is in rr_live_ray.inl, included
// in rrlive::Controller in rr_live.h): the proofs at the
// game's cs_vrrt_resolve and VRRT apply, and the lock-free check of indirect commands that read
// the ray lists. Included by mw4_rr.cpp after rr_sparse_render.h and before rr_vrrt_render.h.
// Every function is noexcept and none calls rrbypass::Fail, GlobalFail or SparseFailure: a proof
// that fails only refuses (the native dispatch runs), an exception stops the test for the session.

// The descriptor range flags of the Budget u0 and the ray lists (rrraybudget::RangeLog, A5).
static rrraybudget::RangeFlag RayRangeFlag(const rrbindings::Location &l) noexcept {
    return l.known && l.flags_known ? rrraybudget::RangeFlag{l.range_flags, true}
                                    : rrraybudget::RangeFlag{};
}
static rrraybudget::RangeLog &RayRanges() {
    static rrraybudget::RangeLog log(&WriteLog);
    return log;
}
// One native root constant buffer at the resolve: register `reg` is the root CBV `parameter`
// bound at `address`, its tracked value a buffer on this device, 256-aligned, with `need` bytes
// inside the resource and the view.
static bool RayConstantProof(reshade::api::device *device, rrbindings::DeviceState *state, UINT64 l,
                             unsigned reg, UINT parameter, UINT64 address, UINT64 need,
                             rrvrrt::RayConstant &out) {
    const auto loc = state->tracker.FindRootConstant(l, reg);
    if (!loc.known || loc.parameter != parameter)
        return false;
    const auto v = state->tracker.Constant(l, reg);
    if (!v.known || !v.buffer || !v.resource)
        return false;
    auto r = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
    if (!r || !rrsparsecopy::SameDevice((ID3D12Device *)device->get_native(), r.Get()))
        return false;
    const auto desc = r->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || v.byte_offset % 256 ||
        v.byte_offset > desc.Width || need > desc.Width - v.byte_offset || v.byte_size < need)
        return false;
    const UINT64 va = r->GetGPUVirtualAddress();
    if (!va || va > UINT64_MAX - v.byte_offset || va + v.byte_offset != address)
        return false;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    out.resource = r;
    out.offset = v.byte_offset;
    out.address = address;
    out.avail = desc.Width - v.byte_offset;
    out.upload =
        SUCCEEDED(r->GetHeapProperties(&heap, &flags)) && heap.Type == D3D12_HEAP_TYPE_UPLOAD;
    return true;
}
// The tracked raw buffer view at a table-bound register of the resolve: the resource on this
// device and the view (r32_typeless, 4-aligned, inside Width). `loc` is where it was found.
static bool RayRawView(reshade::api::device *device, rrbindings::DeviceState *state,
                       ID3D12GraphicsCommandList *native, UINT64 l, ID3D12PipelineState *pipeline,
                       ID3D12RootSignature *root, rrbindings::Register reg,
                       rrbindings::Location &loc, rrbindings::Value &v,
                       Microsoft::WRL::ComPtr<ID3D12Resource> &r, D3D12_RESOURCE_DESC &desc) {
    loc = state->tracker.Find(l, reg);
    if (!loc.known || !ComputeState().TableBound(native, pipeline, root, loc.parameter, loc.table))
        return false;
    const auto slot = rrbindings::Resolve(device, {loc.table}, loc.offset);
    if (!slot.heap)
        return false;
    v = state->tracker.Get(slot);
    if (!v.known || !v.buffer || !v.resource ||
        v.view_format != unsigned(reshade::api::format::r32_typeless))
        return false;
    r = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
    if (!r || !rrsparsecopy::SameDevice((ID3D12Device *)device->get_native(), r.Get()))
        return false;
    desc = r->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && !(v.byte_offset % 4) &&
           v.byte_offset <= desc.Width && v.byte_size <= desc.Width - v.byte_offset;
}
// The resolve's t0 (A2b): the governor's input, copied by LogResolve when its state allows. Any
// failure leaves `out` empty; that is never a refusal by itself (the gate's StatsUnverified).
static void RayStatsProof(reshade::api::device *device, rrbindings::DeviceState *state,
                          ID3D12GraphicsCommandList *native, UINT64 l,
                          ID3D12PipelineState *pipeline, ID3D12RootSignature *root,
                          rrvrrt::RayStats &out) {
    out = {};
    rrbindings::Location loc;
    rrbindings::Value v;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    D3D12_RESOURCE_DESC desc{};
    if (!RayRawView(device, state, native, l, pipeline, root, {false, 0}, loc, v, r, desc))
        return;
    D3D12_RESOURCE_STATES observed{};
    out.known = SparseLayouts().Get(native, r.Get(), observed);
    out.state = observed;
    out.resource = r;
    out.offset = v.byte_offset;
    out.avail = v.byte_size;
}
// The Budget u0 the resolve writes: a table-bound raw view of at least 32 bytes in a UAV buffer,
// not a root UAV, in UNORDERED_ACCESS or COMMON when its state is known (A6: unknown is
// accepted). Its identity is (native resource pointer, view byte offset), as the apply sees it.
static bool RayBudgetProof(reshade::api::device *device, rrbindings::DeviceState *state,
                           ID3D12GraphicsCommandList *native, UINT64 l,
                           ID3D12PipelineState *pipeline, ID3D12RootSignature *root,
                           rrvrrt::RayResolve &in, rrbindings::Location &loc) {
    const auto root_uav = state->tracker.FindRootResource(l, {true, 0});
    if (root_uav.known) {
        loc = root_uav;
        return false;
    }
    rrbindings::Value v;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    D3D12_RESOURCE_DESC desc{};
    if (!RayRawView(device, state, native, l, pipeline, root, {true, 0}, loc, v, r, desc) ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || v.byte_size < 32)
        return false;
    D3D12_RESOURCE_STATES observed{};
    if (SparseLayouts().Get(native, r.Get(), observed) &&
        observed != D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
        observed != D3D12_RESOURCE_STATE_COMMON)
        return false;
    in.budget_resource = UINT64(r.Get());
    in.budget_offset = v.byte_offset;
    return true;
}
// The ladder of design §5 at a resolve inside a live recording. b1 proven and in an upload heap
// enables the logging copies (`constants`) even when the patch is refused later.
static rrvrrt::RayResolve ProveRayResolve(reshade::api::command_list *list,
                                          ID3D12GraphicsCommandList *native,
                                          const rrshader::Binding &b, UINT x, UINT y,
                                          UINT z) noexcept {
    using rrraybudget::Why;
    rrvrrt::RayResolve in; // Why::Context
    try {
        auto *device = list->get_device();
        auto *state = rrbindings::State(device);
        if (!state || !state->tracker.Healthy() || !AlbedoInspector().SafeCopies(native) ||
            !AlbedoInspector().StateCoverage(native) || rrbypass::Health(device).failed)
            return in;
        const UINT64 l = list->get_native();
        auto *pipeline = (ID3D12PipelineState *)b.pipeline;
        auto *root = (ID3D12RootSignature *)state->tracker.CurrentLayout(l);
        if (!pipeline || !root)
            return in;
        in.pipeline = pipeline;
        in.root = root;
        in.why = Why::Shape;
        if (x != 2 || y != 1 || z != 1)
            return in;
        in.why = Why::Constants;
        const auto b1 = ComputeState().RootCBV(native, pipeline, root, 1);
        if (!b1.valid ||
            !RayConstantProof(device, state, l, 1, b1.parameter, b1.address, 48, in.settings))
            return in;
        in.parameter = b1.parameter;
        if (!in.settings.upload) {
            in.why = Why::Heap;
            return in;
        }
        in.constants = true;
        // The patch copies the game's first GameBytes of b1 (A6); logging needs 48.
        Why why = in.settings.avail < rrraycopy::GameBytes ? Why::Constants : Why::None;
        // b3 register 86 (W, H) is the governor's extent for this frame (A2): a patch needs it.
        const auto b3 = ComputeState().RootCBV(native, pipeline, root, 3);
        rrvrrt::RayConstant scene;
        if (b3.valid &&
            RayConstantProof(device, state, l, 3, b3.parameter, b3.address, 1392, scene))
            in.scene = scene;
        else if (why == Why::None)
            why = Why::Constants;
        RayStatsProof(device, state, native, l, pipeline, root, in.stats);
        // Proven whatever the refusal: its identity clears the Budget's mark (A4).
        rrbindings::Location u0;
        const bool budget = RayBudgetProof(device, state, native, l, pipeline, root, in, u0);
        RayRanges().Resolve(RayRangeFlag(u0));
        if (why == Why::None && !budget)
            why = Why::BudgetBinding;
        in.why = why;
        return in;
    } catch (...) {
        rrvrrt::RayResolve context; // a tracker or lookup failure proves nothing
        return context;
    }
}
// The VRRT apply's ray lists u5, u6, u3, u2 (Lists::v order): each a raw table view of a UAV
// buffer on this device, bound in the captured state, pairwise disjoint on one resource. The
// Budget identity, extent and grid are filled even when a list is not proven. `where` receives
// each list's location (for the range flags).
static rrraybudget::Lists ProveVrrtLists(reshade::api::device *device,
                                         rrbindings::DeviceState *state, UINT64 l,
                                         const rrcompute::Snapshot &restore,
                                         const rrvrrt::Inputs &input, UINT x, UINT y,
                                         std::array<rrbindings::Location, 4> &where) {
    using rrraybudget::Binding;
    using rrraybudget::Why;
    rrraybudget::Lists out;
    out.budget_resource = UINT64(input.resources[1].Get());
    out.budget_offset = input.budget_offset;
    out.w = input.w;
    out.h = input.h;
    out.gx = x;
    out.gy = y;
    Why why = Why::None;
    auto refuse = [&](Why w) {
        if (why == Why::None)
            why = w;
    };
    static constexpr unsigned registers[4] = {5, 6, 3, 2};
    for (unsigned k = 0; k < 4; ++k) {
        auto &list = out.v[k];
        const auto root_uav = state->tracker.FindRootResource(l, {true, registers[k]});
        if (root_uav.known) {
            where[k] = root_uav;
            list.binding = Binding::Root;
            refuse(Why::ListsRoot);
            continue;
        }
        const auto loc = state->tracker.Find(l, {true, registers[k]});
        where[k] = loc;
        if (!loc.known || loc.parameter >= restore.args.size() ||
            restore.args[loc.parameter].kind != rrcompute::Kind::Table ||
            restore.args[loc.parameter].address != loc.table) {
            refuse(Why::ListsUnproven);
            continue;
        }
        const auto slot = rrbindings::Resolve(device, {loc.table}, loc.offset);
        const auto v = slot.heap ? state->tracker.Get(slot) : rrbindings::Value{};
        auto r = v.known && v.resource
                     ? fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource)
                     : Microsoft::WRL::ComPtr<ID3D12Resource>{};
        D3D12_RESOURCE_DESC desc{};
        if (r)
            desc = r->GetDesc();
        if (!r || !v.buffer || v.view_format != unsigned(reshade::api::format::r32_typeless) ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || v.byte_offset % 4 ||
            !v.byte_size || v.byte_offset > desc.Width ||
            v.byte_size > desc.Width - v.byte_offset ||
            !rrsparsecopy::SameDevice((ID3D12Device *)device->get_native(), r.Get())) {
            list.binding = Binding::Invalid;
            refuse(Why::ListsView);
            continue;
        }
        list = {Binding::Table, v.resource, v.byte_offset, v.byte_size};
    }
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = a + 1; b < 4; ++b) {
            const auto &p = out.v[a], &q = out.v[b];
            if (p.binding == Binding::Table && q.binding == Binding::Table &&
                p.resource == q.resource && p.offset < q.offset + q.bytes &&
                q.offset < p.offset + p.bytes)
                refuse(Why::ListsOverlap);
        }
    out.why = why;
    return out;
}
// One of the apply's constant buffers, already proven by PreserveNativeSampling, for its logging
// copies: the offset from the resource's VA, the bytes after it, and whether it is an upload heap.
static void RayApplyConstant(const Microsoft::WRL::ComPtr<ID3D12Resource> &r, UINT64 address,
                             rrvrrt::RayConstant &out) {
    if (!r)
        return;
    const UINT64 va = r->GetGPUVirtualAddress();
    const auto desc = r->GetDesc();
    if (!va || address < va || address - va > desc.Width)
        return;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    out.resource = r;
    out.offset = address - va;
    out.address = address;
    out.avail = desc.Width - out.offset;
    out.upload =
        SUCCEEDED(r->GetHeapProperties(&heap, &flags)) && heap.Type == D3D12_HEAP_TYPE_UPLOAD;
}
// After PreserveNativeSampling's reader ran: the apply's list proof and logging constants. It
// always reports the apply (A3), unproven when the proof itself failed.
static void ObserveVrrtApply(reshade::api::command_list *list, reshade::api::device *device,
                             rrbindings::DeviceState *state, ID3D12GraphicsCommandList *native,
                             const rrcompute::Snapshot &restore, const rrvrrt::Inputs &input,
                             UINT x, UINT y) noexcept {
    try {
        std::array<rrbindings::Location, 4> where{};
        const auto lists =
            ProveVrrtLists(device, state, list->get_native(), restore, input, x, y, where);
        rrvrrt::RayApply log;
        RayApplyConstant(input.resources[2], input.cbv[0], log.settings);
        RayApplyConstant(input.resources[3], input.cbv[1], log.scene);
        RayRanges().Apply({RayRangeFlag(where[0]), RayRangeFlag(where[1]), RayRangeFlag(where[2]),
                           RayRangeFlag(where[3])});
        LiveRR().RayBudgetApply(native, lists, log);
    } catch (...) {
        try {
            LiveRR().RayBudgetApply(native, rrraybudget::Lists{}, rrvrrt::RayApply{});
        } catch (...) {
        }
    }
}
// Reports a recognised native VRRT apply as unproven (A3) unless Done() was called: constructed
// right after PreserveNativeSampling recognises the shader, so its early returns stay unchanged.
class RayApplyReporter {
    reshade::api::command_list *list;
    bool done{};

  public:
    explicit RayApplyReporter(reshade::api::command_list *l) noexcept : list(l) {}
    RayApplyReporter(const RayApplyReporter &) = delete;
    RayApplyReporter &operator=(const RayApplyReporter &) = delete;
    void Done() noexcept {
        done = true;
    }
    ~RayApplyReporter() {
        if (done)
            return;
        try {
            auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
                (ID3D12GraphicsCommandList *)list->get_native());
            if (native)
                LiveRR().RayBudgetApply(native.Get(), rrraybudget::Lists{}, rrvrrt::RayApply{});
        } catch (...) {
        }
    }
};
// OnDispatch, after the guide prepass: true when the controller recorded the patched resolve in
// place of this dispatch (the caller then skips it), false to dispatch natively.
static bool PreserveRayBudget(reshade::api::command_list *list, uint32_t x, uint32_t y,
                              uint32_t z) noexcept {
    bool dispatched = false;
    try {
        auto *device = list->get_device();
        if (device->get_api() != reshade::api::device_api::d3d12)
            return false;
        const auto b = ShaderDispatches().Get(list->get_native());
        if ((b.shader != rrraybudget::Resolve && b.shader != rrraybudget::Clear) || !b.pipeline)
            return false;
        if (b.shader == rrraybudget::Resolve) {
            // Every resolve ages the Budget marks, RR on or off, before the Enabled check below
            // (controller-lifecycle: RayBudget "Marks age by resolves", RayOldMark, RayRestart).
            LiveRR().RayBudgetResolveSeen();
            static std::atomic<bool> seen{};
            if (!seen.exchange(true)) { // Addition 7: shows the resolve's build is recognised
                char line[96];
                std::snprintf(line, sizeof(line),
                              "RR ray budget: cs_vrrt_resolve seen (pipeline 0x%llx)",
                              static_cast<unsigned long long>(b.pipeline));
                WriteLog(line);
            }
        }
        if (!LiveRR().Enabled())
            return false;
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            (ID3D12GraphicsCommandList *)list->get_native());
        if (!native)
            return false;
        if (b.shader == rrraybudget::Clear) {
            LiveRR().RayBudgetClear(native.Get());
            return false;
        }
        rrvrrt::RayResolve in;
        in.why = rrraybudget::Why::Outside;
        if (LiveRR().CanRaw(native.Get()))
            in = ProveRayResolve(list, native.Get(), b, x, y, z);
        return LiveRR().RayBudgetResolve(native.Get(), in, x, y, z, dispatched);
    } catch (...) {
        try {
            LiveRR().RayBudgetException();
        } catch (...) {
        }
        return dispatched;
    }
}
// OnIndirect: counts an indirect command whose argument buffer is a published ray list.
static void ObserveRayListConsumer(reshade::api::command_list *,
                                   reshade::api::indirect_command type,
                                   reshade::api::resource buffer, uint64_t offset, uint32_t count,
                                   uint32_t stride) noexcept {
    if (!buffer.handle)
        return;
    for (unsigned lobe = 0; lobe < 2; ++lobe)
        if (rrraybudget::published[lobe].load(std::memory_order_relaxed) == buffer.handle) {
            try {
                // The buffer offset as given; the controller makes it relative to the list's view
                // and drops the call unless this handle is still the published one.
                LiveRR().RayBudgetConsumer({unsigned(type), lobe, offset, count, stride},
                                           buffer.handle);
            } catch (...) {
            }
            return;
        }
}
