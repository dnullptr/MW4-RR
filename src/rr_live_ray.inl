// Included inside Controller. The ray budget test's state, marks, serials, patch and verification.
private:
// The ray budget test (rr_ray_budget.h; hooks in rr_vrrt_budget_render.h). Session only, Off
// by default. Every path catches everything and none calls Failure, rrbypass::Fail, GlobalFail
// or SparseFailure: a failure refuses (the native resolve runs), waits, or stops the test for
// the session with one log line.
using RayWhy = rrraybudget::Why;
using RayLogWords = std::array<std::uint32_t, rrraybudget::LogWords>;
enum class RayLogState : unsigned { None, Read, Failed }; // a slot's ray log at completion
// A Budget identity's last patch (A4): the lobes its governor raised and that plan's arming
// snapshot. Cleared when an unpatched native resolve in a recording writes that Budget; live
// until RayMarkLife further cs_vrrt_resolve dispatches were seen (ray_resolves), whatever their
// placement and whether RR was enabled. Recordings without a resolve leave the Budget as the
// governor wrote it, so they do not age a mark.
struct RayMark {
    UINT64 resource{}, offset{};
    unsigned mask{};
    UINT resolves{}; // ray_resolves just after the patched resolve was counted
    rrraybudget::Armed armed;
};
static constexpr UINT RayMarkLife = 8;
static constexpr UINT64 RayActiveMs = 1000; // Status shows a patch as active this long
rrraybudget::Gate ray_gate;
rrraybudget::Summary ray_summary;
unsigned ray_logged_mode = rrraybudget::Off; // the mode the waiting/active lines are for
UINT64 ray_retry_after{}, ray_active_at{};
ID3D12GraphicsCommandList *ray_early{}; // compared only, never dereferenced
std::array<bool, unsigned(RayWhy::Count)> ray_waited{};
rrraybudget::Plan ray_plan;    // the last patched plan
RayWhy ray_why = RayWhy::Mode; // the last resolve's refusal; None after a patch
bool ray_announced{};
std::array<RayMark, 4> ray_marks{};
unsigned ray_next_mark{};
UINT ray_next_serial{};
// Every cs_vrrt_resolve dispatch PreserveRayBudget sees (RayBudgetResolveSeen, lock-free: it is
// counted before the RR-enabled check, without the mutex).
std::atomic<UINT> ray_resolves{};
// The logging copies stop for the session after a failure other than a memory wait; the
// Summary has no failure counter, so the first failure and the first dropped readback are
// logged once each.
bool ray_log_failed{}, ray_log_warned{}, ray_memory_waiting{};
std::array<UINT64, 2> ray_list_offsets{}; // the view offsets of the published u5 and u6

