#include "../src/rr_trace.h"
#include "ngx-recording-params.h"
#include "../src/rr_evaluation_params.h"
#include "../src/rr_ngx_log.h"
#include <cassert>
#include <cctype>
#include <climits>
#include <stdexcept>
#include <string>
#include <string_view>
static unsigned originals{}, replacements{}, afters{}, skip_seen{}, replace_note{};
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
    if (replace_note)
        rrtrace::Probe::NoteSkip(replace_note);
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
                                  NVSDK_NGX_Result) {
            ++afters;
            skip_seen = Probe::CurrentSkip(); // What AfterSR reads on the native route.
        };
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
        // Why native SR ran, for the capture log: set at the start of every evaluate, so a
        // stale reason never survives, and refined by the replacement.
        const auto skip = [&](const NVSDK_NGX_Parameter *q, bool c, auto callback) {
            Probe::NoteSkip(rrsr::SkipCount);
            skip_seen = rrsr::SkipCount;
            c ? Probe::EvalCommon<0, true>(nullptr, h, q, nullptr)
              : Probe::EvalCommon<0, false>(nullptr, h, q, callback);
            return skip_seen;
        };
        handle_rr = false;
        assert(skip(&p, false, nullptr) == rrsr::Declined);
        assert(skip(&p, true, nullptr) == rrsr::Declined);
        replace_note = rrsr::Unsafe; // TryReplaceSR names its own reason.
        assert(skip(&p, false, nullptr) == rrsr::Unsafe);
        replace_note = 0;
        assert(skip(&readonly_reset, false, nullptr) == rrsr::ResetFixed);
        throw_rr = true;
        assert(skip(&p, false, nullptr) == rrsr::Threw);
        throw_rr = false;
        assert(skip(&p, false, cb) == rrsr::Callback);
        s.creation[UINT_PTR(h)].valid = false;
        assert(skip(&p, false, nullptr) == rrsr::Creation);
        s.creation[UINT_PTR(h)].valid = true;
        probe.replace_evaluate = nullptr;
        assert(skip(&p, false, nullptr) == rrsr::NotCalled);
        Probe::active = nullptr;
    }
};
} // namespace rrtrace
// RR creation drops the game's AutoExposure bit, asks NGX to free released features, and keeps
// the game's own flags in the key that recreation detection compares.
static void CreationParameters() {
    rreval::Options o;
    o.width = 1280;
    o.height = 720;
    o.out_width = 2560;
    o.out_height = 1440;
    o.quality = 2;
    o.flags = 75;
    RecordingParams p;
    rreval::CreateParameters(p, o, 6);
    int flags = -1, free_mem = -1, quality = -1, denoise = -1, subrects = -1;
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)));
    assert(flags == 11);
    assert(NVSDK_NGX_SUCCEED(p.Get(RR_FREE_MEM_ON_RELEASE_KEY, &free_mem)));
    assert(free_mem == 1);
    unsigned width = 0, height = 0, out_width = 0, out_height = 0, roughness = 0, depth = 0;
    unsigned creation_mask = 0, visibility_mask = 0;
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_Width, &width)) && width == 1280);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_Height, &height)) && height == 720);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_OutWidth, &out_width)) && out_width == 2560);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_OutHeight, &out_height)) &&
           out_height == 1440);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_PerfQualityValue, &quality)) &&
           quality == 2);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, &subrects)) &&
           subrects == 0);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, &denoise)) &&
           denoise == int(NVSDK_NGX_DLSS_Denoise_Mode_DLUnified));
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &roughness)) &&
           roughness == unsigned(NVSDK_NGX_DLSS_Roughness_Mode_Packed));
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth)) &&
           depth == unsigned(NVSDK_NGX_DLSS_Depth_Type_HW));
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_CreationNodeMask, &creation_mask)) &&
           creation_mask == 1);
    assert(NVSDK_NGX_SUCCEED(p.Get(NVSDK_NGX_Parameter_VisibilityNodeMask, &visibility_mask)) &&
           visibility_mask == 1);
    assert(rrpreset::Requested(p, 6));
    // The stored key keeps the game's flags, so a flag change is still a recreation.
    auto changed = o;
    changed.flags = 11;
    assert(!rreval::SameCreation(o, changed));
    // Bits other than AutoExposure reach RR as the game set them.
    o.flags = 0x1ff;
    RecordingParams q;
    rreval::CreateParameters(q, o);
    assert(NVSDK_NGX_SUCCEED(q.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)));
    assert(flags == 0x1bf);
    // Linear depth (the weapon depth test) drops DepthInverted as well, AutoExposure still.
    o.flags = 75;
    RecordingParams l;
    rreval::CreateParameters(l, o, 6, true);
    assert(NVSDK_NGX_SUCCEED(l.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)));
    assert(flags == 3);
}
static RecordingParams *capability_map{};
static NVSDK_NGX_Result capability_result = NVSDK_NGX_Result_Success;
static bool capability_map_on_failure{}; // a failed query that still hands back a map
static unsigned capability_destroys{};
static NVSDK_NGX_Result NVSDK_CONV Capabilities(NVSDK_NGX_Parameter **out) {
    *out = NVSDK_NGX_SUCCEED(capability_result) || capability_map_on_failure ? capability_map
                                                                              : nullptr;
    return capability_result;
}
static NVSDK_NGX_Result NVSDK_CONV DestroyCapabilities(NVSDK_NGX_Parameter *p) {
    assert(p == capability_map);
    ++capability_destroys;
    return NVSDK_NGX_Result_Success;
}
// Result codes by numeric value (older SDKs lack some names), versions, and the lines RR logs.
static void NgxLog() {
    using rrngxlog::Code;
    assert(!strcmp(Code(0x1u).text, "0x00000001 Success"));
    assert(!strcmp(Code(0xBAD00000u).text, "0xBAD00000 Fail"));
    assert(!strcmp(Code(0xBAD00001u).text, "0xBAD00001 FAIL_FeatureNotSupported"));
    assert(!strcmp(Code(0xBAD00005u).text, "0xBAD00005 FAIL_InvalidParameter"));
    assert(!strcmp(Code(0xBAD0000Du).text, "0xBAD0000D FAIL_OutOfGPUMemory"));
    assert(!strcmp(Code(0xBAD00012u).text, "0xBAD00012 FAIL_NotImplemented"));
    assert(!strcmp(Code(0xBAD00013u).text, "0xBAD00013 unknown"));
    assert(!strcmp(Code(0u).text, "0x00000000 unknown"));
    assert(!strcmp(rrngxlog::ResultName(0xBAD0000Fu), "FAIL_UnableToWriteToAppDataPath"));
    // A success that hands back no object is named as such, not as "Success".
    int object{};
    assert(!strcmp(rrngxlog::Returned(0x1u, &object).text, "0x00000001 Success"));
    assert(!strcmp(rrngxlog::Returned(0x1u, nullptr).text, "null handle"));
    assert(!strcmp(rrngxlog::Returned(0xBAD00005u, nullptr).text,
                   "0xBAD00005 FAIL_InvalidParameter"));
    assert(!strcmp(rrngxlog::Returned(0xBAD00005u, &object).text,
                   "0xBAD00005 FAIL_InvalidParameter"));
    rrngxlog::Require(true, "unused", Code(0xBAD00000u));
    const auto thrown = [](const Code &outcome, const char *expected) {
        try {
            rrngxlog::Require(false, "live RR feature creation failed", outcome);
        } catch (const std::runtime_error &e) {
            return !strcmp(e.what(), expected);
        }
        return false;
    };
    assert(thrown(Code(0xBAD0000Du),
                  "live RR feature creation failed: 0xBAD0000D FAIL_OutOfGPUMemory"));
    assert(thrown(rrngxlog::Returned(0x1u, nullptr),
                  "live RR feature creation failed: null handle"));

    // VS_FIXEDFILEINFO at DWORD 3: signature, struct version, file version MS/LS (310.9.1.0).
    uint32_t info[20]{};
    info[3] = 0xFEEF04BDu;
    info[4] = 0x00010000u;
    info[5] = (310u << 16) | 9u;
    info[6] = 1u << 16;
    unsigned v[4]{};
    assert(rrngxlog::FixedFileVersion(info, sizeof(info), v));
    assert(v[0] == 310 && v[1] == 9 && v[2] == 1 && v[3] == 0);
    assert(!strcmp(rrngxlog::Version(v).text, "310.9.1.0"));
    assert(rrngxlog::FixedFileVersion(info, 12 + 52, v));
    assert(!rrngxlog::FixedFileVersion(info, 12 + 51, v)); // truncated struct
    unsigned char shifted[sizeof(info) + 2]{};
    memcpy(shifted + 2, info, sizeof(info));
    assert(!rrngxlog::FixedFileVersion(shifted, sizeof(shifted), v)); // misaligned signature
    info[3] = 0;
    assert(!rrngxlog::FixedFileVersion(info, sizeof(info), v)); // absent
    assert(!rrngxlog::FixedFileVersion(nullptr, 64, v));
    unsigned system[4]{};
    assert(rrngxlog::ModuleVersion(GetModuleHandleW(L"kernel32.dll"), system) && system[0]);
    assert(!rrngxlog::ModuleVersion(nullptr, system));

    // The probe's model rule, case-insensitively.
    assert(rrngxlog::ModelPath(L"D:\\Games\\Call of Duty\\NVNGX_DLSSD.DLL"));
    assert(rrngxlog::ModelPath(L"nvngx_dlssd.dll"));
    assert(rrngxlog::ModelPath(
        L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssd\\versions\\20316928\\files\\a.bin"));
    assert(!rrngxlog::ModelPath(L"D:\\Games\\Call of Duty\\nvngx_dlss.dll"));
    assert(!rrngxlog::ModelPath(L"D:\\Games\\Call of Duty\\nvngx_dlssd.dll.bak"));
    assert(!rrngxlog::ModelPath(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\1\\a.bin"));

    char text[256];
    const unsigned drivers[][4] = {{32, 0, 15, 6094}, {32, 0, 15, 6603}, {27, 21, 14, 5671}};
    const char *numbers[] = {"560.94", "566.03", "456.71"};
    for (unsigned i = 0; i < 3; ++i) {
        rrngxlog::NvidiaVersion(drivers[i], text, sizeof(text));
        assert(!strcmp(text, numbers[i]));
    }
    rrngxlog::Adapter adapter;
    rrngxlog::DescribeDriver(adapter, text, sizeof(text));
    assert(!strcmp(text, "?"));
    adapter.found = true;
    adapter.vendor = 0x10DE;
    snprintf(adapter.name, sizeof(adapter.name), "NVIDIA GeForce RTX 4090");
    rrngxlog::DescribeDriver(adapter, text, sizeof(text));
    assert(!strcmp(text, "? NVIDIA GeForce RTX 4090"));
    adapter.versioned = true;
    memcpy(adapter.version, drivers[0], sizeof(adapter.version));
    rrngxlog::DescribeDriver(adapter, text, sizeof(text));
    assert(!strcmp(text, "32.0.15.6094 (NVIDIA 560.94) NVIDIA GeForce RTX 4090"));
    adapter.vendor = 0x1002;
    snprintf(adapter.name, sizeof(adapter.name), "AMD Radeon RX 7900 XTX");
    rrngxlog::DescribeDriver(adapter, text, sizeof(text));
    assert(!strcmp(text, "32.0.15.6094 AMD Radeon RX 7900 XTX"));
    // No core module, device or model in this process.
    assert(rrngxlog::RuntimeLine(nullptr, nullptr) ==
           "RR runtime: NGX core ? ?; model not loaded; driver ?");
    // A loaded core module prints its path and its dotted file version.
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    unsigned kernel_version[4]{};
    assert(rrngxlog::ModuleVersion(kernel, kernel_version));
    const auto runtime = rrngxlog::RuntimeLine(kernel, nullptr);
    const std::string head = "RR runtime: NGX core ", tail = "; model not loaded; driver ?";
    assert(runtime.rfind(head, 0) == 0 && runtime.ends_with(tail));
    const auto module = runtime.substr(head.size(), runtime.size() - head.size() - tail.size());
    const auto space = module.rfind(' ');
    assert(space != std::string::npos);
    std::string path = module.substr(0, space);
    for (auto &ch : path)
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    assert(path.ends_with("\\kernel32.dll"));
    assert(module.substr(space + 1) == rrngxlog::Version(kernel_version).text);

    RecordingParams caps;
    caps.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 1);
    caps.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, 0);
    caps.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, 535u);
    caps.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, 98u);
    caps.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, 1);
    const auto c = rrngxlog::ReadCapability(caps);
    assert(c.available == 1 && c.needs_driver == 0 && c.major == 535u && c.minor == 98u &&
           c.init == 1);
    const char *full = "RR capability: available=1 needsUpdatedDriver=0 minDriver=535.98 "
                       "featureInit=0x00000001 Success";
    rrngxlog::DescribeCapability(c, text, sizeof(text));
    assert(!strcmp(text, full));
    RecordingParams partial;
    partial.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 0);
    partial.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, 535u);
    partial.Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, int(0xBAD0000Bu));
    rrngxlog::DescribeCapability(rrngxlog::ReadCapability(partial), text, sizeof(text));
    assert(!strcmp(text, "RR capability: available=0 needsUpdatedDriver=? minDriver=535.? "
                         "featureInit=0xBAD0000B FAIL_UnableToInitializeFeature"));
    rrngxlog::DescribeCapability({}, text, sizeof(text));
    assert(!strcmp(text, "RR capability: available=? needsUpdatedDriver=? minDriver=?.? "
                         "featureInit=?"));
    // The query: a missing export, the map destroyed after reading, a failed query's code.
    rreval::Api api;
    rrngxlog::CapabilityLine(api, text, sizeof(text));
    assert(!strcmp(text, "RR capability: export missing"));
    api.capabilities = &Capabilities;
    api.destroy = &DestroyCapabilities;
    capability_map = &caps;
    rrngxlog::CapabilityLine(api, text, sizeof(text));
    assert(!strcmp(text, full) && capability_destroys == 1);
    capability_result = NVSDK_NGX_Result(0xBAD0000Cu);
    rrngxlog::CapabilityLine(api, text, sizeof(text));
    assert(!strcmp(text, "RR capability query failed: 0xBAD0000C FAIL_OutOfDate") &&
           capability_destroys == 1);
    // A map handed back with a failure is still destroyed.
    capability_map_on_failure = true;
    rrngxlog::CapabilityLine(api, text, sizeof(text));
    assert(!strcmp(text, "RR capability query failed: 0xBAD0000C FAIL_OutOfDate") &&
           capability_destroys == 2);
    capability_result = NVSDK_NGX_Result_Success;
    capability_map = nullptr;
    rrngxlog::CapabilityLine(api, text, sizeof(text));
    assert(!strcmp(text, "RR capability query failed: null handle") && capability_destroys == 2);

    rreval::Options o;
    o.width = 2224;
    o.height = 1248;
    o.out_width = 3840;
    o.out_height = 2160;
    o.quality = 1;
    o.flags = 75;
    rrngxlog::DescribeCreation(o, 6, false, text, sizeof(text));
    assert(!strcmp(text, "RR created: preset F, quality 1 (Balanced), flags 75 -> 11, "
                         "render 2224x1248, output 3840x2160"));
    // Linear depth (the weapon depth test): the flags RR got lose DepthInverted too.
    rrngxlog::DescribeCreation(o, 6, true, text, sizeof(text));
    assert(!strcmp(text, "RR created: preset F, quality 1 (Balanced), flags 75 -> 3, "
                         "render 2224x1248, output 3840x2160"));
    o.flags = 0;
    o.quality = 5;
    rrngxlog::DescribeCreation(o, 0, false, text, sizeof(text));
    assert(!strcmp(text, "RR created: preset Default, quality 5 (Native / DLAA), flags 0 -> 0, "
                         "render 2224x1248, output 3840x2160"));
    o.quality = 6;
    rrngxlog::DescribeCreation(o, 1, false, text, sizeof(text));
    assert(!strncmp(text, "RR created: preset unknown, quality 6 (unknown),",
                    strlen("RR created: preset unknown, quality 6 (unknown),")));
    // The widest line still fits the 200-byte buffer AfterSR formats it into.
    o = {};
    o.quality = INT_MIN;
    o.flags = INT_MIN;
    char creation[200];
    rrngxlog::DescribeCreation(o, 1, false, creation, sizeof(creation));
    assert(std::string_view(creation).ends_with(" output 4294967295x4294967295"));
}
int main() {
    rrtrace::ProbeTestAccess::Run();
    CreationParameters();
    NgxLog();
    puts("PASS: replacement skips native SR; declines forward once; native history resets and "
         "caller parameters restore.");
    puts("PASS: RR creation drops AutoExposure, frees released features and keeps the game's key.");
    puts("PASS: NGX result codes, module and driver versions, capability, creation and runtime "
         "lines.");
}
