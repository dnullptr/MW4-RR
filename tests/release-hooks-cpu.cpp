#include "../src/rr_trace.h"
#include <cstdlib>
#include <string>
#include "ngx-recording-params.h"
static void Check(bool ok, const char *message) {
    if (!ok) {
        printf("FAIL: %s\n", message);
        exit(1);
    }
}
static void Log(const char *text) {
    puts(text);
    SetLastError(12345);
}
static_assert(NVSDK_NGX_Feature_SuperSampling == 1 && NVSDK_NGX_Feature_RayReconstruction == 13);
static int creates{}, evaluates{}, evaluates_c{}, releases{}, callbacks{};
static int shutdowns{}, shutdowns1{};
static bool allow_shutdown = true;
static ID3D12Device *shutdown_device = reinterpret_cast<ID3D12Device *>(0x4567);
static ID3D12GraphicsCommandList *expected_cl =
    reinterpret_cast<ID3D12GraphicsCommandList *>(0x1234);
struct NVSDK_NGX_Handle {
    unsigned id{};
};
static NVSDK_NGX_Handle handle{};
static const NVSDK_NGX_Parameter *expected_create_params{};
static NVSDK_NGX_Result next_result = NVSDK_NGX_Result_Success;
__declspec(noinline) static NVSDK_NGX_Result NVSDK_CONV Shutdown() {
    Check(GetLastError() == 99, "shutdown incoming error preserved");
    ++shutdowns;
    SetLastError(75);
    return next_result;
}
__declspec(noinline) static NVSDK_NGX_Result NVSDK_CONV Shutdown1(ID3D12Device *device) {
    Check(device == shutdown_device && GetLastError() == 99, "shutdown1 argument/error preserved");
    ++shutdowns1;
    SetLastError(76);
    return next_result;
}
static void NVSDK_CONV Callback(float progress, bool &cancel) {
    Check(progress == 0.5f, "C++ callback progress");
    ++callbacks;
    cancel = false;
}
static void NVSDK_CONV CallbackC(float progress, bool *cancel) {
    Check(progress == 0.25f, "C callback progress");
    ++callbacks;
    *cancel = false;
}
__declspec(noinline) static NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList *cl,
                                                               NVSDK_NGX_Feature f,
                                                               const NVSDK_NGX_Parameter *p,
                                                               NVSDK_NGX_Handle **out) {
    Check(cl == expected_cl && p == expected_create_params &&
              (f == NVSDK_NGX_Feature_SuperSampling || f == NVSDK_NGX_Feature_RayReconstruction),
          "create arguments");
    ++creates;
    *out = &handle;
    SetLastError(71);
    return next_result;
}
__declspec(noinline) static NVSDK_NGX_Result NVSDK_CONV
Evaluate(ID3D12GraphicsCommandList *cl, const NVSDK_NGX_Handle *h, const NVSDK_NGX_Parameter *p,
         PFN_NVSDK_NGX_ProgressCallback cb) {
    Check(cl == expected_cl && h == &handle && !p && cb == &Callback, "evaluate arguments");
    Check(GetLastError() == 99, "incoming error preserved");
    ++evaluates;
    bool cancel = true;
    cb(0.5f, cancel);
    Check(!cancel, "callback reference preserved");
    SetLastError(72);
    return next_result;
}
__declspec(noinline) static NVSDK_NGX_Result NVSDK_CONV
EvaluateC(ID3D12GraphicsCommandList *cl, const NVSDK_NGX_Handle *h, const NVSDK_NGX_Parameter *p,
          PFN_NVSDK_NGX_ProgressCallback_C cb) {
    Check(cl == expected_cl && h == &handle && !p && cb == &CallbackC, "evaluate_C arguments");
    ++evaluates_c;
    bool cancel = true;
    cb(0.25f, &cancel);
    Check(!cancel, "C callback pointer preserved");
    SetLastError(73);
    return next_result;
}
__declspec(noinline) static NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle *h) {
    Check(h == &handle, "release arguments");
    ++releases;
    SetLastError(74);
    return next_result;
}

