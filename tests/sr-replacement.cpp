#include "../src/rr_trace.h"
#include "ngx-recording-params.h"
#include <cassert>
#include <stdexcept>
static unsigned originals{}, replacements{}, afters{};
static bool handle_rr = true, throw_rr = false, expect_reset = false, throw_original = false;
static NVSDK_NGX_Result original_result = NVSDK_NGX_Result_Success;
static NVSDK_NGX_Result Original(const NVSDK_NGX_Parameter *p) {
    int reset = -1;
    assert(NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_Reset, &reset)));
    assert(reset == int(expect_reset));
    ++originals;
    if (throw_original)
        throw std::runtime_error("native failure");
    SetLastError(72);
    return original_result;
}
static NVSDK_NGX_Result NVSDK_CONV Eval(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                        const NVSDK_NGX_Parameter *p,
                                        PFN_NVSDK_NGX_ProgressCallback) {
    return Original(p);
}
static NVSDK_NGX_Result NVSDK_CONV EvalC(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                         const NVSDK_NGX_Parameter *p,
                                         PFN_NVSDK_NGX_ProgressCallback_C) {
    return Original(p);
}
static bool Replace(ID3D12GraphicsCommandList *, const NVSDK_NGX_Parameter *,
                    NVSDK_NGX_Result &result) {
    ++replacements;
    if (throw_rr)
        throw std::runtime_error("decline");
    result = NVSDK_NGX_Result_Success;
    return handle_rr;
}
struct ReadOnlyReset : RecordingParams {
    void Set(const char *, int) override {}
};
namespace rrtrace {
struct ProbeTestAccess {
    static void Run() {
        Probe probe;
        Probe::active = &probe;
        auto h = reinterpret_cast<NVSDK_NGX_Handle *>(0x2222);
        auto &s = probe.slots[0];
        s.eval = &Eval;
        s.eval_c = &EvalC;
        s.family = Kind::SR;
        s.handles.Created(UINT_PTR(h), 1, true);
        s.creation[UINT_PTR(h)] = {};
        s.creation[UINT_PTR(h)].valid = true;
        probe.replace_evaluate = &Replace;
        probe.after_evaluate = [](ID3D12GraphicsCommandList *, Kind, const NVSDK_NGX_Parameter *,
                                  NVSDK_NGX_Result) { ++afters; };
        RecordingParams p;
        p.Set(NVSDK_NGX_Parameter_Reset, 0);
        auto call = [&](bool c) {
            SetLastError(99);
            return c ? Probe::EvalCommon<0, true>(nullptr, h, &p, nullptr)
                     : Probe::EvalCommon<0, false>(nullptr, h, &p, nullptr);
        };
        assert(call(false) == NVSDK_NGX_Result_Success && originals == 0 && afters == 0 &&
               replacements == 1 && GetLastError() == 99);
        assert(call(true) == NVSDK_NGX_Result_Success && originals == 0 && afters == 0 &&
               replacements == 2);
        handle_rr = false;
        expect_reset = true;
        assert(call(false) == NVSDK_NGX_Result_Success && originals == 1 && afters == 1 &&
               GetLastError() == 72);
        int restored = -1;
        p.Get(NVSDK_NGX_Parameter_Reset, &restored);
        assert(restored == 0);
        expect_reset = false;
        assert(call(true) == NVSDK_NGX_Result_Success && originals == 2 && afters == 2);
        throw_rr = true;
        assert(call(false) == NVSDK_NGX_Result_Success && originals == 3);
        throw_rr = false;
        handle_rr = true;
        assert(call(false) == NVSDK_NGX_Result_Success && originals == 3);
        handle_rr = false;
        expect_reset = true;
        original_result = NVSDK_NGX_Result_FAIL_PlatformError;
        assert(call(false) == original_result);
        assert(call(true) == original_result); // failure must not clear reset debt
        original_result = NVSDK_NGX_Result_Success;
        assert(call(false) == original_result);
        expect_reset = false;
        s.family = Kind::RR;
        const auto count = replacements;
        assert(call(false) == original_result && replacements == count);
        s.family = Kind::Unknown;
        s.handles = Handles{};
        assert(call(true) == original_result && replacements == count);
        // A non-null callback keeps native SR semantics, including progress callbacks.
        s.family = Kind::SR;
        handle_rr = true;
        expect_reset = false;
        auto cb = +[](float, bool &) {};
        auto cbc = +[](float, bool *) {};
        const auto unchanged = replacements;
        Probe::EvalCommon<0, false>(nullptr, h, &p, cb);
        Probe::EvalCommon<0, true>(nullptr, h, &p, cbc);
        assert(replacements == unchanged);
        // A thrown native call must restore caller parameters and retain reset debt.
        call(false);
        handle_rr = false;
        expect_reset = true;
        throw_original = true;
        try {
            call(false);
            assert(false);
        } catch (const std::runtime_error &) {
        }
        p.Get(NVSDK_NGX_Parameter_Reset, &restored);
        assert(restored == 0);
        throw_original = false;
        call(false);
        expect_reset = false;
        call(false);
        RecordingParams unsigned_params;
        unsigned_params.Set(NVSDK_NGX_Parameter_Reset, 0u);
        {
            rrsr::ResetParameter reset(&unsigned_params);
            assert(reset.Ready() && reset.Force());
            unsigned v = 0;
            unsigned_params.Get(NVSDK_NGX_Parameter_Reset, &v);
            assert(v == 1);
        }
        unsigned v = 99;
        unsigned_params.Get(NVSDK_NGX_Parameter_Reset, &v);
        assert(v == 0);
        RecordingParams missing;
        rrsr::ResetParameter reset_missing(&missing);
        assert(!reset_missing.Ready() && !reset_missing.Force());
        ReadOnlyReset readonly_reset;
        readonly_reset.values[NVSDK_NGX_Parameter_Reset] = 0;
        rrsr::ResetParameter readonly_check(&readonly_reset);
        assert(!readonly_check.Ready());
        Probe::active = nullptr;
    }
};
} // namespace rrtrace
int main() {
    rrtrace::ProbeTestAccess::Run();
    puts("PASS: replacement skips native SR; declines forward once; native history resets and "
         "caller parameters restore.");
}
