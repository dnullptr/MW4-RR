// Included inside Controller. Sheet motion test (rr_sheet_motion.h): where the glass
// capture's coverage is near-opaque, RR gets the depth and camera motion of a surface
// at an assumed distance. RR reads a copy of its motion (a development test texture,
// UseTest); the linear depth copy (weapon depth) is changed in place.
private:
DevTest sheet_test;
rrsheet::Pipeline sheet_pipeline;
// A failed setup keeps the test off until a sheet setting changes.
bool sheet_unavailable{};
unsigned sheet_unavailable_changes{};
// The camera of the latest evaluation, and the pair the current evaluation uses.
rrsheet::Camera sheet_previous{}, sheet_current{}, sheet_before{};
// The latest evaluation that used the copy: no linear depth, a usable previous camera,
// and the glass capture's coverage target for its frame.
bool sheet_no_depth{}, sheet_camera{}, sheet_source{};
// 10 s summary while on.
UINT64 sheet_report{};
unsigned sheet_frames{}, sheet_camera_frames{}, sheet_source_frames{};

void SheetSummaryReset() {
    sheet_report = 0;
    sheet_frames = sheet_camera_frames = sheet_source_frames = 0;
}
void SheetReconfigured() {
    sheet_test = {};
    sheet_pipeline = {};
    sheet_unavailable = sheet_no_depth = sheet_camera = sheet_source = false;
    sheet_unavailable_changes = 0;
    sheet_previous = sheet_current = sheet_before = {};
    SheetSummaryReset();
}
// Called once per evaluation: keeps the camera for the next frame, whether or not the
// test is on. Then whether this evaluation sends RR the motion copy. Everything that can
// fail happens here, before any command is recorded; a failure only keeps the test off.
bool SheetUse(Slot &s, const rrsheet::Tuning &t, bool linear_depth, bool reset,
              DXGI_FORMAT format) {
    sheet_before = sheet_previous;
    sheet_current = {s.view, s.projection, s.frame, s.viewport, true};
    sheet_previous = sheet_current;
    sheet_camera = rrsheet::Consecutive(sheet_before, s.frame, s.viewport, reset);
    sheet_test.active = false;
    if (!t.enabled) {
        SheetSummaryReset();
        return false;
    }
    sheet_no_depth = !linear_depth;
    if (!linear_depth)
        return false;
    const auto changes = rrsheet::changes.load(std::memory_order_relaxed);
    if (sheet_unavailable && sheet_unavailable_changes == changes)
        return false;
    sheet_unavailable = false;
    try {
        sheet_pipeline.Initialize(pipelines.device.Get());
        Require(sheet_pipeline.Writable(format), "no typed UAV store for the motion format");
        s.gpu.sheet.Prepare(sheet_pipeline);
    } catch (const std::exception &e) {
        sheet_unavailable = true;
        sheet_unavailable_changes = changes;
        char line[200];
        snprintf(line, sizeof(line), "Sheet motion test unavailable: %s", e.what());
        Log(line);
        return false;
    }
    return UseTest(sheet_test, true, changes, s.gpu.w, s.gpu.h, format, "Sheet motion test");
}
// After CoverageBlend and WeaponDepth, so this frame's coverage is known and the
// linear depth is written.
void SheetRecord(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Options &o,
                 const rrsheet::Tuning &t, ID3D12Resource *linear_depth) {
    const auto coverage = DropoutCoverageOf(s, o);
    rrsheet::Constants c{};
    const bool camera =
        sheet_camera && rrsheet::Make(t, s.gpu.w, s.gpu.h, o.motion_x, o.motion_y,
                                      o.motion_x_scale, o.motion_y_scale, sheet_current,
                                      sheet_before, coverage.source, c);
    if (!camera)
        c.flags = 0;
    rrsheet::Inputs in{s.gpu.sr[2].Get(), sheet_test.texture.Get(), linear_depth,
                       c.flags ? coverage.target : nullptr,
                       c.flags ? coverage.admitted : s.gpu.header->GetGPUVirtualAddress()};
    s.gpu.sheet.Record(cl, sheet_pipeline, in, c);
    sheet_camera = camera;
    sheet_source = coverage.source;
    ++sheet_frames;
    sheet_camera_frames += camera;
    sheet_source_frames += camera && coverage.source;
    const auto now = GetTickCount64();
    if (!sheet_report)
        sheet_report = now;
    else if (now - sheet_report >= 10000) {
        char line[280];
        snprintf(line, sizeof(line),
                 "Sheet motion test (%.1fs): %u evaluations, %u with the previous camera, %u "
                 "also with transparency captured (before the submission check); distance "
                 "%.0f, opacity threshold %.2f",
                 double(now - sheet_report) / 1000, sheet_frames, sheet_camera_frames,
                 sheet_source_frames, rrsheet::ClampDistance(t.distance),
                 rrsheet::ClampThreshold(t.threshold));
        Log(line);
        sheet_report = now;
        sheet_frames = sheet_camera_frames = sheet_source_frames = 0;
    }
}
public:
rrsheet::Report SheetStatus() {
    std::lock_guard lock(mutex);
    const auto t = rrsheet::Read();
    if (!t.enabled)
        return rrsheet::Report::Off;
    const auto running = enabled && !failed && !retired && feature;
    if (running && sheet_no_depth)
        return rrsheet::Report::NoLinearDepth;
    const auto changes = rrsheet::changes.load(std::memory_order_relaxed);
    if (sheet_unavailable && sheet_unavailable_changes == changes)
        return rrsheet::Report::Unavailable;
    switch (Test(sheet_test, changes, true)) {
    case TestState::Blocked:
        return rrsheet::Report::Blocked;
    case TestState::Active:
        return !sheet_camera  ? rrsheet::Report::NoCamera
               : sheet_source ? rrsheet::Report::Active
                              : rrsheet::Report::NoSource;
    default:
        return rrsheet::Report::Waiting;
    }
}
