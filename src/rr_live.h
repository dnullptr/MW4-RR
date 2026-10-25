#pragma once
#include "rr_live_gpu.h"
#include "rr_live_policy.h"
#include "rr_guide_health.h"
#include "rr_ray_density.h"
#include "rr_responsivity.h"
#include "rr_sky_depth_test.h"
#include "rr_status.h"
#include "rr_compute_state.h"
#include "rr_sparse_copy.h"
#include "rr_sss_producer.h"
#include "rr_inputs.h"
#include "rr_internal.h"
#include "rr_ngx_api.h"
#include "rr_trace.h"
#include "rr_sr_eligibility.h"
#include "rr_capture.h"
#include <mutex>
#include <deque>
#ifdef RR_FILTER_DIAGNOSTIC
#include "rr_filter_diagnostic.h"
#endif
namespace rrlive {
// Private resources remain retained until native recordings retire and every
// submitted producer and consumer completes on the tracked fence.
class Controller {
    friend struct ControllerCpuTest;
    struct Observed {
        D3D12_RESOURCE_STATES state{};
        bool known{}, complete{};
    };
    struct Slot {
        rrsss::Producer sss;
        bool sss_used{}, awaiting_prepass{}, hit_used{};
        rrtrans::Ticket transparency;
        bool transparency_used{}, transparency_accepted{};
        GpuFrame gpu;
        bool busy{};
        ComPtr<ID3D12GraphicsCommandList> pre, sr;
        bool pre_live{}, sr_live{}, pending_sr{}, recorded{}, sealed{}, inference_ok{}, reported{},
            poisoned{};
        UINT frame = UINT_MAX, viewport = UINT_MAX, generation{}, serial{};
        bool raw_corrected{}, native_sampling{}, pre_submitted{}, gate_recorded{}, replaced_sr{};
        UINT64 lighting_device{}, lighting_revision{};
#ifdef RR_FILTER_DIAGNOSTIC
        bool native_filtered{};
        rrfilter::Chain native_filter_chain;
        rrfilter::SpatialChain spatial_only_chain;
        std::array<ComPtr<ID3D12Resource>, 2> native_filter_history, native_filter_next;
#endif
        UINT64 fence_value{};
        rrtrace::CameraInfo camera;
        std::array<float, 16> view{}, projection{};
        std::array<Observed, 4> states{};
        rreval::Options options;
        rrcapture::Capture captured; // The frame recorded on this slot, if any.
        // Held until the slot retires: a replayed SR recording must never copy into freed memory.
        ComPtr<ID3D12Resource> capture_hold;
    };
    struct Call {
        bool locked{};
        std::vector<bool> touched;
    };
    static inline thread_local std::vector<Call> submissions;
    std::recursive_mutex mutex;
    std::deque<Slot> slots{6};
    Pipelines pipelines;
    Budget budget;
    GpuFrame persistent;
    ComPtr<ID3D12Resource> history, zero;
    ComPtr<ID3D12CommandAllocator> init_allocator;
    ComPtr<ID3D12GraphicsCommandList> init_list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> last_queue;
    UINT64 signaled{};
    bool enabled{}, failed{}, initialized{}, history_queued{}, retired{};
    UINT generation{}, camera_frame = UINT_MAX, camera_view = UINT_MAX, selected_view = UINT_MAX,
                       next_serial{};
    rrtrace::CameraInfo latest;
    UINT64 camera_time{};
    rrstatus::Telemetry status;
    rrpreset::Selection preset;
    bool layout_pending{};
#ifdef RR_FILTER_DIAGNOSTIC
    rrfilter::Mode native_filter_desired{}, native_filter_effective{};
#endif
    rreval::Options observed_options;
    HistoryPolicy policy;
    rrhealth::Tracker health;
    rrdensity::Tracker density;
    bool tolerate_isolated = true;
    bool responsivity_enabled{}, responsivity_active{};
    float responsivity_strength = rrresponsivity::DefaultStrength;
    bool transparency_enabled{}, transparency_failed{};
    rrtrans::Pipeline transparency_pipeline;
    ComPtr<ID3D12Resource> transparency_source;
    bool sss_enabled{}, sss_failed{};
    UINT64 sss_retry_after{};
    bool hit_enabled{};
    unsigned hit_reports{};
    HMODULE runtime{};
    UINT64 runtime_epoch{};
    ComPtr<ID3D12Device> public_device;
    rreval::Api api;
    NVSDK_NGX_Parameter *parameters{};
    NVSDK_NGX_Handle *feature{};
    rreval::Options key;
    rreval::Api (*resolve)(HMODULE) = &rreval::Api::Load;
    rrcapture::Session capture;
    std::shared_ptr<rrcapture::Outcome> capture_outcome;
    unsigned capture_lighting{};
    void Log(const char *s) {
        if (logger)
            logger(s);
    }