class Params final : public NVSDK_NGX_Parameter {
  public:
#define UNUSED_SET(T)                                                                              \
    void Set(const char *, T) override {                                                           \
        Check(false, "observer must never set parameters");                                        \
    }
    UNUSED_SET(unsigned long long)
    UNUSED_SET(float)
    UNUSED_SET(double)
    UNUSED_SET(unsigned int) UNUSED_SET(int) UNUSED_SET(ID3D11Resource *)
        UNUSED_SET(ID3D12Resource *) UNUSED_SET(void *)
#undef UNUSED_SET
#define MISSING_GET(T)                                                                             \
    NVSDK_NGX_Result Get(const char *, T *) const override {                                       \
        return NVSDK_NGX_Result_FAIL_InvalidParameter;                                             \
    }
            MISSING_GET(unsigned long long) MISSING_GET(float) MISSING_GET(double) MISSING_GET(int)
                MISSING_GET(ID3D11Resource *)
#undef MISSING_GET
                    NVSDK_NGX_Result Get(const char *key, unsigned *out) const override {
        if (std::string(key) == "Width") {
            *out = 1920;
            return NVSDK_NGX_Result_Success;
        }
        *out = 123;
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char *key, ID3D12Resource **out) const override {
        if (std::string(key) == "Depth") {
            *out = reinterpret_cast<ID3D12Resource *>(0x2345);
            return NVSDK_NGX_Result_Success;
        }
        *out = reinterpret_cast<ID3D12Resource *>(0xdead);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char *key, void **out) const override {
        if (std::string(key) == "Color") {
            *out = reinterpret_cast<void *>(0x3456);
            return NVSDK_NGX_Result_Success;
        }
        *out = reinterpret_cast<void *>(0xdead);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    void Reset() override {
        Check(false, "observer must never reset parameters");
    }
};
struct TestFrame : sl::FrameToken {
    operator uint32_t() const override {
        return 47;
    }
};
namespace rrtrace {
struct ProbeTestAccess {
    static inline unsigned calls{};
    static inline sl::Result result = sl::Result::eOk;
    static inline const sl::BaseStructure **expected_inputs{};
    static void FrameScopeTest(Probe &probe) {
        TestFrame frame;
        sl::ViewportHandle view(9);
        const sl::BaseStructure *inputs[] = {&view};
        static unsigned depth = 0;
        static unsigned camera_calls = 0;
        probe.sl_eval = [](sl::Feature feature, const sl::FrameToken &frame,
                           const sl::BaseStructure **inputs, uint32_t count,
                           sl::CommandBuffer *cl) {
            const auto scope = Probe::CurrentCall();
            Check(scope.frame == 47 && scope.viewport == 9 && scope.command == cl &&
                      scope.feature == feature,
                  "SL frame scope matches actual call");
            if (depth++ == 0) {
                Probe::SlEval(sl::kFeatureDLSS_RR, frame, inputs, count, cl);
                Check(Probe::CurrentCall().feature == sl::kFeatureDLSS, "nested SL scope restored");
            }
            --depth;
            SetLastError(86);
            return sl::Result::eOk;
        };
        Check(!Probe::CurrentCall().valid, "no invented scope outside SL");
        Check(Probe::SlEval(sl::kFeatureDLSS, frame, inputs, 1, expected_cl) == sl::Result::eOk &&
                  GetLastError() == 86,
              "scoped SL return preserved");
        Check(!Probe::CurrentCall().valid, "scope removed after forwarding");
        Check(ReadViewport(reinterpret_cast<const sl::BaseStructure *>(1)) == UINT_MAX,
              "invalid viewport stays unknown");
        view.structVersion = 99;
        Check(ReadViewport(&view) == UINT_MAX, "future viewport stays unknown");
        sl::Constants c;
        Check(!ReadCamera(c).projection_valid, "default camera invalid sentinel rejected");
        for (int i = 0; i < 16; ++i) {
            reinterpret_cast<float *>(&c.cameraViewToClip)[i] = i % 5 == 0 ? 1.f : 0.f;
            reinterpret_cast<float *>(&c.clipToCameraView)[i] = i % 5 == 0 ? 1.f : 0.f;
        }
        c.cameraPos = {0, 0, 0};
        c.cameraRight = {1, 0, 0};
        c.cameraUp = {0, 1, 0};
        c.cameraFwd = {0, 0, 1};
        auto camera = ReadCamera(c);
        Check(camera.readable && camera.projection_valid && camera.basis_valid,
              "valid camera copied and checked");
        c.cameraFwd = {1, 0, 0};
        Check(!ReadCamera(c).basis_valid, "degenerate camera basis rejected");
        c.structVersion = 99;
        Check(!ReadCamera(c).readable, "unknown camera layout rejected");
        Check(!ReadCamera(*reinterpret_cast<sl::Constants *>(1)).readable,
              "unreadable camera ignored");
        probe.after_constants = [](uint32_t frame, uint32_t viewport, const sl::Constants &) {
            Check(frame == 47 && viewport == 9, "camera callback frame labels");
            ++camera_calls;
            SetLastError(999);
        };
        probe.constants = [](const sl::Constants &, const sl::FrameToken &,
                             const sl::ViewportHandle &) {
            SetLastError(87);
            return result;
        };
        view.structVersion = 1;
        result = sl::Result::eErrorInvalidParameter;
        Probe::Constants(c, frame, view);
        Check(camera_calls == 0 && GetLastError() == 87, "failed constants not consumed");
        result = sl::Result::eOk;
        Probe::Constants(c, frame, view);
        Check(camera_calls == 1 && GetLastError() == 87,
              "successful constants callback preserves original error");
        view.structVersion = 99;
        probe.after_constants = [](uint32_t, uint32_t viewport, const sl::Constants &) {
            Check(viewport == UINT_MAX, "successful future viewport never labels camera as known");
        };
        Probe::Constants(c, frame, view);
        frame.structVersion = 99;
        Check(FrameIndex(frame) == UINT_MAX, "future frame token stays unknown");
        probe.after_constants = nullptr;
    }
    static void Run(Probe &probe) {
        TestFrame frame;
        sl::ViewportHandle view(9);
        sl::DLSSDOptions rr_options;
        rr_options.mode = sl::DLSSMode::eDLAA;
        rr_options.outputWidth = 2560;
        rr_options.outputHeight = 1440;
        probe.sl_eval = [](sl::Feature f, const sl::FrameToken &token,
                           const sl::BaseStructure **inputs, uint32_t count,
                           sl::CommandBuffer *cl) {
            Check(f == sl::kFeatureDLSS_RR && uint32_t(token) == 47 && inputs == expected_inputs &&
                      count == 1 && cl == expected_cl,
                  "Streamline eval arguments");
            ++calls;
            SetLastError(81);
            return result;
        };
        // An invalid optional input array must not prevent forwarding to an API
        // that rejects it; the tracer itself may not dereference it unchecked.
        expected_inputs = reinterpret_cast<const sl::BaseStructure **>(1);
        for (unsigned i = 0; i < 4; ++i)
            Check(Probe::SlEval(sl::kFeatureDLSS_RR, frame, expected_inputs, 1, expected_cl) ==
                          sl::Result::eOk &&
                      GetLastError() == 81,
                  "SL success passed unchanged");
        result = sl::Result::eErrorInvalidParameter;
        Check(Probe::SlEval(sl::kFeatureDLSS_RR, frame, expected_inputs, 1, expected_cl) ==
                      result &&
                  GetLastError() == 81,
              "unsampled SL failure passed unchanged");
        Check(calls == 5, "SL original forwarded exactly once for successes and failure");
        probe.rr_options = [](const sl::ViewportHandle &v, const sl::DLSSDOptions &o) {
            Check(uint32_t(v) == 9 && o.mode == sl::DLSSMode::eDLAA && o.outputWidth == 2560,
                  "RR options forwarded");
            ++calls;
            SetLastError(82);
            return result;
        };
        Check(Probe::RROptions(view, rr_options) == result && GetLastError() == 82 && calls == 6,
              "RR option failure preserved");
        rr_options.structVersion = 99;
        Check(!ReadOptions(rr_options).valid, "future options ignored");
        probe.sl_tags = [](const sl::FrameToken &, const sl::ViewportHandle &,
                           const sl::ResourceTag *tags, uint32_t n, sl::CommandBuffer *cl) {
            Check(tags == nullptr && n == 0 && cl == expected_cl, "tag arguments unchanged");
            ++calls;
            SetLastError(83);
            return result;
        };
        Check(Probe::Tags(frame, view, nullptr, 0, expected_cl) == result && GetLastError() == 83 &&
                  calls == 7,
              "tag error passed unchanged");
        Check(Probe::Tags(*reinterpret_cast<sl::FrameToken *>(1),
                          *reinterpret_cast<sl::ViewportHandle *>(1), nullptr, 0,
                          expected_cl) == result &&
                  GetLastError() == 83 && calls == 8,
              "early rejection with unreadable frame and viewport stays safe");
    }
};
} // namespace rrtrace
int main() {
    Params params;
    auto in = rrtrace::ReadInputs(&params);
    Check(in.readable && in.width == 1920 && in.depth == reinterpret_cast<void *>(0x2345) &&
              in.color == reinterpret_cast<void *>(0x3456),
          "typed and void resource getters");
    Check(!in.output && in.denoise == UINT_MAX,
          "failed getters do not invent input presence or modes");
    Check(!rrtrace::ReadInputs(reinterpret_cast<NVSDK_NGX_Parameter *>(1)).readable,
          "invalid parameter guarded");
    Check(!rrtrace::ReadTag(reinterpret_cast<sl::BaseStructure *>(1)).valid, "invalid tag guarded");
    sl::Resource resource;
    resource.native = reinterpret_cast<void *>(0x7777);
    sl::ResourceTag tag(&resource, sl::kBufferTypeNormals,
                        sl::ResourceLifecycle::eValidUntilPresent);
    Check(rrtrace::ReadTag(&tag).native == resource.native, "tag resource identity");
    tag.structVersion = 99;
    Check(!rrtrace::ReadTag(&tag).valid, "unknown tag layout ignored");
    tag.structVersion = 1;
    resource.structVersion = 99;
    Check(rrtrace::ReadTag(&tag).valid && !rrtrace::ReadTag(&tag).resource_valid,
          "unknown resource layout ignored");
    rrtrace::Probe probe;
    probe.logger = &Log;
    rrtrace::Probe::active = &probe;
    Check(MH_Initialize() == MH_OK, "MinHook initializes");
    Check(probe.AttachNgx("stub-core", rrtrace::Kind::Unknown, reinterpret_cast<void *>(&Create),
                          reinterpret_cast<void *>(&Evaluate), reinterpret_cast<void *>(&EvaluateC),
                          reinterpret_cast<void *>(&Release), reinterpret_cast<HMODULE>(0x12345000),
                          reinterpret_cast<void *>(&Shutdown),
                          reinterpret_cast<void *>(&Shutdown1)),
          "all hooks install");
    Check(probe.slots[0].create && probe.slots[0].eval && probe.slots[0].eval_c &&
              probe.slots[0].release,
          "complete coverage");
    probe.slots[0].module = reinterpret_cast<HMODULE>(0x12345000);
    static unsigned after_evals{};
    probe.after_evaluate = [](ID3D12GraphicsCommandList *cl, rrtrace::Kind,
                              const NVSDK_NGX_Parameter *p, NVSDK_NGX_Result result) {
        Check(cl == expected_cl && !p && result == next_result,
              "post-evaluate receives actual arguments/result");
        Check(rrtrace::Probe::CurrentRuntime() == reinterpret_cast<HMODULE>(0x12345000),
              "post-evaluate retains runtime scope");
        Check(evaluates + evaluates_c == int(after_evals + 1),
              "post-evaluate runs after original call exactly once");
        ++after_evals;
        SetLastError(444);
    };
    probe.before_evaluate = [](ID3D12GraphicsCommandList *, rrtrace::Kind,
                               const NVSDK_NGX_Parameter *) {
        Check(rrtrace::Probe::CurrentRuntime() == reinterpret_cast<HMODULE>(0x12345000),
              "callback sees exact selected runtime");
    };
    Check(rrtrace::Probe::CurrentRuntime() == nullptr, "runtime scope starts empty");
    NVSDK_NGX_Handle *h{};
    Check(Create(expected_cl, NVSDK_NGX_Feature_RayReconstruction, nullptr, &h) ==
                  NVSDK_NGX_Result_Success &&
              GetLastError() == 71 && h == &handle,
          "create return and error preserved");
    SetLastError(99);
    Check(Evaluate(expected_cl, h, nullptr, &Callback) == NVSDK_NGX_Result_Success &&
              GetLastError() == 72,
          "evaluate return and error preserved");
    next_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    Check(EvaluateC(expected_cl, h, nullptr, &CallbackC) == next_result && GetLastError() == 73,
          "failure and C ABI preserved");
    Check(Release(h) == next_result && GetLastError() == 74, "release failure preserved");
    Check(probe.slots[0].handles.Lookup(reinterpret_cast<uintptr_t>(h)) == rrtrace::Kind::RR,
          "failed release retains identity");
    next_result = NVSDK_NGX_Result_Success;
    Release(h);
    SetLastError(99);
    Evaluate(expected_cl, h, nullptr, &Callback);
    Check(probe.slots[0].handles.Lookup(reinterpret_cast<uintptr_t>(h)) == rrtrace::Kind::Unknown,
          "unobserved creation stays unknown");
    Create(expected_cl, NVSDK_NGX_Feature_SuperSampling, nullptr, &h);
    SetLastError(99);
    Evaluate(expected_cl, h, nullptr, &Callback);
    Check(probe.slots[0].handles.Lookup(reinterpret_cast<uintptr_t>(h)) == rrtrace::Kind::SR,
          "reused handle now SR");
    Check(creates == 2 && evaluates == 3 && evaluates_c == 1 && releases == 2 && callbacks == 4,
          "every original and callback called exactly once");
    Check(after_evals == 4, "post-evaluate receives both successful and failed C/C++ evaluations");
    probe.after_evaluate = nullptr;
    rrtrace::ProbeTestAccess::Run(probe);
    rrtrace::ProbeTestAccess::FrameScopeTest(probe);
    probe.before_evaluate = [](ID3D12GraphicsCommandList *, rrtrace::Kind,
                               const NVSDK_NGX_Parameter *) { throw 1; };
    const auto prior = evaluates;
    SetLastError(99);
    try {
        Check(Evaluate(expected_cl, h, nullptr, &Callback) == NVSDK_NGX_Result_Success &&
                  GetLastError() == 72,
              "throwing pre-evaluate observer preserves result");
    } catch (...) {
        Check(false, "observer exception must not prevent original NGX call");
    }
    Check(evaluates == prior + 1,
          "original NGX evaluation survives observer exception exactly once");
    const auto prior_c = evaluates_c;
    Check(EvaluateC(expected_cl, h, nullptr, &CallbackC) == NVSDK_NGX_Result_Success &&
              GetLastError() == 73 && evaluates_c == prior_c + 1,
          "throwing observer also preserves C ABI and callback");
    probe.after_evaluate = [](ID3D12GraphicsCommandList *, rrtrace::Kind,
                              const NVSDK_NGX_Parameter *, NVSDK_NGX_Result) { throw 1; };
    SetLastError(99);
    Check(Evaluate(expected_cl, h, nullptr, &Callback) == NVSDK_NGX_Result_Success &&
              GetLastError() == 72,
          "throwing post-evaluate preserves result/error");
    probe.after_evaluate = nullptr;
    RecordingParams creation;
    creation.Set("Width", 1920u);
    creation.Set("Height", 1088u);
    creation.Set("OutWidth", 3840u);
    creation.Set("OutHeight", 2160u);
    creation.Set("DLSS.Feature.Create.Flags", 11);
    creation.Set("PerfQualityValue", 0);
    expected_create_params = &creation;
    Create(expected_cl, NVSDK_NGX_Feature_SuperSampling, &creation, &h);
    expected_create_params = nullptr;
    probe.before_evaluate = [](ID3D12GraphicsCommandList *, rrtrace::Kind,
                               const NVSDK_NGX_Parameter *) {
        auto v = rrtrace::Probe::CurrentCreation();
        Check(v.valid && v.out_width == 3840 && v.out_height == 2160 && v.flags == 11 &&
                  v.quality == 0,
              "SR callback retains actual creation options despite missing evaluation keys");
    };
    SetLastError(99);
    Evaluate(expected_cl, h, nullptr, &Callback);
    Release(h);
    probe.before_evaluate = nullptr;
    Check(rrtrace::Probe::CurrentRuntime() == nullptr,
          "runtime scope restores after callback exceptions and both ABIs");
    Check(rrtrace::Probe::ShutdownCovered(reinterpret_cast<HMODULE>(0x12345000)),
          "both available shutdown exports covered");
    probe.before_shutdown = [](HMODULE m, ID3D12Device *device) {
        Check(m == reinterpret_cast<HMODULE>(0x12345000) &&
                  (device == nullptr || device == shutdown_device),
              "shutdown callback identity");
        SetLastError(888);
        return allow_shutdown;
    };
    static unsigned after_calls{};
    probe.after_shutdown = [](HMODULE, ID3D12Device *, bool success) {
        Check(success == NVSDK_NGX_SUCCEED(next_result), "shutdown result relayed");
        ++after_calls;
        SetLastError(888);
    };
    const auto epoch = rrtrace::Probe::RuntimeEpoch();
    allow_shutdown = false;
    SetLastError(99);
    Check(Shutdown() == NVSDK_NGX_Result_FAIL_PlatformError && GetLastError() == 99 &&
              shutdowns == 0 && after_calls == 0,
          "unfinished private work prevents original shutdown");
    Check(rrtrace::Probe::RuntimeEpoch() > epoch, "shutdown attempt invalidates old captures");
    allow_shutdown = true;
    SetLastError(99);
    Check(Shutdown() == NVSDK_NGX_Result_Success && GetLastError() == 75 && shutdowns == 1 &&
              after_calls == 1,
          "legacy shutdown success forwarded once");
    next_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    SetLastError(99);
    Check(Shutdown1(shutdown_device) == next_result && GetLastError() == 76 && shutdowns1 == 1 &&
              after_calls == 2,
          "device shutdown failure forwarded once");
    next_result = NVSDK_NGX_Result_Success;
    SetLastError(99);
    Check(Shutdown1(shutdown_device) == next_result && GetLastError() == 76 && shutdowns1 == 2 &&
              after_calls == 3,
          "device shutdown success forwarded once");
    probe.RemoveForTest();
    MH_Uninitialize();
    puts("PASS: real hook forwarding, both callback ABIs, exact results, input validation and "
         "SR/RR identity");
}
