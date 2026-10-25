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
// governor wrote it, so they do not age a mark. A mark outlives a generation change; an apply of
// a later generation that consumes it is raised but unverified (its snapshot is of other lists).
struct RayMark {
    UINT64 resource{}, offset{};
    unsigned mask{};
    UINT resolves{};   // ray_resolves just after the patched resolve was counted
    UINT generation{}; // the controller generation of the patch
    rrraybudget::Armed armed;
};
static constexpr UINT RayMarkLife = 8;
// A stop's details buffer: the widest, ListsAfterArming with every field at its maximum and
// register 253, fits.
static constexpr std::size_t RayDetailsBytes = 384;
static constexpr UINT64 RayActiveMs = 1000; // Status reports the last resolve's outcome this long
rrraybudget::Gate ray_gate;
rrraybudget::Summary ray_summary;
unsigned ray_logged_mode = rrraybudget::Off; // the mode the waiting/active lines are for
UINT64 ray_retry_after{};
ID3D12GraphicsCommandList *ray_early{}; // compared only, never dereferenced
std::array<bool, unsigned(RayWhy::Count)> ray_waited{};
rrraybudget::Plan ray_plan; // the last patched plan
// The outcome of the last resolve in a recording (a match, not Repeated), at ray_why_at: its
// refusal, None after a patch. A resolve placed elsewhere (Outside, AfterSR, Ambiguous, Repeated)
// sets only ray_misplaced at ray_misplaced_at, so it never changes what the status says.
RayWhy ray_why = RayWhy::Mode, ray_misplaced = RayWhy::None;
UINT64 ray_why_at{}, ray_misplaced_at{};
bool ray_announced{};
std::array<RayMark, 4> ray_marks{};
unsigned ray_next_mark{};
// Applies in a row, in every mode, that read the Budget their own recording's resolve wrote
// before them (A2: the governor then bounds the frame that consumes the Budget it governed). A
// resolve patches only from rrraybudget::ProofFrames on; any other apply in a RawSlot resets it.
unsigned ray_placement{};
UINT ray_next_serial{};
// Every cs_vrrt_resolve dispatch PreserveRayBudget sees (RayBudgetResolveSeen, lock-free: it is
// counted before the RR-enabled check, without the mutex).
std::atomic<UINT> ray_resolves{};
// The logging copies stop for the session after a failure other than a memory wait; the
// Summary has no failure counter, so the first failure and the first dropped readback are
// logged once each.
bool ray_log_failed{}, ray_log_warned{}, ray_memory_waiting{};
// What the ray buffers grew budget.limit by (AllocateRay reserves on top of the limit, never out
// of the headroom below it), given back at Reconfigure with test_reserve so that rebuilds do not
// ratchet the limit.
UINT64 ray_reserve{};
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
// A refused resolve, counted and logged once per reason per mode selection while a test mode is
// selected. Returns false: the native resolve runs.
bool RayCounted(RayWhy why, unsigned m) noexcept {
    ray_summary.Refused(why);
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
// A resolve in a recording refused: the status's reason (RayCounted).
bool RayRefuse(RayWhy why, unsigned m, UINT64 now) noexcept {
    ray_why = why;
    ray_why_at = now;
    return RayCounted(why, m);
}
// A resolve outside any single recording, or a second one in it (RayCounted).
bool RayMisplaced(RayWhy why, unsigned m, UINT64 now) noexcept {
    ray_misplaced = why;
    ray_misplaced_at = now;
    return RayCounted(why, m);
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
// Runs `prepare` (a PrepareRayLog or PrepareRayPatch call) and adds what it grew budget.limit by
// to ray_reserve, saturating, on return and on every exception: AllocateRay's catch keeps the
// growth for the buffers it allocated, and PrepareRayLog's first Map can throw after all of it.
// Nothing else changes budget.limit inside those calls.
template <class Prepare> bool RayPrepare(Prepare prepare) {
    struct Growth {
        Controller &c;
        const UINT64 before;
        ~Growth() {
            if (c.budget.limit > before) {
                const UINT64 grown = c.budget.limit - before;
                c.ray_reserve =
                    grown > UINT64_MAX - c.ray_reserve ? UINT64_MAX : c.ray_reserve + grown;
            }
        }
    } growth{*this, budget.limit};
    return prepare();
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
    *at = {resource, offset, plan.mask, resolves, generation, plan.armed};
}
// What an apply reading the Budget (resource, offset) consumed: that identity's live mark, or
// for an unknown identity (resource 0) the union of the live marks' lobes with the newest
// mark's snapshot (conservative: the frame is then checked as raised), of an older generation
// when any of them is.
RayMark RayConsumed(UINT64 resource, UINT64 offset) const noexcept {
    RayMark found;
    found.generation = generation;
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
        if (m.generation != generation)
            found.generation = m.generation;
        const UINT age = RayMarkAge(m);
        if (age < newest) {
            newest = age;
            found.armed = m.armed;
        }
    }
    return found;
}
static bool RayRecent(UINT64 at, UINT64 now) noexcept {
    return at && now >= at && now - at <= RayActiveMs;
}
// While RR runs, the outcome of the last resolve in a recording when it is under RayActiveMs old
// and was in the selected mode: active after a patch, else its refusal. Otherwise a recent
// misplaced resolve's reason, else ResolveIdle. RrOff while RR is off, failed or retired.
rrraybudget::Status StatusLocked(UINT64 now) const noexcept {
    rrraybudget::Status s;
    s.mode = rrraybudget::Valid(rrraybudget::mode.load(std::memory_order_relaxed));
    s.stopped = ray_gate.Stopped();
    const bool rr = enabled && !failed && !retired;
    const bool placed = rr && s.mode == ray_logged_mode && RayRecent(ray_why_at, now);
    s.active = placed && ray_why == RayWhy::None && ray_plan.mask;
    s.native = ray_plan.native;
    s.target = ray_plan.target;
    for (unsigned l = 0; l < 2; ++l)
        s.raised[l] = (ray_plan.mask >> l & 1) != 0;
    if (s.mode == rrraybudget::Off)
        s.why = RayWhy::Mode;
    else if (!rr)
        s.why = RayWhy::RrOff;
    else if (placed)
        s.why = ray_why;
    else if (s.mode == ray_logged_mode && RayRecent(ray_misplaced_at, now))
        s.why = ray_misplaced;
    else
        s.why = RayWhy::ResolveIdle;
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
            if ((raised >> l & 1) && !rrraybudget::SafeThreshold(int(f[Limits + 2 * l]))) {
                std::snprintf(out, size, "lobe%u threshold=%d", l + 1, int(f[Limits + 2 * l]));
                return;
            }
        break;
    case RayWhy::PatchMismatch:
        for (unsigned l = 0; l < 2; ++l)
            if ((c.patched >> l & 1) && !rrraybudget::Executed(c, l)) {
                if (c.logged & rrraybudget::LogExecuted)
                    std::snprintf(out, size, "lobe%u executed=0x%08x written=0x%08x", l + 1,
                                  unsigned(c.log[rrraybudget::LogWordExecuted + l]),
                                  unsigned(c.written[l]));
                else
                    std::snprintf(out, size, "lobe%u executed=n/a written=0x%08x", l + 1,
                                  unsigned(c.written[l]));
                return;
            }
        break;
    case RayWhy::ListsAfterArming: {
        const int at =
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
        if ((c.logged & rrraybudget::LogApplyScene253) && at > 0 && std::size_t(at) < size)
            std::snprintf(out + at, size - at, " scene253=%u",
                          unsigned(c.log[rrraybudget::LogWordScene253]));
        return;
    }
    case RayWhy::Unsupported:
        std::snprintf(out, size, "pixels=%u", unsigned(f[rrdensity::Unsupported]));
        return;
    case RayWhy::StatsMismatch: {
        // A raised frame that reached this check has its own usable proof (ChangedAfterArming).
        const auto pixels = rrraybudget::FramePixels(c, {});
        for (unsigned l = 0; l < 2; ++l)
            if ((raised >> l & 1) && !rrraybudget::StatsCover(c, l, pixels)) {
                std::snprintf(out, size, "lobe%u x=%u rawRays=%u max=%llu", l + 1,
                              unsigned(c.log[rrraybudget::LogWordStats + l]),
                              unsigned(f[RawRays + l]),
                              static_cast<unsigned long long>(rrraybudget::MaxPixelRays * pixels));
                return;
            }
        break;
    }
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
    if (!serial || log[LogWordResolveSerial] != serial)
        logged &= ~LogResolveHalf;
    if (!serial || log[LogWordApplySerial] != serial)
        logged &= ~LogApplyHalf;
    return logged;
}
// A raised recording that no Complete covers: counted once (from UnverifiedLimit in a row
// Decide waits; the UnverifiedStop-th since the last verified raised frame stops the test).
void RayUnverified(Slot &s) noexcept {
    if ((s.ray_patched | s.ray_budget) && !s.ray_done) {
        const auto why = ray_gate.Unverified();
        if (why != RayWhy::None) {
            char details[RayDetailsBytes];
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
        // Complete runs only for a frame whose reader rates and log halves are its own. Rates of
        // a recording whose prepass list was not submitted, or whose apply half did not run as
        // recorded (a stale apply serial), are not this frame's; nor is the arming snapshot of a
        // frame that consumed a mark of an older generation, or raised Budgets of two plans (no
        // one snapshot judges both). A patch is verified against its own resolve half: one that
        // was predicated away, skipped or not read back verifies nothing. Such a frame, raised, is
        // unverified (an unraised one measures nothing: no Complete, no Summary::Completed), after
        // the PatchMismatch check when its resolve half did verify.
        using rrraybudget::LogApplyHalf;
        const bool read = state == RayLogState::Read;
        const bool resolve_own =
            read && s.ray_serial && log[rrraybudget::LogWordResolveSerial] == s.ray_serial;
        const bool apply_recorded = read && (s.ray_logged & LogApplyHalf);
        const bool apply_stale =
            apply_recorded && log[rrraybudget::LogWordApplySerial] != s.ray_serial;
        if (!s.pre_submitted || s.ray_old_mark || s.ray_mixed_armed || apply_stale ||
            (s.ray_patched && !resolve_own)) {
            if (s.ray_patched && resolve_own)
                for (unsigned l = 0; l < 2; ++l)
                    if ((s.ray_patched >> l & 1) && !rrraybudget::Executed(c, l)) {
                        s.ray_done = true;
                        char details[RayDetailsBytes];
                        RayStopDetails(RayWhy::PatchMismatch, c, details, sizeof(details));
                        RayStop(RayWhy::PatchMismatch, details);
                        return;
                    }
            RayUnverified(s);
            return;
        }
        // A raised frame whose apply half was not recorded (a logging wait, logging off, apply
        // constants outside an upload heap) has no proof that the reader's rates are its own: the
        // log and list checks run, then it counts as unverified (Complete on an uncounted frame).
        if (raised && !apply_recorded)
            c.rates = {};
        ray_summary.Completed(c);
        const auto why = ray_gate.Complete(c);
        s.ray_done = true;
        if (why != RayWhy::None) {
            char details[RayDetailsBytes];
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
                      "thresholds at %d or above and, in a frame whose rays would exceed a "
                      "lobe's headroom, sets that lobe's threshold to %d (at most one ray per "
                      "pixel with clamp on; the game's own behaviour with clamp off)",
                      extent ? double(plan.armed.capacity[0]) / extent : 0.0,
                      extent ? double(plan.armed.capacity[1]) / extent : 0.0,
                      rrraybudget::NegativeLimit, rrraybudget::FallbackThreshold);
    RayWrite(line);
}
// The logging copies at a ray site, in every mode (A6): PrepareRayLog, then the recording's
// serial (WriteRaySerial drops it while ray_map is null), then `record` under the internal
// guards. A memory wait (PrepareRayLog false or AllocationDeferred) retries after RetryMs; any
// other failure disables logging for the session. Never a stop. Returns the bits recorded.
template <class Record> unsigned RayLogCopies(Slot &s, UINT64 now, Record record) noexcept {
    try {
        if (!RayPrepare([&] { return s.gpu.PrepareRayLog(pipelines, budget); })) {
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
        ray_why_at = now;
        ray_why = RayWhy::None;
        if (!ray_announced) {
            ray_announced = true;
            RayActiveLog(m, plan);
        }
    } catch (...) {
        RayStop(RayWhy::Exception, "after the controller dispatch");
    }
}
// What a Pipelines::Governor result means at a resolve: None with a PSO; waits for a full cache
// (GovernorCache) or memory (Memory, retried); Context when nothing was created (a root of
// another device: never sticky); a sticky Pipeline stop only for a real creation failure.
static RayWhy RayGovernor(const Pipelines::GovernorPipeline &g) noexcept {
    if (g.pso)
        return RayWhy::None;
    if (g.full)
        return RayWhy::GovernorCache;
    if (g.hr == E_OUTOFMEMORY)
        return RayWhy::Memory;
    return g.created ? RayWhy::Pipeline : RayWhy::Context;
}
// At every generation change (Reconfigure, SetEnabled on either edge): the gate's apply streak
// and the placement streak restart, so the first resolves of the new generation are refused
// until both are proven again.
void RayRestart() noexcept {
    ray_gate.Restart(GetTickCount64());
    ray_placement = 0;
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
        // This resolve writes its Budget natively unless the patch below succeeds (A4), wherever
        // it runs: a proven identity loses its mark (a patch sets it again).
        if (in.budget_resource)
            RayClearMark(in.budget_resource, in.budget_offset);
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
            return RayMisplaced(RayWhy::Ambiguous, m, now);
        if (!match) {
            if (!after_sr)
                ray_early = cl; // a later prepass on this list counts it as beforePrepass
            return RayMisplaced(after_sr ? RayWhy::AfterSR : RayWhy::Outside, m, now);
        }
        auto &s = *match;
        if (s.ray_resolved)
            return RayMisplaced(RayWhy::Repeated, m, now);
        s.ray_resolved = true;
        s.ray_resolve_cb = in.settings.address;
        RaySerial(s);
        // Its Budget identity, whatever the refusal below, is what a same-frame apply must read
        // (A2).
        if (in.budget_resource) {
            s.ray_resolve_budget_resource = in.budget_resource;
            s.ray_resolve_budget_offset = in.budget_offset;
        }
        // sameB1AsApply compares two known constant blocks only.
        ray_summary.Resolve(s.ray_applied, s.ray_applied && s.ray_apply_cb == in.settings.address,
                            s.ray_apply_cb && in.settings.address);
        unsigned logged = 0;
        if (in.constants && now >= ray_retry_after && !ray_log_failed)
            logged = RayLogCopies(
                s, now, [&] { return s.gpu.LogResolve(cl, in.settings, in.scene, in.stats); });
        if (in.why != RayWhy::None)
            return RayRefuse(in.why, m, now);
        const bool same_budget = ray_gate.KnownBudget(in.budget_resource, in.budget_offset);
        ray_summary.Budget(same_budget);
        if (m == rrraybudget::Off)
            return RayRefuse(RayWhy::Mode, m, now);
        if (!same_budget)
            return RayRefuse(RayWhy::BudgetIdentity, m, now);
        // A2: the Budget this resolve governs must be read by this recording's apply, after it.
        // A resolve after the apply would govern the next recording's frame instead, and the
        // placement streak shows that the applies read the Budget their own resolve wrote.
        if (s.ray_applied)
            return RayRefuse(RayWhy::AfterApply, m, now);
        if (ray_placement < rrraybudget::ProofFrames)
            return RayRefuse(RayWhy::Placement, m, now);
        if (NeedsReconfigure())
            return RayRefuse(RayWhy::Reconfigure, m, now);
        if (now < ray_retry_after)
            return RayRefuse(RayWhy::Memory, m, now);
        // No resolve records its stats copy any more: say so, not StatsUnverified forever.
        if (ray_log_failed)
            return RayRefuse(RayWhy::LoggingOff, m, now);
        const auto plan = ray_gate.Decide(m, now, nullptr); // no apply in this recording yet
        if (plan.why != RayWhy::None)
            return RayRefuse(plan.why, m, now);
        // Patch only when this recording's LogResolve copied the governor's input (t0) for
        // this very frame: the gate checks it against the reader only for frames with
        // LogResolveStats. It also guarantees that LogResolve, which alone copies the resolve
        // serial (LogWordResolveSerial), was recorded before PatchResolve, which requires a
        // serial too.
        if (!(logged & rrraybudget::LogResolveStats))
            return RayRefuse(RayWhy::StatsUnverified, m, now);
        const auto governor = pipelines.Governor(in.root);
        const auto governor_why = RayGovernor(governor);
        if (governor_why == RayWhy::Memory)
            ray_retry_after = now + rrraybudget::RetryMs;
        if (governor_why == RayWhy::Pipeline) {
            char details[32];
            std::snprintf(details, sizeof(details), "hr=0x%08lx",
                          static_cast<unsigned long>(governor.hr));
            return RayStopResolve(RayWhy::Pipeline, details);
        }
        if (governor_why != RayWhy::None)
            return RayRefuse(governor_why, m, now);
        if (!RayPrepare([&] { return s.gpu.PrepareRayPatch(pipelines, budget); })) {
            ray_retry_after = now + rrraybudget::RetryMs;
            return RayRefuse(RayWhy::Memory, m, now);
        }
        ray_memory_waiting = false;
        // Everything that can throw is above this line, and nothing above it recorded anything
        // but the self-contained logging copies (each closes its own t0 barrier pair), after which
        // a refusal still runs the native resolve. Below: noexcept calls and D3D12 recording
        // only; once the native Dispatch is recorded this returns true.
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        const UINT64 va = s.gpu.PatchResolve(cl, in.settings, plan);
        if (!va)
            return RayRefuse(RayWhy::Constants, m, now); // nothing was recorded
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
        // Only from PrepareRayPatch: the list holds at most the self-contained logging copies, and
        // the native resolve still runs.
        ray_retry_after = now + rrraybudget::RetryMs;
        if (!ray_memory_waiting) {
            ray_memory_waiting = true;
            RayWrite("Ray budget test waiting for GPU memory; it will retry");
        }
        ray_summary.Refused(RayWhy::Memory);
        ray_why = RayWhy::Memory;
        ray_why_at = now;
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
                if (match) {
                    // More than one RR recording on this list: an unproven apply (A3), which
                    // also breaks the placement streak.
                    ray_gate.Apply(rrraybudget::Lists{}, now);
                    ray_placement = 0;
                    return;
                }
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
        // A2 placement: a proven apply that reads the Budget its own recording's resolve wrote
        // before it. Any other apply breaks the streak (lagged consumption, a skipped resolve, a
        // ping-pong Budget read a frame late, an unproven apply).
        const bool same_frame = lists.why == RayWhy::None && s.ray_resolved &&
                                s.ray_resolve_budget_resource &&
                                s.ray_resolve_budget_resource == lists.budget_resource &&
                                s.ray_resolve_budget_offset == lists.budget_offset;
        if (!same_frame)
            ray_placement = 0;
        else if (ray_placement < UINT_MAX)
            ++ray_placement;
        // A raised Budget stays raised for the recording even if a later apply reads another.
        const auto consumed = RayConsumed(lists.budget_resource, lists.budget_offset);
        if (consumed.mask) {
            // A3: each raised frame is judged against its own plan's snapshot; a recording whose
            // applies read raised Budgets of two plans has none (counted unverified).
            s.ray_mixed_armed |= s.ray_budget && !(s.ray_armed == consumed.armed);
            s.ray_budget |= consumed.mask;
            s.ray_armed = consumed.armed;
            s.ray_old_mark |= consumed.generation != generation;
        }
        if (s.ray_resolved && s.ray_resolve_cb && s.ray_apply_cb)
            ray_summary.SameBlock(s.ray_resolve_cb == s.ray_apply_cb);
        ray_gate.Apply(lists, now);
        ray_summary.Lists(lists);
        if (same_frame)
            ray_summary.SameFrame();
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
