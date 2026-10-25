#define NOMINMAX
#define RR_FILTER_DIAGNOSTIC
#include "../src/rr_live.h"
#include <algorithm>
#include <cassert>
#include <string>
#include <thread>

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
// CPU-only resource double: its reference count is observed; Map hands out mapped, when set,
// and records the range read.
struct CpuResource final : ID3D12Resource {
    ULONG refs = 1;
    void *mapped{};
    D3D12_RANGE read{};
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
    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *range, void **data) override {
        if (!mapped)
            return E_NOTIMPL;
        read = range ? *range : D3D12_RANGE{};
        *data = mapped;
        return S_OK;
    }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) override {}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        return {};
    }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        return 0;
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT,
                                                 UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT,
                                                  const D3D12_BOX *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *,
                                                D3D12_HEAP_FLAGS *) override {
        return E_NOTIMPL;
    }
};
static unsigned releases{}, destroys{}, log_lines{};
static void CountLog(const char *) {
    ++log_lines;
}
static std::string last_log;
static void KeepLog(const char *s) {
    last_log = s;
}
static NVSDK_NGX_Result NVSDK_CONV ReleaseFeature(NVSDK_NGX_Handle *) {
    ++releases;
    return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result NVSDK_CONV DestroyParameters(NVSDK_NGX_Parameter *) {
    ++destroys;
    return NVSDK_NGX_Result_Success;
}
static std::string logged;
static void Logged(const char *s) {
    logged += s;
    logged += '\n';
}
static NVSDK_NGX_Result NVSDK_CONV FeatureNotFound(NVSDK_NGX_Handle *) {
    return NVSDK_NGX_Result(0xBAD00004u);
}
static NVSDK_NGX_Result NVSDK_CONV PlatformError(NVSDK_NGX_Parameter *) {
    return NVSDK_NGX_Result(0xBAD00002u);
}

namespace rrlive {
struct ControllerCpuTest {
    static void DevTests() {
        // Off and blocked development tests never touch the device; a blocked
        // test is reported until its setting changes.
        Controller c;
        Controller::DevTest t;
        assert(!c.UseTest(t, false, 0, 8, 8, DXGI_FORMAT_R16_FLOAT, "test") && !t.active);
        t.blocked = true;
        t.changes = 3;
        assert(!c.UseTest(t, true, 3, 8, 8, DXGI_FORMAT_R16_FLOAT, "test") && t.blocked &&
               !t.active);
        assert(c.Test(t, 3, true) == Controller::TestState::Blocked);
        assert(c.Test(t, 3, false) == Controller::TestState::Idle); // setting turned off
        assert(c.Test(t, 4, true) == Controller::TestState::Idle);
        t.blocked = false;
        t.active = true;
        assert(c.Test(t, 4, true) == Controller::TestState::Idle); // RR not running
        // The contrast and brightness tests share the color copy: a blocked copy is
        // reported for the contrast test too, until either setting changes.
        const bool was_enabled = c.enabled;
        c.enabled = true;
        c.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        c.color_test.blocked = true;
        c.color_test.changes = Controller::ColorChanges();
        rrcontrast::mode = rrcontrast::Medium;
        assert(c.ContrastTest() == Controller::TestState::Blocked);
        ++rrcolorscale::changes;
        assert(c.ContrastTest() == Controller::TestState::Idle);
        c.color_test.changes = Controller::ColorChanges();
        assert(c.ContrastTest() == Controller::TestState::Blocked);
        ++rrcontrast::changes;
        assert(c.ContrastTest() == Controller::TestState::Idle);
        c.color_test.blocked = false;
        c.color_test.active = true;
        assert(c.ContrastTest() == Controller::TestState::Active);
        rrcontrast::mode = rrcontrast::Off;
        assert(c.ContrastTest() == Controller::TestState::Idle);
        c.color_test.active = false;
        c.feature = nullptr;
        c.enabled = was_enabled;
        // Reconfigure gives back only the reserve the test textures added.
        c.budget.limit = Budget::Minimum + 300;
        c.test_reserve = 200;
        c.color_test.blocked = c.sky_edge_test.active = c.wide_output.blocked = true;
        c.hit_scratch.active = true;
        c.applied_color_scale = 4.0f;
        c.applied_contrast_knee = 0.25f;
        c.QueueReconfigure();
        assert(c.Reconfigure());
        assert(c.budget.limit == Budget::Minimum + 100 && !c.test_reserve);
        assert(!c.color_test.blocked && !c.sky_edge_test.active && !c.wide_output.blocked &&
               !c.hit_scratch.active && c.applied_color_scale == 1.0f &&
               c.applied_contrast_knee == 0.0f);
        c.budget.limit = Budget::Minimum + 50;
        c.test_reserve = 200;
        c.QueueReconfigure();
        assert(c.Reconfigure() && c.budget.limit == Budget::Minimum);
    }
    // A failed retirement keeps its allocations and logs the NGX result, or the missing export.
    static void RetirementFailure() {
        Controller f;
        f.logger = &Logged;
        f.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        f.parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(2);
        f.api.release = FeatureNotFound;
        f.QueueReconfigure();
        assert(!f.Reconfigure() && f.failed && f.feature && f.parameters);
        assert(logged == "RR feature retirement failed (0xBAD00004 FAIL_FeatureNotFound); "
                         "allocations retained\n");
        logged.clear();
        Controller p;
        p.logger = &Logged;
        p.parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(2);
        p.api.destroy = PlatformError;
        p.QueueReconfigure();
        assert(!p.Reconfigure() && p.failed && p.parameters);
        assert(logged == "RR parameter retirement failed (0xBAD00002 FAIL_PlatformError); "
                         "allocations retained\n");
        logged.clear();
        Controller m;
        m.logger = &Logged;
        m.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        m.QueueReconfigure();
        assert(!m.Reconfigure() && m.failed && m.feature);
        assert(logged == "RR feature retirement failed (export missing); allocations retained\n");
        logged.clear();
    }
    // Each stats reader maps only its own bytes, one after another: the gate (0..15), the
    // hit distance statistics (20..63), the ray density (64..227), the non-finite counts
    // (228, 232) and the water counts (16, then the prepass flags at 236..255).
    static void StatsReaders() {
        UINT words[64]; // the whole 256-byte readback
        for (UINT i = 0; i < 64; ++i)
            words[i] = 100 + i; // word i holds 100 + its byte offset / 4
        GpuFrame g;
        auto *readback = new CpuResource;
        readback->mapped = words;
        g.readback.Attach(readback);
        assert((g.ReadStats() == std::array<UINT, 4>{100, 101, 102, 103}));
        assert(readback->read.Begin == 0 && readback->read.End == 16);
        const auto hit = g.ReadHitStats();
        assert(hit.front() == 105 && hit.back() == 115);
        assert(readback->read.Begin == 20 && readback->read.End == 64);
        const auto rates = g.ReadRates();
        assert(rates.front() == 116 && rates.back() == 156);
        assert(readback->read.Begin == 64 && readback->read.End == 228);
        assert((g.ReadFinite() == std::array<UINT, 2>{157, 158}));
        assert(readback->read.Begin == 228 && readback->read.End == 236);
        const auto water = g.ReadWaterCounts();
        assert(water.water == 104);
        assert((water.prepass == std::array<std::uint32_t, rrwater::PrepassCount>{159, 160, 161,
                                                                                  162, 163}));
        assert(readback->read.Begin == 236 && readback->read.End == 256);
    }
    // A creation attempt's runtime is logged once, by the next Poll, with the controller mutex
    // released: its module and DXGI queries never run under it.
    static inline Controller *polled{};
    static inline bool unlocked{};
    static void LogUnlocked(const char *s) {
        Logged(s);
        // From another thread: the mutex is recursive, so this one would always get it.
        std::thread([] {
            if (polled->mutex.try_lock()) {
                polled->mutex.unlock();
                unlocked = true;
            }
        }).join();
    }
    static void RuntimeLog() {
        Controller c;
        polled = &c;
        c.logger = &LogUnlocked;
        c.runtime_report = {GetModuleHandleW(L"kernel32.dll"), nullptr};
        c.Poll();
        assert(logged.rfind("RR runtime: NGX core ", 0) == 0 && unlocked);
        assert(std::count(logged.begin(), logged.end(), '\n') == 1);
        c.Poll(); // Drained: the next Poll logs nothing.
        assert(std::count(logged.begin(), logged.end(), '\n') == 1);
        logged.clear();
        polled = nullptr;
    }
    static void WeaponDepth() {
        // Weapon depth restarts RR only when its setting differs from the depth type RR was
        // created with. Linear depth that could not run stays off through Reconfigure until
        // the setting next changes, so it restarts RR once, never over and over.
        using State = Controller::WeaponState;
        auto *const created = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        Controller c;
        rrweapondepth::enabled = true;
        ++rrweapondepth::changes;
        assert(c.WeaponDepthWanted() && !c.NeedsReconfigure()); // The next RR takes it.
        assert(c.WeaponDepthState() == State::Waiting);
        c.enabled = true;
        c.feature = created; // Created with hardware depth.
        assert(c.NeedsReconfigure() && c.WeaponDepthState() == State::Restarting);
        c.weapon_linear = true;
        assert(!c.NeedsReconfigure() && c.WeaponDepthState() == State::Waiting);
        c.weapon_test.active = true;
        assert(c.WeaponDepthState() == State::Active);
        // The refusal names NGX's outcome.
        c.logger = Logged;
        const rrngxlog::Code refused(0xBAD00005u);
        c.HoldWeaponDepth(Controller::WeaponHold::Refused, rrweapondepth::changes, refused.text);
        assert(!c.WeaponDepthWanted() && c.NeedsReconfigure());
        assert(c.WeaponDepthState() == State::Refused);
        const std::string refusal = "Weapon depth: RR did not accept linear depth (0xBAD00005 "
                                    "FAIL_InvalidParameter); off until the setting changes, RR "
                                    "runs with hardware depth\n";
        assert(logged == refusal);
        c.HoldWeaponDepth(Controller::WeaponHold::Refused, rrweapondepth::changes, "threw");
        assert(logged == refusal); // Frames still in flight repeat the hold; logged once.
        logged.clear();
        c.logger = nullptr;
        c.feature = nullptr; // Released by Reconfigure in the game; no NGX here.
        c.QueueReconfigure();
        assert(c.Reconfigure());
        assert(!c.weapon_linear && !c.weapon_test.active && !c.WeaponDepthWanted());
        c.feature = created; // Recreated with hardware depth: no second restart.
        assert(!c.NeedsReconfigure() && c.WeaponDepthState() == State::Refused);
        ++rrweapondepth::changes; // Turned off and on again: tried again.
        assert(c.WeaponDepthWanted() && c.NeedsReconfigure());
        c.HoldWeaponDepth(Controller::WeaponHold::Memory, rrweapondepth::changes);
        assert(c.WeaponDepthState() == State::NoMemory && !c.NeedsReconfigure());
        c.HoldWeaponDepth(Controller::WeaponHold::Projection, rrweapondepth::changes);
        assert(c.WeaponDepthState() == State::Unsupported);
        rrweapondepth::enabled = false;
        ++rrweapondepth::changes;
        assert(!c.NeedsReconfigure() && c.WeaponDepthState() == State::Off);
        c.weapon_linear = true; // Turned off while RR has linear depth: back to hardware depth.
        assert(c.NeedsReconfigure() && c.WeaponDepthState() == State::Restarting);
        c.weapon_linear = false;
        c.feature = nullptr;
        // While weapon depth runs, its linear depth carries the sky guide test's floor.
        c.feature = created;
        c.weapon_linear = c.weapon_test.active = true;
        rrguidetest::mode = rrguidetest::SkySurface;
        assert(c.DepthFloorTest() == Controller::TestState::Active);
        c.weapon_linear = c.weapon_test.active = false;
        assert(c.DepthFloorTest() == Controller::TestState::Idle);
        rrguidetest::mode = 0;
        c.feature = nullptr;
    }
    static void HitDistanceTests() {
        // The hit distance tests record nothing while off and act once on the
        // single guide recording of a list.
        using rrhitcond::Outcome;
        Controller c;
        rrcompute::Snapshot restore;
        auto state_of = [](ID3D12Resource *, D3D12_RESOURCE_STATES &) { return false; };
        auto *weights = new CpuResource;
        // Never called: slots only compare it with their recording.
        auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(weights);
        auto run = [&](ID3D12GraphicsCommandList *cl) {
            return c.ConditionSpecularDistance(
                cl, weights, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, restore, state_of);
        };
        auto pending = [&](Outcome expected) {
            Outcome why{};
            return !c.HitDistancePending(list, why) && why == expected;
        };
        assert(run(list) == Outcome::Off && pending(Outcome::Off)); // no test on
        rrhitcond::fill = true;
        assert(run(list) == Outcome::Off); // RR off
        c.SetEnabled(true);
        assert(run(list) == Outcome::Off); // hit distance off
        c.SetSpecularHitEnabled(true);
        assert(run(nullptr) == Outcome::Off);
        assert(run(list) == Outcome::NoSlot && pending(Outcome::NoSlot));
        auto &s = c.slots[0], &other = c.slots[1];
        s.generation = other.generation = c.generation;
        s.pre_live = true;
        s.pre.Attach(list);
        Outcome why{};
        assert(c.HitDistancePending(list, why));
        assert(run(list) == Outcome::NoCopy);
        // A guide test that replaces normals leaves the fill out.
        s.gpu.guide_test = rrguidetest::FaceCamera;
        assert(run(list) == Outcome::Off);
        rrhitcond::sky_miss = true;
        assert(run(list) == Outcome::NoCopy);
        rrhitcond::sky_miss = false;
        s.gpu.guide_test = rrguidetest::FlipBackFaces;
        assert(run(list) == Outcome::NoCopy);
        s.gpu.hit_ready = true;
        s.gpu.specular_hit = weights;
        assert(run(list) == Outcome::Weights); // not a 2D texture
        other.pre_live = true;
        other.pre.Attach(list);
        assert(run(list) == Outcome::NoSlot && pending(Outcome::NoSlot)); // two on one list
        other.pre.Detach();
        other.pre_live = false;
        s.hit_conditioned = true;
        assert(run(list) == Outcome::Repeat && pending(Outcome::Repeat));
        c.hit_condition_failed = true;
        assert(run(list) == Outcome::Off);
        s.pre.Detach();
        rrhitcond::fill = false;
        weights->Release();
    }
    static void CaptureLighting() {
        // A first spatial pass notes the lighting it prepared on its frame's one guide
        // recording, only while a capture is Armed or Collecting (capture_active).
        Controller c;
        c.SetEnabled(true);
        auto &s = c.slots[0], &other = c.slots[1];
        s.generation = other.generation = c.generation;
        s.pre_live = s.native_sampling = true;
        assert(!c.capture_active);
        c.NoteLighting(nullptr, 9, 1); // No capture: nothing noted.
        assert(!s.lighting.noted);
        c.RequestCapture();
        assert(c.capture.state == rrcapture::State::Armed && c.capture_active);
        c.NoteLighting(nullptr, 9, 1);
        assert(s.lighting.noted && s.lighting.input == 9 && s.lighting.masked == 1 &&
               !s.lighting.mixed);
        c.NoteLighting(nullptr, 9, 0); // A later pass of the frame without the SSS guide.
        assert(s.lighting.mixed);
        s.lighting = {};
        other.pre_live = other.native_sampling = true; // Two match: neither is noted.
        c.NoteLighting(nullptr, 4, -1);
        assert(!s.lighting.noted && !other.lighting.noted);
        other.native_sampling = false; // The dispatch's sampling rates are not its.
        c.NoteLighting(nullptr, 4, -1);
        assert(s.lighting.noted && s.lighting.input == 4 && !other.lighting.noted);
        s.pending_sr = true; // Its SR call has begun.
        c.NoteLighting(nullptr, 5, -1);
        assert(s.lighting.input == 4 && !s.lighting.mixed);
        s.pending_sr = false;
        c.capture_active = false; // Never locks or notes while the flag is off.
        c.NoteLighting(nullptr, 5, -1);
        assert(!s.lighting.mixed);
        c.capture_active = true;
        c.SetEnabled(false); // Armed waits for RR.
        assert(c.capture.state == rrcapture::State::Armed && c.capture_active);
        c.CaptureFailed("test");
        assert(c.capture.state == rrcapture::State::Failed && !c.capture_active);
        c.SetEnabled(true);
        s.generation = c.generation;
        c.NoteLighting(nullptr, 5, -1);
        assert(!s.lighting.mixed);
        c.capture_active = true; // Stale: the capture's state under the lock decides.
        c.NoteLighting(nullptr, 5, -1);
        assert(!s.lighting.mixed);
        c.capture_active = false;
        c.RequestCapture();
        assert(c.capture_active);
        c.retired = true; // A request while RR is retired fails at once.
        c.CaptureFailed("test");
        c.RequestCapture();
        assert(c.capture.state == rrcapture::State::Failed && !c.capture_active);
        c.retired = false;
        c.RequestCapture();
        c.Failure("test"); // RR failing ends the capture.
        assert(c.capture.state == rrcapture::State::Failed && !c.capture_active);
        // A slot's notes go with it (Reconfigure waits for no live recording).
        s.pre_live = other.pre_live = false;
        c.QueueReconfigure();
        assert(c.Reconfigure() && !c.slots[0].lighting.noted);
    }
    static void ReplaceSkip() {
        // Each refusal in TryReplaceSR names itself for this thread's evaluate
        // (rrtrace::Probe::CurrentSkip), with or without a capture.
        Controller c;
        rrsparsecopy::Layouts layouts;
        rrtrace::SlCall scope{};
        const auto why = [&](bool safe) {
            rrtrace::Probe::NoteSkip(rrsr::SkipCount);
            assert(!c.TryReplaceSR(nullptr, nullptr, scope, nullptr, 0, safe, layouts));
            return rrtrace::Probe::CurrentSkip();
        };
        assert(why(true) == rrsr::Off);
        c.SetEnabled(true);
        c.failed = true;
        assert(why(true) == rrsr::Failed);
        c.failed = false;
        c.retired = true;
        assert(why(true) == rrsr::Failed);
        c.retired = false;
        c.layout_pending = true;
        assert(why(true) == rrsr::Reconfigure);
        c.layout_pending = false;
        assert(why(false) == rrsr::Unsafe);
        assert(why(true) == rrsr::Scope); // No SL call.
        scope = {true, sl::kFeatureDLSS, 5, 0};
        assert(why(true) == rrsr::Scope); // No frame of that call.
        auto &s = c.slots[0];
        s.pre_live = true;
        s.frame = 5;
        s.viewport = 0;
        assert(why(true) == rrsr::Scope); // Its SR call has not begun.
        s.pending_sr = true;
        assert(why(true) == rrsr::List); // Not on the SR call's list.
        // The route log says why from the slot: a reason, or the failed proof.
        c.logger = KeepLog;
        const rrcapture::Sources sources{};
        const std::array<unsigned, rrcapture::InputCount> from{};
        s.skip = rrsr::ResetFixed;
        c.LogCaptureRoute(s, sources, from, false);
        assert(last_log == "RR capture: native SR; input states: color none 0x40, depth none "
                           "0x40, motion none 0x40; replacement not tried (reset parameter not "
                           "writable)");
        c.LogCaptureRoute(s, sources, from, true);
        assert(last_log == "RR capture: RR replaces SR; input states: color none 0x40, depth none "
                           "0x40, motion none 0x40");
        s.proof_failed = true;
        s.skip = rrsr::Ineligible;
        c.LogCaptureRoute(s, sources, from, false);
        assert(last_log.ends_with("; replacement not eligible: color missing, depth missing, "
                                  "motion missing, output missing"));
        s = {}; // Reset with the slot.
        assert(s.skip == rrsr::NotCalled && !s.proof_failed);
        c.LogCaptureRoute(s, sources, from, false);
        assert(last_log.ends_with("; replacement not tried (not called)"));
        c.logger = nullptr;
    }
    static void Run() {
        DevTests();
        RetirementFailure();
        StatsReaders();
        RuntimeLog();
        WeaponDepth();
        HitDistanceTests();
        CaptureLighting();
        ReplaceSkip();
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
        // Running water tests: settings are clamped, and the panel only reports
        // a mask the latest evaluation sent that would still be sent now.
        Controller water;
        rrwater::Settings wet;
        wet.responsivity = true;
        wet.strength = 3.0f;
        wet.guide = 7;
        water.SetWater(wet);
        assert(water.water.strength == 1.0f && water.water.guide == rrwater::Mirror);
        auto wet_status = water.WaterStatus();
        assert(wet_status.wanted && !wet_status.ready && !wet_status.unavailable &&
               !wet_status.responsivity && wet_status.coverage < 0);
        water.water_status.ready = water.water_status.responsivity = true;
        water.water_status.discard = true;
        assert(!water.WaterStatus().ready); // RR is off
        water.SetEnabled(true);
        assert(water.WaterStatus().ready && !water.WaterStatus().responsivity); // no preset yet
        water.preset.Created();
        wet_status = water.WaterStatus();
        assert(wet_status.ready && wet_status.responsivity && !wet_status.discard);
        // The panel's game flags follow the latest delivered frame's counts.
        assert(wet_status.prepass[rrwater::Special] < 0);
        water.water_stats.Add({2, {4, 2, 1, 3, 0}}, 16);
        wet_status = water.WaterStatus();
        assert(wet_status.coverage == 0.125f && wet_status.prepass[rrwater::Special] == 0.25f &&
               wet_status.prepass[rrwater::Override] == 0.0625f);
        wet.discard = true;
        water.SetWater(wet);
        assert(water.WaterStatus().discard);
        // Water storage waits for a first SR allocation and for the retry
        // window; a shortage then waits again without failing the tests.
        GpuFrame dry;
        assert(!water.PrepareWater(dry, 1000) && water.water_retry_after == 0);
        dry.rr_output.Attach(new CpuResource);
        water.water_retry_after = 2000;
        assert(water.WaterStatus().waiting_memory);
        assert(!water.PrepareWater(dry, 1999) && water.water_retry_after == 2000);
        assert(!water.PrepareWater(dry, 2000) && water.water_retry_after == 3000); // no device
        assert(!water.water_failed && water.WaterStatus().waiting_memory);
        water.SetEnabled(false);
        assert(!water.WaterStatus().waiting_memory); // nothing retries while RR is off
        water.SetEnabled(true);
        water.SetWater({});
        assert(water.water_retry_after == 0);
        water.SetWater(wet);
        water.water_failed = true;
        wet_status = water.WaterStatus();
        assert(wet_status.unavailable && !wet_status.ready);
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
        assert(f->queries == before_queries && releases == 0 &&
               destroys == 0); // UI never calls driver.
        bool *held[] = {&s.pre_live,
                        &s.sr_live,
                        &s.sss.live,
                        &s.transparency.prep_live,
                        &s.transparency.anchor_live,
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

        // Only an admitted SR submission hands its SSS guide to masked lighting,
        // for the next MaxAge frames of the same generation and extent. A rejected
        // resolve of that guide, an SSS toggle or a rebuild releases it.
        auto *guide = new CpuResource;
        {
            Controller masked;
            masked.SetEnabled(true);
            masked.SetSssEnabled(true);
            auto &producer = masked.slots[1];
            producer.frame = 10;
            producer.generation = masked.generation;
            producer.gpu.sss.guide = guide;
            auto &ms = masked.slots[0];
            ms.generation = masked.generation;
            ms.pre_live = ms.native_sampling = true;
            ms.frame = 11;
            assert(!masked.SssMask(nullptr));
            masked.SealSssMask(producer, false);
            assert(!masked.SssMask(nullptr));
            masked.SealSssMask(producer, true);
            unsigned age{};
            assert(masked.SssMask(nullptr, &age).Get() == guide && age == 1);
            ms.frame = 10 + rrsssmask::MaxAge;
            assert(masked.SssMask(nullptr, &age).Get() == guide && age == rrsssmask::MaxAge);
            ms.frame = 11 + rrsssmask::MaxAge;
            assert(!masked.SssMask(nullptr));
            ms.frame = 10;
            assert(!masked.SssMask(nullptr));
            ms.frame = 11;
            ms.gpu.w = 8;
            assert(!masked.SssMask(nullptr));
            ms.gpu.w = 0;
            ms.pending_sr = true;
            assert(!masked.SssMask(nullptr));
            ms.pending_sr = false;
            ms.native_sampling = false;
            assert(!masked.SssMask(nullptr));
            ms.native_sampling = true;
            assert(masked.SssMask(nullptr));
            masked.SealSssMask(producer, false);
            assert(!masked.SssMask(nullptr) && !masked.sss_mask.valid && !masked.sss_mask_guide);
            masked.SealSssMask(producer, true);
            masked.SetSssEnabled(false);
            assert(!masked.SssMask(nullptr) && !masked.sss_mask_guide);
            masked.SetSssEnabled(true);
            masked.SealSssMask(producer, true);
            assert(masked.SssMask(nullptr));
            masked.SetEnabled(false);
            masked.SetEnabled(true);
            ms.generation = masked.generation;
            assert(!masked.SssMask(nullptr)); // Recorded under the previous generation.
            ms.pre_live = false;
            masked.QueueReconfigure();
            assert(masked.Reconfigure());
            assert(!masked.sss_mask.valid && !masked.sss_mask_guide);
        }
        assert(guide->refs == 1);
        guide->Release();
    }
};
} // namespace rrlive
int main() {
    rrlive::ControllerCpuTest::Run();
}