    template <class Prepare>
    bool PrepareSssAllocation(UINT64 now, bool ready, Prepare prepare) {
        // Preparation records no GPU commands. Retain partial allocations and
        // retry them after budget pressure subsides; ready slots remain usable.
        if (!ready && now < sss_retry_after)
            return false;
        try {
            prepare();
            if (!ready)
                sss_retry_after = 0;
            return true;
        } catch (const rrprivate::AllocationDeferred &) {
            if (!sss_retry_after)
                Log("Subsurface Scattering guide waiting for GPU memory; allocation will retry");
            sss_retry_after = now + 1000;
            return false;
        }
    }
    bool Complete(const Slot &s) const {
        if (s.poisoned)
            return false;
        if (!s.fence_value)
            return !s.sealed;
        const auto v = fence->GetCompletedValue();
        return v != UINT64_MAX && v >= s.fence_value;
    }
    void Failure(const char *why) {
        failed = true;
        policy.Reject();
        Log(why);
        CaptureFailed("RR failed");
    }
    void Initialize(ID3D12Device *d) {
        if (initialized) {
            Require(fg::SameNativeObject(pipelines.device.Get(), d),
                    "live device generation was not retired before device change");
            return;
        }
        pipelines.Initialize(d);
        auto bd = rrprivate::Buffer(256);
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        history = persistent.Allocate(pipelines, budget, bd);
        zero =
            persistent.Allocate(pipelines, budget, rrprivate::Buffer(256), D3D12_HEAP_TYPE_UPLOAD);
        void *m{};
        Check(zero->Map(0, nullptr, &m), "live history init map failed");
        memset(m, 0, 256);
        zero->Unmap(0, nullptr);
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&init_allocator)),
              "live init allocator failed");
        Check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, init_allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&init_list)),
              "live init list failed");
        Change(init_list.Get(), history.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_DEST);
        init_list->CopyBufferRegion(history.Get(), 0, zero.Get(), 0, 256);
        Change(init_list.Get(), history.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COMMON);
        Check(init_list->Close(), "live init close failed");
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "live timeline fence failed");
        initialized = true;
    }
    bool NeedsReconfigure() const {
        return layout_pending || (preset.has_effective && preset.desired != preset.effective);
    }
    void QueueReconfigure() {
        layout_pending = true;
        policy.Reject();
        status.Fallback(generation, GetTickCount64());
    }
    // Called only at native rendering boundaries, never by panel methods. Poll
    // retirement first: a reset recording can still have outstanding submissions.
    bool Reconfigure() {
        if (!NeedsReconfigure())
            return true;
        for (const auto &s : slots)
            if (!rrpreset::CanRetire(
                    s.poisoned, s.pre_live, s.sr_live, s.sss.live, s.transparency.Live(),
                    s.pending_sr, s.recorded && !s.sealed && s.sr_live,
                    (s.pre_submitted || s.sss.submitted || s.transparency.prep_submitted ||
                     s.transparency.anchor_submitted) &&
                        !s.fence_value,
                    Complete(s)))
                return false;
        if (fence && signaled) {
            const auto done = fence->GetCompletedValue();
            if (done == UINT64_MAX || done < signaled)
                return false;
        }
        // Pooled and collected readback buffers may belong to the device being retired. An Armed
        // capture has none yet and keeps waiting for RR.
        if (capture.state == rrcapture::State::Collecting)
            CaptureFailed("RR was reconfigured");
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        if (feature) {
            if (!api.release || NVSDK_NGX_FAILED(api.release(feature))) {
                Failure("RR feature retirement failed; allocations retained");
                return false;
            }
            feature = nullptr;
            preset.Retired();
        }
        if (parameters) {
            if (!api.destroy || NVSDK_NGX_FAILED(api.destroy(parameters))) {
                Failure("RR parameter retirement failed; allocations retained");
                return false;
            }
            parameters = nullptr;
        }
        for (auto &s : slots) {
            s.gpu.Release(budget);
            s = {};
        }
        history.Reset();
        zero.Reset();
        persistent.Release(budget);
        init_list.Reset();
        init_allocator.Reset();
        fence.Reset();
        last_queue.Reset();
        pipelines = {};
        transparency_pipeline = {};
        transparency_source.Reset();
        transparency_composition = {};
        initialized = history_queued = false;
        signaled = 0;
        runtime = nullptr;
        runtime_epoch = 0;
        public_device.Reset();
        api = {};
        selected_view = UINT_MAX;
        key = {};
        if (++generation == 0) {
            Failure("RR feature generation overflow");
            return false;
        }
        status.Reset(generation);
        policy.Reject();
        preset.Retired();
        layout_pending = false;
#ifdef RR_FILTER_DIAGNOSTIC
        native_filter_effective = native_filter_desired;
        char filter_log[160];
        snprintf(filter_log, sizeof(filter_log), "Diagnostic: %s applied; RR history restarted",
                 rrfilter::Label(native_filter_effective));
        Log(filter_log);
#endif
        return true;
    }
    bool Key(const rreval::Options &o, HMODULE mod, UINT64 epoch, ID3D12Device *d, UINT viewport) {
        if (!feature)
            return true;
        return runtime == mod && runtime_epoch == epoch && public_device.Get() == d &&
               selected_view == viewport && rreval::SameCreation(key, o);
    }
    Slot *Match(rrtrace::SlCall call) {
        if (!call.valid || call.feature != sl::kFeatureDLSS)
            return nullptr;
        Slot *result{};
        for (auto &s : slots)
            if (s.pre_live && s.frame == call.frame && s.viewport == call.viewport && !s.recorded) {
                if (result)
                    return nullptr;
                result = &s;
            }
        return result;
    }
    void PollLocked() {
        for (auto &s : slots) {
            if (s.recorded && s.sealed && !s.reported && Complete(s)) {
                auto v = s.gate_recorded ? s.gpu.ReadStats() : std::array<UINT, 4>{1, 0, 0, 0};
                s.reported = true;
                health.Frame(v);
                if (s.gate_recorded && s.native_sampling)
                    density.Add(s.gpu.ReadRates());
                status.Completed(s.serial, s.generation, GetTickCount64(), v[0] == 0 && v[1] != 0,
                                 s.sss_used && s.sss.admission.accepted,
                                 s.transparency_used && s.transparency_accepted, s.replaced_sr,
                                 s.hit_used);
                if (s.captured.buffer) {
                    auto &m = s.captured.meta;
                    m.stats = v;
                    m.delivered = v[0] == 0 && v[1] != 0;
                    m.replaced_sr = s.replaced_sr;
                    try {
                        capture.Complete(std::move(s.captured));
                    } catch (...) {
                        CaptureFailed("out of memory");
                    }
                    s.captured = {};
                }
                if (v[0] || !v[1]) {
                    policy.Reject();
                }
            }
            if (s.busy && !s.pre_live && !s.sr_live && !s.sss.live && !s.transparency.Live() &&
                !s.pending_sr && Complete(s)) {
                if (s.captured.buffer) {
                    try {
                        capture.Recycle(std::move(s.captured.buffer));
                    } catch (...) {
                        CaptureFailed("out of memory");
                    }
                }
                auto cache = std::move(s.gpu);
                cache.Retire();
                s = {};
                s.gpu = std::move(cache);
                s.busy = false;
            }
        }
        if (!capture.completed.empty()) {
            std::vector<rrcapture::Capture> done;
            try {
                done = capture.Settle();
            } catch (...) {
                CaptureFailed("out of memory");
            }
            if (!done.empty())
                WriteCaptureLocked(std::move(done));
        }
        std::string report;
        if (health.Report(GetTickCount64(), report))
            Log(report.c_str());
        if (density.Report(GetTickCount64(), report))
            Log(report.c_str());
    }
    void LogCaptureFailure() {
        char line[320];
        snprintf(line, sizeof(line), "RR capture failed: %s", capture.message.c_str());
        try {
            Log(line);
        } catch (...) {
        }
    }
    // A capture problem ends the capture, never RR. Fail is ignored while Writing, so a writer
    // that could not start (writer = true) ends its session with Finished instead.
    void CaptureFailed(const char *why, bool writer = false) {
        if (writer ? capture.state != rrcapture::State::Writing : !capture.Active())
            return; // Nothing to stop, or a running writer owns the outcome.
        try {
            if (writer)
                capture.Finished(false, why);
            else
                capture.Fail(why);
        } catch (...) { // The text could not be allocated; an empty one allocates nothing.
            if (writer)
                capture.Finished(false, {});
            else
                capture.Fail({});
        }
        LogCaptureFailure();
    }
    // Everything a capture copies this frame: what RR is given, the Status guide and RR's output.
    rrcapture::Sources CaptureSources(const Slot &s, const rreval::Resources &r,
                                      const rreval::Options &o) const {
        rrcapture::Sources sources{};
        sources[rrcapture::Color] = {r.color, o.color_x, o.color_y};
        sources[rrcapture::Depth] = {r.depth, o.depth_x, o.depth_y};
        sources[rrcapture::Motion] = {r.motion, o.motion_x, o.motion_y};
        sources[rrcapture::NormalRoughness] = {r.normal};
        sources[rrcapture::DiffuseAlbedo] = {r.diffuse};
        sources[rrcapture::SpecularAlbedo] = {r.specular};
        sources[rrcapture::Status] = {s.gpu.guides[3].Get()};
        sources[rrcapture::SssGuide] = {r.sss};
        sources[rrcapture::ColorBeforeTransparency] = {r.before_transparency};
        sources[rrcapture::ResponsivityMask] = {r.responsivity};
        sources[rrcapture::SpecularHitDistance] = {r.specular_hit};
        sources[rrcapture::RrOutput] = {r.output, 0, 0, o.out_width, o.out_height};
        return sources;
    }
    // Copies what RR is given this frame, plus the Status guide, before RR reads it: inputs
    // [0, RrOutput), each in NON_PIXEL_SHADER_RESOURCE here, where Record leaves it. Returns
    // whether it recorded this frame; CaptureOutputLocked then adds RR's output.
    bool CaptureLocked(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Resources &r,
                       const rreval::Options &o, bool reset,
                       const std::array<float, 16> &projection) {
        try {
            const bool armed = capture.state == rrcapture::State::Armed;
            if (!armed) {
                unsigned in_flight = 0;
                for (const auto &v : slots)
                    if (v.captured.buffer && v.captured.session == capture.session)
                        ++in_flight;
                if (!capture.Wants(in_flight))
                    return false;
            }
            const auto sources = CaptureSources(s, r, o);
            ID3D12Device *device = pipelines.device.Get();
            rrcapture::Capture frame;
            if (!rrcapture::Plan(device, o.width, o.height, sources, frame)) {
                CaptureFailed("unsupported input layout");
                return false;
            }
            char line[160];
            const auto mb = [](UINT64 bytes) {
                return static_cast<unsigned long long>((bytes >> 20) + ((bytes & 0xFFFFF) != 0));
            };
            const auto admit = [&](UINT64 bytes) { // Fails the capture when refused.
                if (rrcapture::Admit(device, bytes))
                    return true;
                snprintf(line, sizeof(line), "not enough memory for %llu MB", mb(bytes));
                CaptureFailed(line);
                return false;
            };
            const auto allocate = [&] { // One frame's buffer; fails the capture when null.
                auto b = rrcapture::Allocate(device, frame.bytes);
                if (!b)
                    CaptureFailed("cannot allocate readback memory");
                return b;
            };
            if (armed) {
                if (frame.bytes > UINT64_MAX / rrcapture::Frames) {
                    CaptureFailed("capture too large");
                    return false;
                }
                if (!admit(frame.bytes * rrcapture::Frames))
                    return false;
                std::vector<ComPtr<ID3D12Resource>> buffers;
                for (unsigned i = 0; i < rrcapture::Frames; ++i) {
                    buffers.push_back(allocate());
                    if (!buffers.back())
                        return false;
                }
                capture.Begin(std::move(buffers));
                snprintf(line, sizeof(line), "RR capture: collecting %u frames, %llu MB each",
                         rrcapture::Frames, mb(frame.bytes));
                Log(line);
                return false; // The allocation hitch stays out of the run.
            }
            frame.buffer = capture.Take();
            if (frame.buffer && frame.buffer->GetDesc().Width < frame.bytes)
                frame.buffer.Reset();
            if (!frame.buffer) {
                if (!admit(frame.bytes))
                    return false;
                frame.buffer = allocate();
                if (!frame.buffer)
                    return false;
            }
            if (!capture.Count()) {
                LogCaptureFailure();
                return false;
            }
            auto &m = frame.meta;
            m.serial = s.serial;
            m.sl_frame = s.frame;
            m.viewport = s.viewport;
            m.reset = reset;
            m.reset_reasons = policy.LastReason();
            const auto pair = [](auto *to, auto a, auto b) {
                to[0] = a;
                to[1] = b;
            };
            pair(m.jitter, o.jitter_x, o.jitter_y);
            pair(m.motion_scale, o.motion_x_scale, o.motion_y_scale);
            m.pre_exposure = o.pre_exposure;
            m.exposure_scale = o.exposure_scale;
            pair(m.color_subrect, o.color_x, o.color_y);
            pair(m.depth_subrect, o.depth_x, o.depth_y);
            pair(m.motion_subrect, o.motion_x, o.motion_y);
            std::copy_n(s.view.begin(), 16, m.world_to_view);
            std::copy_n(projection.begin(), 16, m.view_to_clip);
            const auto &c = s.camera;
            auto &k = m.camera;
            std::copy_n(c.position, 3, k.position);
            std::copy_n(c.right, 3, k.right);
            std::copy_n(c.up, 3, k.up);
            std::copy_n(c.forward, 3, k.forward);
            std::copy_n(c.jitter, 2, k.jitter);
            std::copy_n(c.motion_scale, 2, k.motion_scale);
            std::copy_n(c.projection, 16, k.projection);
            std::copy_n(c.inverse, 16, k.inverse);
            k.near_plane = c.near_plane;
            k.far_plane = c.far_plane;
            k.fov = c.fov;
            k.aspect = c.aspect;
            k.depth_inverted = c.depth_inverted;
            k.camera_motion = c.camera_motion;
            k.motion3d = c.motion3d;
            k.reset = c.reset;
            k.orthographic = c.orthographic;
            k.motion_dilated = c.motion_dilated;
            k.motion_jittered = c.motion_jittered;
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            if (frequency.QuadPart)
                m.cpu_time_ms = double(now.QuadPart) * 1000.0 / double(frequency.QuadPart);
            frame.session = capture.session;
            // Nothing below throws: the recorded copies always keep their buffer alive.
            rrcapture::Record(cl, frame, sources, 0, rrcapture::RrOutput,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            s.capture_hold = frame.buffer;
            s.captured = std::move(frame);
            return true;
        } catch (const std::exception &e) {
            CaptureFailed(e.what());
        } catch (...) {
            CaptureFailed("unexpected error");
        }
        return false;
    }
    // Adds RR's output to the frame CaptureLocked recorded, right after a successful evaluate.
    // rr_output is in UNORDERED_ACCESS here, where Record leaves it for GateEnd.
    void CaptureOutputLocked(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Resources &r,
                             const rreval::Options &o) {
        try {
            rrcapture::Record(cl, s.captured, CaptureSources(s, r, o), rrcapture::RrOutput,
                              rrcapture::InputCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } catch (const std::exception &e) {
            CaptureFailed(e.what());
        } catch (...) {
            CaptureFailed("unexpected error");
        }
    }
    // Starts the background writer for a complete run; the session is already Writing.
    void WriteCaptureLocked(std::vector<rrcapture::Capture> done) {
        try {
            rrcapture::Header h;
            h.render_width = done[0].width;
            h.render_height = done[0].height;
            h.output_width = key.out_width;
            h.output_height = key.out_height;
            h.quality = key.quality;
            h.preset = preset.has_effective ? preset.effective : preset.desired;
            h.sss_enabled = sss_enabled;
            h.transparency_enabled = transparency_enabled;
            h.specular_hit_enabled = hit_enabled;
            h.tolerate_isolated = tolerate_isolated;
            h.responsivity_enabled = responsivity_enabled;
            h.responsivity_strength = responsivity_strength;
            h.guide_test = rrguidetest::Valid(rrguidetest::mode.load(std::memory_order_relaxed));
            h.sky_depth_test = rrskydepth::enabled.load(std::memory_order_relaxed);
            h.lighting_input = capture_lighting;
            capture_outcome = rrcapture::Write(std::move(done), std::move(h), logger);
        } catch (const std::exception &e) {
            CaptureFailed(e.what(), true);
        } catch (...) {
            CaptureFailed("unexpected error", true);
        }
    }
    void CaptureOutcomeLocked() { // A finished writer ends its session.
        if (capture.state != rrcapture::State::Writing || !capture_outcome)
            return;
        std::lock_guard lock(capture_outcome->mutex);
        if (capture_outcome->done)
            capture.Finished(capture_outcome->ok, std::move(capture_outcome->text));
    }
#include "rr_live_pool.inl"
  public:
    explicit Controller(rreval::Api (*loader)(HMODULE) = &rreval::Api::Load) : resolve(loader) {}
#include "rr_live_transparency.inl"

    bool SssEnabled() {
        std::lock_guard lock(mutex);
        return sss_enabled;
    }
    // Isolated unusable guide pixels are counted instead of rejecting the frame.
    void SetTolerateIsolated(bool value) {
        std::lock_guard lock(mutex);
        tolerate_isolated = value;
    }
    // Optional RR responsivity mask on SSS pixels; strength is clamped to -1..0.
    void SetResponsivity(bool enabled, float strength) {
        std::lock_guard lock(mutex);
        responsivity_enabled = enabled;
        responsivity_strength = rrresponsivity::Clamp(strength);
    }
    // Whether the latest evaluation sent the mask and it would still be sent now;
    // the flag alone goes stale once RR stops evaluating.
    bool ResponsivityActive() {
        std::lock_guard lock(mutex);
        return responsivity_active && enabled && !failed && !retired && sss_enabled &&
               !sss_failed &&
               rrresponsivity::Use(responsivity_enabled, responsivity_strength, true,
                                   preset.has_effective, preset.effective);
    }
    void SetSssEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (sss_enabled != value) {
            sss_enabled = value;
            policy.Reject();
        }
    }
    void ToggleSss() {
        std::lock_guard lock(mutex);
        SetSssEnabled(!sss_enabled);
    }
    bool SpecularHitEnabled() {
        std::lock_guard lock(mutex);
        return hit_enabled;
    }
    void SetSpecularHitEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (hit_enabled != value) {
            hit_enabled = value;
            policy.Reject();
        }
    }
    bool PrepassSurface(ID3D12GraphicsCommandList *cl, ID3D12Resource *surface) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.gpu.sources[3].Get() == surface)
                return true;
        return false;
    }
    // The native distance blur runs after the prepass on the same recording and
    // reads that prepass's PackedSurface. Only a copy is recorded; the caller has
    // proven the dispatch, tables, views and the source's tracked state.
    bool SpecularDistance(ID3D12GraphicsCommandList *cl, ID3D12Resource *surface,
                          ID3D12Resource *source, D3D12_RESOURCE_STATES source_state) {
        std::lock_guard lock(mutex);
        if (!enabled || !hit_enabled || failed || retired || !cl || !surface || !source)
            return false;
        Slot *match{};
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.gpu.sources[3].Get() == surface) {
                if (match)
                    return false;
                match = &s;
            }
        if (!match)
            return false;
        try {
            rrinternal::Scope copy_guard;
            rrcompute::Tracker::Internal state_guard;
            const bool copied =
                match->gpu.CopySpecularDistance(cl, pipelines, source, source_state);
            if (hit_reports < 2) {
                ++hit_reports;
                const auto d = source->GetDesc();
                char line[192];
                snprintf(line, sizeof(line),
                         "Specular hit distance %s: source=%llux%u format=%u state=0x%x "
                         "guides=%ux%u",
                         copied ? "copied" : "rejected", d.Width, d.Height, unsigned(d.Format),
                         unsigned(source_state), match->gpu.w, match->gpu.h);
                Log(line);
            }
            return copied;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }

    bool OwnedSssDraw(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *variant,
                      const rrsss::GraphicsOutputSnapshot &original,
                      const rrsss::ProducerDraw &draw, const rrsss::StencilSetup &stencil = {}) {
        std::lock_guard lock(mutex);
        if (!enabled || !sss_enabled || sss_failed || failed || retired || NeedsReconfigure())
            return false;
        if (!cl || !variant || original.command != cl || !rrsss::GeometryState(original, stencil) ||
            draw.count != 3 || draw.instances != 1) {
            return false;
        }
        // Native SR and the SSS producer can be recorded on different CPU threads.
        // AfterSR takes this same mutex and freezes guide selection at recorded=true.
        // pending_sr alone is not the cutoff; GPU submission order is checked separately.
        Slot *match{};
        for (auto &s : slots)
            if ((s.pre_live || s.awaiting_prepass) && !s.recorded && s.frame == camera_frame &&
                s.viewport == camera_view && s.generation == generation && !s.poisoned) {
                if (match) {
                    return false;
                }
                match = &s;
            }
        bool early = false;
        if (!match) {
            // CPU recording order need not match GPU execution order. Reserve only a
            // free warmed slot; the later prepass adopts this exact frame/view ticket.
            bool conflict = false;
            for (const auto &slot : slots)
                if (slot.busy && slot.frame == camera_frame && slot.viewport == camera_view)
                    conflict = true;
            if (!conflict && initialized && camera_frame != UINT_MAX && camera_view != UINT_MAX &&
                latest.readable && latest.basis_valid && latest.projection_valid &&
                GetTickCount64() - camera_time <= 1000 &&
                (selected_view == UINT_MAX || selected_view == camera_view)) {
                for (auto &slot : slots)
                    if (!slot.busy && slot.gpu.rr_output &&
                        rrsss::Geometry(original, slot.gpu.w, slot.gpu.h, stencil) &&
                        slot.gpu.sss.MatchesLayout(slot.gpu.w, slot.gpu.h, stencil.samples)) {
                        match = &slot;
                        early = true;
                        break;
                    }
            }
        }
        if (!match) {
            return false;
        }
        auto &s = *match;
        if (s.sss.command) {
            s.sss.written = false;
            return false;
        }
        if (!rrsss::Geometry(original, s.gpu.w, s.gpu.h, stencil) ||
            !s.gpu.sss.MatchesLayout(s.gpu.w, s.gpu.h, stencil.samples)) {
            return false;
        }
        if (!s.gpu.sss.ready) {
            // Observe a complete mandatory SR allocation before reserving optional MRTs.
            if (!s.gpu.rr_output) {
                return false;
            }
            UINT64 base = 0, depth = 0;
            for (const auto &slot : slots) {
                UINT64 b{}, d{};
                if (!SlotReservationParts(pipelines.device.Get(), slot.gpu, budget.limit, b, d) ||
                    b < slot.gpu.sss.bytes)
                    return false;
                base = (std::max)(base, b - slot.gpu.sss.bytes);
                depth = (std::max)(depth, d);
            }
            auto texture = TextureDesc(s.gpu.w, s.gpu.h, DXGI_FORMAT_R32_FLOAT);
            texture.Flags = D3D12_RESOURCE_FLAG_NONE;
            auto buffer = rrprivate::Buffer(((UINT64(s.gpu.w) * 4 + 255) & ~255ull) * s.gpu.h);
            const auto texture_bytes =
                           pipelines.device->GetResourceAllocationInfo(0, 1, &texture).SizeInBytes,
                       buffer_bytes =
                           pipelines.device->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes;
            if (!texture_bytes || !buffer_bytes || texture_bytes > budget.limit ||
                buffer_bytes > budget.limit - texture_bytes)
                return false;
            depth = (std::max)(depth, texture_bytes + buffer_bytes);
            UINT64 required{};
            const auto optional_sss = rrsss::Gpu::RequiredBytes(pipelines.device.Get(), s.gpu.w,
                                                                s.gpu.h, stencil.samples);
            if (!rrtrans::RequiredSlots(slots.size(), base, depth, optional_sss, 0, 0,
                                        persistent.bytes, required) ||
                !budget.Reserve(pipelines.device.Get(), required) ||
                !rrsss::FitsOwnedBudget(base, depth, persistent.bytes, optional_sss, 0,
                                        budget.limit, slots.size())) {
                return false;
            }
        }
        bool issued = false;
        try {
            ComPtr<ID3D12Device> d;
            Check(cl->GetDevice(IID_PPV_ARGS(&d)), "SSS producer device missing");
            Require(fg::SameNativeObject(d.Get(), pipelines.device.Get()),
                    "SSS producer device mismatch");
            if (!PrepareSssAllocation(GetTickCount64(), s.gpu.sss.ready, [&] {
                    s.gpu.sss.Prepare(s.gpu, pipelines, budget, stencil.samples);
                }))
                return false;
            if (early) {
                s.busy = true;
                s.awaiting_prepass = true;
                s.frame = camera_frame;
                s.viewport = camera_view;
                s.generation = generation;
            }
            s.sss.command = cl;
            s.sss.variant = variant;
            s.sss.original = original;
            s.sss.stencil = stencil;
            s.sss.live = true;
            rrinternal::Scope copies;
            rrcompute::Tracker::Internal state;
            rrsss::DrawResult result;
            result = s.sss.Record(cl, draw, s.gpu.sss);
            issued = result.issued;
            s.sss.written = result.issued && !result.failed;
            if (result.failed) {
                sss_failed = true;
                policy.Reject();
                Log("RR SSS draw failure contained; optional guide disabled, issued draw not "
                    "repeated");
            }
            return result.issued;
        } catch (const std::exception &e) {
            sss_failed = true;
            s.sss.written = false;
            policy.Reject();
            try {
                Log(e.what());
            } catch (...) {
            }
            return issued;
        } catch (...) {
            sss_failed = true;
            s.sss.written = false;
            policy.Reject();
            return issued;
        }
    }
    void (*logger)(const char *){};
    bool (*lighting_valid)(UINT64, UINT64){};
