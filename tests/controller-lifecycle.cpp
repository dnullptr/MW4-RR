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
// CPU-only resource double: only its reference count is observed.
struct CpuResource final : ID3D12Resource {
    ULONG refs = 1;
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
    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *, void **) override {
        return E_NOTIMPL;
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
            assert(idle.ray_why == Why::Outside && !idle.failed);
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
            assert(s.ray_resolved && !s.ray_logged && live.ray_why == Why::Shape);
            assert(!live.RayBudgetResolve(nullptr, shape, 3, 1, 1, dispatched) && !dispatched);
            assert(live.ray_why == Why::Repeated && !live.failed);
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
            assert(two.ray_why == Why::Ambiguous && !two.slots[0].ray_resolved);
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
                log[4] = 0xBADu; // the executed register 2 differs from the written bits
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
                log[28] = scene253;
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
        // Reconfigure gives back only the reserve the test textures added.
        c.budget.limit = Budget::Minimum + 300;
        c.test_reserve = 200;
        c.color_test.blocked = c.sky_edge_test.active = c.wide_output.blocked = true;
        c.hit_scratch.active = true;
        c.applied_color_scale = 4.0f;
        c.QueueReconfigure();
        assert(c.Reconfigure());
        assert(c.budget.limit == Budget::Minimum + 100 && !c.test_reserve);
        assert(!c.color_test.blocked && !c.sky_edge_test.active && !c.wide_output.blocked &&
               !c.hit_scratch.active && c.applied_color_scale == 1.0f);
        c.budget.limit = Budget::Minimum + 50;
        c.test_reserve = 200;
        c.QueueReconfigure();
        assert(c.Reconfigure() && c.budget.limit == Budget::Minimum);
    }
    static void WeaponDepth() {
        // The weapon depth test restarts RR only when its setting differs from the depth
        // type RR was created with. A test that could not run stays off through Reconfigure
        // until its setting next changes, so it restarts RR once, never over and over.
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
        c.logger = CountLog;
        c.HoldWeaponDepth(Controller::WeaponHold::Refused, rrweapondepth::changes);
        assert(!c.WeaponDepthWanted() && c.NeedsReconfigure());
        assert(c.WeaponDepthState() == State::Refused && log_lines == 1);
        c.HoldWeaponDepth(Controller::WeaponHold::Refused, rrweapondepth::changes);
        assert(log_lines == 1); // Frames still in flight repeat the hold; logged once.
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
        // While the weapon test runs, its linear depth carries the sky guide test's floor.
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
    static void Run() {
        DevTests();
        WeaponDepth();
        HitDistanceTests();
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
        RayBudget();
    }
};
} // namespace rrlive
int main() {
    rrlive::ControllerCpuTest::Run();
}
