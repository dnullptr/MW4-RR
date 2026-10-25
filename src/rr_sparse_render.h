#pragma once
// Included after observer, bindings and audit helpers.
// The recognized native cs_spatial_denoiser writes BOTH u0/u1.
// Replacing that dispatch inherits its UAV-state precondition, even when the
// app prepared the outputs on another command list. Binding alone is not used
// as proof: exact pipeline provenance, root/table, views and shapes are required.
// https://learn.microsoft.com/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states
static bool SparsePair(reshade::api::command_list *list, ID3D12GraphicsCommandList *native,
                       uint64_t effective, bool uav, unsigned first, rrsparsecopy::Pair &out,
                       bool nativeSpatialWrites = false) {
    auto *state = rrbindings::State(list->get_device());
    if (!state || !state->tracker.Healthy())
        return false;
    auto root = state->tracker.CurrentLayout(list->get_native());
    const auto binding = ShaderDispatches().Get(list->get_native());
    const bool contract =
        nativeSpatialWrites && uav && first == 0 && binding.shader == rrbypass::Spatial &&
        rrbypass::SpatialWriteContract(list->get_device(), binding.pipeline, effective);
    for (unsigned i = 0; i < 2; ++i) {
        rrbindings::Value v;
        auto loc = state->tracker.Find(list->get_native(), {uav, first + i});
        bool layoutKnown = false;
        D3D12_RESOURCE_STATES observed{};
        if (!loc.known ||
            !ComputeState().TableBound(native, (ID3D12PipelineState *)effective,
                                       (ID3D12RootSignature *)root, loc.parameter, loc.table))
            return false;
        auto slot = rrbindings::Resolve(list->get_device(), {loc.table}, loc.offset);
        if (!slot.heap)
            return false;
        v = state->tracker.Get(slot);
        if (!v.known || v.buffer || !v.resource ||
            v.view_type != unsigned(reshade::api::resource_view_type::texture_2d) ||
            v.first_level || v.levels != 1 || v.first_layer || v.layers > 1)
            return false;
        if (i == 0 ? (v.view_format != 2 && v.view_format != 3)
                   : (v.view_format != 41 && v.view_format != 42))
            return false;
        out.r[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
        if (!out.r[i])
            return false;
        const auto desc = out.r[i]->GetDesc();
        if (v.width != desc.Width || v.height != desc.Height || v.format != unsigned(desc.Format) ||
            v.view_format != unsigned(desc.Format))
            return false;
        layoutKnown = SparseLayouts().Get(native, out.r[i].Get(), observed);
        if (contract) {
            if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                return false;
            if (layoutKnown && observed != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                return false;
            out.state[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        } else {
            if (!layoutKnown)
                return false;
            out.state[i] = observed;
        }
    }
    return rrsparsecopy::Shape(out);
}
static bool SparseDispatch(reshade::api::command_list *list, uint32_t x, uint32_t y, uint32_t z) {
    const auto binding = ShaderDispatches().Get(list->get_native());
    if (binding.shader != rrbypass::Temporal && binding.shader != rrbypass::Spatial)
        return false;
    auto *device = list->get_device();
    if (device->get_api() != reshade::api::device_api::d3d12 || rrbypass::Health(device).failed)
        return false;
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
        (ID3D12GraphicsCommandList *)list->get_native());
    if (!native || !AlbedoInspector().SafeCopies(native.Get()) ||
        !AlbedoInspector().StateCoverage(native.Get())) {
        rrbypass::Fail(device,
                       "RR unsupported native copy context; correction disabled and RR blocked");
        return false;
    }
    const auto effective = rrbypass::Effective(device, binding.pipeline);
    rrsparsecopy::Pair input, output;
    if (binding.shader == rrbypass::Temporal) {
        if (!SparsePair(list, native.Get(), effective, false, 4, input) ||
            !SparsePair(list, native.Get(), effective, true, 0, output) || z != 1 ||
            x != (input.r[0]->GetDesc().Width + 7) / 8 ||
            y != (input.r[0]->GetDesc().Height + 7) / 8) {
            rrbypass::Fail(device,
                           "RR current/native-history bindings or layouts unproven; RR blocked");
            return false;
        }
#ifdef RR_FILTER_DIAGNOSTIC
        if (LiveRR().NativeFiltering()) {
            LiveRR().NativeTemporalObserved(native.Get(), output);
            return false; // Preserve native accumulation; no snapshot or replacement.
        }
#endif
        rrinternal::Scope ignoreCopies;
        rrcompute::Tracker::Internal ignoreState;
        if (!SparsePool().Snapshot(native.Get(), input, output))
            rrbypass::Fail(device, "RR current snapshot pool/shape validation failed; native "
                                   "temporal continues, RR blocked");
        return false;
    }
    if (!SparsePair(list, native.Get(), effective, false, 0, input)) {
        rrbypass::Fail(device, "RR spatial input proof failed; RR blocked");
        return false;
    }
#ifdef RR_FILTER_DIAGNOSTIC
    // The first spatial pass is still replaced by current-frame preparation.
    // In this diagnostic only the later passes run native, with exact linkage
    // to that prepared output. No temporal history resource is overwritten.
    if (LiveRR().SpatialOnlyFiltering() && LiveRR().RawWasCorrected(native.Get())) {
        bool valid = SparsePair(list, native.Get(), effective, true, 0, output, true);
        if (valid) {
            std::array<rrfilter::Surface,2> source{}, destination{};
            for (unsigned i = 0; i < 2; ++i) {
                source[i] = {UINT64(input.r[i].Get()), input.r[i]->GetDesc(), input.state[i]};
                destination[i] = {UINT64(output.r[i].Get()), output.r[i]->GetDesc(), output.state[i]};
            }
            valid = rrfilter::SpatialPassValid(source, destination, x, y, z, false);
        }
        LiveRR().ObserveSpatialOnly(native.Get(), input, output, valid);
        return false; // Execute native filtering; never bind raw passthrough here.
    }
    const bool native_filter = LiveRR().NativeFiltering();
    const bool native_first = native_filter && LiveRR().NativeSpatialCandidate(native.Get(), input);
    const std::array<UINT64,2> input_ids{UINT64(input.r[0].Get()), UINT64(input.r[1].Get())};
    const bool temporal_bypass = native_filter && LiveRR().NativeSpatialBypass(native.Get(), input_ids);
    if (native_filter ? !(native_first || temporal_bypass)
                      : !SparsePool().Pending(native.Get(), input)) {
        if (native_filter) LiveRR().RejectNativeSpatial(native.Get());
        return false;
    }
#else
    if (!SparsePool().Pending(native.Get(), input))
        return false;
#endif
    if (!SparsePair(list, native.Get(), effective, true, 0, output, true)) {
        rrbypass::Fail(device, "RR first spatial output proof failed; RR blocked");
        return false;
    }
#ifdef RR_FILTER_DIAGNOSTIC
    if (native_filter) {
        // The wrapper restored the native pipeline. Temporal + spatial leaves it
        // intact; temporal-only uses the existing packed-lighting passthrough.
        // Check EVERY bypassed spatial dispatch, including subsequent passes.
        std::array<rrfilter::Surface,2> source{}, destination{};
        for (unsigned i = 0; i < 2; ++i) {
            source[i] = {UINT64(input.r[i].Get()), input.r[i]->GetDesc(), input.state[i]};
            destination[i] = {UINT64(output.r[i].Get()), output.r[i]->GetDesc(), output.state[i]};
        }
        const bool temporal_only = LiveRR().NativeFilterStatus().effective == rrfilter::Mode::TemporalOnly;
        if (!rrfilter::SpatialPassValid(source, destination, x, y, z, temporal_only)) {
            rrbypass::Fail(device, "Diagnostic spatial format/state/extent/alias unproven; RR blocked");
            return false;
        }
        const auto health = rrbypass::Health(device);
        if (native_first)
            LiveRR().NativeSpatialObserved(native.Get(), input, UINT64(device), health.revision);
        if (LiveRR().NativeSpatialBypass(native.Get(), input_ids)) {
            auto selected = rrbypass::Bind(list, reshade::api::pipeline_stage::all_compute,
                                          {binding.pipeline});
            if (!selected.handle || selected.handle == binding.pipeline) {
                rrbypass::Fail(device, "Diagnostic spatial passthrough unavailable; RR blocked");
            } else if (!LiveRR().AdvanceNativeSpatial(native.Get(), input, output)) {
                // Binding alone is not a valid temporal-only comparison.
                native->SetPipelineState((ID3D12PipelineState *)binding.pipeline);
                rrbypass::Fail(device, "Diagnostic spatial chain changed; RR blocked");
            }
        }
        return false; // Dispatch the selected native or passthrough pipeline normally.
    }
#endif
    // One read per dispatch keeps the native-root and private paths on the same lobe source.
    const unsigned lobe = rrbypass::lobe_source.load(std::memory_order_relaxed);
    auto scale = rrbypass::Scale(device, binding.pipeline, lobe);
    rrinternal::Scope ignoreCopies;
    rrcompute::Tracker::Internal ignoreState;
    const auto rates = LiveRR().SamplingData(native.Get());
    const auto cbvs = ComputeState().RateCBVs(
        native.Get(), (ID3D12PipelineState *)effective,
        (ID3D12RootSignature *)rrbindings::State(device)->tracker.CurrentLayout(
            list->get_native()));
    rrcompute::Snapshot privateRestore;
    if (!cbvs.valid)
        privateRestore = ComputeState().Capture(native.Get(), (ID3D12PipelineState *)effective);
    const bool privateValid =
        privateRestore.valid && privateRestore.complete && privateRestore.heap_restore_valid &&
        UINT64(privateRestore.root.Get()) ==
            rrbindings::State(device)->tracker.CurrentLayout(list->get_native());
    if (!rates.buffer || (!cbvs.valid && !privateValid)) {
        char text[256];
        snprintf(text, sizeof(text),
                 "RR scale rejected: rates=%d rootCBVs=%d snapshot=%s complete=%d heapRestore=%d",
                 bool(rates.buffer), cbvs.valid, privateRestore.status, privateRestore.complete,
                 privateRestore.heap_restore_valid);
        rrbypass::Fail(device, text);
        return false;
    }
    const bool replaced = SparsePool().Apply(
        native.Get(), input, output, scale.Get(), (ID3D12PipelineState *)effective, x, y, z, &rates,
        cbvs.valid ? &cbvs : nullptr, cbvs.valid ? nullptr : &privateRestore, lobe);
    if (replaced) {
#ifdef RR_FILTER_DIAGNOSTIC
        LiveRR().SeedSpatialOnly(native.Get(), output);
#endif
        rrbypass::Corrected(device);
    }
    else
        rrbypass::Fail(
            device,
            "RR first spatial correction validation failed; native temporal continues, RR blocked");
    return replaced;
}