#ifdef RR_FILTER_DIAGNOSTIC
    bool (*native_lighting_valid)(UINT64, UINT64){};
    bool RequestFilterMode(rrfilter::Mode value) {
        std::lock_guard lock(mutex);
        if (!rrfilter::Valid(value)) return false;
        if (native_filter_desired != value) {
            native_filter_desired = value;
            QueueReconfigure();
        }
        return true;
    }
    void RestartFilterComparison() {
        std::lock_guard lock(mutex);
        QueueReconfigure();
    }
    rrfilter::Status NativeFilterStatus() {
        std::lock_guard lock(mutex);
        return {native_filter_desired, native_filter_effective, NeedsReconfigure()};
    }
    bool NativeFiltering() {
        std::lock_guard lock(mutex);
        return rrfilter::Native(native_filter_effective);
    }
    bool SpatialOnlyFiltering() {
        std::lock_guard lock(mutex);
        return native_filter_effective == rrfilter::Mode::SpatialRemaining;
    }
    void SeedSpatialOnly(ID3D12GraphicsCommandList *cl, const rrsparsecopy::Pair &output) {
        std::lock_guard lock(mutex);
        if (native_filter_effective != rrfilter::Mode::SpatialRemaining) return;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_sampling) {
            if (s.spatial_only_chain.Seed({UINT64(output.r[0].Get()), UINT64(output.r[1].Get())}))
                s.native_filter_next = output.r;
        }
    }
    bool ObserveSpatialOnly(ID3D12GraphicsCommandList *cl, const rrsparsecopy::Pair &input,
                            const rrsparsecopy::Pair &output, bool valid) {
        std::lock_guard lock(mutex);
        if (native_filter_effective != rrfilter::Mode::SpatialRemaining) return false;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_sampling && s.raw_corrected) {
            if (!valid) s.spatial_only_chain.Invalidate();
            if (s.spatial_only_chain.Observe({UINT64(input.r[0].Get()), UINT64(input.r[1].Get())},
                                             {UINT64(output.r[0].Get()), UINT64(output.r[1].Get())})) {
                s.native_filter_next = output.r;
                return true;
            }
        }
        return false;
    }
    void NativeTemporalObserved(ID3D12GraphicsCommandList *cl, const rrsparsecopy::Pair &output) {
        std::lock_guard lock(mutex);
        if (!rrfilter::Native(native_filter_effective)) return;
        for (auto &s : slots) if (RawSlot(s, cl)) {
            s.native_filtered = false;
            if (s.native_filter_chain.Temporal({UINT64(output.r[0].Get()), UINT64(output.r[1].Get())}))
                s.native_filter_history = output.r;
        }
    }
    bool NativeSpatialCandidate(ID3D12GraphicsCommandList *cl, const rrsparsecopy::Pair &input) {
        std::lock_guard lock(mutex);
        if (!rrfilter::Native(native_filter_effective)) return false;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_sampling &&
            s.native_filter_chain.Matches({UINT64(input.r[0].Get()), UINT64(input.r[1].Get())}))
            return true;
        return false;
    }
    // A proven first spatial consumer permits passthrough only in temporal-only mode.
    // Never reuse this proof across recording, retirement, or feature boundaries.
    bool NativeSpatialBypass(ID3D12GraphicsCommandList *cl, std::array<UINT64,2> input) {
        std::lock_guard lock(mutex);
        if (native_filter_effective != rrfilter::Mode::TemporalOnly) return false;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_sampling &&
            !s.raw_corrected && s.native_filtered && s.native_filter_chain.Next(input))
            return true;
        return false;
    }
    void RejectNativeSpatial(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        if (native_filter_effective != rrfilter::Mode::TemporalOnly) return;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_filtered) {
            s.native_filter_chain.Invalidate();
            s.native_filtered = false;
        }
    }
    bool AdvanceNativeSpatial(ID3D12GraphicsCommandList *cl, const rrsparsecopy::Pair &input,
                              const rrsparsecopy::Pair &output) {
        std::lock_guard lock(mutex);
        if (native_filter_effective != rrfilter::Mode::TemporalOnly) return false;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_sampling && s.native_filtered &&
            s.native_filter_chain.Forward({UINT64(input.r[0].Get()), UINT64(input.r[1].Get())},
                                          {UINT64(output.r[0].Get()), UINT64(output.r[1].Get())})) {
            s.native_filter_next = output.r;
            return true;
        }
        return false;
    }
    void NativeSpatialObserved(ID3D12GraphicsCommandList *cl, const rrsparsecopy::Pair &input,
                               UINT64 device, UINT64 revision) {
        std::lock_guard lock(mutex);
        if (!rrfilter::Native(native_filter_effective)) return;
        for (auto &s : slots) if (RawSlot(s, cl) && s.native_sampling &&
            s.native_filter_chain.Spatial({UINT64(input.r[0].Get()), UINT64(input.r[1].Get())})) {
            s.native_filtered = true;
            s.lighting_device = device;
            s.lighting_revision = revision;
        }
    }
