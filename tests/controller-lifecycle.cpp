#define NOMINMAX
#define RR_FILTER_DIAGNOSTIC
#include "../src/rr_live.h"
#include <cassert>

// CPU-only COM double. No device, adapter, queue, or GPU is created.
struct CpuFence final : ID3D12Fence {
    ULONG refs = 1;
    UINT64 completed{};
    unsigned queries{}, waits{}, signals{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override {
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override {
        return E_NOTIMPL;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        ++queries;
        return completed;
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override {
        ++waits;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override {
        ++signals;
        return E_NOTIMPL;
    }
};
static unsigned releases{}, destroys{};
static NVSDK_NGX_Result NVSDK_CONV ReleaseFeature(NVSDK_NGX_Handle *) {
    ++releases;
    return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result NVSDK_CONV DestroyParameters(NVSDK_NGX_Parameter *) {
    ++destroys;
    return NVSDK_NGX_Result_Success;
}

namespace rrlive {
struct ControllerCpuTest {
    static void Run() {
        // Diagnostic route changes must retire existing work, then reset once.
        Controller diagnostic;
        diagnostic.SetEnabled(true);
        assert(diagnostic.NativeFilterStatus().effective == rrfilter::Mode::Raw);
        auto &ds = diagnostic.slots[0];
        ds.busy = true;
        ds.pre_live = true;
        diagnostic.RequestFilterMode(rrfilter::Mode::TemporalSpatial);
        assert(diagnostic.NativeFilterStatus().desired == rrfilter::Mode::TemporalSpatial);
        assert(diagnostic.NativeFilterStatus().pending);
        assert(!diagnostic.Reconfigure());
        assert(diagnostic.NativeFilterStatus().effective == rrfilter::Mode::Raw);
        ds.pre_live = false;
        assert(diagnostic.Reconfigure());
        assert(diagnostic.NativeFilterStatus().effective == rrfilter::Mode::TemporalSpatial);
        assert(!diagnostic.NativeFilterStatus().pending);
        assert(diagnostic.policy.Next(10, diagnostic.generation, false));
        assert(!diagnostic.policy.Next(11, diagnostic.generation, false));
        diagnostic.RequestFilterMode(rrfilter::Mode::TemporalSpatial);
        assert(!diagnostic.NeedsReconfigure()); // Repeated UI request is inert.
        diagnostic.RequestFilterMode(rrfilter::Mode::Raw);
        assert(diagnostic.Reconfigure());
        assert(diagnostic.NativeFilterStatus().effective == rrfilter::Mode::Raw);
        assert(diagnostic.policy.Next(12, diagnostic.generation, false));
        diagnostic.RestartFilterComparison();
        assert(diagnostic.Reconfigure());
        assert(diagnostic.policy.Next(13, diagnostic.generation, false));
        assert(!diagnostic.policy.Next(14, diagnostic.generation, false));

        // Temporal-only selection uses the same retirement/reset boundary.
        assert(!diagnostic.RequestFilterMode(static_cast<rrfilter::Mode>(99)));
        assert(!diagnostic.NeedsReconfigure());
        diagnostic.slots[0].pre_live = true;
        assert(diagnostic.RequestFilterMode(rrfilter::Mode::TemporalOnly));
        assert(!diagnostic.Reconfigure());
        assert(diagnostic.NativeFilterStatus().effective == rrfilter::Mode::Raw);
        diagnostic.slots[0].pre_live = false;
        assert(diagnostic.Reconfigure());
        assert(diagnostic.NativeFilterStatus().effective == rrfilter::Mode::TemporalOnly);
        assert(diagnostic.policy.Next(15, diagnostic.generation, false));
        assert(!diagnostic.policy.Next(16, diagnostic.generation, false));
        assert(diagnostic.RequestFilterMode(rrfilter::Mode::TemporalOnly));
        assert(!diagnostic.NeedsReconfigure());

        // Bypass is restricted to temporal-only and a proven current recording.
        Controller bypass;
        bypass.SetEnabled(true);
        auto &bs = bypass.slots[0];
        bs.generation = bypass.generation;
        bs.pre_live = bs.native_sampling = true;
        bypass.native_filter_effective = rrfilter::Mode::TemporalOnly;
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        assert(bs.native_filter_chain.Temporal({11,22}));
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        assert(bs.native_filter_chain.Spatial({11,22}));
        bs.native_filtered = true;
        assert(bypass.NativeSpatialBypass(nullptr, {11,22}));
        bypass.native_filter_effective = rrfilter::Mode::TemporalSpatial;
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        bypass.native_filter_effective = rrfilter::Mode::Raw;
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        bypass.native_filter_effective = rrfilter::Mode::TemporalOnly;
        bs.poisoned = true;
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        bs.poisoned = false;
        bs.pending_sr = true;
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        bs.pending_sr = false;
        bs.generation++;
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));
        bs.generation--;
        assert(!bs.native_filter_chain.Temporal({11,22}));
        assert(!bypass.NativeSpatialBypass(nullptr, {11,22}));

        // Native filtering is never accepted through the raw-light callback.
        Controller admission;
        admission.lighting_valid = [](UINT64, UINT64) { return true; };
        admission.native_lighting_valid = [](UINT64, UINT64) { return true; };
        auto &as = admission.slots[0];
        as.raw_corrected = true;
        assert(admission.LightingValid(as));
        admission.native_filter_effective = rrfilter::Mode::TemporalSpatial;
        assert(!admission.LightingValid(as));
        as.raw_corrected = false;
        as.native_filtered = true;
        assert(as.native_filter_chain.Temporal({11, 22}));
        assert(as.native_filter_chain.Spatial({11, 22}));
        assert(admission.LightingValid(as));
        admission.native_lighting_valid = [](UINT64, UINT64) { return false; };
        assert(!admission.LightingValid(as));
        admission.native_filter_effective = rrfilter::Mode::Raw;
        assert(!admission.LightingValid(as));

        admission.native_filter_effective = rrfilter::Mode::TemporalOnly;
        admission.native_lighting_valid = [](UINT64, UINT64) { return true; };
        assert(!admission.LightingValid(as)); // Seeing the consumer alone is insufficient.
        assert(as.native_filter_chain.Forward({11,22}, {33,44}));
        assert(admission.LightingValid(as));
        admission.SetEnabled(true);
        as.generation = admission.generation;
        as.pre_live = as.native_sampling = true;
        assert(admission.NativeSpatialBypass(nullptr, {33,44}));
        assert(!admission.NativeSpatialBypass(nullptr, {99,100}));
        admission.RejectNativeSpatial(nullptr);
        assert(!admission.LightingValid(as));
        assert(!admission.NativeSpatialBypass(nullptr, {33,44}));
        as.pre_live = false;
        admission.RestartFilterComparison();
        assert(admission.Reconfigure());
        assert(!admission.slots[0].native_filter_chain.Accepted());
        assert(!admission.slots[0].native_filtered);

        rrfilter::Chain chain;
        assert(!chain.Spatial({11, 22})); // Spatial without temporal is rejected.
        rrfilter::Chain null_chain, alias_chain;
        assert(!null_chain.Temporal({0, 22}));
        assert(!null_chain.Temporal({11, 22})); // Failed chain stays rejected.
        assert(!alias_chain.Temporal({11, 11}));
        assert(chain.Temporal({11, 22}));
        assert(!chain.Spatial({11, 23}));
        assert(chain.Spatial({11, 22}));
        assert(!chain.Spatial({11, 22})); // Only the first spatial use is admitted.
        assert(!chain.Temporal({11, 22})); // Duplicate temporal invalidates proof.
        assert(!chain.Accepted());
        rrfilter::Chain passes;
        assert(passes.Temporal({11,22}) && passes.Spatial({11,22}));
        assert(!passes.Bypassed());
        assert(passes.Next({11,22}));
        assert(passes.Forward({11,22}, {33,44}));
        assert(passes.Bypassed());
        assert(!passes.Next({11,22}));
        assert(!passes.Next({55,66}));
        assert(passes.Forward({33,44}, {55,66}));
        assert(passes.Next({55,66}));
        assert(!passes.Forward({33,44}, {77,88})); // Reordering invalidates the slot.
        assert(!passes.Accepted() && !passes.Bypassed());
        rrfilter::Chain alias_pass;
        assert(alias_pass.Temporal({11,22}) && alias_pass.Spatial({11,22}));
        assert(!alias_pass.Forward({11,22}, {33,11}));
        assert(!alias_pass.Accepted());
        rrfilter::Chain other_recording;
        assert(!other_recording.Next({55,66}));

        using Surface = rrfilter::Surface;
        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = 1280; color.Height = 720;
        color.MipLevels = color.DepthOrArraySize = color.SampleDesc.Count = 1;
        color.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        auto weights = color; weights.Format = DXGI_FORMAT_R32_UINT;
        std::array<Surface,2> input{{{11,color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                                    {22,weights,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}}};
        std::array<Surface,2> output{{{33,color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
                                     {44,weights,D3D12_RESOURCE_STATE_UNORDERED_ACCESS}}};
        assert(rrfilter::SpatialPassValid(input, output, 80, 45, 1, true));
        assert(!rrfilter::SpatialPassValid(input, output, 79, 45, 1, true));
        assert(!rrfilter::SpatialPassValid(input, output, 80, 45, 2, true));
        auto bad = output; bad[1].identity = 11;
        assert(!rrfilter::SpatialPassValid(input, bad, 80, 45, 1, true));
        bad = output; bad[0].desc.Width = 1279;
        assert(!rrfilter::SpatialPassValid(input, bad, 80, 45, 1, true));
        bad = output; bad[1].state = D3D12_RESOURCE_STATE_COMMON;
        assert(!rrfilter::SpatialPassValid(input, bad, 80, 45, 1, true));
        auto bad_input = input; bad_input[0].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        assert(!rrfilter::SpatialPassValid(bad_input, output, 80, 45, 1, true));
        bad_input = input; bad_input[0].desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        bad = output; bad[0].desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        assert(!rrfilter::SpatialPassValid(bad_input, bad, 80, 45, 1, true));
        assert(rrfilter::SpatialPassValid(bad_input, bad, 80, 45, 1, false));
        bad_input[1].desc.Format = bad[1].desc.Format = DXGI_FORMAT_R32_FLOAT;
        assert(!rrfilter::SpatialPassValid(bad_input, bad, 80, 45, 1, true));
        assert(rrfilter::SpatialPassValid(bad_input, bad, 80, 45, 1, false));

        // Spatial-only keeps raw preparation, but requires an actual later native filter.
        assert(rrfilter::Valid(rrfilter::Mode::SpatialRemaining));
        assert(!rrfilter::Native(rrfilter::Mode::SpatialRemaining));
        rrfilter::SpatialChain spatial;
        assert(!spatial.Accepted());
        assert(spatial.Seed({11,22}));
        assert(!spatial.Accepted()); // A seed without a later filter is not enough.
        assert(spatial.Observe({11,22}, {33,44}));
        assert(spatial.Accepted());
        assert(spatial.Observe({33,44}, {11,22})); // Native ping-pong is valid.
        assert(!spatial.Observe({99,100}, {55,66}));
        assert(!spatial.Accepted());
        rrfilter::SpatialChain unseeded, duplicated, alias_spatial;
        assert(!unseeded.Observe({11,22}, {33,44}));
        assert(duplicated.Seed({11,22}));
        assert(!duplicated.Seed({33,44}));
        assert(!duplicated.Observe({11,22}, {33,44}));
        assert(alias_spatial.Seed({11,22}));
        assert(!alias_spatial.Observe({11,22}, {22,33}));
        assert(!alias_spatial.Accepted());

        Controller spatial_mode;
        spatial_mode.SetEnabled(true);
        auto &ss = spatial_mode.slots[0];
        ss.generation = spatial_mode.generation;
        ss.pre_live = ss.native_sampling = ss.raw_corrected = true;
        spatial_mode.lighting_valid = [](UINT64,UINT64) { return true; };
        assert(spatial_mode.RawSpatialPassthrough(nullptr));
        assert(spatial_mode.RequestFilterMode(rrfilter::Mode::SpatialRemaining));
        assert(!spatial_mode.Reconfigure());
        assert(spatial_mode.NativeFilterStatus().effective == rrfilter::Mode::Raw);
        ss.pre_live = false;
        assert(spatial_mode.Reconfigure());
        auto &fresh = spatial_mode.slots[0];
        fresh.generation = spatial_mode.generation;
        fresh.pre_live = fresh.native_sampling = fresh.raw_corrected = true;
        assert(!spatial_mode.RawSpatialPassthrough(nullptr));
        assert(!spatial_mode.LightingValid(fresh));
        assert(fresh.spatial_only_chain.Seed({11,22}));
        assert(!spatial_mode.LightingValid(fresh));
        assert(fresh.spatial_only_chain.Observe({11,22}, {33,44}));
        assert(spatial_mode.LightingValid(fresh));
        fresh.raw_corrected = false;
        assert(!spatial_mode.LightingValid(fresh));
        fresh.raw_corrected = true;
        fresh.spatial_only_chain.Invalidate();
        assert(!spatial_mode.LightingValid(fresh));
        fresh.pre_live = false;
        spatial_mode.RestartFilterComparison();
        assert(spatial_mode.Reconfigure());
        assert(!spatial_mode.slots[0].spatial_only_chain.Accepted());

        // A rejected allocation after preset retirement must wait, then recover.
        // Ready guide slots must remain usable while another slot waits.
        bool deferred = false;
        try { rrprivate::CheckAllocation(E_OUTOFMEMORY, "allocation"); }
        catch (const rrprivate::AllocationDeferred &) { deferred = true; }
        assert(deferred);
        bool removed = false;
        try { rrprivate::CheckAllocation(DXGI_ERROR_DEVICE_REMOVED, "removed"); }
        catch (const rrprivate::AllocationDeferred &) { assert(false); }
        catch (const std::runtime_error &) { removed = true; }
        assert(removed);
        rrprivate::CheckAllocation(S_OK, "successful allocation");
        Controller retry;
        retry.preset.Created();
        assert(retry.RequestPreset(4) && retry.Reconfigure());
        retry.preset.Created();
        assert(retry.PresetStatus().effective == 4);
        unsigned attempts = 0;
        auto reject = [&] { ++attempts; throw rrprivate::AllocationDeferred(); };
        bool escaped = false;
        try {
            assert(!retry.PrepareSssAllocation(1000, false, reject));
        } catch (const rrprivate::AllocationDeferred &) { escaped = true; }
        assert(!escaped && "temporary allocation rejection must not reach the fatal SSS catch");
        assert(!retry.sss_failed && attempts == 1);
        assert(!retry.PrepareSssAllocation(1100, false, reject) && attempts == 1);
        assert(retry.PrepareSssAllocation(1100, true, [&] { ++attempts; }));
        assert(attempts == 2);
        assert(!retry.PrepareSssAllocation(1999, false, reject) && attempts == 2);
        assert(retry.PrepareSssAllocation(2000, false, [&] { ++attempts; }));
        assert(attempts == 3 && !retry.sss_failed);
        bool fatal = false;
        try {
            retry.PrepareSssAllocation(2100, false, [] {
                throw std::runtime_error("invalid resource layout");
            });
        } catch (const std::runtime_error &) { fatal = true; }
        assert(fatal); // Genuine failures retain the existing containment path.
        Controller startup;
        assert(startup.RequestPreset(4) && !startup.NeedsReconfigure());
        assert(!startup.PresetStatus().has_effective && !startup.PresetStatus().pending);
        startup.preset.Retired();
        assert(startup.PresetStatus().pending &&
               !startup.NeedsReconfigure()); // Await creation without blocking producers.
        assert(startup.RequestPreset(0) && startup.Reconfigure());
        Controller c;
        c.SetEnabled(true);
        c.SetSssEnabled(true);
        c.SetTransparencyEnabled(true);
        assert(c.Enabled() && c.SssEnabled() && c.TransparencyEnabled());
        c.preset.Created();
        c.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        c.parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(2);
        c.api.release = ReleaseFeature;
        c.api.destroy = DestroyParameters;
        auto *f = new CpuFence;
        c.fence.Attach(f);
        c.signaled = 9;
        auto &s = c.slots[0];
        s.busy = true;
        s.fence_value = 9;
        assert(!c.RequestPreset(1) && c.PresetStatus().desired == 6);
        assert(c.RequestPreset(4) && c.PresetStatus().pending);
        assert(c.RequestPreset(5) && c.PresetStatus().desired == 5 &&
               c.PresetStatus().effective == 6);
        const auto before_queries = f->queries;
        c.SetEnabled(false);
        c.SetEnabled(true);
        c.SetSssEnabled(false);
        c.SetTransparencyEnabled(false);
        c.Options();
        c.Status();
        c.PresetStatus();
        // The transparency albedo blend row with no driver or device: RR is on and
        // nothing ran yet. Toggling it restarts history.
        assert(std::string(rrstatus::Label(c.TransparencyBlendState(true, false))) ==
               "ON / waiting");
        assert(std::string(rrstatus::Label(c.TransparencyBlendState(false, false))) ==
               "unavailable");
        c.policy.Next(20, c.generation, false);
        assert(!c.policy.Next(21, c.generation, false));
        c.SetTransparencyBlendEnabled(false);
        assert(!c.TransparencyBlendEnabled() &&
               std::string(rrstatus::Label(c.TransparencyBlendState(true, false))) == "OFF");
        assert(c.policy.Next(22, c.generation, false));
        c.SetTransparencyBlendEnabled(true);
        c.SetTransparencyBlendTuning({});
        assert(c.TransparencyBlendEnabled() && c.policy.Next(23, c.generation, false));
        assert(f->queries == before_queries && releases == 0 &&
               destroys == 0); // UI never calls driver.
        bool *held[] = {&s.pre_live,
                        &s.sr_live,
                        &s.sss.live,
                        &s.transparency.prep_live,
                        &s.transparency.anchor_live,
                        &s.coverage.live,
                        &s.pending_sr,
                        &s.poisoned};
        f->completed = 9;
        for (auto flag : held) {
            *flag = true;
            assert(!c.Reconfigure() && releases == 0);
            *flag = false;
        }
        f->completed = 8;
        assert(!c.Reconfigure() && releases == 0);
        f->completed = UINT64_MAX;
        assert(!c.Reconfigure() && releases == 0); // Device removal retains allocations.
        f->completed = 9;
        f->AddRef();
        assert(c.Reconfigure() && releases == 1 && destroys == 1);
        assert(!c.feature && !c.parameters && !c.PresetStatus().has_effective &&
               c.PresetStatus().pending);
        assert(f->waits == 0 && f->signals == 0);
        f->Release();
        assert(c.Reconfigure() &&
               releases == 1); // No duplicate release while awaiting next creation.
        c.preset.Created();
        assert(c.PresetStatus().effective == 5 && !c.PresetStatus().pending);
        // New observed dimensions/mode queue the same safe retirement path.
        c.key.width = 1280;
        c.key.height = 720;
        c.key.out_width = 2560;
        c.key.out_height = 1440;
        c.key.flags = 0;
        c.key.quality = 1;
        auto actual = c.key;
        actual.width = 1720;
        actual.height = 720;
        actual.out_width = 3440;
        assert(!rreval::SameCreation(c.key, actual));
        actual = c.key;
        actual.quality = 2;
        assert(!rreval::SameCreation(c.key, actual));
        actual = c.key;
        actual.jitter_x = 0.25f;
        assert(rreval::SameCreation(c.key, actual));
        c.QueueReconfigure();
        assert(c.PresetStatus().pending && c.Reconfigure());
        assert(!c.initialized && c.selected_view == UINT_MAX && !c.layout_pending);
    }
};
} // namespace rrlive
int main() {
    rrlive::ControllerCpuTest::Run();
}