void RayWrite(const char *line) noexcept {
    try {
        Log(line);
    } catch (...) {
    }
}
void RayStopLog(RayWhy why, const char *details) noexcept {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "Ray budget test stopped: %s (%s); the game's own ray budget is used for the "
                  "rest of the session",
                  rrraybudget::Text(why), details && *details ? details : "no details");
    RayWrite(line);
}
// A sticky stop; logged when this call stopped the test.
void RayStop(RayWhy why, const char *details) noexcept {
    if (ray_gate.Stop(why))
        RayStopLog(why, details);
}
// A resolve refused (counted, and the status's reason), logged once per reason per mode
// selection while a test mode is selected. Returns false: the native resolve runs.
bool RayRefuse(RayWhy why, unsigned m) noexcept {
    ray_summary.Refused(why);
    ray_why = why;
    const unsigned i = unsigned(why);
    if (m != rrraybudget::Off && why != RayWhy::Stopped && i < ray_waited.size() &&
        !ray_waited[i]) {
        ray_waited[i] = true;
        char line[192];
        std::snprintf(line, sizeof(line), "Ray budget test waiting: %s", rrraybudget::Text(why));
        RayWrite(line);
    }
    return false;
}
// A sticky stop raised at a resolve, which then runs natively.
bool RayStopResolve(RayWhy why, const char *details) noexcept {
    ray_summary.Refused(why);
    ray_why = why;
    RayStop(why, details);
    return false;
}
void RayLogFailed(const char *what) noexcept {
    if (ray_log_failed)
        return;
    ray_log_failed = true;
    char line[320];
    std::snprintf(line, sizeof(line), "RR ray budget: logging copies disabled for the session (%s)",
                  what && *what ? what : "unknown error");
    RayWrite(line);
}
UINT RayNextSerial() noexcept {
    if (++ray_next_serial == 0)
        ++ray_next_serial; // serials are nonzero (rr_ray_copies.h)
    return ray_next_serial;
}
// Two proven applies measure the same lists: extent and grid, both lobes' entries, the pixel
// list (u3) and the counter (u2).
static bool RaySameProof(const rrraybudget::Lists &a, const rrraybudget::Lists &b) noexcept {
    return rrraybudget::SameExtent(a, b) && a.Entries(0) == b.Entries(0) &&
           a.Entries(1) == b.Entries(1) && a.v[2].bytes == b.v[2].bytes &&
           a.v[3].bytes == b.v[3].bytes;
}
// The recording's ray serial for the halves of its ray log, taken at its first ray site
// (resolve or apply) whether or not that site copies anything.
void RaySerial(Slot &s) noexcept {
    if (!s.ray_serial)
        s.ray_serial = RayNextSerial();
}
// The resolves seen since the mark's patch.
UINT RayMarkAge(const RayMark &m) const noexcept {
    return UINT(ray_resolves.load(std::memory_order_relaxed) - m.resolves);
}
bool RayMarkLive(const RayMark &m) const noexcept {
    return m.resource && RayMarkAge(m) < RayMarkLife;
}
void RayClearMark(UINT64 resource, UINT64 offset) noexcept {
    for (auto &m : ray_marks)
        if (m.resource == resource && m.offset == offset)
            m = {};
}
// After a patch: the identity's entry, else an empty or expired one, else the next in turn.
void RaySetMark(UINT64 resource, UINT64 offset, const rrraybudget::Plan &plan,
                UINT resolves) noexcept {
    RayMark *at{};
    for (auto &m : ray_marks)
        if (m.resource == resource && m.offset == offset) {
            at = &m;
            break;
        }
    if (!at)
        for (auto &m : ray_marks)
            if (!RayMarkLive(m)) {
                at = &m;
                break;
            }
    if (!at) {
        at = &ray_marks[ray_next_mark % ray_marks.size()];
        ray_next_mark = unsigned((ray_next_mark + 1) % ray_marks.size());
    }
    *at = {resource, offset, plan.mask, resolves, plan.armed};
}
// What an apply reading the Budget (resource, offset) consumed: that identity's live mark, or
// for an unknown identity (resource 0) the union of the live marks' lobes with the newest
// mark's snapshot (conservative: the frame is then checked as raised).
RayMark RayConsumed(UINT64 resource, UINT64 offset) const noexcept {
    RayMark found;
    UINT newest = UINT_MAX;
    for (const auto &m : ray_marks) {
        if (!RayMarkLive(m))
            continue;
        if (resource) {
            if (m.resource == resource && m.offset == offset)
                return m;
            continue;
        }
        found.mask |= m.mask;
        const UINT age = RayMarkAge(m);
        if (age < newest) {
            newest = age;
            found.armed = m.armed;
        }
    }
    return found;
}
// Active while the last patch is under RayActiveMs old and was made in the selected mode.
rrraybudget::Status StatusLocked(UINT64 now) const noexcept {
    rrraybudget::Status s;
    s.mode = rrraybudget::Valid(rrraybudget::mode.load(std::memory_order_relaxed));
    s.stopped = ray_gate.Stopped();
    s.active = s.mode == ray_logged_mode && ray_plan.mask && ray_active_at &&
               now >= ray_active_at && now - ray_active_at <= RayActiveMs;
    s.native = ray_plan.native;
    s.target = ray_plan.target;
    for (unsigned l = 0; l < 2; ++l)
        s.raised[l] = (ray_plan.mask >> l & 1) != 0;
    if (s.mode == rrraybudget::Off)
        s.why = RayWhy::Mode;
    else if (ray_why != RayWhy::None && ray_why != RayWhy::Mode)
        s.why = ray_why;
    else { // no resolve refused in this mode yet, or the last patch is old: the gate's reason
        const auto why = ray_gate.Decide(s.mode, now, nullptr).why;
        s.why = why == RayWhy::None ? RayWhy::ListsStale : why;
    }
    return s;
}
// The details of a stop Gate::Complete or Gate::Unverified returned for `c`.
static void RayStopDetails(RayWhy why, const rrraybudget::Completed &c, char *out,
                           std::size_t size) noexcept {
    using rrdensity::Limits;
    using rrdensity::RawRays;
    const auto &f = c.rates;
    const unsigned raised = rrraybudget::RaisedLobes(c);
    out[0] = 0;
    switch (why) {
    case RayWhy::Governor:
        for (unsigned l = 0; l < 2; ++l)
            if ((c.budget >> l & 1) && !rrraybudget::SafeThreshold(int(f[Limits + 2 * l]))) {
                std::snprintf(out, size, "lobe%u threshold=%d", l + 1, int(f[Limits + 2 * l]));
                return;
            }
        break;
    case RayWhy::PatchMismatch:
        for (unsigned l = 0; l < 2; ++l)
            if ((c.patched >> l & 1) && !rrraybudget::Executed(c, l)) {
                if (c.logged & rrraybudget::LogExecuted)
                    std::snprintf(out, size, "lobe%u executed=0x%08x written=0x%08x", l + 1,
                                  unsigned(c.log[4 + l]), unsigned(c.written[l]));
                else
                    std::snprintf(out, size, "lobe%u executed=n/a written=0x%08x", l + 1,
                                  unsigned(c.written[l]));
                return;
            }
        break;
    case RayWhy::ListsAfterArming:
        std::snprintf(out, size,
                      "proof=%s extent=%ux%u armed=%ux%u entries=%llu/%llu armed=%llu/%llu "
                      "u3=%lluB armed=%lluB u2=%lluB armed=%lluB",
                      rrraybudget::Text(c.lists.why), unsigned(c.lists.w), unsigned(c.lists.h),
                      unsigned(c.armed.w), unsigned(c.armed.h),
                      static_cast<unsigned long long>(c.lists.Entries(0)),
                      static_cast<unsigned long long>(c.lists.Entries(1)),
                      static_cast<unsigned long long>(c.armed.capacity[0]),
                      static_cast<unsigned long long>(c.armed.capacity[1]),
                      static_cast<unsigned long long>(c.lists.v[2].bytes),
                      static_cast<unsigned long long>(c.armed.pixels),
                      static_cast<unsigned long long>(c.lists.v[3].bytes),
                      static_cast<unsigned long long>(c.armed.counter));
        return;
    case RayWhy::Unsupported:
        std::snprintf(out, size, "pixels=%u", unsigned(f[rrdensity::Unsupported]));
        return;
    case RayWhy::StatsMismatch:
        for (unsigned l = 0; l < 2; ++l)
            if ((raised >> l & 1) && !rrraybudget::StatsCover(c, l)) {
                std::snprintf(out, size, "lobe%u x=%u rawRays=%u", l + 1, unsigned(c.log[25 + l]),
                              unsigned(f[RawRays + l]));
                return;
            }
        break;
    case RayWhy::Overflow:
        for (unsigned l = 0; l < 2; ++l)
            if ((raised >> l & 1) &&
                rrraybudget::Scheduled(f, l) + rrraybudget::ConsumerSlack > c.armed.capacity[l]) {
                std::snprintf(out, size, "lobe%u worst=%llu entries=%llu", l + 1,
                              static_cast<unsigned long long>(rrraybudget::Scheduled(f, l)),
                              static_cast<unsigned long long>(c.armed.capacity[l]));
                return;
            }
        break;
    case RayWhy::Unverified:
        std::snprintf(out, size,
                      "%u raised frames without reader statistics since the last verified one",
                      rrraybudget::UnverifiedStop);
        return;
    default:
        break;
    }
}
// A retired recording as the gate judges it, before its ray log: c.armed is the snapshot of
// the plan whose governor wrote the Budget its apply read, else of its own patch.
static rrraybudget::Completed RayFrame(const Slot &s, const rrdensity::Frame &rates) noexcept {
    rrraybudget::Completed c;
    c.rates = rates;
    c.lists = s.ray_lists;
    c.patched = s.ray_patched;
    c.budget = s.ray_budget;
    c.written = {s.ray_written[0], s.ray_written[1]};
    c.armed = s.ray_budget ? s.ray_armed : s.ray_patch_armed;
    return c;
}
// The Log bits of the halves that ran as recorded: each half copies the recording's serial
// first (resolve: log word 24, apply: word 27), so a half predicated away or skipped keeps
// a stale or zero serial and its bits are dropped.
static unsigned RaySerialBits(unsigned logged, const RayLogWords &log, UINT serial) noexcept {
    using namespace rrraybudget;
    constexpr unsigned resolve = LogResolveTarget | LogExecuted | LogResolveScene | LogResolveStats,
                       apply = LogApplyTarget | LogApplySettings36 | LogApplyScene251 |
                               LogApplyScene253;
    static_assert((resolve | apply) == 0xFF && !(resolve & apply), "two halves, every bit");
    if (!serial || log[rrraycopy::ResolveSerial.at / 4] != serial)
        logged &= ~resolve;
    if (!serial || log[rrraycopy::ApplySerial.at / 4] != serial)
        logged &= ~apply;
    return logged;
}
// A raised recording that no Complete covers: counted once (from UnverifiedLimit in a row
// Decide waits; the UnverifiedStop-th since the last verified raised frame stops the test).
void RayUnverified(Slot &s) noexcept {
    if ((s.ray_patched | s.ray_budget) && !s.ray_done) {
        const auto why = ray_gate.Unverified();
        if (why != RayWhy::None) {
            char details[128];
            RayStopDetails(why, {}, details, sizeof(details));
            RayStopLog(why, details);
        }
    }
    s.ray_done = true;
}
// A reported recording whose reader ran: its ray log (read only when it recorded copies and
// was submitted), then RayJudge.
void RayCompleted(Slot &s, const rrdensity::Frame &rates) noexcept {
    if (s.ray_done)
        return;
    RayLogWords log{};
    auto state = RayLogState::None;
    if (s.ray_logged && s.pre_submitted)
        state = s.gpu.ReadRayLog(log) ? RayLogState::Read : RayLogState::Failed;
    RayJudge(s, rates, log, state);
}
void RayJudge(Slot &s, const rrdensity::Frame &rates, const RayLogWords &log,
              RayLogState state) noexcept {
    if (s.ray_done)
        return;
    try {
        const bool raised = (s.ray_patched | s.ray_budget) != 0;
        auto c = RayFrame(s, rates);
        if (state == RayLogState::Failed) {
            if (raised) {
                s.ray_done = true;
                RayStop(RayWhy::Readback, "the ray log could not be mapped");
                return;
            }
            if (!ray_log_warned) {
                ray_log_warned = true;
                RayWrite("RR ray budget: a ray log readback failed; that frame is reported "
                         "without its log (logged once)");
            }
        } else if (state == RayLogState::Read) {
            c.log = log;
            c.logged = RaySerialBits(s.ray_logged, log, s.ray_serial);
        }
        // Rates of a recording whose prepass list was not submitted, or whose apply half did not
        // run as recorded (a stale apply serial), are not this frame's: a raised frame is
        // unverified, an unraised one measures nothing (no Complete, no Summary::Completed).
        constexpr unsigned apply_bits =
            rrraybudget::LogApplyTarget | rrraybudget::LogApplySettings36 |
            rrraybudget::LogApplyScene251 | rrraybudget::LogApplyScene253;
        if (!s.pre_submitted || (state == RayLogState::Read && (s.ray_logged & apply_bits) &&
                                 log[rrraycopy::ApplySerial.at / 4] != s.ray_serial)) {
            RayUnverified(s);
            return;
        }
        // A patch is verified against its own resolve half. One that was predicated away,
        // skipped or not read back verifies nothing: unverified, never a PatchMismatch.
        if (s.ray_patched && (state != RayLogState::Read || !s.ray_serial ||
                              log[rrraycopy::ResolveSerial.at / 4] != s.ray_serial)) {
            RayUnverified(s);
            return;
        }
        ray_summary.Completed(c);
        const auto why = ray_gate.Complete(c);
        s.ray_done = true;
        if (why != RayWhy::None) {
            char details[320];
            RayStopDetails(why, c, details, sizeof(details));
            RayStopLog(why, details);
        }
    } catch (...) {
        s.ray_done = true;
        RayStop(RayWhy::Readback, "exception while judging a frame");
    }
}
void RayReport(UINT64 now) noexcept {
    try {
        std::string controller, lists;
        if (ray_summary.Report(now, ray_gate, StatusLocked(now), controller, lists)) {
            Log(controller.c_str());
            if (!lists.empty())
                Log(lists.c_str());
        }
    } catch (...) {
    }
}
void RayActiveLog(unsigned m, const rrraybudget::Plan &plan) noexcept {
    char line[512];
    int at =
        std::snprintf(line, sizeof(line),
                      "Ray budget test active (%s):", rrraybudget::labels[rrraybudget::Valid(m)]);
    for (unsigned l = 0; l < 2 && at > 0 && at < int(sizeof(line)); ++l)
        at +=
            (plan.mask >> l & 1)
                ? std::snprintf(line + at, sizeof(line) - at, "%s lobe%u %.3f -> %.3f",
                                l ? "," : "", l + 1, double(plan.native[l]), double(plan.target[l]))
                : std::snprintf(line + at, sizeof(line) - at, "%s lobe%u %.3f (not raised)",
                                l ? "," : "", l + 1, double(plan.native[l]));
    const double extent = double(plan.armed.w) * double(plan.armed.h);
    if (at > 0 && at < int(sizeof(line)))
        std::snprintf(line + at, sizeof(line) - at,
                      "; lists hold %.2fx/%.2fx the extent; the governor keeps raised "
                      "thresholds at %d or above and holds a lobe to one ray per pixel in a "
                      "frame whose rays exceed its headroom",
                      extent ? double(plan.armed.capacity[0]) / extent : 0.0,
                      extent ? double(plan.armed.capacity[1]) / extent : 0.0,
                      rrraybudget::NegativeLimit);
    RayWrite(line);
}
// The logging copies at a ray site, in every mode (A6): PrepareRayLog, then the recording's
// serial (WriteRaySerial drops it while ray_map is null), then `record` under the internal
// guards. A memory wait (PrepareRayLog false or AllocationDeferred) retries after RetryMs; any
// other failure disables logging for the session. Never a stop. Returns the bits recorded.
template <class Record> unsigned RayLogCopies(Slot &s, UINT64 now, Record record) noexcept {
    try {
        if (!s.gpu.PrepareRayLog(pipelines, budget)) {
            ray_retry_after = now + rrraybudget::RetryMs;
            return 0;
        }
        s.gpu.WriteRaySerial(s.ray_serial);
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        const unsigned bits = record();
        s.ray_logged |= bits;
        return bits;
    } catch (const rrprivate::AllocationDeferred &) {
        ray_retry_after = now + rrraybudget::RetryMs;
    } catch (const std::exception &e) {
        RayLogFailed(e.what());
    } catch (...) {
        RayLogFailed("unknown error");
    }
    return 0;
}
// Bookkeeping after the patched resolve and the governor were recorded. It never undoes them:
// a failure here is a sticky stop.
void RayPatched(Slot &s, const rrraybudget::Plan &plan, const rrvrrt::RayResolve &in, UINT64 now,
                unsigned m) noexcept {
    try {
        s.ray_patched = plan.mask;
        s.ray_written = {plan.bits[0], plan.bits[1]};
        s.ray_logged |= rrraybudget::LogExecuted;
        s.ray_patch_armed = plan.armed;
        RaySetMark(in.budget_resource, in.budget_offset, plan,
                   ray_resolves.load(std::memory_order_relaxed));
        ray_summary.Patched(plan);
        ray_plan = plan;
        ray_active_at = now;
        ray_why = RayWhy::None;
        if (!ray_announced) {
            ray_announced = true;
            RayActiveLog(m, plan);
        }
    } catch (...) {
        RayStop(RayWhy::Exception, "after the controller dispatch");
    }
}
bool RayResolveLocked(ID3D12GraphicsCommandList *cl, const rrvrrt::RayResolve &in, UINT x, UINT y,
                      UINT z, bool &dispatched) noexcept {
    const UINT64 now = GetTickCount64();
    const unsigned m = rrraybudget::Valid(rrraybudget::mode.load(std::memory_order_relaxed));
    if (m != ray_logged_mode) {
        ray_logged_mode = m;
        ray_waited = {};
        ray_announced = false;
    }
    try {
        Slot *match{};
        bool ambiguous{}, after_sr{};
        for (auto &s : slots) {
            if (RawSlot(s, cl)) {
                ambiguous |= match != nullptr;
                match = &s;
            } else if (s.pre_live && s.pre.Get() == cl && (s.pending_sr || s.recorded))
                after_sr = true;
        }
        if (ambiguous)
            return RayRefuse(RayWhy::Ambiguous, m);
        if (!match) {
            if (!after_sr)
                ray_early = cl; // a later prepass on this list counts it as beforePrepass
            return RayRefuse(after_sr ? RayWhy::AfterSR : RayWhy::Outside, m);
        }
        auto &s = *match;
        if (s.ray_resolved)
            return RayRefuse(RayWhy::Repeated, m);
        s.ray_resolved = true;
        s.ray_resolve_cb = in.settings.address;
        RaySerial(s);
        // This resolve writes its Budget natively unless the patch below succeeds (A4).
        if (in.budget_resource)
            RayClearMark(in.budget_resource, in.budget_offset);
        ray_summary.Resolve(s.ray_applied, s.ray_applied && s.ray_apply_cb == in.settings.address);
        unsigned logged = 0;
        if (in.constants && now >= ray_retry_after && !ray_log_failed)
            logged = RayLogCopies(
                s, now, [&] { return s.gpu.LogResolve(cl, in.settings, in.scene, in.stats); });
        if (in.why != RayWhy::None)
            return RayRefuse(in.why, m);
        const bool same_budget = ray_gate.KnownBudget(in.budget_resource, in.budget_offset);
        ray_summary.Budget(same_budget);
        if (m == rrraybudget::Off)
            return RayRefuse(RayWhy::Mode, m);
        if (!same_budget)
            return RayRefuse(RayWhy::BudgetIdentity, m);
        if (NeedsReconfigure())
            return RayRefuse(RayWhy::Reconfigure, m);
        if (now < ray_retry_after)
            return RayRefuse(RayWhy::Memory, m);
        const auto plan = ray_gate.Decide(m, now, s.ray_applied ? &s.ray_lists : nullptr);
        if (plan.why != RayWhy::None)
            return RayRefuse(plan.why, m);
        // Patch only when this recording's LogResolve copied the governor's input (t0) for
        // this very frame: the gate checks it against the reader only for frames with
        // LogResolveStats. It also guarantees that LogResolve, which alone copies the resolve
        // serial (log word 24), was recorded before PatchResolve, which does not check it.
        if (!(logged & rrraybudget::LogResolveStats))
            return RayRefuse(RayWhy::StatsUnverified, m);
        const auto governor = pipelines.Governor(in.root);
        if (governor.full)
            return RayRefuse(RayWhy::GovernorCache, m);
        if (!governor.pso && governor.hr == E_OUTOFMEMORY) {
            ray_retry_after = now + rrraybudget::RetryMs;
            return RayRefuse(RayWhy::Memory, m);
        }
        if (!governor.pso) {
            char details[32];
            std::snprintf(details, sizeof(details), "hr=0x%08lx",
                          static_cast<unsigned long>(governor.hr));
            return RayStopResolve(RayWhy::Pipeline, details);
        }
        if (!s.gpu.PrepareRayPatch(pipelines, budget)) {
            ray_retry_after = now + rrraybudget::RetryMs;
            return RayRefuse(RayWhy::Memory, m);
        }
        ray_memory_waiting = false;
        // Everything that can throw is above this line. Below: noexcept calls and D3D12
        // recording only; once the native Dispatch is recorded this returns true.
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        const UINT64 va = s.gpu.PatchResolve(cl, in.settings, plan);
        if (!va)
            return RayRefuse(RayWhy::Constants, m); // nothing was recorded
        cl->SetComputeRootConstantBufferView(in.parameter, va);
        cl->Dispatch(x, y, z);
        dispatched = true;
        Order(cl); // the governor reads the Budget and t0 the resolve wrote
        cl->SetPipelineState(governor.pso);
        cl->Dispatch(1, 1, 1);
        Order(cl); // defensive (A6)
        cl->SetPipelineState(in.pipeline);
        cl->SetComputeRootConstantBufferView(in.parameter, in.settings.address);
        s.gpu.EndPatch(cl);
        RayPatched(s, plan, in, now, m);
        return true;
    } catch (const rrprivate::AllocationDeferred &) {
        // Only before anything was recorded (PrepareRayPatch).
        ray_retry_after = now + rrraybudget::RetryMs;
        if (!ray_memory_waiting) {
            ray_memory_waiting = true;
            RayWrite("Ray budget test waiting for GPU memory; it will retry");
        }
        ray_summary.Refused(RayWhy::Memory);
        ray_why = RayWhy::Memory;
        return dispatched;
    } catch (...) {
        RayStopResolve(RayWhy::Exception, "at the controller resolve");
        return dispatched;
    }
}
// Every recognised native VRRT apply of a recording, proven or not (A3).
void RayApplyLocked(ID3D12GraphicsCommandList *cl, const rrraybudget::Lists &lists,
                    const rrvrrt::RayApply &log) noexcept {
    try {
        const UINT64 now = GetTickCount64();
        Slot *match{};
        for (auto &s : slots)
            if (RawSlot(s, cl)) {
                if (match)
                    return; // more than one RR recording on this list
                match = &s;
            }
        if (!match)
            return;
        auto &s = *match;
        RaySerial(s);
        // The recording keeps its worst proof: an unproven one sticks, and a proven one that
        // differs from an earlier proven one in what the gate measures makes it ListsChanged.
        if (!s.ray_applied)
            s.ray_lists = lists;
        else if (s.ray_lists.why == RayWhy::None) {
            if (lists.why != RayWhy::None || RaySameProof(s.ray_lists, lists))
                s.ray_lists = lists;
            else
                s.ray_lists.why = RayWhy::ListsChanged;
        }
        s.ray_apply_cb = log.settings.address;
        s.ray_applied = true;
        // A raised Budget stays raised for the recording even if a later apply reads another.
        const auto consumed = RayConsumed(lists.budget_resource, lists.budget_offset);
        if (consumed.mask) {
            s.ray_budget |= consumed.mask;
            s.ray_armed = consumed.armed;
        }
        if (s.ray_resolved)
            ray_summary.SameBlock(s.ray_resolve_cb == s.ray_apply_cb);
        ray_gate.Apply(lists, now);
        ray_summary.Lists(lists);
        for (unsigned k = 0; k < 2; ++k) {
            const bool table = lists.v[k].binding == rrraybudget::Binding::Table;
            ray_list_offsets[k] = table ? lists.v[k].offset : 0;
            rrraybudget::published[k].store(table ? lists.v[k].resource : 0,
                                            std::memory_order_relaxed);
        }
        if ((log.settings.upload || log.scene.upload) && now >= ray_retry_after && !ray_log_failed)
            RayLogCopies(s, now, [&] { return s.gpu.LogApply(cl, log.settings, log.scene); });
    } catch (...) {
        RayStop(RayWhy::Exception, "at the VRRT apply");
    }
}