#endif
  private:
    bool LightingValid(const Slot &s) const {
#ifdef RR_FILTER_DIAGNOSTIC
        if (rrfilter::Native(native_filter_effective))
            return !s.raw_corrected && s.native_filtered && s.native_filter_chain.Accepted() &&
                   (native_filter_effective != rrfilter::Mode::TemporalOnly || s.native_filter_chain.Bypassed()) &&
                   native_lighting_valid && native_lighting_valid(s.lighting_device, s.lighting_revision);
        if (native_filter_effective == rrfilter::Mode::SpatialRemaining && !s.spatial_only_chain.Accepted())
            return false;
#endif
        return s.raw_corrected && lighting_valid && lighting_valid(s.lighting_device, s.lighting_revision);
    }
  public:

    rrstatus::Snapshot Status() {
        std::lock_guard lock(mutex);
        return status.Read({enabled, sss_enabled, transparency_enabled, failed, retired, sss_failed,
                            transparency_failed, hit_enabled},
                           GetTickCount64());
    }
    bool Enabled() {
        std::lock_guard lock(mutex);
        return enabled;
    }
    void SetEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (enabled == value)
            return;
        enabled = value;
        // Collecting holds readback buffers; an Armed capture keeps waiting for RR.
        if (!value && capture.state == rrcapture::State::Collecting)
            CaptureFailed("RR was turned off");
        if (++generation == 0) {
            Failure("RR enable generation overflow");
            return;
        }
        status.Reset(generation);
        policy.Reject();
    }
    void Toggle() {
        std::lock_guard lock(mutex);
        SetEnabled(!enabled);
    }
    bool RequestPreset(unsigned value) {
        std::lock_guard lock(mutex);
        return preset.Request(value);
    }
    rrpreset::Status PresetStatus() {
        std::lock_guard lock(mutex);
        return {preset.desired, preset.effective, preset.has_effective,
                preset.Pending() || layout_pending};
    }
    // Session-only development capture of RR's inputs; nothing is allocated or recorded until
    // requested.
    void RequestCapture(unsigned lighting_input) {
        std::lock_guard lock(mutex);
        CaptureOutcomeLocked(); // A writer that has finished no longer blocks a new request.
        if (capture.Request()) {
            capture_lighting = lighting_input;
            capture_outcome.reset();
            Log("RR capture: requested");
        }
    }
    struct CaptureView {
        rrcapture::State state{};
        unsigned collected{};
        std::string message;
    };
    CaptureView CaptureStatus() {
        std::lock_guard lock(mutex);
        CaptureOutcomeLocked();
        return {capture.state, unsigned(capture.run.size()), capture.message};
    }
    rreval::Options Options() {
        std::lock_guard lock(mutex);
        return observed_options;
    }
    void Camera(UINT frame, UINT viewport, const sl::Constants &c) {
        std::lock_guard lock(mutex);
        latest = rrtrace::ReadCamera(c);
        camera_frame = frame;
        camera_view = viewport;
        camera_time = GetTickCount64();
    }

    void Poll() {
        std::lock_guard lock(mutex);
        try {
            PollLocked();
        } catch (const std::exception &e) {
            Failure(e.what());
        }
    }
    // All native identity/binding/view/constant/context proofs are required at
    // the call site. Prepare completes before dispatch; once native dispatch is
    // issued it MUST be suppressed at the wrapper even if later conversion fails.
    bool Prepass(ID3D12GraphicsCommandList *cl,
                 const std::array<ComPtr<ID3D12Resource>, 4> &textures,
                 const std::array<ComPtr<ID3D12Resource>, 2> &cb,
                 const std::array<UINT64, 2> &offsets, const rrcompute::Snapshot &restore, UINT x,
                 UINT y, UINT z) {
        std::lock_guard lock(mutex);
        if (!enabled || failed || retired)
            return false;
        bool dispatched = false;
        try {
            PollLocked();
            if (!restore.valid || !restore.heap_restore_valid || restore.command != cl ||
                camera_frame == UINT_MAX || camera_view == UINT_MAX || !latest.readable ||
                !latest.basis_valid || !latest.projection_valid ||
                GetTickCount64() - camera_time > 1000) {
                return false;
            }
            if (textures[3]) {
                const auto incoming = textures[3]->GetDesc();
                for (const auto &slot : slots)
                    if (slot.gpu.heap &&
                        (incoming.Width != slot.gpu.w || incoming.Height != slot.gpu.h)) {
                        QueueReconfigure();
                        break;
                    }
            }
            if (selected_view != UINT_MAX && selected_view != camera_view)
                QueueReconfigure();
            ComPtr<ID3D12Device> incoming_device;
            Check(cl->GetDevice(IID_PPV_ARGS(&incoming_device)), "live prepass device missing");
            if (initialized && !fg::SameNativeObject(pipelines.device.Get(), incoming_device.Get()))
                QueueReconfigure();
            if (!Reconfigure())
                return false;
            for (auto &s : slots)
                if (s.pre_live &&
                    (s.pre.Get() == cl || (s.frame == camera_frame && s.viewport == camera_view))) {
                    return false;
                }
            if (selected_view != UINT_MAX && selected_view != camera_view) {
                return false;
            }
            Slot *free{};
            for (auto &s : slots)
                if (s.busy && s.awaiting_prepass && s.frame == camera_frame &&
                    s.viewport == camera_view) {
                    if (free || s.generation != generation || s.poisoned || s.recorded ||
                        s.pending_sr || s.sss.command.Get() == cl ||
                        s.transparency.preparation.Get() == cl ||
                        s.transparency.anchor.Get() == cl) {
                        return false;
                    }
                    free = &s;
                }
            if (!free)
                for (auto &s : slots)
                    if (!s.busy) {
                        free = &s;
                        break;
                    }
            if (!free)
                free = GrowPool();
            if (!free) {
                status.Fallback(generation, GetTickCount64());
                policy.Reject();
                return false;
            }
            ComPtr<ID3D12Device> d;
            Check(cl->GetDevice(IID_PPV_ARGS(&d)), "live prepass device missing");
            Initialize(d.Get());
            const auto guide_extent = textures[3]->GetDesc();
            Require(
                guide_extent.Width <= UINT_MAX &&
                    (free->gpu.heap || ReservePool(slots.size(), nullptr, nullptr,
                                                   UINT(guide_extent.Width), guide_extent.Height)),
                "live prepass pool exceeds available GPU budget");
            auto &s = *free;
            s.busy = true;
            s.gpu.Prepare(pipelines, budget, textures, cb, offsets);
            s.pre = cl;
            s.pre_live = true;
            s.awaiting_prepass = false;
            s.frame = camera_frame;
            s.viewport = camera_view;
            s.generation = generation;
            s.camera = latest;
            s.view = rreval::WorldToView(latest.position, latest.right, latest.up, latest.forward);
            std::copy_n(latest.projection, 16, s.projection.begin());
            rrinternal::Scope copy_guard;
            rrcompute::Tracker::Internal state_guard;
            cl->Dispatch(x, y, z);
            dispatched = true;
            s.gpu.Convert(cl, pipelines);
            restore.Restore(cl, true);
            return true;
        } catch (const std::exception &e) {
            Failure(e.what());
            if (dispatched) {
                rrinternal::Scope a;
                rrcompute::Tracker::Internal b;
                restore.Restore(cl, true);
            }
            return dispatched;
        }
    }

    void RawCorrected(ID3D12GraphicsCommandList *cl, UINT64 device, UINT64 revision) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (enabled && !failed && !retired && s.generation == generation && s.pre_live &&
                s.pre.Get() == cl && !s.recorded) {
                s.raw_corrected = true;
                s.lighting_device = device;
                s.lighting_revision = revision;
            }
    }
    bool RawSlot(const Slot &s, ID3D12GraphicsCommandList *cl) const {
        return enabled && !failed && !retired && s.generation == generation && s.pre_live &&
               s.pre.Get() == cl && !s.pending_sr && !s.recorded && !s.poisoned;
    }
    bool CanRaw(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl))
                return true;
        return false;
    }
    bool NativeSampling(ID3D12GraphicsCommandList *cl, const rrvrrt::Inputs &inputs,
                        const rrcompute::Snapshot &restore) {
        std::lock_guard lock(mutex);
        if (!restore.valid || !restore.complete || !restore.heap_restore_valid ||
            restore.command != cl)
            return false;
        Slot *match{};
        for (auto &s : slots)
            if (RawSlot(s, cl)) {
                if (match || s.native_sampling) {
                    Failure("RR ambiguous/repeated native VRRT in one guide recording; raw path "
                            "blocked");
                    return false;
                }
                match = &s;
            }
        if (!match)
            return false;
        try {
            rrinternal::Scope copies;
            rrcompute::Tracker::Internal state;
            match->gpu.Probability(cl, pipelines, budget, inputs);
            restore.Restore(cl, true);
            match->native_sampling = true;
            return true;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }
    rrvrrt::Data SamplingData(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        rrvrrt::Data result;
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                if (result.buffer)
                    return {};
                result = {s.gpu.probability, s.gpu.w, s.gpu.h};
            }
        return result;
    }
    bool SamplingReady(ID3D12GraphicsCommandList *cl) {
        return bool(SamplingData(cl).buffer);
    }
    bool RawWasCorrected(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.raw_corrected)
                return true;
        return false;
    }
    bool RawSpatialPassthrough(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
#ifdef RR_FILTER_DIAGNOSTIC
        if (native_filter_effective == rrfilter::Mode::SpatialRemaining) return false;
#endif
        return RawWasCorrected(cl);
    }
    void BeforeSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                  rrtrace::SlCall scope, bool safe) {
        std::lock_guard lock(mutex);
        auto actual = rrinputs::ReadEvaluation(p);
        auto creation = rrtrace::Probe::CurrentCreation();
        if (creation.valid) {
            actual.out_width = creation.out_width;
            actual.out_height = creation.out_height;
            actual.flags = creation.flags;
            actual.quality = creation.quality;
        }
        if (rreval::Valid(actual))
            observed_options = actual;
        if (failed || retired)
            return;
        if (feature && rreval::Valid(actual) && !rreval::SameCreation(key, actual)) {
            QueueReconfigure();
            return;
        }
        auto *s = Match(scope);
        if (!s) {
            return;
        }
        try {
            Require(safe && LightingValid(*s),
                    "RR SR rejected: lighting route or pipeline revision unproven");
            auto cl = fg::NativeObject<ID3D12GraphicsCommandList>(public_cl);
            Require(bool(cl), "live native SR list missing");
            s->options = actual;
            Require(rreval::Valid(s->options) && s->options.width == s->gpu.w &&
                        s->options.height == s->gpu.h,
                    "live SR dimensions/options missing or mismatched");
            auto input = rrtrace::ReadInputs(p);
            void *raw[] = {input.color, input.depth, input.motion, input.output};
            for (unsigned i = 0; i < 4; ++i) {
                s->gpu.sr[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)raw[i]);
                Require(bool(s->gpu.sr[i]) &&
                            rrsparsecopy::SameDevice(pipelines.device.Get(), s->gpu.sr[i].Get()),
                        "live SR resource identity mismatch");
            }
            s->sr = cl;
            s->sr_live = true;
            s->pending_sr = true;
            s->states = {};
        } catch (const std::exception &e) {
            policy.Reject();
            Log(e.what());
        }
    }
    void Barriers(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
        std::lock_guard lock(mutex);
        TransparencyProducerBarriersLocked(cl, n, b);
        if (n > 4096 || (n && !b))
            return;
        for (auto &s : slots)
            if (s.pending_sr && s.sr.Get() == cl)
                for (UINT i = 0; i < n; ++i)
                    if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                        auto &t = b[i].Transition;
                        for (unsigned k = 0; k < 4; ++k)
                            if (t.pResource == s.gpu.sr[k].Get() &&
                                (t.Subresource == 0 ||
                                 t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
                                s.states[k] = {t.StateAfter, true,
                                               b[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE};
                    }
    }
    bool TryReplaceSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                      rrtrace::SlCall scope, HMODULE mod, UINT64 epoch, bool safe,
                      rrsparsecopy::Layouts &layouts) {
        std::lock_guard lock(mutex);
        if (!enabled || failed || retired || NeedsReconfigure() || !safe || !scope.valid)
            return false;
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(public_cl);
        auto *s = Match(scope);
        if (!native || !s || !s->pending_sr || s->sr.Get() != native.Get())
            return false;
        rrsr::StateProof proof;
        for (unsigned i = 0; i < 4; ++i) {
            proof.resources[i] = reinterpret_cast<uintptr_t>(s->gpu.sr[i].Get());
            proof.known[i] = layouts.Get(native.Get(), s->gpu.sr[i].Get(), proof.states[i]);
        }
        if (!rrsr::Eligible(proof))
            return false;
        for (unsigned i = 0; i < 4; ++i)
            s->states[i] = {proof.states[i], true, true};
        return AfterSR(public_cl, p, scope, NVSDK_NGX_Result_Success, mod, epoch, safe, true);
    }
    bool AfterSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                 rrtrace::SlCall scope, NVSDK_NGX_Result result, HMODULE mod, UINT64 epoch,
                 bool safe, bool replacement = false) {

        std::lock_guard lock(mutex);
        Slot *s{};
        for (auto &v : slots)
            if (v.pending_sr && v.frame == scope.frame && v.viewport == scope.viewport) {
                if (s) {
                    return false;
                }
                s = &v;
            }
        if (!s) {
            return false;
        }
        s->pending_sr = false;
        if (failed || retired)
            return false;
        try {
            auto cl = fg::NativeObject<ID3D12GraphicsCommandList>(public_cl);
            Require(LightingValid(*s) && safe &&
                        NVSDK_NGX_SUCCEED(result) && cl.Get() == s->sr.Get() && scope.valid &&
                        scope.feature == sl::kFeatureDLSS && mod,
                    "live original SR failed or scope changed");
            ComPtr<ID3D12Device> pub;
            Check(public_cl->GetDevice(IID_PPV_ARGS(&pub)), "live public device missing");
            if (!Key(s->options, mod, epoch, pub.Get(), scope.viewport)) {
                QueueReconfigure();
                return false;
            }
            auto input = rrtrace::ReadInputs(p);
            void *raw[] = {input.color, input.depth, input.motion, input.output};
            for (unsigned i = 0; i < 4; ++i)
                Require(fg::SameNativeObject((ID3D12Resource *)raw[i], s->gpu.sr[i].Get()),
                        "live SR input identity changed");
            auto &o = s->options;
            const UINT xs[] = {o.color_x, o.depth_x, o.motion_x},
                       ys[] = {o.color_y, o.depth_y, o.motion_y};
            for (unsigned i = 0; i < 4; ++i) {
                auto d = s->gpu.sr[i]->GetDesc();
                Require(d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                            d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1,
                        "live SR unsupported resource shape");
                if (i < 3)
                    Require(xs[i] <= d.Width && o.width <= d.Width - xs[i] && ys[i] <= d.Height &&
                                o.height <= d.Height - ys[i],
                            "live SR subrect exceeds resource");
                else
                    Require(d.Width == o.out_width && d.Height == o.out_height &&
                                (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                                (d.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                                 d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                 d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                                rrtrace::ReadUnsigned(
                                    p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0) == 0 &&
                                rrtrace::ReadUnsigned(
                                    p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0) == 0,
                            "live output shape/format/subrect unsupported");
            }
            const auto color_format = s->gpu.sr[0]->GetDesc().Format,
                       motion_format = s->gpu.sr[2]->GetDesc().Format;
            Require((color_format == DXGI_FORMAT_R11G11B10_FLOAT ||
                     color_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                     color_format == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                        (motion_format == DXGI_FORMAT_R16G16_FLOAT ||
                         motion_format == DXGI_FORMAT_R32G32_FLOAT),
                    "live color/motion format unsupported");
            Require(!s->states[3].known ||
                        (s->states[3].complete &&
                         s->states[3].state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    "live output observed state contradicts successful SR UAV postcondition");
            auto depth_format = s->gpu.sr[1]->GetDesc().Format;
            Require(depth_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                        depth_format == DXGI_FORMAT_R32_FLOAT,
                    "live depth format unsupported");
            std::array<D3D12_RESOURCE_STATES, 3> states;
            for (unsigned i = 0; i < 3; ++i) {
                auto &v = s->states[i];
                constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                Require(!v.known || (v.complete &&
                                     (v.state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
                                     !(v.state & ~read)),
                        "live post-SR observed input state conflicts");
                states[i] = v.known ? v.state : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            auto sr_inputs = s->gpu.sr;
            const auto native_depth = sr_inputs[1]->GetDesc();
            if (s->gpu.depth) {
                const auto cached = s->gpu.depth->GetDesc();
                if (native_depth.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
                    (cached.Width != native_depth.Width || cached.Height != native_depth.Height)) {
                    QueueReconfigure();
                    return false;
                }
            }
            if (!s->gpu.rr_output ||
                (native_depth.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT && !s->gpu.depth))
                Require(ReservePool(slots.size(), &o, &native_depth),
                        "live frame pool exceeds available GPU budget");
            s->gpu.PrepareSR(pipelines, budget, o, sr_inputs);
            Require(next_serial < UINT_MAX, "live serial overflow");
            s->serial = ++next_serial;
            const unsigned availability = !sss_enabled               ? 0
                                          : sss_failed               ? 1
                                          : !s->sss.command          ? 2
                                          : !s->sss.live             ? 3
                                          : !s->sss.written          ? 4
                                          : (o.color_x || o.color_y) ? 5
                                                                     : 6;
            s->sss_used = availability == 6;

            // Prebind only an already-written owned producer snapshot; first composition
            // may be recorded later on the CPU. Before seals exact original queue order,
            // and GPU Resolve writes zero without reading the snapshot on rejection.
            const auto &t = s->transparency;
            s->transparency_used =
                transparency_enabled && !transparency_failed &&
                (!t.producer_copy || !transparency_producer_suspended) && t.written &&
                t.prep_live && (t.producer_copy || (t.anchored && t.anchor_live)) && !o.color_x &&
                !o.color_y && s->gpu.transparency.guide &&
                s->gpu.transparency.Matches(t.source->GetDesc(), o.width, o.height) &&
                color_format == DXGI_FORMAT_R11G11B10_FLOAT;

            // An optional per-evaluation hint may be NULL without invalidating native
            // radiance/history. Explicit toggles and real frame rejection still Reject.
            s->hit_used = hit_enabled && s->gpu.hit_ready && s->gpu.specular_hit &&
                          !o.color_x && !o.color_y;

            const bool reset =
                policy.Next(s->frame, s->generation, o.reset == 1 || s->camera.reset == 1);
            if (reset)
                health.Reset(policy.LastReason());
            s->gpu.Header(s->serial, reset, replacement, o.color_x, o.color_y, tolerate_isolated,
                          responsivity_enabled ? responsivity_strength : 0.0f);
            rrinternal::Scope copy_guard;
            rrcompute::Tracker::Internal state_guard;
            s->recorded = true;
            if (!feature) {
                runtime = mod;
                runtime_epoch = epoch;
                public_device = pub;
                selected_view = scope.viewport;
                key = o;
                api = resolve(mod);
                Require(api.Valid(), "live NGX exports missing");
                Require(NVSDK_NGX_SUCCEED(api.allocate(&parameters)) && parameters,
                        "live NGX parameter allocation failed");
                rreval::CreateParameters(*parameters, o, preset.desired);
                Require(rrpreset::Requested(*parameters, preset.desired),
                        "live RR preset parameter readback failed");
                auto created = api.create(public_cl, NVSDK_NGX_Feature_RayReconstruction,
                                          parameters, &feature);
                Require(NVSDK_NGX_SUCCEED(created) && feature, "live RR feature creation failed");
                preset.Created();
            }
            // Written by the SSS resolve below; null keeps RR's default responsiveness.
            responsivity_active =
                rrresponsivity::Use(responsivity_enabled, responsivity_strength, s->sss_used,
                                    preset.has_effective, preset.effective);
            rreval::Resources resources{s->gpu.sr[0].Get(),
                                        s->gpu.use_depth ? s->gpu.depth.Get() : s->gpu.sr[1].Get(),
                                        s->gpu.sr[2].Get(),
                                        s->gpu.rr_output.Get(),
                                        s->gpu.guides[0].Get(),
                                        s->gpu.guides[1].Get(),
                                        s->gpu.guides[2].Get(),
                                        s->sss_used ? s->gpu.sss.guide.Get() : nullptr,
                                        s->transparency_used ? s->gpu.transparency.guide.Get()
                                                             : nullptr,
                                        responsivity_active ? s->gpu.sss.mask.Get() : nullptr,
                                        s->hit_used ? s->gpu.specular_hit.Get() : nullptr};
            auto projection = s->projection;
            if (rrskydepth::enabled.load(std::memory_order_relaxed)) {
                static bool described{};
                if (!described) {
                    described = true;
                    char line[200];
                    rrskydepth::Describe(projection, line, sizeof(line));
                    Log(line);
                }
                rrskydepth::Apply(projection);
            }
            rreval::EvaluateParameters(*parameters, o, resources, s->view.data(),
                                       projection.data());
            parameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
            s->gpu.Depth(cl.Get(), states[1]);
            for (unsigned i = 0; i < 3; ++i)
                if (i != 1 || !s->gpu.use_depth)
                    Change(cl.Get(), s->gpu.sr[i].Get(), states[i],
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
            s->gpu.GateBegin(cl.Get(), pipelines, history.Get());
            if (s->sss_used) {
                s->gpu.sss.Resolve(cl.Get(), pipelines, s->gpu.header.Get(), s->gpu.stats.Get(),
                                   history.Get());
            }
            if (s->transparency_used)
                s->gpu.transparency.Resolve(cl.Get(), transparency_pipeline, s->gpu.header.Get());
            if (s->hit_used)
                Change(cl.Get(), s->gpu.specular_hit.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            bool captured = false;
            if (capture.Active())
                captured = CaptureLocked(*s, cl.Get(), resources, o, reset, projection);

            NVSDK_NGX_Result evaluated_result = NVSDK_NGX_Result_FAIL_PlatformError;
            try {
                evaluated_result = api.evaluate(public_cl, feature, parameters, nullptr);
            } catch (...) {
                Log("RR evaluation threw; restoring resources for native SR");
            }
            s->inference_ok = NVSDK_NGX_SUCCEED(evaluated_result);
            if (captured && s->inference_ok)
                CaptureOutputLocked(*s, cl.Get(), resources, o);
            if (s->hit_used)
                Change(cl.Get(), s->gpu.specular_hit.Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (s->transparency_used)
                s->gpu.transparency.Finish(cl.Get());
            if (s->sss_used)
                s->gpu.sss.Finish(cl.Get());
            s->gpu.GateEnd(cl.Get(), pipelines, history.Get(), s->inference_ok);
            s->gate_recorded = true;
            s->replaced_sr = replacement && s->inference_ok;
            for (unsigned i = 0; i < 3; ++i)
                if (i != 1 || !s->gpu.use_depth)
                    Change(cl.Get(), s->gpu.sr[i].Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, states[i], 0);
            s->recorded = true;
            if (!s->inference_ok)
                Failure("RR inference failed; native SR retained");
            return s->inference_ok;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots) {
            if (s.pre.Get() == cl)
                s.pre_live = false;
            if (s.sss.command.Get() == cl)
                s.sss.live = false;
            if (s.transparency.preparation.Get() == cl)
                s.transparency.prep_live = false;
            if (s.transparency.anchor.Get() == cl)
                s.transparency.anchor_live = false;
            if (s.sr.Get() == cl) {
                s.sr_live = false;
                s.pending_sr = false;
            }
            if (!s.recorded && (s.pre.Get() == cl || s.sr.Get() == cl))
                policy.Reject();
        }
        PollLocked();
    }
    void Before(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists) {
        Call call;
        submissions.push_back(std::move(call));
        if (n && !lists)
            return;
        mutex.lock();
        submissions.back().locked = true;
        submissions.back().touched.resize(slots.size());
        std::vector<int> pre(slots.size(), -1), producer(pre), consumer(pre), glass_pre(pre),
            glass_anchor(pre);
        bool any = false;
        auto index = [&](ID3D12CommandList *c) { return rrsss::RecordingIndex(n, lists, c); };
        for (unsigned k = 0; k < slots.size(); ++k) {
            const auto &s = slots[k];
            pre[k] = s.pre_live ? index(s.pre.Get()) : -1;
            producer[k] = s.sss.live ? index(s.sss.command.Get()) : -1;
            consumer[k] = s.sr_live ? index(s.sr.Get()) : -1;
            glass_pre[k] = s.transparency.prep_live ? index(s.transparency.preparation.Get()) : -1;
            glass_anchor[k] = s.transparency.anchor_live ? index(s.transparency.anchor.Get()) : -1;
            if (rrsss::Present(pre[k]) || rrsss::Present(producer[k]) ||
                rrsss::Present(consumer[k]) || rrsss::Present(glass_pre[k]) ||
                rrsss::Present(glass_anchor[k])) {
                submissions.back().touched[k] = true;
                any = true;
            }
        }
        if (!any)
            return;
        bool queue_ready = false;
        try {
            ComPtr<ID3D12Device> d;
            Check(q->GetDevice(IID_PPV_ARGS(&d)), "live queue device missing");
            Require(fg::SameNativeObject(d.Get(), pipelines.device.Get()),
                    "live queue identity mismatch");
            if (last_queue && last_queue.Get() != q && signaled) {
                const auto wait_result = q->Wait(fence.Get(), signaled);
                Check(wait_result, "live cross-queue GPU wait failed");
            }
            if (!history_queued) {
                rrinternal::Scope guard;
                ID3D12CommandList *setup = init_list.Get();
                {
                    q->ExecuteCommandLists(1, &setup);
                }
                history_queued = true;
            }
            last_queue = q;
            queue_ready = q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        } catch (const std::exception &e) {
            for (unsigned k = 0; k < slots.size(); ++k)
                if (submissions.back().touched[k])
                    slots[k].poisoned = true;
            Failure(e.what());
        }
        for (unsigned k = 0; k < slots.size(); ++k) {
            if (!submissions.back().touched[k])
                continue;
            auto &s = slots[k];
            const int a = pre[k], c = producer[k], b = consumer[k];
            if (s.recorded && !s.sealed && rrsss::Present(b)) {
                s.sealed = true;
                const bool baseline = queue_ready && a >= 0 && b >= a && !s.pre_submitted &&
                                      !failed && s.gate_recorded && s.inference_ok &&
                                      LightingValid(s);
                const bool sss_accept =
                    s.sss_used && s.sss.written &&
                    rrsss::Admit(a, c, b, s.pre_submitted, s.sss.submitted, baseline);
                s.sss.admission.Seal(b, sss_accept);
                s.gpu.header_map[7] = sss_accept ? 1u : 0u;
                s.transparency_accepted =
                    s.transparency_used && s.transparency.written && s.transparency.anchored &&
                    (s.transparency.producer_copy
                         ? rrtrans::AdmitProducer(a, glass_pre[k], glass_anchor[k], b,
                                                  s.transparency.prep_submitted,
                                                  s.transparency.anchor_submitted, baseline)
                     : s.transparency.at_draw
                         ? rrtrans::AdmitAtDraw(a, glass_pre[k], glass_anchor[k], b,
                                                s.transparency.prep_submitted,
                                                s.transparency.anchor_submitted, baseline)
                         : rrtrans::Admit(a, glass_pre[k], glass_anchor[k], b,
                                          s.transparency.prep_submitted,
                                          s.transparency.anchor_submitted, baseline));
                if (s.transparency_used && s.transparency.producer_copy &&
                    !s.transparency_accepted) {
                    transparency_producer_suspended = true;
                }
                const bool accepted = baseline && (!s.sss_used || sss_accept) &&
                                      (!s.transparency_used || s.transparency_accepted);
                s.gpu.Accept(accepted);

                if (!accepted)
                    policy.Reject();
            } else if (s.recorded && s.sealed && rrsss::Present(b)) {
                policy.Reject();
                Log("RR replayed SR recording; immutable acceptance retained; GPU serial gate "
                    "suppresses output and requires reset");
            }
            if (rrsss::Present(a))
                s.pre_submitted = true;
            if (rrsss::Present(c))
                s.sss.submitted = true;
            if (rrsss::Present(glass_pre[k]))
                s.transparency.prep_submitted = true;
            if (rrsss::Present(glass_anchor[k]))
                s.transparency.anchor_submitted = true;
        }
    }
    void After(ID3D12CommandQueue *q) {
        if (submissions.empty())
            return;
        auto call = submissions.back();
        submissions.pop_back();
        if (!call.locked)
            return;
        {
            std::lock_guard lock(mutex);
            bool any = false;
            for (bool v : call.touched)
                any |= v;
            if (any) {
                const auto value = ++signaled;
                const auto signal_result = q->Signal(fence.Get(), value);
                if (FAILED(signal_result)) {
                    for (size_t k = 0; k < call.touched.size(); ++k)
                        if (call.touched[k])
                            slots[k].poisoned = true;
                    Failure("RR native completion fence failed; allocations retained");
                } else
                    for (size_t k = 0; k < call.touched.size(); ++k)
                        if (call.touched[k]) {
                            slots[k].fence_value = value;
                            slots[k].reported = false;
                        }
            }
        }
        mutex.unlock();
    }
    bool BeforeShutdown(HMODULE mod, ID3D12Device *d) {
        std::lock_guard lock(mutex);
        if (runtime != mod || (d && !fg::SameNativeObject(public_device.Get(), d)))
            return true;
        enabled = false;
        for (const auto &s : slots)
            if (s.poisoned || s.pre_live || s.sr_live || s.sss.live || s.transparency.Live() ||
                s.pending_sr) {
                Log("RR shutdown deferred: an RR command recording remains replayable; "
                    "reset/dispose required");
                return false;
            }
        rrprivate::FencePoint point{fence, signaled};
        if (!point.Wait(30000)) {
            Failure("RR shutdown deferred: live GPU inference unfinished");
            return false;
        }
        return true;
    }
    void AfterShutdown(HMODULE mod, ID3D12Device *d, bool success) {
        std::lock_guard lock(mutex);
        if (success && runtime == mod && (!d || fg::SameNativeObject(public_device.Get(), d))) {
            feature = nullptr;
            parameters = nullptr;
            preset.has_effective = false;
            preset.draining = false;
            retired = true;
            enabled = false;
            Log("RR NGX shutdown invalidated private handles; live mode retired");
            CaptureFailed("RR shut down");
        }
    }
};
} // namespace rrlive
