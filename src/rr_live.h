#pragma once
#include "rr_live_gpu.h"
#include "rr_live_policy.h"
#include "rr_guide_health.h"
#include "rr_ray_density.h"
#include "rr_responsivity.h"
#include "rr_water.h"
#include "rr_sky_depth_test.h"
#include "rr_vrrt_apply_patch.h"
#include "rr_weapon_depth.h"
#include "rr_sky_edge.h"
#include "rr_color_scale.h"
#include "rr_status.h"
#include "rr_compute_state.h"
#include "rr_sparse_copy.h"
#include "rr_sss_producer.h"
#include "rr_sss_mask.h"
#include "rr_inputs.h"
#include "rr_internal.h"
#include "rr_ngx_api.h"
#include "rr_ngx_log.h"
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
        bool sss_used{}, awaiting_prepass{}, hit_used{}, water_used{};
        // The hit distance tests conditioned this slot's copy, in mode (rrhitcond).
        bool hit_conditioned{};
        unsigned hit_mode{};
        rrtrans::Ticket transparency;
        bool transparency_used{}, transparency_accepted{};
        // The color brightness test kept the SSS and transparency guides from RR.
        bool guides_withheld{};
        bool responsivity_used{}; // RR was given a responsivity mask (sky edge test, water or SSS)
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
        // The ray budget test (rr_ray_budget.h); `s = {}` clears it at retirement. ray_lists is
        // this recording's apply proof, ray_budget/ray_armed the raised lobes and arming snapshot
        // of the Budget that apply read, ray_patched/ray_written/ray_patch_armed the patch's,
        // ray_serial the recording's ray-log serial (0 until a ray site ran), ray_done that the
        // recording was judged or counted unverified. ray_resolve_budget_* is the Budget identity
        // its resolve proved (0 when not proven): an apply reading it is same-frame (A2).
        // ray_old_mark: an apply consumed a mark of an older generation (counted unverified).
        rrraybudget::Lists ray_lists;
        UINT64 ray_apply_cb{}, ray_resolve_cb{};
        UINT64 ray_resolve_budget_resource{}, ray_resolve_budget_offset{};
        unsigned ray_logged{}, ray_patched{}, ray_budget{};
        std::array<UINT, 2> ray_written{};
        bool ray_resolved{}, ray_applied{}, ray_done{}, ray_old_mark{};
        UINT ray_serial{};
        rrraybudget::Armed ray_armed{}, ray_patch_armed{};
        rrcapture::Capture captured; // The frame recorded on this slot, if any.
        // Held until the slot retires: a replayed SR recording must never copy into freed memory.
        ComPtr<ID3D12Resource> capture_hold;
        unsigned capture_session{}; // The capture session capture_hold was recorded in.
        // TryReplaceSR's proof when it was not eligible while a capture was active.
        rrsr::StateProof proof;
        bool proof_failed{};
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
    // Development tests (sky edge mask, color brightness, sky depth floor, weapon
    // depth, hit distance scratch): one texture each, shared by every slot, created
    // on first use and released by Reconfigure. They sit in the persistent ledger,
    // so pool projections count each once, and the reserve grows by exactly their
    // size (test_reserve, given back on release).
    struct DevTest {
        ComPtr<ID3D12Resource> texture;
        bool active{};     // the latest evaluation used it
        bool blocked{};    // no GPU memory: retried once its setting changes
        unsigned changes{}; // the setting's change count when it was blocked
    };
    // wide_output: RR's output while the color test brightens its input, in FP32
    // so the brightened result does not clip in the FP16 rr_output.
    // weapon_test: the linear depth RR reads while the weapon depth test runs.
    // hit_scratch: the raw hit distance the conditioning reads while it rewrites
    // a slot's copy; used only inside one guide recording.
    DevTest sky_edge_test, color_test, depth_test, wide_output, weapon_test, hit_scratch;
    UINT64 test_reserve{};
    // Weapon depth test (rrweapondepth): weapon_linear is the depth type the current RR
    // feature was created with; weapon_hold says why the test stays off until its setting
    // next changes. The hold outlives Reconfigure, so a test that cannot run restarts RR
    // once, never over and over.
    enum class WeaponHold { None, Memory, Projection, Refused };
    bool weapon_linear{};
    WeaponHold weapon_hold{};
    unsigned weapon_hold_changes{};
    rrweapondepth::Mapping weapon_logged{}; // The mapping last written to the log.
    // Color scale RR's history was built with (color brightness test).
    float applied_color_scale = 1.0f;
    float responsivity_strength = rrresponsivity::DefaultStrength;
    // Running water tests (rr_water.h): session-only, off by default.
    rrwater::Settings water;
    rrwater::Status water_status;
    rrwater::Tracker water_stats;
    bool water_failed{};
    UINT64 water_retry_after{};
    bool transparency_enabled{}, transparency_failed{};
    rrtrans::Pipeline transparency_pipeline;
    ComPtr<ID3D12Resource> transparency_source;
    bool sss_enabled{}, sss_failed{};
    UINT64 sss_retry_after{};
    // Latest guide an admitted SR submission resolves, for SSS-masked lighting.
    rrsssmask::Record sss_mask;
    ComPtr<ID3D12Resource> sss_mask_guide;
    bool hit_enabled{};
    unsigned hit_reports{};
    // The hit distance tests' pipelines could not be created: off this session.
    bool hit_condition_failed{};
    rrhitcond::Tracker hit_pixels;
    HMODULE runtime{};
    UINT64 runtime_epoch{};
    ComPtr<ID3D12Device> public_device;
    // A creation attempt's runtime, logged by Poll after it releases the mutex: module and DXGI
    // queries never run under it. The probe pins the NGX core module.
    struct RuntimeReport {
        HMODULE core{};
        ComPtr<ID3D12Device> device;
    } runtime_report;
    rreval::Api api;
    NVSDK_NGX_Parameter *parameters{};
    NVSDK_NGX_Handle *feature{};
    rreval::Options key;
    rreval::Api (*resolve)(HMODULE) = &rreval::Api::Load;
    rrcapture::Session capture;
    std::shared_ptr<rrcapture::Outcome> capture_outcome;
    unsigned capture_lighting{};
    unsigned capture_logged{}; // The capture session whose route and input states were logged.
    void Log(const char *s) {
        if (logger)
            logger(s);
    }

    // Whether a development test runs in this evaluation; creates its shared
    // texture on first use. The reserve first grows by the texture's size on top
    // of everything it already holds, so the texture never takes room the pool or
    // the SSS and transparency guides counted on (Reconfigure gives the growth
    // back). This needs free VRAM for the whole unused reserve, so a test can stay
    // off on a tight card; a failure only keeps the test off (RR keeps running)
    // until its setting changes.
    bool UseTest(DevTest &t, bool wanted, unsigned changes, UINT w, UINT h, DXGI_FORMAT format,
                 const char *name) {
        t.active = false;
        if (!wanted)
            return false;
        if (t.blocked && t.changes == changes)
            return false;
        t.blocked = false;
        if (!t.texture) {
            const auto d = TextureDesc(w, h, format);
            const UINT64 n = pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            const UINT64 old = budget.limit;
            bool ok = n && n != UINT64_MAX && old <= UINT64_MAX - n &&
                      budget.Reserve(pipelines.device.Get(), old + n);
            if (ok) {
                try {
                    t.texture = persistent.Allocate(pipelines, budget, d);
                    test_reserve += budget.limit - old;
                } catch (const std::exception &) {
                    budget.limit = old;
                    ok = false;
                }
            }
            if (!ok) {
                t.blocked = true;
                t.changes = changes;
                char line[160];
                snprintf(line, sizeof(line),
                         "%s: not enough GPU memory; test off until its setting changes", name);
                Log(line);
                return false;
            }
        }
        const auto d = t.texture->GetDesc();
        if (d.Width != w || d.Height != h || d.Format != format)
            return false;
        return t.active = true;
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
    // The mandatory depth copy for a w x h input, as the SSS and glass
    // reservations protect it.
    UINT64 DepthCopyBytes(UINT w, UINT h) const {
        if (!pipelines.device)
            return 0;
        auto texture = TextureDesc(w, h, DXGI_FORMAT_R32_FLOAT);
        texture.Flags = D3D12_RESOURCE_FLAG_NONE;
        const auto buffer = rrprivate::Buffer(((UINT64(w) * 4 + 255) & ~255ull) * h);
        UINT64 total{};
        for (const auto &d : {texture, buffer}) {
            const auto n = pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
                return 0;
            total += n;
        }
        return total;
    }
    // Water storage is optional and all or nothing: a slot runs the water tests
    // only while every prepared slot holds the storage, so RR never sees them
    // switch on and off from frame to frame. Like the SSS storage it waits for
    // a mandatory SR allocation, then reserves the whole pool with water
    // storage on every slot plus the prepass and depth copy the SSS and glass
    // reservations protect, so no later mandatory reservation needs more memory
    // because of it. A slot used for the first time later gets its storage at
    // that first prepass. A budget or memory shortage leaves the water tests
    // waiting and retries a second later; any other failure turns them off for
    // the session instead of turning RR off.
    bool PrepareWater(GpuFrame &frame, UINT64 now) {
        bool complete = frame.water.ready, warm = frame.rr_output != nullptr;
        for (const auto &s : slots) {
            complete = complete && (!s.gpu.heap || s.gpu.water.ready);
            warm = warm || s.gpu.rr_output != nullptr;
        }
        if (complete)
            return true;
        if (!warm || now < water_retry_after)
            return false;
        try {
            const auto need = GpuFrame::WaterBytes(pipelines.device.Get(), frame.w, frame.h);
            const auto depth = need ? DepthCopyBytes(frame.w, frame.h) : 0;
            bool ready = depth && ReservePool(slots.size(), nullptr, nullptr, frame.w, frame.h,
                                              0, need, depth);
            for (auto &s : slots)
                if (ready && s.gpu.heap && !s.gpu.water.ready)
                    ready = s.gpu.PrepareWater(pipelines, budget);
            if (ready) {
                water_retry_after = 0;
                return true;
            }
            if (!water_retry_after)
                Log("Water tests waiting for GPU budget; allocation will retry");
        } catch (const rrprivate::AllocationDeferred &) {
            if (!water_retry_after)
                Log("Water tests waiting for GPU memory; allocation will retry");
        } catch (const std::exception &e) {
            water_failed = true;
            const std::string line = std::string("Water tests unavailable: ") + e.what();
            Log(line.c_str());
            return false;
        }
        water_retry_after = now + 1000;
        return false;
    }
    // done is the fence's completed value, which PollLocked reads once for every slot.
    bool Complete(const Slot &s, UINT64 done) const {
        if (s.poisoned)
            return false;
        if (!s.fence_value)
            return !s.sealed;
        return done != UINT64_MAX && done >= s.fence_value;
    }
    bool Complete(const Slot &s) const {
        return Complete(s, (s.poisoned || !s.fence_value) ? 0 : fence->GetCompletedValue());
    }
    void Failure(const char *why) {
        failed = true;
        policy.Reject();
        Log(why);
        CaptureFailed("RR failed");
    }
    // A failed retirement call logs its NGX result; a missing export has none.
    bool RetirementFailed(const char *what, bool exported, NVSDK_NGX_Result result) {
        char line[160];
        if (exported)
            snprintf(line, sizeof(line), "%s (%s); allocations retained", what,
                     rrngxlog::Code(result).text);
        else
            snprintf(line, sizeof(line), "%s (export missing); allocations retained", what);
        Failure(line);
        return false;
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
        return layout_pending || (preset.has_effective && preset.desired != preset.effective) ||
               (feature && weapon_linear != WeaponDepthWanted());
    }
    // The weapon depth test's setting, unless held off since its last change. RR's depth
    // type is fixed at creation, so a new feature takes it and a change restarts RR.
    bool WeaponDepthWanted() const {
        return rrweapondepth::enabled.load(std::memory_order_relaxed) &&
               (weapon_hold == WeaponHold::None ||
                weapon_hold_changes != rrweapondepth::changes.load(std::memory_order_relaxed));
    }
    // outcome (Refused): why NGX refused, an rrngxlog::Code text, "null handle" or "threw".
    void HoldWeaponDepth(WeaponHold why, unsigned changes, const char *outcome = nullptr) {
        // Frames already in flight on the same RR repeat the same hold: log it once.
        if (weapon_hold == why && weapon_hold_changes == changes)
            return;
        weapon_hold = why;
        weapon_hold_changes = changes;
        // UseTest already logged a memory failure.
        if (why == WeaponHold::Projection)
            Log("Weapon depth test: the game's projection does not map to linear depth; test "
                "off until its setting changes");
        else if (why == WeaponHold::Refused) {
            char line[200];
            snprintf(line, sizeof(line),
                     "Weapon depth test: RR did not accept linear depth%s%s%s; test off until its "
                     "setting changes, RR runs with hardware depth",
                     outcome ? " (" : "", outcome ? outcome : "", outcome ? ")" : "");
            Log(line);
        }
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
            const bool exported = api.release != nullptr;
            const auto r = exported ? api.release(feature) : NVSDK_NGX_Result_Fail;
            if (!exported || NVSDK_NGX_FAILED(r))
                return RetirementFailed("RR feature retirement failed", exported, r);
            feature = nullptr;
            preset.Retired();
        }
        if (parameters) {
            const bool exported = api.destroy != nullptr;
            const auto r = exported ? api.destroy(parameters) : NVSDK_NGX_Result_Fail;
            if (!exported || NVSDK_NGX_FAILED(r))
                return RetirementFailed("RR parameter retirement failed", exported, r);
            parameters = nullptr;
        }
        for (auto &s : slots) {
            RayUnverified(s);
            s.gpu.Release(budget);
            s = {};
        }
        water_retry_after = 0;
        sss_mask = {};
        sss_mask_guide.Reset();
        history.Reset();
        zero.Reset();
        persistent.Release(budget);
        // The test textures were in the persistent ledger and the ray buffers in the slots;
        // give back the reserve they added (test_reserve, ray_reserve, saturating), never below
        // what is still allocated.
        sky_edge_test = color_test = depth_test = wide_output = weapon_test = hit_scratch = {};
        weapon_linear = false;
        weapon_logged = {};
        const UINT64 reserved =
            test_reserve > UINT64_MAX - ray_reserve ? UINT64_MAX : test_reserve + ray_reserve;
        budget.limit = (std::max)(
            {Budget::Minimum, budget.bytes, budget.limit - (std::min)(reserved, budget.limit)});
        test_reserve = ray_reserve = 0;
        applied_color_scale = 1.0f;
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
        RayRestart(); // the ray budget test proves the new generation's lists again
        for (auto &p : rrraybudget::published)
            p.store(0, std::memory_order_relaxed);
        ray_list_offsets = {};
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
        // One read for both checks of every slot, so a slot never retires a frame it did not
        // report. Only a signaled slot has a fence value, and only with a fence.
        const UINT64 done = fence ? fence->GetCompletedValue() : 0;
        for (auto &s : slots) {
            if (s.recorded && s.sealed && !s.reported && Complete(s, done)) {
                auto v = s.gate_recorded ? s.gpu.ReadStats() : std::array<UINT, 4>{1, 0, 0, 0};
                s.reported = true;
                // After reported, as the readers below: a failed map is then reported once.
                const auto finite = s.gate_recorded ? s.gpu.ReadFinite() : std::array<UINT, 2>{};
                const bool sss = s.sss_used && !s.guides_withheld && s.sss.admission.accepted;
                const bool glass =
                    s.transparency_used && !s.guides_withheld && s.transparency_accepted;
                health.Frame(v,
                             {finite[0], finite[1], sss, glass, s.hit_used, s.responsivity_used});
                if (s.gate_recorded && s.native_sampling) {
                    const auto rates = s.gpu.ReadRates();
                    density.Add(rates);
                    RayCompleted(s, rates);
                } else
                    RayUnverified(s); // raised frames without reader stats are not verified
                if (s.gate_recorded && s.hit_conditioned)
                    hit_pixels.Add(s.gpu.ReadHitStats(), s.hit_mode, UINT64(s.gpu.w) * s.gpu.h);
                if (s.gate_recorded && s.water_used && v[0] == 0 && v[1] != 0)
                    water_stats.Add(s.gpu.ReadWaterPixels(),
                                    UINT64((std::min)(s.options.width, s.gpu.w)) *
                                        (std::min)(s.options.height, s.gpu.h));
                status.Completed(s.serial, s.generation, GetTickCount64(), v[0] == 0 && v[1] != 0,
                                 sss, glass, s.replaced_sr, s.hit_used);
                if (s.captured.buffer) {
                    auto &m = s.captured.meta;
                    m.stats = v;
                    m.delivered = v[0] == 0 && v[1] != 0;
                    m.replaced_sr = s.replaced_sr;
                    m.hit_distance_test = s.hit_used && s.hit_conditioned ? s.hit_mode : 0u;
                    m.water_masks = s.water_used;
                    m.water_guide = s.gpu.water_guide;
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
                !s.pending_sr && Complete(s, done)) {
                RayUnverified(s);
                // Another session's buffer is only dropped (s = {} below), never pooled.
                if (s.captured.buffer && s.captured.session == capture.session) {
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
        const UINT64 now = GetTickCount64();
        std::string report;
        if (health.Report(now, report))
            Log(report.c_str());
        if (density.Report(now, report))
            Log(report.c_str());
        if (hit_pixels.Report(GetTickCount64(), report))
            Log(report.c_str());
        if (water_stats.Report(GetTickCount64(), report, water))
            Log(report.c_str());
        RayReport(now);
    }
    // A submitted SR recording that resolves its guide hands it to later
    // SSS-masked lighting if admitted. A rejected one writes zeros, so a record
    // of that same guide is dropped. A discarded recording never gets here.
    void SealSssMask(const Slot &s, bool accepted) {
        if (accepted) {
            sss_mask = {true, s.frame, s.generation, s.gpu.w, s.gpu.h};
            sss_mask_guide = s.gpu.sss.guide;
        } else if (sss_mask_guide && sss_mask_guide == s.gpu.sss.guide) {
            sss_mask = {};
            sss_mask_guide.Reset();
        }
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
        // Motion vectors are render size only when RR was created with MVLowRes.
        const bool low_res = (o.flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        sources[rrcapture::Motion] = {r.motion, o.motion_x, o.motion_y, low_res ? 0 : o.out_width,
                                      low_res ? 0 : o.out_height};
        sources[rrcapture::NormalRoughness] = {r.normal};
        sources[rrcapture::DiffuseAlbedo] = {r.diffuse};
        sources[rrcapture::SpecularAlbedo] = {r.specular};
        sources[rrcapture::Status] = {s.gpu.guides[3].Get()};
        sources[rrcapture::SssGuide] = {r.sss};
        sources[rrcapture::ColorBeforeTransparency] = {r.before_transparency};
        sources[rrcapture::ResponsivityMask] = {r.responsivity};
        sources[rrcapture::SpecularHitDistance] = {r.specular_hit};
        sources[rrcapture::DisocclusionMask] = {r.disocclusion};
        sources[rrcapture::AnimatedTextureMask] = {r.animated};
        // RR's output (wide or not) is in UNORDERED_ACCESS where CaptureOutputLocked runs.
        sources[rrcapture::RrOutput] = {
            r.output, 0, 0, o.out_width, o.out_height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        return sources;
    }
    // Once per capture: the route, the state each game input was copied from and where it came
    // from, and on the native route why replacement was not eligible.
    void LogCaptureRoute(const Slot &s, const rrcapture::Sources &sources,
                         const std::array<unsigned, rrcapture::InputCount> &from,
                         bool replacement) {
        char inputs[3][64], why[256] = "", line[448];
        for (unsigned k = rrcapture::Color; k <= rrcapture::Motion; ++k)
            snprintf(inputs[k], sizeof(inputs[k]), "%s %s 0x%x", rrcapture::names[k],
                     sources[k].resource ? rrcapture::StateSourceName(from[k]) : "none",
                     unsigned(sources[k].state));
        if (!replacement && s.proof_failed)
            rrsr::Describe(s.proof, why, sizeof(why));
        snprintf(line, sizeof(line), "RR capture: %s; input states: %s, %s, %s%s%s",
                 replacement ? "RR replaces SR" : "native SR", inputs[0], inputs[1], inputs[2],
                 replacement      ? ""
                 : s.proof_failed ? "; replacement not eligible: "
                                  : "; replacement not tried",
                 why);
        Log(line);
    }
    // Copies what RR is given this frame, plus the Status guide, before RR reads it: inputs
    // [0, RrOutput), each from the state it is in here, where Record leaves it. The addon's own
    // resources are in NON_PIXEL_SHADER_RESOURCE. A game input is in NPSR once its state was
    // proven (replacement path, or barriers seen in the SR call: AfterSR moved it there); else
    // nothing moved it, so it is copied from the tracker's state when the tracker (layouts, keyed
    // by the native list cl) knows it, else from NPSR, the DLSS input contract RR's own evaluate
    // assumes. Each region records which. Returns whether it recorded this frame;
    // CaptureOutputLocked then adds RR's output.
    bool CaptureLocked(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Resources &r,
                       const rreval::Options &o, bool reset,
                       const std::array<float, 16> &projection, bool replacement,
                       rrsparsecopy::Layouts *layouts) {
        try {
            const bool armed = capture.state == rrcapture::State::Armed;
            // A frame already past Prepass can still evaluate after RR is turned off or while a
            // reconfigure is pending; arming then would hold the buffers with nothing to collect.
            if (armed && (!enabled || NeedsReconfigure()))
                return false;
            if (!armed) {
                unsigned in_flight = 0;
                for (const auto &v : slots)
                    if (v.captured.buffer && v.captured.session == capture.session)
                        ++in_flight;
                if (!capture.Wants(in_flight))
                    return false;
            }
            auto sources = CaptureSources(s, r, o);
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
            std::array<unsigned, rrcapture::InputCount> from{}; // rrcapture::StateSource
            for (unsigned k = rrcapture::Color; k <= rrcapture::Motion; ++k) {
                auto &v = sources[k];
                if (!v.resource || v.resource != s.gpu.sr[k].Get())
                    continue; // The addon's own copy or test texture.
                D3D12_RESOURCE_STATES tracked{};
                if (s.states[k].known) {
                    from[k] = rrcapture::Proven;
                } else if (layouts && layouts->Get(cl, v.resource, tracked)) {
                    from[k] = rrcapture::Tracked;
                    v.state = tracked;
                } else {
                    from[k] = rrcapture::Contract;
                }
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
            m.lighting_input = capture_lighting;
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
            for (unsigned i = 0; i < rrcapture::InputCount; ++i) {
                frame.regions[i].state_source = from[i];
                frame.regions[i].state_before = unsigned(sources[i].state);
            }
            if (capture_logged != capture.session) {
                capture_logged = capture.session;
                LogCaptureRoute(s, sources, from, replacement);
            }
            // Nothing below throws: the recorded copies always keep their buffer alive.
            rrcapture::Record(cl, frame, sources, 0, rrcapture::RrOutput);
            s.capture_hold = frame.buffer;
            s.capture_session = frame.session;
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
    // rr_output is in UNORDERED_ACCESS here (CaptureSources), where Record leaves it for GateEnd.
    void CaptureOutputLocked(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Resources &r,
                             const rreval::Options &o) {
        try {
            rrcapture::Record(cl, s.captured, CaptureSources(s, r, o), rrcapture::RrOutput,
                              rrcapture::InputCount);
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
            // As RR was created: one feature per run, so one depth type (CreateParameters).
            h.create_flags = unsigned(rreval::CreateFlags(key.flags, done[0].meta.linear_depth));
            h.sss_enabled = sss_enabled;
            h.transparency_enabled = transparency_enabled;
            h.specular_hit_enabled = hit_enabled;
            h.tolerate_isolated = tolerate_isolated;
            h.responsivity_enabled = responsivity_enabled;
            h.responsivity_strength = responsivity_strength;
            h.guide_test = rrguidetest::Valid(rrguidetest::mode.load(std::memory_order_relaxed));
            h.sky_depth_test = rrskydepth::enabled.load(std::memory_order_relaxed);
            h.water_responsivity = water.responsivity;
            h.water_strength = water.strength;
            h.water_discard = water.discard;
            h.water_animated = water.animated;
            h.water_guide = water.guide;
            // One RR history, so one feature and one depth type for the whole run, and Settle
            // keeps one lighting input.
            h.weapon_depth = done[0].meta.linear_depth;
            h.sky_edge_enabled = rrskyedge::enabled.load(std::memory_order_relaxed);
            h.sky_edge_strength =
                rrresponsivity::Clamp(rrskyedge::strength.load(std::memory_order_relaxed));
            h.sky_edge_radius =
                rrskyedge::ClampRadius(rrskyedge::radius.load(std::memory_order_relaxed));
            h.ray_selection = rrapply::last_variant.load(std::memory_order_relaxed);
            h.lighting_input = done[0].meta.lighting_input;
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
#include "rr_live_ray.inl"
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
    // Development test state for the panel, for the current setting. Active: the
    // latest evaluation used it and RR is still running. Blocked: no GPU memory
    // until its setting changes.
    enum class TestState { Idle, Active, Blocked };
    TestState SkyEdgeTest() {
        std::lock_guard lock(mutex);
        return Test(sky_edge_test, rrskyedge::changes.load(std::memory_order_relaxed),
                    rrskyedge::Use(rrskyedge::enabled.load(std::memory_order_relaxed),
                                   rrskyedge::strength.load(std::memory_order_relaxed),
                                   preset.has_effective, preset.effective));
    }
    TestState ColorTest() {
        std::lock_guard lock(mutex);
        const auto changes = rrcolorscale::changes.load(std::memory_order_relaxed);
        const float scale =
            rrcolorscale::Factor(rrcolorscale::mode.load(std::memory_order_relaxed));
        if (scale > 1.0f && Test(wide_output, changes, true) == TestState::Blocked)
            return TestState::Blocked;
        return Test(color_test, changes, scale != 1.0f);
    }
    TestState DepthFloorTest() {
        std::lock_guard lock(mutex);
        const bool wanted = rrguidetest::Valid(rrguidetest::mode.load(std::memory_order_relaxed)) ==
                            rrguidetest::SkySurface;
        // While the weapon depth test runs, its linear depth carries the floor instead.
        if (wanted && weapon_linear && weapon_test.active)
            return Test(weapon_test, weapon_test.changes, true);
        return Test(depth_test, rrguidetest::changes.load(std::memory_order_relaxed), wanted);
    }
    // Weapon depth test state for the panel. Restarting: RR is being recreated with the
    // depth type the setting asks for (the change takes effect once in-flight frames retire).
    enum class WeaponState { Off, Active, Waiting, Restarting, NoMemory, Unsupported, Refused };
    WeaponState WeaponDepthState() {
        std::lock_guard lock(mutex);
        const bool running = enabled && !failed && !retired && feature;
        if (!rrweapondepth::enabled.load(std::memory_order_relaxed))
            return running && weapon_linear ? WeaponState::Restarting : WeaponState::Off;
        if (!WeaponDepthWanted())
            return weapon_hold == WeaponHold::Memory       ? WeaponState::NoMemory
                   : weapon_hold == WeaponHold::Projection ? WeaponState::Unsupported
                                                           : WeaponState::Refused;
        if (!running)
            return WeaponState::Waiting;
        if (!weapon_linear)
            return WeaponState::Restarting;
        return weapon_test.active ? WeaponState::Active : WeaponState::Waiting;
    }
    // The specular hit distance tests (rrhitcond), while one is on and the hit
    // distance is sent. Failed: their pipelines could not be created.
    TestState HitDistanceTest() {
        std::lock_guard lock(mutex);
        return Test(hit_scratch, rrhitcond::changes.load(std::memory_order_relaxed),
                    rrhitcond::Mode() != 0 && hit_enabled);
    }
    bool HitDistanceTestFailed() {
        std::lock_guard lock(mutex);
        return hit_condition_failed;
    }
    bool Running() {
        std::lock_guard lock(mutex);
        return enabled && !failed && !retired && feature;
    }
    // Running water tests (rr_water.h). Nothing is allocated or sent to RR
    // until a test is on; then each slot keeps its water storage until the
    // next rebuild (Reconfigure), like the SSS storage. Every test, the guide
    // mode included, applies from the first prepass that has that storage. A
    // nonzero colour subrect origin keeps the guide mode but drops the masks,
    // as it drops the SSS and glass guides.
    void SetWater(const rrwater::Settings &value) {
        std::lock_guard lock(mutex);
        water = value;
        water.strength = rrwater::Clamp(value.strength);
        water.guide = rrwater::ValidGuide(value.guide);
        if (!rrwater::Wanted(water))
            water_retry_after = 0;
    }
    // What the latest evaluation sent, limited to what would still be sent now.
    rrwater::Status WaterStatus() {
        std::lock_guard lock(mutex);
        auto v = water_status;
        v.wanted = rrwater::Wanted(water);
        v.unavailable = water_failed;
        const bool live = v.wanted && enabled && !failed && !retired && !water_failed;
        v.waiting_memory = live && water_retry_after != 0;
        v.ready = v.ready && live;
        v.responsivity = v.responsivity && v.ready &&
                         rrwater::UseResponsivity(water, true, preset.has_effective,
                                                  preset.effective);
        v.discard = v.discard && v.ready && water.discard;
        v.animated = v.animated && v.ready && water.animated;
        v.coverage = v.ready ? float(water_stats.Last()) : -1.0f;
        return v;
    }
    void SetSssEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (sss_enabled != value) {
            sss_enabled = value;
            sss_mask = {};
            sss_mask_guide.Reset();
            policy.Reject();
        }
    }
    // The SSS guide an SSS-masked first spatial pass on cl's frame reads, or null
    // for raw lighting; age is how many frames older it is. A guide is resolved
    // after its own frame's first spatial pass, so this is an earlier frame's,
    // submitted before cl; that recording leaves it COMMON.
    ComPtr<ID3D12Resource> SssMask(ID3D12GraphicsCommandList *cl, unsigned *age = nullptr) {
        std::lock_guard lock(mutex);
        if (!sss_enabled || sss_failed || !sss_mask_guide)
            return {};
        const Slot *match{};
        for (const auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                if (match)
                    return {};
                match = &s;
            }
        if (!match ||
            !rrsssmask::Usable(sss_mask, match->frame, generation, match->gpu.w, match->gpu.h))
            return {};
        if (age)
            *age = match->frame - sss_mask.frame;
        return sss_mask_guide;
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

    // Whether the hit distance tests would condition cl's guide recording now,
    // and why not (Off, NoSlot or Repeat), so the caller can skip capturing.
    bool HitDistancePending(ID3D12GraphicsCommandList *cl, rrhitcond::Outcome &why) {
        std::lock_guard lock(mutex);
        return HitSlotLocked(cl, why);
    }
    // Specular hit distance tests (rrhitcond): conditions this recording's copy of
    // the native distance in place, just before the native temporal pass. Its t5,
    // weights, holds this frame's raw lighting weights, whose lobe-1 bits mark a
    // fresh specular ray. The caller proved that binding and its state and captured
    // restore, the pass's compute state; state_of(resource, state) gives a
    // resource's tracked state on cl (used for PackedSurface, which this pass does
    // not bind). Nothing is recorded unless it conditions. The fill is left out
    // while a guide test replaces the normals it compares.
    template <class StateOf>
    rrhitcond::Outcome ConditionSpecularDistance(ID3D12GraphicsCommandList *cl,
                                                 ID3D12Resource *weights,
                                                 D3D12_RESOURCE_STATES weights_state,
                                                 const rrcompute::Snapshot &restore,
                                                 StateOf state_of) {
        using rrhitcond::Outcome;
        std::lock_guard lock(mutex);
        Outcome why{};
        auto *match = HitSlotLocked(cl, why);
        if (!match)
            return why;
        auto &s = *match;
        unsigned mode = rrhitcond::Mode();
        if (rrhitcond::NormalsReplaced(s.gpu.guide_test))
            mode &= ~rrhitcond::FillHoles;
        if (!mode)
            return Outcome::Off;
        if (!s.gpu.hit_ready || !s.gpu.specular_hit)
            return Outcome::NoCopy;
        if (!weights)
            return Outcome::Weights;
        const auto d = weights->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != s.gpu.w ||
            d.Height != s.gpu.h || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
            d.SampleDesc.Count != 1 ||
            (d.Format != DXGI_FORMAT_R32_UINT && d.Format != DXGI_FORMAT_R32_FLOAT) ||
            !rrsparsecopy::Readable(weights_state) ||
            !rrsparsecopy::SameDevice(pipelines.device.Get(), weights))
            return Outcome::Weights;
        auto *surface = s.gpu.sources[3].Get();
        D3D12_RESOURCE_STATES surface_state{};
        if (!surface || !state_of(surface, surface_state) ||
            !rrsparsecopy::Readable(surface_state))
            return Outcome::Surface;
        if (!restore.valid || !restore.complete || !restore.heap_restore_valid ||
            restore.command != cl)
            return Outcome::Context;
        try {
            pipelines.HitConditionPipeline();
        } catch (const rrprivate::AllocationDeferred &) {
            return Outcome::Memory; // retried on a later frame
        } catch (const std::exception &e) {
            hit_condition_failed = true;
            char line[256];
            snprintf(line, sizeof(line),
                     "Specular hit distance tests off for this session: %s", e.what());
            Log(line);
            return Outcome::Failed;
        }
        // The panel's "active" is left to the evaluation (AfterSR), which knows
        // whether RR got the conditioned copy.
        const bool shown = hit_scratch.active;
        const bool ready =
            UseTest(hit_scratch, true, rrhitcond::changes.load(std::memory_order_relaxed),
                    s.gpu.w, s.gpu.h, DXGI_FORMAT_R16_FLOAT, "Specular hit distance tests");
        hit_scratch.active = shown;
        if (!ready)
            return Outcome::Memory;
        rrinternal::Scope copy_guard;
        rrcompute::Tracker::Internal state_guard;
        s.gpu.ConditionDistance(cl, pipelines, hit_scratch.texture.Get(), weights, mode);
        restore.Restore(cl, true);
        s.hit_conditioned = true;
        s.hit_mode = mode;
        return Outcome::Conditioned;
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
            UINT64 base = 0, depth = 0, water_bytes = 0, kept = 0;
            for (const auto &slot : slots) {
                UINT64 b{}, d{};
                if (!SlotReservationParts(pipelines.device.Get(), slot.gpu, budget.limit, b, d) ||
                    b < slot.gpu.sss.bytes || b - slot.gpu.sss.bytes < slot.gpu.water.bytes ||
                    b - slot.gpu.sss.bytes - slot.gpu.water.bytes < slot.gpu.ray_bytes)
                    return false;
                base = (std::max)(base, b - slot.gpu.sss.bytes - slot.gpu.water.bytes -
                                            slot.gpu.ray_bytes);
                depth = (std::max)(depth, d);
                water_bytes = (std::max)(water_bytes, slot.gpu.water.bytes);
            }
            if (!PersistentBytes(kept))
                return false;
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
            // Water test storage (rr_water.h) takes the otherwise unused hit term.
            if (!rrtrans::RequiredSlots(slots.size(), base, depth, optional_sss, 0, water_bytes,
                                        kept, required) ||
                !budget.Reserve(pipelines.device.Get(), required) ||
                !rrsss::FitsOwnedBudget(base, depth, kept, optional_sss, water_bytes, budget.limit,
                                        slots.size())) {
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
    // The one guide recording on cl the hit distance tests have not conditioned
    // yet, or null with why not.
    Slot *HitSlotLocked(ID3D12GraphicsCommandList *cl, rrhitcond::Outcome &why) {
        why = rrhitcond::Outcome::Off;
        if (!rrhitcond::Mode() || !enabled || !hit_enabled || failed || retired ||
            hit_condition_failed || !cl)
            return nullptr;
        why = rrhitcond::Outcome::NoSlot;
        Slot *match{};
        for (auto &s : slots)
            if (RawSlot(s, cl)) {
                if (match)
                    return nullptr;
                match = &s;
            }
        if (match && match->hit_conditioned) {
            why = rrhitcond::Outcome::Repeat;
            return nullptr;
        }
        return match;
    }
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
    TestState Test(const DevTest &t, unsigned changes, bool wanted) const {
        if (!wanted)
            return TestState::Idle;
        if (t.blocked && t.changes == changes)
            return TestState::Blocked;
        return t.active && enabled && !failed && !retired && feature ? TestState::Active
                                                                     : TestState::Idle;
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
        RayRestart(); // either edge: the ray budget test proves its lists again
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
    void RequestCapture() {
        std::lock_guard lock(mutex);
        CaptureOutcomeLocked(); // A writer that has finished no longer blocks a new request.
        if (capture.Request()) {
            capture_outcome.reset();
            Log("RR capture: requested");
            if (failed || retired) // Neither is ever cleared: nothing would be collected.
                CaptureFailed("RR is not running");
        }
    }
    // The Lighting input the settings apply; each captured frame records the one it ran with.
    void SetCaptureLighting(unsigned value) {
        std::lock_guard lock(mutex);
        capture_lighting = value;
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
        RuntimeReport report;
        {
            std::lock_guard lock(mutex);
            try {
                PollLocked();
            } catch (const std::exception &e) {
                Failure(e.what());
            }
            report = std::exchange(runtime_report, {});
        }
        if (report.core)
            try {
                Log(rrngxlog::RuntimeLine(report.core, report.device.Get()).c_str());
            } catch (...) {
                // A line that cannot be built is dropped: nothing may escape into ReShade.
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
            // A null classification view unless a water test wants it this frame.
            // The guide mode follows it, so every water test waits and fails together.
            s.gpu.WaterTarget(pipelines, rrwater::Wanted(water) && !water_failed &&
                                             PrepareWater(s.gpu, GetTickCount64()));
            s.pre = cl;
            if (ray_early == cl) { // a resolve ran on this list before its guide prepass
                ray_summary.BeforePrepass();
                ray_early = nullptr;
            }
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
            s.gpu.Convert(cl, pipelines, s.gpu.water_written ? water.guide : rrwater::Mirror);
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
    // The ray budget test's entry points (rr_vrrt_budget_render.h). All noexcept: they take the
    // mutex, never call ComputeState or the trackers, and never fail RR.
    // At the game's cs_vrrt_resolve: true once the patched native Dispatch was recorded (then
    // `dispatched` is set and the caller skips the original), false to run it natively.
    bool RayBudgetResolve(ID3D12GraphicsCommandList *cl, const rrvrrt::RayResolve &in, UINT x,
                          UINT y, UINT z, bool &dispatched) noexcept {
        try {
            std::lock_guard lock(mutex);
            return RayResolveLocked(cl, in, x, y, z, dispatched);
        } catch (...) {
            return dispatched;
        }
    }
    // Every cs_vrrt_resolve dispatch the hook sees, RR enabled or not: the Budget marks age by
    // these. Lock-free (an atomic counter), so it is safe before any other check.
    void RayBudgetResolveSeen() noexcept {
        ray_resolves.fetch_add(1, std::memory_order_relaxed);
    }
    // At every recognised native VRRT apply: its list proof (ListsUnproven when not proven) and
    // the constants its logging copies read.
    void RayBudgetApply(ID3D12GraphicsCommandList *cl, const rrraybudget::Lists &lists,
                        const rrvrrt::RayApply &log) noexcept {
        try {
            std::lock_guard lock(mutex);
            RayApplyLocked(cl, lists, log);
        } catch (...) {
        }
    }
    // At the game's cs_vrrt_clear_counters: counted inside or outside an RR guide recording.
    void RayBudgetClear(ID3D12GraphicsCommandList *cl) noexcept {
        try {
            std::lock_guard lock(mutex);
            bool in = false;
            for (const auto &s : slots)
                in |= RawSlot(s, cl);
            ray_summary.Clear(in);
        } catch (...) {
        }
    }
    // An indirect command that read the published ray list `handle` of lobe `c.lobe`. The hook
    // passes the argument buffer's offset through unchanged; it is made relative to the list's
    // view here, with the view offset of the same apply that published the handle. A handle that
    // is no longer published (a later apply or Reconfigure replaced it) is ignored.
    void RayBudgetConsumer(rrraybudget::Consumer c, std::uint64_t handle) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (c.lobe >= 2 || !handle ||
                rrraybudget::published[c.lobe].load(std::memory_order_relaxed) != handle)
                return;
            if (c.offset >= ray_list_offsets[c.lobe])
                c.offset -= ray_list_offsets[c.lobe];
            ray_summary.Consumer(c);
        } catch (...) {
        }
    }
    // An exception in a ray budget hook: stops the test for the session; RR is unaffected.
    void RayBudgetException() noexcept {
        try {
            std::lock_guard lock(mutex);
            RayStop(rrraybudget::Why::Exception, "in a ray budget hook");
        } catch (...) {
        }
    }
    rrraybudget::Status RayBudgetStatus() noexcept {
        try {
            std::lock_guard lock(mutex);
            return StatusLocked(GetTickCount64());
        } catch (...) {
            rrraybudget::Status s;
            s.mode = rrraybudget::Valid(rrraybudget::mode.load(std::memory_order_relaxed));
            return s;
        }
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
        if (!rrsr::Eligible(proof)) {
            if (capture.Active()) { // CaptureLocked logs why; never fails replacement's caller.
                try {
                    for (unsigned i = 0; i < 4; ++i) {
                        D3D12_RESOURCE_STATES unaliased{};
                        proof.aliased[i] =
                            proof.known[i] &&
                            !layouts.GetUnaliased(native.Get(), s->gpu.sr[i].Get(), unaliased);
                    }
                    s->proof = proof;
                    s->proof_failed = true;
                } catch (...) {
                }
            }
            return false;
        }
        for (unsigned i = 0; i < 4; ++i)
            s->states[i] = {proof.states[i], true, true};
        return AfterSR(public_cl, p, scope, NVSDK_NGX_Result_Success, mod, epoch, safe, true);
    }
    bool AfterSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                 rrtrace::SlCall scope, NVSDK_NGX_Result result, HMODULE mod, UINT64 epoch,
                 bool safe, bool replacement = false, rrsparsecopy::Layouts *layouts = nullptr) {

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
            // The panel shows the hit distance tests active only while RR gets the
            // conditioned copy.
            hit_scratch.active = s->hit_used && s->hit_conditioned;
            // Water tests read the classification this slot's prepass wrote.
            s->water_used = rrwater::Wanted(water) && !water_failed && s->gpu.water_written &&
                            !o.color_x && !o.color_y;

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
            // Weapon depth test: RR's depth type is fixed when RR is created, so a frame
            // follows the feature's type and a new feature takes the setting's. Linear depth
            // matches the projection RR is given; with the sky guide test, sky stands for
            // that test's depth floor.
            const auto weapon_changes = rrweapondepth::changes.load(std::memory_order_relaxed);
            bool weapon_on = feature ? weapon_linear : WeaponDepthWanted();
            rrweapondepth::Mapping weapon_map;
            const bool weapon_mapped =
                weapon_on &&
                rrweapondepth::Map(s->projection, projection,
                                   s->gpu.guide_test == rrguidetest::SkySurface
                                       ? rrguidetest::DepthFloor
                                       : 0.0f,
                                   weapon_map);
            if (!UseTest(weapon_test, weapon_mapped, weapon_changes, s->gpu.w, s->gpu.h,
                         DXGI_FORMAT_R32_FLOAT, "Weapon depth test") &&
                weapon_on) {
                // No GPU memory or an unsupported projection holds the test off. A texture
                // left at another size is only released: the restart allocates it again.
                if (!weapon_mapped || weapon_test.blocked)
                    HoldWeaponDepth(weapon_mapped ? WeaponHold::Memory : WeaponHold::Projection,
                                    weapon_changes);
                // A linear-depth RR is never given hardware depth: restart it instead.
                if (feature) {
                    QueueReconfigure();
                    return false;
                }
                weapon_on = false;
            }

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
                // Logged by the next Poll whether or not this attempt creates RR.
                runtime_report = {mod, pub};
                selected_view = scope.viewport;
                key = o;
                api = resolve(mod);
                Require(api.Valid(), "live NGX exports missing");
                const auto allocated = api.allocate(&parameters);
                rrngxlog::Require(NVSDK_NGX_SUCCEED(allocated) && parameters,
                                  "live NGX parameter allocation failed",
                                  rrngxlog::Returned(allocated, parameters));
                rreval::CreateParameters(*parameters, o, preset.desired, weapon_on);
                Require(rrpreset::Requested(*parameters, preset.desired),
                        "live RR preset parameter readback failed");
                auto created = api.create(public_cl, NVSDK_NGX_Feature_RayReconstruction,
                                          parameters, &feature);
                if (weapon_on && (NVSDK_NGX_FAILED(created) || !feature)) {
                    // RR refused linear depth: hold the test and create RR as before.
                    HoldWeaponDepth(WeaponHold::Refused, weapon_changes,
                                    rrngxlog::Returned(created, feature).text);
                    weapon_on = weapon_test.active = false;
                    feature = nullptr;
                    rreval::CreateParameters(*parameters, o, preset.desired);
                    created = api.create(public_cl, NVSDK_NGX_Feature_RayReconstruction,
                                         parameters, &feature);
                }
                char line[200];
                rrngxlog::CapabilityLine(api, line, sizeof(line));
                Log(line);
                rrngxlog::Require(NVSDK_NGX_SUCCEED(created) && feature,
                                  "live RR feature creation failed",
                                  rrngxlog::Returned(created, feature));
                preset.Created();
                weapon_linear = weapon_on;
                rrngxlog::DescribeCreation(o, preset.desired, weapon_on, line, sizeof(line));
                Log(line);
                if (weapon_on) {
                    int sent = -1;
                    parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &sent);
                    // Read back from the parameters; decimal, as the "RR created" line above.
                    snprintf(line, sizeof(line),
                             "Weapon depth test: RR created with linear depth (game create flags "
                             "%d, sent %d)",
                             o.flags, sent);
                    Log(line);
                }
            }
            // Development tests (shared textures, see UseTest). RR takes one mask:
            // while in use, the sky edge mask replaces the SSS one.
            const float sky_edge_strength =
                rrresponsivity::Clamp(rrskyedge::strength.load(std::memory_order_relaxed));
            const UINT sky_edge_radius =
                rrskyedge::ClampRadius(rrskyedge::radius.load(std::memory_order_relaxed));
            const bool sky_edge_on =
                UseTest(sky_edge_test,
                        rrskyedge::Use(rrskyedge::enabled.load(std::memory_order_relaxed),
                                       sky_edge_strength, preset.has_effective, preset.effective),
                        rrskyedge::changes.load(std::memory_order_relaxed), s->gpu.w, s->gpu.h,
                        DXGI_FORMAT_R16_FLOAT, "Sky edge mask");
            // Written by the SSS resolve below; null keeps RR's default responsiveness.
            responsivity_active =
                !sky_edge_on &&
                rrresponsivity::Use(responsivity_enabled, responsivity_strength, s->sss_used,
                                    preset.has_effective, preset.effective);
            const float color_scale =
                rrcolorscale::Factor(rrcolorscale::mode.load(std::memory_order_relaxed));
            // Brightened, RR's output needs FP32 too; without it the test stays off.
            const auto color_changes = rrcolorscale::changes.load(std::memory_order_relaxed);
            const bool wide_on =
                UseTest(wide_output, color_scale > 1.0f, color_changes, s->gpu.ow, s->gpu.oh,
                        DXGI_FORMAT_R32G32B32A32_FLOAT, "Color brightness test output");
            const bool color_on =
                UseTest(color_test, color_scale != 1.0f && (color_scale < 1.0f || wide_on),
                        color_changes, s->gpu.w, s->gpu.h, DXGI_FORMAT_R32G32B32A32_FLOAT,
                        "Color brightness test");
            if (!color_on)
                wide_output.active = false;
            // The gate's Display reads RR's output from heap slot 9.
            s->gpu.Reconstructed(pipelines, wide_output.active ? wide_output.texture.Get()
                                                                : s->gpu.rr_output.Get());
            if (color_on)
                s->gpu.OutputScale(1.0f / color_scale);
            // RR's history is in the previous scale: start it over when the scale changes.
            const float applied = color_on ? color_scale : 1.0f;
            const bool color_scale_changed = applied != applied_color_scale;
            applied_color_scale = applied;
            // The guide mode this slot's guides were converted with, not the panel's
            // current one, so normals and depth always come from the same mode.
            // The weapon depth test's linear depth carries the floor itself.
            const bool depth_on =
                UseTest(depth_test, s->gpu.guide_test == rrguidetest::SkySurface && !weapon_on,
                        rrguidetest::changes.load(std::memory_order_relaxed), s->gpu.w, s->gpu.h,
                        DXGI_FORMAT_R32_FLOAT, "Sky depth floor");
            // The SSS guide and color before transparency are in the game's color
            // scale, so they are left out while the test scales RR's color.
            s->guides_withheld = color_on;
            // Water masks, written by WaterPass below. RR takes one responsivity
            // mask: the water one carries the SSS values too when both are on, and
            // the sky edge test replaces both.
            const bool water_responsivity =
                           !sky_edge_on &&
                           rrwater::UseResponsivity(water, s->water_used, preset.has_effective,
                                                    preset.effective),
                       water_discard = s->water_used && water.discard,
                       water_animated = s->water_used && water.animated,
                       water_merge = water_responsivity && responsivity_active;
            if (s->water_used)
                s->gpu.WaterHeader(rrwater::Clamp(water.strength),
                                   rrwater::Flags(water_responsivity, water_discard, water_merge));
            water_status = {};
            water_status.ready = s->water_used;
            water_status.responsivity = water_responsivity;
            water_status.discard = water_discard;
            water_status.animated = water_animated;
            rreval::Resources resources{color_on ? color_test.texture.Get() : s->gpu.sr[0].Get(),
                                        weapon_on          ? weapon_test.texture.Get()
                                        : depth_on         ? depth_test.texture.Get()
                                        : s->gpu.use_depth ? s->gpu.depth.Get()
                                                           : s->gpu.sr[1].Get(),
                                        s->gpu.sr[2].Get(),
                                        wide_output.active ? wide_output.texture.Get()
                                                           : s->gpu.rr_output.Get(),
                                        s->gpu.guides[0].Get(),
                                        s->gpu.guides[1].Get(),
                                        s->gpu.guides[2].Get(),
                                        s->sss_used && !color_on ? s->gpu.sss.guide.Get()
                                                                 : nullptr,
                                        s->transparency_used && !color_on
                                            ? s->gpu.transparency.guide.Get()
                                            : nullptr,
                                        sky_edge_on           ? sky_edge_test.texture.Get()
                                        : water_responsivity  ? s->gpu.water.responsivity.Get()
                                        : responsivity_active ? s->gpu.sss.mask.Get()
                                                              : nullptr,
                                        s->hit_used ? s->gpu.specular_hit.Get() : nullptr,
                                        water_discard ? s->gpu.water.disocclusion.Get()
                                                      : nullptr,
                                        water_animated ? s->gpu.water.mask.Get() : nullptr};
            s->responsivity_used = resources.responsivity != nullptr;
            // The test copies are read at their own origin.
            auto evaluated = o;
            if (color_on)
                evaluated.color_x = evaluated.color_y = 0;
            if (depth_on || weapon_on)
                evaluated.depth_x = evaluated.depth_y = 0;
            if (weapon_on && (weapon_map.scale != weapon_logged.scale ||
                              weapon_map.offset != weapon_logged.offset ||
                              weapon_map.sky != weapon_logged.sky)) {
                weapon_logged = weapon_map;
                char line[224];
                snprintf(line, sizeof(line),
                         "Weapon depth test: world depth %g / (depth + %g), sky %g, weapon "
                         "slice in front of %g",
                         weapon_map.scale, weapon_map.offset, weapon_map.sky,
                         weapon_map.scale / (1 + weapon_map.offset));
                Log(line);
            }
            rreval::EvaluateParameters(*parameters, evaluated, resources, s->view.data(),
                                       projection.data());
            parameters->Set(NVSDK_NGX_Parameter_Reset, reset || color_scale_changed ? 1 : 0);
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
            if (s->water_used)
                s->gpu.WaterPass(cl.Get(), pipelines,
                                 water_merge ? s->gpu.sss.mask.Get() : nullptr);
            if (s->hit_used)
                Change(cl.Get(), s->gpu.specular_hit.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (sky_edge_on)
                s->gpu.SkyEdge(cl.Get(), pipelines, sky_edge_test.texture.Get(), sky_edge_strength,
                               sky_edge_radius);
            if (color_on)
                s->gpu.ScaleColor(cl.Get(), pipelines, color_test.texture.Get(), o.color_x,
                                  o.color_y, color_scale);
            if (depth_on)
                s->gpu.FloorDepth(cl.Get(), pipelines, depth_test.texture.Get(), o.depth_x,
                                  o.depth_y, rrguidetest::DepthFloor);
            if (weapon_on)
                s->gpu.WeaponDepth(cl.Get(), pipelines, weapon_test.texture.Get(), o.depth_x,
                                   o.depth_y, weapon_map);
            if (wide_output.active)
                Change(cl.Get(), wide_output.texture.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // After the test passes, so a capture holds exactly what RR is given.
            bool captured = false;
            if (capture.Active())
                captured =
                    CaptureLocked(*s, cl.Get(), resources, evaluated, reset || color_scale_changed,
                                  projection, replacement, layouts);
            if (captured) {
                auto &m = s->captured.meta;
                m.linear_depth = weapon_on;
                m.color_scale = color_on ? color_scale : 1.0f;
                m.guide_test = s->gpu.guide_test;
                m.depth_floor = depth_on ? rrguidetest::DepthFloor : 0.0f;
                m.responsivity_source = sky_edge_on           ? rrcapture::SkyEdgeMask
                                        : water_responsivity  ? rrcapture::WaterMask
                                        : responsivity_active ? rrcapture::SssMask
                                                              : rrcapture::NoMask;
                if (color_scale_changed)
                    m.reset_reasons |= rrcapture::ColorScaleReset;
            }

            NVSDK_NGX_Result evaluated_result = NVSDK_NGX_Result_FAIL_PlatformError;
            bool threw = false; // then evaluated_result is the sentinel above, not NGX's
            try {
                evaluated_result = api.evaluate(public_cl, feature, parameters, nullptr);
            } catch (...) {
                threw = true;
                Log("RR evaluation threw; restoring resources for native SR");
            }
            s->inference_ok = NVSDK_NGX_SUCCEED(evaluated_result);
            if (captured && s->inference_ok)
                CaptureOutputLocked(*s, cl.Get(), resources, evaluated);
            if (s->hit_used)
                Change(cl.Get(), s->gpu.specular_hit.Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            for (auto *t : {&sky_edge_test, &color_test, &depth_test, &weapon_test})
                if (t->active)
                    s->gpu.TestFinish(cl.Get(), t->texture.Get());
            // Readable by the gate's Display (GateEnd), then back to COMMON below.
            if (wide_output.active)
                Change(cl.Get(), wide_output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (s->water_used)
                s->gpu.WaterFinish(cl.Get());
            if (s->transparency_used)
                s->gpu.transparency.Finish(cl.Get());
            if (s->sss_used)
                s->gpu.sss.Finish(cl.Get());
            s->gpu.GateEnd(cl.Get(), pipelines, history.Get(), s->inference_ok);
            if (wide_output.active)
                s->gpu.TestFinish(cl.Get(), wide_output.texture.Get());
            s->gate_recorded = true;
            s->replaced_sr = replacement && s->inference_ok;
            for (unsigned i = 0; i < 3; ++i)
                if (i != 1 || !s->gpu.use_depth)
                    Change(cl.Get(), s->gpu.sr[i].Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, states[i], 0);
            s->recorded = true;
            if (!s->inference_ok) {
                const rrngxlog::Code code(evaluated_result);
                const char *outcome = threw ? "threw" : code.text;
                // A linear-depth RR that fails: restart it with hardware depth rather than
                // turning RR off.
                if (weapon_on) {
                    HoldWeaponDepth(WeaponHold::Refused, weapon_changes, outcome);
                    QueueReconfigure();
                } else {
                    char line[128];
                    snprintf(line, sizeof(line), "RR inference failed (%s); native SR retained",
                             outcome);
                    Failure(line);
                }
            }
            return s->inference_ok;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        if (ray_early == cl)
            ray_early = nullptr;
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
                if (s.sss_used)
                    SealSssMask(s, sss_accept);
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
                // The replay copies into this session's capture buffer again.
                if (s.capture_hold && s.capture_session == capture.session) {
                    if (capture.state == rrcapture::State::Collecting) {
                        CaptureFailed("RR replayed a captured frame");
                    } else if (capture.state == rrcapture::State::Writing && capture_outcome) {
                        std::lock_guard taint(capture_outcome->mutex);
                        capture_outcome->tainted = true;
                    }
                }
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
        if (capture.state == rrcapture::State::Collecting)
            CaptureFailed("RR shut down");
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
