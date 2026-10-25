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
    unsigned descs{}; // GetDesc calls
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        ++descs;
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
    // The ray budget test's controller bookkeeping. CPU only: no slot here records GPU work, and
    // the gate is asserted through its public status.
    static rrraybudget::Lists RayLists() {
        rrraybudget::Lists l;
        for (unsigned k = 0; k < 4; ++k)
            l.v[k] = {rrraybudget::Binding::Table, 0x1000u + 0x100u * k, 0,
                      k < 2    ? 144u + 4u * 1024u
                      : k == 2 ? 256u
                               : 4u};
        l.budget_resource = 0x5000;
        l.budget_offset = 64;
        l.w = l.h = 8;
        l.gx = l.gy = 1;
        l.why = rrraybudget::Why::None;
        return l;
    }
    // What a plan armed RayLists() with.
    static rrraybudget::Armed RayArmed() {
        rrraybudget::Armed a;
        a.w = a.h = 8;
        a.capacity = {1024, 1024};
        a.pixels = 256;
        a.counter = 4;
        return a;
    }
    // A frame the reader counted: lobe 1 has 64 requested pixels at rate 1, lobe 2 none.
    static rrdensity::Frame RayRates() {
        rrdensity::Frame f{};
        f[1] = 64;
        f[rrdensity::Buckets] = 64;
        return f;
    }
    // Decide waits once 8 raised frames in a row went unverified.
    static bool RayWaiting(Controller &c) {
        return c.ray_gate.Decide(rrraybudget::Times2, 1000, nullptr).why ==
               rrraybudget::Why::Unverified;
    }
    static Controller::Slot &RayLive(Controller &c) {
        c.SetEnabled(true);
        auto &s = c.slots[0];
        s.generation = c.generation;
        s.pre_live = true;
        return s;
    }
    // The slot's next recording, from a cleared slot as retirement leaves it.
    static Controller::Slot &RayNext(Controller &c) {
        c.slots[0] = {};
        return RayLive(c);
    }
    static bool RaySame(const rrraybudget::Armed &a, const rrraybudget::Armed &b) {
        return a.w == b.w && a.h == b.h && a.capacity == b.capacity && a.margin == b.margin &&
               a.pixels == b.pixels && a.counter == b.counter;
    }
    // The controller's 10 s line for the window so far, which this ends. Each call reports later
    // than every earlier one, so a window that has not started yet starts and is reported at once.
    static std::string RayLine(Controller &c) {
        static UINT64 clock = 0;
        std::string controller, lists;
        for (int i = 0; i < 2; ++i) {
            clock += rrraybudget::Summary::Interval;
            if (c.ray_summary.Report(clock, c.ray_gate, c.StatusLocked(clock), controller, lists))
                return controller;
        }
        return {};
    }
    // The window's refusals: the line after " | refused " ("" when nothing was refused).
    static std::string RayRefused(Controller &c) {
        const auto line = RayLine(c);
        const auto at = line.find(" | refused ");
        return at == std::string::npos ? std::string() : line.substr(at + 11);
    }
    // A measured, unraised frame of `l`: the reader counted every pixel (lobe 1 requested at rate
    // 1, lobe 2 not requested), and the game's target 0.4, the governor's input and register 253
    // (no second segment) were copied.
    static rrraybudget::Completed RayMeasured(const rrraybudget::Lists &l) {
        rrraybudget::Completed c;
        c.lists = l;
        const std::uint32_t pixels = l.w * l.h;
        c.rates[1] = pixels;
        c.rates[rrdensity::Buckets] = pixels;
        c.rates[rrdensity::Settings1] = 1;
        c.rates[rrdensity::Limits] = c.rates[rrdensity::Limits + 2] = 9;
        c.log[rrraybudget::LogWordResolveTarget] = c.log[rrraybudget::LogWordResolveTarget + 1] =
            std::bit_cast<std::uint32_t>(0.4f);
        c.logged = rrraybudget::LogResolveTarget | rrraybudget::LogResolveStats |
                   rrraybudget::LogApplyScene253;
        return c;
    }
    // The game's resolve as PreserveRayBudget reports it: counted, then given to the controller
    // while RR is enabled. It proves the Budget (resource, offset) and the hook refused nothing;
    // with no GPU here it is never patched.
    static void RayResolve(Controller &c, UINT64 resource = 0x5000, UINT64 offset = 64) {
        rrvrrt::RayResolve in;
        in.why = rrraybudget::Why::None;
        in.budget_resource = resource;
        in.budget_offset = offset;
        c.RayBudgetResolveSeen();
        bool dispatched = false;
        const bool patched = c.Enabled() && c.RayBudgetResolve(nullptr, in, 2, 1, 1, dispatched);
        assert(!patched && !dispatched);
    }
    // `n` recordings on slot 0, each a resolve that proves the Budget `l` names and then the apply
    // of `l`: same-frame placement (A2).
    static void RayFrames(Controller &c, const rrraybudget::Lists &l, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            RayNext(c);
            RayResolve(c, l.budget_resource, l.budget_offset);
            c.RayBudgetApply(nullptr, l, {});
        }
    }
    // A gate that arms at `l`: ProofFrames same-frame recordings, then ProofFrames measured frames.
    static void RayPrime(Controller &c, const rrraybudget::Lists &l) {
        RayFrames(c, l, rrraybudget::ProofFrames);
        for (unsigned i = 0; i < rrraybudget::ProofFrames; ++i)
            c.ray_gate.Complete(RayMeasured(l));
    }
    // F1 (A2): the apply that consumes a governed Budget runs in the same recording, after the
    // patch. A resolve after its recording's apply is refused (AfterApply), and a resolve patches
    // only after ProofFrames applies in a row read the Budget their own resolve wrote (Placement).
    // There is no GPU here: a resolve that passes every check up to Decide then waits for its stats
    // copy (statsUnverified), which is where a patch would follow.
    static void RayPlacement() {
        using rrraybudget::Why;
        const auto l = RayLists();
        // The normal order arms. The streak counts in every mode: this one is primed with the test
        // off.
        rrraybudget::mode = rrraybudget::Off;
        {
            Controller c;
            RayPrime(c, l);
            assert(c.ray_placement == rrraybudget::ProofFrames);
            rrraybudget::mode = rrraybudget::Times2;
            RayLine(c);
            RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "statsUnverified=1");
        }
        // 119 same-frame applies (after 5 without a resolve, so the gate's own streak is long):
        // the resolve is refused although the gate would arm; its apply is the 120th, and the next
        // resolve arms. An apply whose recording had no resolve then resets the streak.
        {
            Controller c;
            for (unsigned i = 0; i < 5; ++i) {
                RayNext(c);
                c.RayBudgetApply(nullptr, l, {});
            }
            RayFrames(c, l, rrraybudget::ProofFrames - 1);
            for (unsigned i = 0; i < rrraybudget::ProofFrames; ++i)
                c.ray_gate.Complete(RayMeasured(l));
            RayLine(c);
            RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "placement=1");
            assert(c.ray_gate.Decide(rrraybudget::Times2, GetTickCount64(), nullptr).why ==
                   Why::None);
            c.RayBudgetApply(nullptr, l, {});
            RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "statsUnverified=1");
            // An unproven apply that reads the Budget its own resolve wrote resets the streak, and
            // so does a proven apply in a recording without a resolve.
            auto unproven = l;
            unproven.why = Why::ListsUnproven;
            c.RayBudgetApply(nullptr, unproven, {});
            assert(c.ray_placement == 0);
            RayFrames(c, l, 1);
            assert(c.ray_placement == 1);
            RayNext(c);
            c.RayBudgetApply(nullptr, l, {});
            assert(c.ray_placement == 0);
            RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "placement=2");
        }
        // A resolve after its recording's apply is refused, and nothing is dispatched (RayResolve
        // asserts that); that apply was not same-frame either.
        {
            Controller c;
            RayPrime(c, l);
            RayLine(c);
            RayNext(c);
            c.RayBudgetApply(nullptr, l, {});
            RayResolve(c);
            assert(RayRefused(c) == "afterApply=1" && c.ray_placement == 0);
        }
        // A ping-pong Budget read a frame late: every apply reads the identity the other resolve
        // wrote, in another resource or at another offset in the same one. The gate knows both and
        // would arm, but the resolve never does.
        for (const UINT64 offset : {UINT64(0), UINT64(128)}) {
            Controller c;
            auto other = l;
            other.budget_resource = offset ? l.budget_resource : 0x6000;
            other.budget_offset = offset;
            for (unsigned i = 0; i < 2 * rrraybudget::ProofFrames; ++i) {
                RayNext(c);
                const auto &wrote = i & 1 ? other : l, &read = i & 1 ? l : other;
                RayResolve(c, wrote.budget_resource, wrote.budget_offset);
                c.RayBudgetApply(nullptr, read, {});
            }
            for (unsigned i = 0; i < rrraybudget::ProofFrames; ++i)
                c.ray_gate.Complete(RayMeasured(l));
            assert(c.ray_placement == 0 && c.ray_gate.KnownBudget(0x5000, 64) &&
                   c.ray_gate.KnownBudget(other.budget_resource, other.budget_offset));
            assert(c.ray_gate.Decide(rrraybudget::Times2, GetTickCount64(), nullptr).why ==
                   Why::None);
            RayLine(c);
            RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "placement=1");
            // The 10 s line counts the window's same-frame applies of all its applies.
            RayFrames(c, l, 3);
            RayNext(c);
            c.RayBudgetApply(nullptr, l, {});
            assert(RayLine(c).find(" sameFrame=3/4 | ") != std::string::npos);
        }
        rrraybudget::mode = rrraybudget::Off;
    }
    // A retired recording's judgement with its own ray log: the measured frame of `l` (RayMeasured)
    // with both halves' serials and the executed bits the slot wrote, if any.
    static void RayJudgeOwn(Controller &c, Controller::Slot &s, const rrraybudget::Lists &l) {
        auto m = RayMeasured(l);
        m.log[rrraybudget::LogWordExecuted] = s.ray_written[0];
        m.log[rrraybudget::LogWordExecuted + 1] = s.ray_written[1];
        m.log[rrraycopy::ResolveSerial.at / 4] = m.log[rrraycopy::ApplySerial.at / 4] =
            s.ray_serial;
        s.ray_logged |= m.logged | rrraybudget::LogApplyTarget;
        s.pre_submitted = true;
        c.RayJudge(s, m.rates, m.log, Controller::RayLogState::Read);
        assert(s.ray_done && !c.failed);
    }
    // F2: every generation change restarts the proof, the gate's apply streak (Gate::Restart) and
    // the placement streak. A gate primed at 8x8 refuses the first resolve after Reconfigure, or
    // after RR was turned off or on (each edge alone), so that recording's 16x16 apply consumes no
    // Budget armed for 8x8 and its judgement stops nothing. There is no GPU here: had the gate
    // armed, RayPatched records the patch the GPU would have.
    static void RayRestart() {
        using rrraybudget::Why;
        const auto l8 = RayLists();
        auto l16 = RayLists();
        l16.w = l16.h = 16;
        rrraybudget::mode = rrraybudget::Times2;
        for (unsigned how = 0; how < 3; ++how) {
            Controller c;
            RayPrime(c, l8);
            c.slots[0] = {};
            assert(c.ray_placement == rrraybudget::ProofFrames &&
                   c.ray_gate.Decide(rrraybudget::Times2, GetTickCount64(), nullptr).why ==
                       Why::None);
            if (how == 0) {
                c.QueueReconfigure();
                assert(c.Reconfigure());
            } else if (how == 1) {
                c.SetEnabled(false);
                c.enabled = true; // back on without the second edge
            } else {
                c.enabled = false; // off without the first edge
                c.SetEnabled(true);
            }
            const UINT64 now = GetTickCount64(); // within FreshMs of the last 8x8 apply
            assert(c.ray_placement == 0);
            assert(c.ray_gate.Decide(rrraybudget::Times2, now, nullptr).why == Why::ListsUnproven);
            RayLine(c);
            auto &s = RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "placement=1");
            const auto plan = c.ray_gate.Decide(rrraybudget::Times2, now, nullptr);
            if (plan.why == Why::None) {
                rrvrrt::RayResolve in;
                in.budget_resource = l8.budget_resource;
                in.budget_offset = l8.budget_offset;
                c.RayPatched(s, plan, in, now, rrraybudget::Times2);
            }
            c.RayBudgetApply(nullptr, l16, {});
            RayJudgeOwn(c, s, l16);
            assert(!s.ray_patched && !s.ray_budget && c.ray_gate.Stopped() == Why::None);
        }
        rrraybudget::mode = rrraybudget::Off;
    }
    // M2: a mark outlives a generation change, since the game's Budget keeps what the governor
    // wrote, so an apply of the new generation that reads it within RayMarkLife resolves is raised.
    // Its arming snapshot is of the old lists: the frame is counted unverified once, not judged
    // (judged at another extent it would stop the test with ListsAfterArming, as the control does).
    static void RayOldMark() {
        using rrraybudget::Why;
        auto l16 = RayLists();
        l16.w = l16.h = 16;
        for (unsigned how = 0; how < 3; ++how) { // Reconfigure, F8 off and on, control
            Controller c;
            auto &s = RayLive(c);
            rrraybudget::Plan plan;
            plan.mask = 1;
            plan.armed = RayArmed(); // 8x8
            rrvrrt::RayResolve in;
            in.budget_resource = 0x5000;
            in.budget_offset = 64;
            c.RayBudgetResolveSeen(); // the patched resolve
            c.RayPatched(s, plan, in, 1, rrraybudget::Times2);
            c.slots[0] = {};
            if (how == 0) {
                c.QueueReconfigure();
                assert(c.Reconfigure());
            } else if (how == 1) {
                c.SetEnabled(false);
                c.SetEnabled(true);
            }
            for (unsigned i = 0; i < Controller::RayMarkLife - 1; ++i)
                c.RayBudgetResolveSeen(); // the mark is still live
            for (unsigned i = 0; i < rrraybudget::UnverifiedLimit - 2; ++i)
                c.ray_gate.Unverified();
            // An apply that reads an unknown Budget takes the union of the live marks, of the old
            // generation when any of them is.
            auto &u = RayNext(c);
            c.RayBudgetApply(nullptr, rrraybudget::Lists{}, {});
            assert(u.ray_budget == 1 && u.ray_old_mark == (how < 2));
            auto &r = RayNext(c);
            c.RayBudgetApply(nullptr, l16, {});
            assert(r.ray_budget == 1 && r.ray_old_mark == (how < 2));
            RayJudgeOwn(c, r, l16);
            if (how == 2) {
                assert(c.ray_gate.Stopped() == Why::ListsAfterArming);
                continue;
            }
            assert(c.ray_gate.Stopped() == Why::None && !RayWaiting(c));
            c.ray_gate.Unverified(); // so that frame was counted exactly once
            assert(RayWaiting(c));
        }
    }
    // M1: the ray buffers' growth of budget.limit (AllocateRay reserves on top of the limit) is
    // given back at Reconfigure together with test_reserve, so rebuilds do not ratchet it.
    // RayPrepare counts it on every exit of a Prepare call. There is no device here (and Budget::
    // Reserve needs DXGI), so each exit changes the limit as AllocateRay and PrepareRayLog do:
    // Reserve raises it to floor + needed, then the catch keeps floor + added and rethrows.
    static void RayReserve() {
        constexpr UINT64 log = 4096, values = 65536, pre = Budget::Minimum + 8192;
        enum Exit { Allocated, DeferredSecond, MapFailed, NotReserved };
        auto prepare = [](Controller &c, Exit exit) {
            const UINT64 floor = c.budget.limit;
            if (exit == NotReserved)
                return false;                      // nothing allocated, the limit unchanged
            c.budget.limit = floor + log + values; // Reserve(floor + needed)
            if (exit == DeferredSecond) {
                c.budget.limit = floor + log; // the first buffer was allocated
                throw rrprivate::AllocationDeferred();
            }
            if (exit == MapFailed)
                throw std::runtime_error("ray budget upload map failed");
            return true;
        };
        for (const Exit exit : {Allocated, DeferredSecond, MapFailed, NotReserved}) {
            Controller c;
            c.budget.limit = pre;
            bool threw = false;
            try {
                assert(c.RayPrepare([&] { return prepare(c, exit); }) == (exit == Allocated));
            } catch (const std::exception &) {
                threw = true;
            }
            assert(threw == (exit == DeferredSecond || exit == MapFailed));
            const UINT64 grown = exit == NotReserved      ? 0
                                 : exit == DeferredSecond ? log
                                                          : log + values;
            assert(c.ray_reserve == grown && c.budget.limit == pre + grown);
            // A retry grows it again (here by the whole sum): counted too.
            assert(c.RayPrepare([&] { return prepare(c, Allocated); }));
            assert(c.ray_reserve == grown + log + values);
            c.QueueReconfigure();
            assert(c.Reconfigure() && c.budget.limit == pre && !c.ray_reserve);
        }
        // Counted saturating, given back with test_reserve in one step, saturating, never below
        // what is allocated.
        Controller c;
        c.budget.limit = pre;
        c.ray_reserve = UINT64_MAX - 5;
        assert(c.RayPrepare([&] { return prepare(c, Allocated); }) && c.ray_reserve == UINT64_MAX);
        c.budget.limit = pre + 300;
        c.test_reserve = 200;
        c.ray_reserve = 100;
        c.QueueReconfigure();
        assert(c.Reconfigure() && c.budget.limit == pre && !c.test_reserve && !c.ray_reserve);
        c.budget.limit = pre;
        c.test_reserve = 10;
        c.ray_reserve = UINT64_MAX - 5;
        c.QueueReconfigure();
        assert(c.Reconfigure() && c.budget.limit == Budget::Minimum);
        c.budget.limit = pre;
        c.budget.bytes = pre - 100;
        c.ray_reserve = 200;
        c.QueueReconfigure();
        assert(c.Reconfigure() && c.budget.limit == pre - 100);
    }
    // M5: the status reports the outcome of the last resolve in a recording while it is under
    // RayActiveMs old and in the selected mode; resolves placed elsewhere never change it. While RR
    // is off or failed it says so, never active; with no recent resolve it says the game's resolve
    // has not run; once the logging copies failed, resolves wait for a reason that names it.
    static void RayStatus() {
        using rrraybudget::Why;
        rrraybudget::Plan plan;
        plan.mask = 1;
        plan.native = {0.4f, 0.4f};
        plan.target = {0.8f, 0.4f};
        plan.armed = RayArmed();
        rrvrrt::RayResolve in;
        in.budget_resource = 0x5000;
        in.budget_offset = 64;
        auto describe = [](const rrraybudget::Status &s) {
            char text[256];
            rrraybudget::Describe(s, text, sizeof(text));
            return std::string(text);
        };
        {
            Controller c;
            assert(describe(c.StatusLocked(1000)) == "off");
            rrraybudget::mode = rrraybudget::Times2;
            assert(c.StatusLocked(1000).why == Why::RrOff); // RR not enabled
            auto &s = RayLive(c);
            assert(c.StatusLocked(1000).why == Why::ResolveIdle);
            c.ray_logged_mode = rrraybudget::Times2;
            c.RayPatched(s, plan, in, 1000, rrraybudget::Times2);
            auto st = c.StatusLocked(1000 + Controller::RayActiveMs);
            assert(st.active &&
                   describe(st) == "active: lobe1 0.400 -> 0.800, lobe2 0.400 (not raised)");
            // Resolves on other lists, and a second one in this recording, do not change it.
            c.RayMisplaced(Why::Outside, rrraybudget::Times2, 1100);
            c.RayMisplaced(Why::Repeated, rrraybudget::Times2, 1100);
            c.RayMisplaced(Why::Ambiguous, rrraybudget::Times2, 1100);
            assert(c.StatusLocked(1100).active);
            // RR off or failed: never active, and the reason says so.
            c.enabled = false;
            st = c.StatusLocked(1100);
            assert(!st.active && describe(st) == "waiting: RR is off");
            c.enabled = true;
            c.failed = true;
            assert(!c.StatusLocked(1100).active && c.StatusLocked(1100).why == Why::RrOff);
            c.failed = false;
            c.retired = true;
            assert(!c.StatusLocked(1100).active && c.StatusLocked(1100).why == Why::RrOff);
            c.retired = false;
            // Another mode selected since: not this outcome.
            rrraybudget::mode = rrraybudget::OnePerPixel;
            assert(!c.StatusLocked(1100).active && c.StatusLocked(1100).why == Why::ResolveIdle);
            rrraybudget::mode = rrraybudget::Times2;
            // Too old: the recent misplaced resolve's reason, then that the resolve is idle.
            st = c.StatusLocked(1001 + Controller::RayActiveMs);
            assert(!st.active && st.why == Why::Ambiguous);
            st = c.StatusLocked(1101 + Controller::RayActiveMs);
            assert(!st.active &&
                   describe(st) == "waiting: the game's resolve has not run recently");
            // A refusal in a recording is the reason; before the patch it was older.
            c.RayRefuse(Why::Evidence, rrraybudget::Times2, 3000);
            assert(!c.StatusLocked(3000).active && c.StatusLocked(3000).why == Why::Evidence);
            c.RayMisplaced(Why::Outside, rrraybudget::Times2, 3001);
            assert(c.StatusLocked(3001).why == Why::Evidence);
        }
        // The logging copies failed for the session: a resolve that would reach Decide says so.
        {
            Controller c;
            const auto l = RayLists();
            rrraybudget::mode = rrraybudget::Times2;
            RayPrime(c, l);
            c.ray_log_failed = true;
            RayLine(c);
            RayNext(c);
            RayResolve(c);
            assert(RayRefused(c) == "loggingOff=1");
            assert(c.StatusLocked(GetTickCount64()).why == Why::LoggingOff);
        }
        rrraybudget::mode = rrraybudget::Off;
        // The governor's outcomes: a PSO; a full cache or memory waits; a root the device cannot
        // use (nothing created) is Context, never sticky; only a creation failure is Pipeline.
        using G = Pipelines::GovernorPipeline;
        auto *pso = reinterpret_cast<ID3D12PipelineState *>(0x40);
        assert(Controller::RayGovernor(G{pso, S_OK, false, true}) == Why::None);
        assert(Controller::RayGovernor(G{nullptr, S_OK, true, false}) == Why::GovernorCache);
        assert(Controller::RayGovernor(G{nullptr, E_OUTOFMEMORY, false, true}) == Why::Memory);
        assert(Controller::RayGovernor(G{nullptr, E_INVALIDARG, false, false}) == Why::Context);
        assert(Controller::RayGovernor(G{nullptr, E_INVALIDARG, false, true}) == Why::Pipeline);
        assert(Controller::RayGovernor(G{nullptr, E_FAIL, false, true}) == Why::Pipeline);
        Pipelines none; // no device: nothing created, so Context
        const auto g = none.Governor(nullptr);
        assert(!g.pso && g.hr == E_INVALIDARG && !g.created && !g.full);
        assert(Controller::RayGovernor(g) == Why::Context);
        // A ListsAfterArming stop names register 253 when it was copied.
        rrraybudget::Completed f;
        f.lists = RayLists();
        f.armed = RayArmed();
        f.log[rrraybudget::LogWordScene253] = 5;
        char details[320];
        Controller::RayStopDetails(Why::ListsAfterArming, f, details, sizeof(details));
        assert(std::string(details).find("scene253") == std::string::npos);
        f.logged = rrraybudget::LogApplyScene253;
        Controller::RayStopDetails(Why::ListsAfterArming, f, details, sizeof(details));
        const std::string text = details;
        assert(text.size() > 13 && text.compare(text.size() - 11, 11, " scene253=5") == 0);
        // The widest details fit the controller's buffer: the longest reason, every field at its
        // maximum.
        f.lists.why = Why::Governor;
        f.lists.w = f.lists.h = f.armed.w = f.armed.h = UINT_MAX;
        for (auto &v : f.lists.v)
            v.bytes = UINT64_MAX;
        f.armed.capacity = {UINT64_MAX, UINT64_MAX};
        f.armed.pixels = f.armed.counter = UINT64_MAX;
        f.log[rrraybudget::LogWordScene253] = UINT_MAX;
        char widest[Controller::RayDetailsBytes];
        Controller::RayStopDetails(Why::ListsAfterArming, f, widest, sizeof(widest));
        const std::string wide = widest;
        assert(wide.size() + 1 < sizeof(widest) &&
               wide.compare(wide.size() - 20, 20, " scene253=4294967295") == 0);
        // The active line names both governor thresholds and the clamp.
        Controller a;
        logged.clear();
        a.logger = Logged;
        a.RayActiveLog(rrraybudget::Times2, plan);
        assert(logged.find("keeps raised thresholds at -16 or above and, in a frame whose rays "
                           "would exceed a lobe's headroom, sets that lobe's threshold to 17 (at "
                           "most one ray per pixel with clamp on; the game's own behaviour with "
                           "clamp off)") != std::string::npos);
    }
    // The verification and bookkeeping details: M3, M4, S2, S3 and the checks no earlier test
    // pinned.
    static void RayChecks() {
        using rrraybudget::Why;
        using Log = std::array<std::uint32_t, rrraybudget::LogWords>;
        // M3: a patch whose resolve half ran as recorded is checked against it although its apply
        // half is stale (log word 27 = 6): PatchMismatch. With the executed bits it wrote, the
        // frame is unverified instead (the 8th in a row), and nothing stops.
        for (const std::uint32_t executed : {0xBADu, 0x3f000000u}) {
            Controller c;
            for (unsigned i = 0; i < rrraybudget::UnverifiedLimit - 1; ++i)
                c.ray_gate.Unverified();
            auto &s = c.slots[0];
            s.pre_submitted = true;
            s.ray_patched = 1;
            s.ray_written = {0x3f000000u, 0};
            s.ray_patch_armed = RayArmed();
            s.ray_lists = RayLists();
            s.ray_serial = 7;
            s.ray_logged = rrraybudget::LogResolveTarget | rrraybudget::LogExecuted |
                           rrraybudget::LogResolveStats | rrraybudget::LogApplyTarget;
            Log log{};
            log[rrraybudget::LogWordExecuted] = executed;
            log[rrraybudget::LogWordResolveSerial] = 7;
            log[rrraybudget::LogWordApplySerial] = 6;
            c.RayJudge(s, RayRates(), log, Controller::RayLogState::Read);
            assert(s.ray_done && !c.failed);
            if (executed == 0xBADu)
                assert(c.ray_gate.Stopped() == Why::PatchMismatch);
            else
                assert(c.ray_gate.Stopped() == Why::None && RayWaiting(c));
        }
        // M4: PatchResolve records nothing, and reads no buffer, without the recording's serial.
        {
            GpuFrame f;
            auto *cb = new CpuResource, *game = new CpuResource;
            f.ray_cb.Attach(cb);
            f.ray_log.Attach(new CpuResource);
            std::array<UINT, 64> map{};
            f.ray_map = map.data();
            rrvrrt::RayConstant b1;
            b1.resource.Attach(game);
            b1.upload = true;
            b1.avail = 4096;
            rrraybudget::Plan plan;
            plan.mask = 1;
            auto *cl = reinterpret_cast<ID3D12GraphicsCommandList *>(0x40);
            assert(!f.PatchResolve(cl, b1, plan) && game->descs == 0);
            f.ray_serial = 1; // control: the b1 check runs (here the view is no buffer)
            assert(!f.PatchResolve(cl, b1, plan) && game->descs == 1);
            f.ray_map = nullptr;
        }
        // S2: sameB1AsApply compares two known constant blocks only.
        {
            Controller c;
            rrraybudget::mode = rrraybudget::Times2;
            RayLine(c);
            for (const UINT64 address : {UINT64(0), UINT64(0x100)}) {
                RayNext(c);
                rrvrrt::RayResolve in;
                in.why = Why::Shape;
                in.settings.address = address;
                bool dispatched = false;
                assert(!c.RayBudgetResolve(nullptr, in, 2, 1, 1, dispatched));
                rrvrrt::RayApply log;
                log.settings.address = address;
                c.RayBudgetApply(nullptr, RayLists(), log);
                // An apply first, then a resolve, in the next recording.
                RayNext(c);
                c.RayBudgetApply(nullptr, RayLists(), log);
                assert(!c.RayBudgetResolve(nullptr, in, 2, 1, 1, dispatched));
            }
            const auto line = RayLine(c);
            assert(line.find(" afterApply=2 ") != std::string::npos &&
                   line.find(" sameB1AsApply=2/2 ") != std::string::npos);
        }
        // S3: a consumer is counted only for the handle still published, with the view offset of
        // the apply that published it; Reconfigure unpublishes both lists.
        {
            Controller c;
            RayLive(c);
            auto l = RayLists();
            l.v[0].offset = 256;
            c.RayBudgetApply(nullptr, l, {});
            assert(rrraybudget::published[0].load() == l.v[0].resource &&
                   rrraybudget::published[1].load() == l.v[1].resource);
            RayLine(c);
            c.RayBudgetConsumer({3, 0, 256 + 64, 1, 12}, l.v[0].resource);
            c.RayBudgetConsumer({5, 1, 0, 1, 12}, 0xDEAD); // not the published handle
            c.RayBudgetConsumer({5, 1, 0, 1, 12}, 0);
            auto line = RayLine(c);
            assert(
                line.find(" | consumers lobe1=1 lobe2=0 dispatch=1 rays=0 other=0 last=3/12/64") !=
                std::string::npos);
            c.slots[0] = {};
            c.QueueReconfigure();
            assert(c.Reconfigure());
            assert(!rrraybudget::published[0].load() && !rrraybudget::published[1].load() &&
                   !c.ray_list_offsets[0] && !c.ray_list_offsets[1]);
            c.RayBudgetConsumer({3, 0, 256 + 64, 1, 12}, l.v[0].resource);
            line = RayLine(c);
            assert(line.find(" | consumers lobe1=0 lobe2=0 dispatch=0 ") != std::string::npos);
        }
        // A resolve whose Budget no proven apply read is refused (BudgetIdentity).
        {
            Controller c;
            RayPrime(c, RayLists());
            RayLine(c);
            RayNext(c);
            RayResolve(c, 0x9000, 0);
            assert(RayRefused(c) == "budgetIdentity=1");
        }
        rrraybudget::mode = rrraybudget::Off;
        // Serials skip 0 when they wrap.
        {
            Controller c;
            c.ray_next_serial = UINT_MAX - 1;
            assert(c.RayNextSerial() == UINT_MAX && c.RayNextSerial() == 1);
        }
        // Reconfigure counts a raised recording it retires as unverified.
        {
            Controller c;
            for (unsigned i = 0; i < rrraybudget::UnverifiedLimit - 1; ++i)
                c.ray_gate.Unverified();
            c.slots[0].ray_patched = 1;
            c.QueueReconfigure();
            assert(c.Reconfigure() && RayWaiting(c));
        }
        // A resolve outside any recording remembers its list for beforePrepass; one after SR does
        // not, and Reset forgets it.
        {
            Controller c;
            auto *x = reinterpret_cast<ID3D12GraphicsCommandList *>(0x40);
            rrvrrt::RayResolve in;
            bool dispatched = false;
            RayLive(c);
            assert(!c.RayBudgetResolve(x, in, 2, 1, 1, dispatched) && c.ray_early == x);
            c.Reset(x);
            assert(!c.ray_early);
            c.slots[0].recorded = true; // slot 0 records on nullptr: after SR there
            assert(!c.RayBudgetResolve(nullptr, in, 2, 1, 1, dispatched) && !c.ray_early);
        }
    }
    static void RayBudget() {
        using rrraybudget::Why;
        using Log = std::array<std::uint32_t, rrraybudget::LogWords>;
        constexpr unsigned resolve_bits = rrraybudget::LogResolveTarget | rrraybudget::LogExecuted |
                                          rrraybudget::LogResolveScene |
                                          rrraybudget::LogResolveStats,
                           apply_bits =
                               rrraybudget::LogApplyTarget | rrraybudget::LogApplySettings36 |
                               rrraybudget::LogApplyScene251 | rrraybudget::LogApplyScene253;
        // Session only: the settings combo starts Off.
        assert(rrraybudget::Valid(rrraybudget::mode.load()) == rrraybudget::Off);
        {
            Controller off;
            char text[64];
            rrraybudget::Describe(off.RayBudgetStatus(), text, sizeof(text));
            assert(std::string(text) == "off");
        }
        // No live recording: refused and counted as Outside, nothing dispatched, RR not failed.
        {
            Controller idle;
            std::string a, b;
            assert(!idle.ray_summary.Report(1, idle.ray_gate, idle.StatusLocked(1), a, b));
            rrvrrt::RayResolve in;
            in.why = Why::None;
            bool dispatched = false;
            assert(!idle.RayBudgetResolve(nullptr, in, 2, 1, 1, dispatched) && !dispatched);
            assert(!idle.failed);
            assert(idle.ray_summary.Report(20001, idle.ray_gate, idle.StatusLocked(20001), a, b));
            assert(a.find(" outside=1 ") != std::string::npos);
            assert(a.find(" | refused outside=1") != std::string::npos);
            idle.RayBudgetApply(nullptr, RayLists(), {}); // no recording: a no-op
            assert(idle.ray_gate.Streak() == 0);
        }
        // A live recording: the first resolve is placed, a second one is Repeated.
        {
            Controller live;
            auto &s = RayLive(live);
            rrvrrt::RayResolve shape;
            shape.why = Why::Shape;
            bool dispatched = false;
            assert(!live.RayBudgetResolve(nullptr, shape, 3, 1, 1, dispatched) && !dispatched);
            assert(s.ray_resolved && !s.ray_logged);
            assert(!live.RayBudgetResolve(nullptr, shape, 3, 1, 1, dispatched) && !dispatched);
            assert(RayRefused(live) == "shape=1 repeated=1" && !live.failed);
            // Every recognised apply reaches the gate, proven or not.
            live.RayBudgetApply(nullptr, RayLists(), {});
            assert(s.ray_applied && s.ray_lists == RayLists() && !s.ray_budget);
            assert(live.ray_gate.Streak() == 1);
            live.RayBudgetApply(nullptr, rrraybudget::Lists{}, {});
            assert(live.ray_gate.Streak() == 0 && s.ray_lists.why == Why::ListsUnproven);
            // An internal error stops the test for the session; RR itself stays up.
            live.RayBudgetException();
            assert(live.ray_gate.Stopped() == Why::Exception && !live.failed && live.Enabled());
        }
        // Two RR recordings on one list: neither the resolve nor the apply is attributed.
        {
            Controller two;
            RayLive(two);
            two.slots[1].generation = two.generation;
            two.slots[1].pre_live = true;
            rrvrrt::RayResolve in;
            bool dispatched = false;
            assert(!two.RayBudgetResolve(nullptr, in, 2, 1, 1, dispatched) && !dispatched);
            assert(RayRefused(two) == "ambiguous=1" && !two.slots[0].ray_resolved);
            two.RayBudgetApply(nullptr, RayLists(), {});
            assert(two.ray_gate.Streak() == 0 && !two.slots[0].ray_applied);
        }
        // The Budget identity table.
        {
            Controller m;
            rrraybudget::Plan lobe1, lobe2;
            lobe1.mask = 1;
            lobe1.armed = RayArmed();
            lobe1.armed.w = 11;
            lobe2.mask = 2;
            lobe2.armed = RayArmed();
            lobe2.armed.w = 22;
            m.ray_resolves = 10;
            m.RaySetMark(0x5000, 64, lobe1, 10);
            auto got = m.RayConsumed(0x5000, 64);
            assert(got.mask == 1 && got.armed.w == 11);
            assert(!m.RayConsumed(0x5000, 128).mask && !m.RayConsumed(0x6000, 64).mask);
            m.RayClearMark(0x5000, 64);
            assert(!m.RayConsumed(0x5000, 64).mask);
            // One entry per identity: a later patch of the same Budget replaces it.
            m.RaySetMark(0x5000, 64, lobe1, 10);
            m.RaySetMark(0x5000, 64, lobe2, 10);
            unsigned entries = 0;
            for (const auto &mark : m.ray_marks)
                entries += mark.resource == 0x5000;
            got = m.RayConsumed(0x5000, 64);
            assert(entries == 1 && got.mask == 2 && got.armed.w == 22);
            // Full: the next identity replaces entries in turn, from the first.
            for (UINT64 r = 0x6000; r <= 0x8000; r += 0x1000)
                m.RaySetMark(r, 0, lobe1, 10);
            m.RaySetMark(0x9000, 0, lobe1, 10);
            assert(m.RayConsumed(0x9000, 0).mask == 1 && !m.RayConsumed(0x5000, 64).mask);
            assert(m.RayConsumed(0x6000, 0).mask == 1);
            m.RaySetMark(0xA000, 0, lobe1, 10);
            assert(m.RayConsumed(0xA000, 0).mask == 1 && !m.RayConsumed(0x6000, 0).mask);
            assert(m.RayConsumed(0x7000, 0).mask == 1);
            // A mark lives until 8 further resolves were seen: live after 7, expired at the 8th.
            m.ray_resolves = 17;
            assert(m.RayConsumed(0x9000, 0).mask == 1 && m.RayConsumed(0, 0).mask == 1);
            m.ray_resolves = 18;
            assert(!m.RayConsumed(0x9000, 0).mask && !m.RayConsumed(0, 0).mask);
            // An empty or expired entry is taken before a live one is replaced.
            m.ray_marks = {};
            m.ray_next_mark = 0;
            m.ray_resolves = 10;
            for (UINT64 r = 0x1000; r <= 0x4000; r += 0x1000)
                m.RaySetMark(r, 0, lobe1, 10);
            m.RayClearMark(0x2000, 0);
            m.RaySetMark(0x5000, 0, lobe1, 10);
            assert(m.RayConsumed(0x1000, 0).mask && m.RayConsumed(0x5000, 0).mask);
            assert(!m.RayConsumed(0x2000, 0).mask);
            m.RaySetMark(0x3000, 0, lobe1, 1); // patched 9 resolves ago: expired
            m.RaySetMark(0x6000, 0, lobe1, 10);
            for (UINT64 r : {0x1000, 0x4000, 0x5000, 0x6000})
                assert(m.RayConsumed(r, 0).mask == 1);
            // An unknown identity: the union of the live marks, the newest one's snapshot.
            m.ray_marks = {};
            m.ray_resolves = 18;
            m.RaySetMark(0x6000, 64, lobe2, 17); // the newest first: not the last one found
            m.RaySetMark(0x5000, 64, lobe1, 16);
            got = m.RayConsumed(0, 0);
            assert(got.mask == 3 && got.armed.w == 22);
        }
        // The apply takes its Budget's mark into the slot; Completed::armed follows it.
        {
            Controller a;
            auto &s = RayLive(a);
            rrraybudget::Plan plan;
            plan.mask = 1;
            plan.armed = RayArmed();
            plan.armed.w = 33;
            a.RaySetMark(0x5000, 64, plan, a.ray_resolves.load());
            a.RayBudgetApply(nullptr, RayLists(), {});
            assert(s.ray_budget == 1 && s.ray_armed.w == 33);
            s.ray_patched = 2;
            s.ray_patch_armed = RayArmed();
            s.ray_patch_armed.w = 44;
            assert(Controller::RayFrame(s, RayRates()).armed.w == 33); // the consumed Budget's plan
            s.ray_budget = 0;
            assert(Controller::RayFrame(s, RayRates()).armed.w == 44); // else the slot's own patch
            // An apply whose Budget is not proven takes the union of the live marks.
            a.RayBudgetApply(nullptr, rrraybudget::Lists{}, {});
            assert(s.ray_budget == 1 && s.ray_armed.w == 33);
            // A later apply that reads a Budget with no live mark keeps the lobe already counted
            // and its snapshot, and its proof does not replace the unproven one stored before it.
            auto other = RayLists();
            other.budget_resource = 0x7000;
            a.RayBudgetApply(nullptr, other, {});
            assert(s.ray_budget == 1 && s.ray_armed.w == 33);
            assert(s.ray_lists.why == Why::ListsUnproven);
            // A resolve without a proven Budget changes no mark; one with it clears that mark.
            rrvrrt::RayResolve native;
            native.why = Why::Shape;
            bool dispatched = false;
            assert(!a.RayBudgetResolve(nullptr, native, 3, 1, 1, dispatched));
            assert(a.RayConsumed(0x5000, 64).mask == 1);
            s.ray_resolved = false;
            native.budget_resource = 0x5000;
            native.budget_offset = 64;
            assert(!a.RayBudgetResolve(nullptr, native, 3, 1, 1, dispatched) && !dispatched);
            assert(!a.RayConsumed(0x5000, 64).mask);
        }
        // A raised recording that retires unreported, or reported without reader stats, is
        // unverified exactly once (the 8th in a row makes Decide wait); unraised ones never are.
        {
            Controller r;
            auto *fence = new CpuFence;
            fence->completed = 1;
            r.fence.Attach(fence);
            auto &s = r.slots[0];
            for (unsigned i = 0; i < 8; ++i) {
                assert(!RayWaiting(r));
                s.busy = true;
                (i & 1 ? s.ray_budget : s.ray_patched) = 1;
                if (i & 1) {
                    s.recorded = s.sealed = true; // reported: the reader did not run
                    s.fence_value = 1;
                }
                r.PollLocked();
                assert(!s.busy && !s.ray_patched && !s.ray_budget); // retired
                s.busy = true;
                r.PollLocked(); // an unraised retirement is not counted
            }
            assert(RayWaiting(r) && r.ray_gate.Stopped() == Why::None);
        }
        // A reported raised frame is judged by Complete, and its retirement adds nothing.
        {
            Controller r;
            auto &s = r.slots[0];
            for (unsigned i = 0; i < 7; ++i)
                r.ray_gate.Unverified();
            s.ray_budget = 1;
            s.ray_armed = RayArmed();
            s.ray_lists = RayLists();
            s.pre_submitted = true; // the reader's rates are this recording's
            r.RayJudge(s, RayRates(), {}, Controller::RayLogState::None);
            assert(s.ray_done && r.ray_gate.Stopped() == Why::None);
            r.RayUnverified(s);
            for (unsigned i = 0; i < 7; ++i)
                r.ray_gate.Unverified();
            assert(!RayWaiting(r)); // the measured frame ended the run: 7 since
            r.ray_gate.Unverified();
            assert(RayWaiting(r));
        }
        // A patched recording whose resolve half did not run as recorded (serial mismatch) is
        // unverified, not a PatchMismatch; with its serial the executed constants are checked.
        {
            auto judge = [](Controller &c, std::uint32_t serial) {
                auto &s = c.slots[0];
                s.pre_submitted = true;
                s.ray_patched = 1;
                s.ray_written = {0x3f000000u, 0};
                s.ray_patch_armed = RayArmed();
                s.ray_lists = RayLists();
                s.ray_serial = 7;
                s.ray_logged = rrraybudget::LogResolveTarget | rrraybudget::LogExecuted |
                               rrraybudget::LogResolveStats;
                Log log{};
                log[rrraybudget::LogWordExecuted] =
                    0xBADu; // the executed register 2 differs from the written bits
                log[rrraycopy::ResolveSerial.at / 4] = serial;
                c.RayJudge(s, RayRates(), log, Controller::RayLogState::Read);
                return s.ray_done;
            };
            Controller skipped;
            for (unsigned i = 0; i < 7; ++i)
                skipped.ray_gate.Unverified();
            assert(judge(skipped, 6) && skipped.ray_gate.Stopped() == Why::None);
            assert(RayWaiting(skipped));
            Controller checked;
            assert(judge(checked, 7) && checked.ray_gate.Stopped() == Why::PatchMismatch);
            Log log{};
            log[rrraycopy::ResolveSerial.at / 4] = 7;
            log[rrraycopy::ApplySerial.at / 4] = 8;
            assert(Controller::RaySerialBits(0xFF, log, 7) == resolve_bits);
            log[rrraycopy::ResolveSerial.at / 4] = 0;
            log[rrraycopy::ApplySerial.at / 4] = 7;
            assert(Controller::RaySerialBits(0xFF, log, 7) == apply_bits);
            assert(Controller::RaySerialBits(0xFF, Log{}, 0) == 0);
        }
        // A failed log readback stops a raised frame; an unraised one only loses its log bits.
        {
            Controller raised;
            auto &s = raised.slots[0];
            s.ray_budget = 1;
            s.ray_armed = RayArmed();
            s.ray_lists = RayLists();
            s.ray_logged = rrraybudget::LogApplyTarget;
            raised.RayJudge(s, RayRates(), {}, Controller::RayLogState::Failed);
            assert(s.ray_done && raised.ray_gate.Stopped() == Why::Readback);
            Controller unraised;
            auto &u = unraised.slots[0];
            u.ray_logged = rrraybudget::LogApplyTarget;
            unraised.RayJudge(u, RayRates(), {}, Controller::RayLogState::Failed);
            assert(u.ray_done && unraised.ray_gate.Stopped() == Why::None && !unraised.failed);
        }
        // RayPatched records the patch: its lobes, the bits it wrote, the executed copy, its
        // arming snapshot, and its Budget's mark from the resolve count it was patched at.
        {
            Controller p;
            auto &s = RayLive(p);
            rrraybudget::Plan plan;
            plan.mask = 2;
            plan.bits = {0x3f800000u, 0x40000000u};
            plan.armed = RayArmed();
            plan.armed.w = 55;
            plan.armed.margin = {3, 4};
            rrvrrt::RayResolve in;
            in.budget_resource = 0x5000;
            in.budget_offset = 64;
            p.ray_resolves = 3; // the patched resolve was the third one seen
            p.RayPatched(s, plan, in, 100, rrraybudget::Times2);
            assert(s.ray_patched == 2 && s.ray_written[0] == plan.bits[0] &&
                   s.ray_written[1] == plan.bits[1]);
            assert(s.ray_logged == rrraybudget::LogExecuted);
            assert(RaySame(s.ray_patch_armed, plan.armed));
            auto got = p.RayConsumed(0x5000, 64);
            assert(got.mask == 2 && RaySame(got.armed, plan.armed));
            assert(!p.RayConsumed(0x5000, 128).mask && !p.RayConsumed(0x6000, 64).mask);
            p.ray_resolves = 10; // 7 further resolves
            assert(p.RayConsumed(0x5000, 64).mask == 2);
            p.ray_resolves = 11;
            assert(!p.RayConsumed(0x5000, 64).mask);
            assert(p.ray_gate.Stopped() == Why::None && !p.failed);
        }
        // Marks age by resolves, not recordings (A4). Recordings that only apply leave the Budget
        // as the governor wrote it; every resolve the hook sees counts, whatever its placement and
        // with RR disabled too: PreserveRayBudget calls RayBudgetResolveSeen before its Enabled
        // check, and RayBudgetResolve only while RR is enabled.
        {
            Controller c;
            auto &s = RayLive(c);
            auto resolve = [&c](ID3D12GraphicsCommandList *cl, const rrvrrt::RayResolve &in) {
                c.RayBudgetResolveSeen();
                bool dispatched = false;
                const bool patched = c.Enabled() && c.RayBudgetResolve(cl, in, 3, 1, 1, dispatched);
                assert(!patched && !dispatched);
            };
            rrraybudget::Plan plan;
            plan.mask = 1;
            plan.armed = RayArmed();
            plan.armed.w = 66;
            rrvrrt::RayResolve proven;
            proven.budget_resource = 0x5000;
            proven.budget_offset = 64;
            c.RayBudgetResolveSeen(); // the patched resolve itself
            c.RayPatched(s, plan, proven, 1, rrraybudget::Times2);
            // 10 recordings that only apply: each one consumed the raised Budget.
            for (unsigned i = 0; i < 10; ++i) {
                auto &r = RayNext(c);
                c.RayBudgetApply(nullptr, RayLists(), {});
                assert(r.ray_budget == 1 && r.ray_armed.w == 66);
            }
            // 7 further resolves: refused in a recording, repeated, after SR, outside any, and 3
            // with RR disabled. None proves a Budget, so none clears the mark.
            rrvrrt::RayResolve shape;
            shape.why = Why::Shape;
            auto &r = RayNext(c);
            resolve(nullptr, shape);
            assert(r.ray_resolved);
            resolve(nullptr, shape);
            r.recorded = true;
            resolve(nullptr, shape);
            resolve(reinterpret_cast<ID3D12GraphicsCommandList *>(0x40), shape);
            c.SetEnabled(false);
            for (unsigned i = 0; i < 3; ++i)
                resolve(nullptr, shape);
            c.SetEnabled(true);
            auto &live = RayNext(c);
            c.RayBudgetApply(nullptr, RayLists(), {});
            assert(live.ray_budget == 1 && live.ray_armed.w == 66);
            // The 8th, with RR disabled: after F8 on again, the next apply is not raised.
            c.SetEnabled(false);
            resolve(nullptr, shape);
            c.SetEnabled(true);
            auto &expired = RayNext(c);
            c.RayBudgetApply(nullptr, RayLists(), {});
            assert(expired.ray_applied && !expired.ray_budget && !c.RayConsumed(0, 0).mask);
        }
        // A recording keeps its worst list proof (A3): an unproven apply's proof sticks, and two
        // proven applies that differ make it ListsChanged. Judged as raised frames, none of these
        // is verified: each one stops the test with ListsAfterArming.
        {
            auto judged = [](const rrraybudget::Lists &first, const rrraybudget::Lists &second,
                             Why stored) {
                Controller c;
                auto &s = RayLive(c);
                rrraybudget::Plan plan;
                plan.mask = 1;
                plan.armed = RayArmed();
                c.RaySetMark(0x5000, 64, plan, 0);
                c.RayBudgetApply(nullptr, first, {});
                c.RayBudgetApply(nullptr, second, {});
                assert(s.ray_budget == 1 && s.ray_lists.why == stored);
                s.pre_submitted = true;
                c.RayJudge(s, RayRates(), {}, Controller::RayLogState::None);
                assert(s.ray_done && !c.failed);
                return c.ray_gate.Stopped();
            };
            const rrraybudget::Lists unproven{};
            auto counter = RayLists();
            counter.v[3].bytes = 8; // a larger counter: either proof alone passes the arming check
            assert(judged(unproven, RayLists(), Why::ListsUnproven) == Why::ListsAfterArming);
            assert(judged(RayLists(), unproven, Why::ListsUnproven) == Why::ListsAfterArming);
            assert(judged(counter, RayLists(), Why::ListsChanged) == Why::ListsAfterArming);
            assert(judged(RayLists(), counter, Why::ListsChanged) == Why::ListsAfterArming);
            // Every field the proof compares, larger than the control so that either proof alone
            // passes the arming check, in both orders: the extent, the grid, both lobes' entries
            // and the pixel list.
            for (unsigned field = 0; field < 7; ++field) {
                auto other = RayLists();
                if (field < 3)
                    other.v[field].bytes += 64 * rrraybudget::EntryBytes;
                else if (field == 3)
                    other.gx = 2;
                else if (field == 4)
                    other.gy = 2;
                else if (field == 5)
                    other.w = 9;
                else
                    other.h = 9;
                assert(judged(other, RayLists(), Why::ListsChanged) == Why::ListsAfterArming);
                assert(judged(RayLists(), other, Why::ListsChanged) == Why::ListsAfterArming);
            }
            // Control: two applies with the same proof are judged on it, and pass.
            assert(judged(RayLists(), RayLists(), Why::None) == Why::None);
        }
        // Every raised Budget the recording's applies consumed stays counted (ORed).
        {
            Controller o;
            auto &s = RayLive(o);
            rrraybudget::Plan lobe1, lobe2;
            lobe1.mask = 1;
            lobe1.armed = RayArmed();
            lobe2.mask = 2;
            lobe2.armed = RayArmed();
            o.RaySetMark(0x5000, 64, lobe1, 0);
            o.RaySetMark(0x6000, 0, lobe2, 0);
            o.RayBudgetApply(nullptr, RayLists(), {});
            assert(s.ray_budget == 1);
            auto second = RayLists();
            second.budget_resource = 0x6000;
            second.budget_offset = 0;
            o.RayBudgetApply(nullptr, second, {});
            assert(s.ray_budget == 3);
        }
        // An apply that OnDispatch reports while rrbypass Health has failed (no proof, no logging
        // copies) still consumes a live mark: the raised slot, reported without reader stats, is
        // counted unverified exactly once.
        {
            Controller h;
            auto *fence = new CpuFence;
            fence->completed = 1;
            h.fence.Attach(fence);
            auto &s = RayLive(h);
            rrraybudget::Plan plan;
            plan.mask = 1;
            plan.armed = RayArmed();
            h.RaySetMark(0x5000, 64, plan, 0);
            h.RayBudgetApply(nullptr, rrraybudget::Lists{}, rrvrrt::RayApply{});
            assert(s.ray_applied && s.ray_budget == 1 && s.ray_lists.why == Why::ListsUnproven);
            for (unsigned i = 0; i < 6; ++i)
                h.ray_gate.Unverified();
            s.pre_live = false;
            s.busy = s.recorded = s.sealed = true;
            s.fence_value = 1;
            h.PollLocked(); // reported without reader stats, then retired
            assert(!s.busy && !s.ray_budget && !RayWaiting(h)); // 7: counted once
            h.ray_gate.Unverified();
            assert(RayWaiting(h));
        }
        // Rates that are not the recording's own (its prepass list was not submitted, or its apply
        // half did not run as recorded) measure nothing: a raised frame is unverified, not judged.
        // The slot is raised only through its Budget; its register 253 (log word 28) is 1.
        {
            auto raised = [](Controller &c, bool submitted, std::uint32_t apply_serial,
                             std::uint32_t scene253) {
                auto &s = c.slots[0];
                for (unsigned i = 0; i < 7; ++i)
                    c.ray_gate.Unverified();
                s.ray_budget = 1;
                s.ray_armed = RayArmed();
                s.ray_lists = RayLists();
                s.ray_serial = 7;
                s.ray_logged = rrraybudget::LogApplyScene253;
                s.pre_submitted = submitted;
                Log log{};
                log[rrraycopy::ApplySerial.at / 4] = apply_serial;
                log[rrraybudget::LogWordScene253] = scene253;
                c.RayJudge(s, RayRates(), log, Controller::RayLogState::Read);
                assert(s.ray_done && !c.failed);
            };
            // A stale apply serial: the stale register 253 is not judged (no ListsAfterArming),
            // and the frame is the 8th unverified one in a row.
            Controller stale, unsubmitted, own, remap;
            raised(stale, true, 6, 1);
            assert(stale.ray_gate.Stopped() == Why::None && RayWaiting(stale));
            raised(unsubmitted, false, 7, 1);
            assert(unsubmitted.ray_gate.Stopped() == Why::None && RayWaiting(unsubmitted));
            // Controls: its own log is judged, and passes or stops on its register 253.
            raised(own, true, 7, 2048);
            assert(own.ray_gate.Stopped() == Why::None && !RayWaiting(own));
            raised(remap, true, 7, 1);
            assert(remap.ray_gate.Stopped() == Why::ListsAfterArming);
        }
        // An unraised frame in either case skips Complete and Summary::Completed: here the
        // reader's uncounted frame is not counted.
        {
            auto uncounted = [](bool submitted, bool stale) {
                Controller u;
                auto &s = RayLive(u);
                std::string a, b;
                assert(!u.ray_summary.Report(1, u.ray_gate, u.StatusLocked(1), a, b));
                rrvrrt::RayResolve shape;
                shape.why = Why::Shape;
                bool dispatched = false;
                assert(!u.RayBudgetResolve(nullptr, shape, 3, 1, 1, dispatched) && !dispatched);
                u.RayBudgetApply(nullptr, RayLists(), {});
                assert(!s.ray_budget && !s.ray_patched && s.ray_serial);
                s.pre_submitted = submitted;
                s.ray_logged = rrraybudget::LogApplyTarget;
                Log log{};
                log[rrraycopy::ApplySerial.at / 4] = stale ? s.ray_serial + 1 : s.ray_serial;
                u.RayJudge(s, rrdensity::Frame{}, log, Controller::RayLogState::Read);
                assert(s.ray_done && u.ray_gate.Stopped() == Why::None && !u.failed);
                assert(u.ray_summary.Report(20001, u.ray_gate, u.StatusLocked(20001), a, b));
                assert(a.find(" uncounted=") != std::string::npos);
                return a.find(" uncounted=1 ") != std::string::npos;
            };
            assert(uncounted(true, false)); // control: its own frame is counted
            assert(!uncounted(false, false));
            assert(!uncounted(true, true));
        }
        // The serial filter at RayJudge: a stale resolve half's LogResolveStats (x = 0, below the
        // reader's raw rays) is dropped, and the frame is judged on its apply half and passes.
        {
            auto stats = [](Controller &c, std::uint32_t resolve_serial) {
                auto &s = c.slots[0];
                for (unsigned i = 0; i < 7; ++i)
                    c.ray_gate.Unverified();
                s.ray_budget = 1;
                s.ray_armed = RayArmed();
                s.ray_lists = RayLists();
                s.ray_serial = 7;
                s.pre_submitted = true;
                s.ray_logged = rrraybudget::LogResolveStats | rrraybudget::LogApplyTarget;
                Log log{};
                log[rrraycopy::ResolveSerial.at / 4] = resolve_serial;
                log[rrraycopy::ApplySerial.at / 4] = 7;
                auto rates = RayRates();
                rates[rrdensity::RawRays] = 64;
                c.RayJudge(s, rates, log, Controller::RayLogState::Read);
                assert(s.ray_done && !c.failed);
            };
            Controller stale, own;
            stats(stale, 6);
            assert(stale.ray_gate.Stopped() == Why::None && !RayWaiting(stale)); // verified
            stats(own, 7); // control: with its serial, x = 0 does not cover 64 raw rays
            assert(own.ray_gate.Stopped() == Why::StatsMismatch);
        }
    }
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
        c.dropout_test.blocked = c.dropout_unavailable = c.dropout_covered = true;
        c.dropout_source = true;
        c.dropout_unavailable_changes = 5;
        c.dropout_report = 1234;
        c.dropout_frames = 7;
        c.applied_color_scale = 4.0f;
        c.applied_contrast_knee = 0.25f;
        c.QueueReconfigure();
        assert(c.Reconfigure());
        assert(c.budget.limit == Budget::Minimum + 100 && !c.test_reserve);
        assert(!c.color_test.blocked && !c.sky_edge_test.active && !c.wide_output.blocked &&
               !c.hit_scratch.active && c.applied_color_scale == 1.0f &&
               c.applied_contrast_knee == 0.0f);
        assert(!c.dropout_test.blocked && !c.dropout_unavailable && !c.dropout_covered &&
               !c.dropout_source && !c.dropout_unavailable_changes && !c.dropout_pipeline.state &&
               !c.dropout_report && !c.dropout_frames);
        // The refraction dropout test is off by default and reports why it is not active.
        assert(c.DropoutStatus() == rrdropout::Report::Off);
        rrdropout::enabled = true;
        assert(c.DropoutStatus() == rrdropout::Report::Waiting); // RR not running
        c.color_test.active = true; // stale: RR is not running, so not the reason
        assert(c.DropoutStatus() == rrdropout::Report::Waiting);
        c.color_test.active = false;
        c.dropout_unavailable = true;
        c.dropout_unavailable_changes = rrdropout::changes;
        assert(c.DropoutStatus() == rrdropout::Report::Unavailable);
        ++rrdropout::changes; // a setting change retries
        assert(c.DropoutStatus() == rrdropout::Report::Waiting);
        c.dropout_unavailable = false;
        c.dropout_test.blocked = true;
        c.dropout_test.changes = rrdropout::changes;
        assert(c.DropoutStatus() == rrdropout::Report::Blocked);
        // Wanted but off for the color brightness test: no device is touched.
        Controller::Slot slot;
        c.dropout_report = 99;
        assert(!c.DropoutUse(slot, rrdropout::Read(), true, DXGI_FORMAT_R11G11B10_FLOAT) &&
               !c.dropout_test.active && !c.dropout_report); // the 10 s window restarts
        // A slot without this frame's glass capture gives the pass no coverage.
        const auto coverage = c.DropoutCoverageOf(slot, rreval::Options{});
        assert(!coverage.target && !coverage.admitted && !coverage.source && !coverage.ready);
        c.dropout_test = {};
        rrdropout::enabled = false;
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
    // water count (16), the hit distance statistics (20..63), the ray density (64..255), the
    // non-finite counts (256, 260) and the water prepass flags (264..283), inside the 512-byte
    // readback.
    static void StatsReaders() {
        static_assert(GpuFrame::StatsBytes == 512 && GpuFrame::FiniteOffset == 256);
        UINT words[GpuFrame::StatsBytes / 4]; // the whole readback
        for (UINT i = 0; i < GpuFrame::StatsBytes / 4; ++i)
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
        assert(rates.front() == 116 && rates.back() == 163);
        assert(readback->read.Begin == 64 && readback->read.End == 256);
        assert((g.ReadFinite() == std::array<UINT, 2>{164, 165}));
        assert(readback->read.Begin == 256 && readback->read.End == 264);
        const auto water = g.ReadWaterCounts();
        assert(water.water == 104);
        assert((water.prepass == std::array<std::uint32_t, rrwater::PrepassCount>{166, 167, 168,
                                                                                  169, 170}));
        assert(readback->read.Begin == 264 && readback->read.End == 284);
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
        {
            // The default responsivity mask classifies water on preset F only.
            Controller f;
            assert(f.WaterWanted() && f.WaterStatus().wanted);
            assert(f.RequestPreset(5) && !f.WaterWanted() && !f.WaterStatus().wanted);
        }
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
        rrwater::Settings dry_settings;
        water.SetWater(dry_settings); // the default responsivity mask still wants water on F
        assert(water.water_retry_after == 3000);
        dry_settings.responsivity = false;
        water.SetWater(dry_settings);
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
        RayBudget();
        RayPlacement();
        RayRestart();
        RayOldMark();
        RayReserve();
        RayStatus();
        RayChecks();
    }
};
} // namespace rrlive
int main() {
    rrlive::ControllerCpuTest::Run();
}
