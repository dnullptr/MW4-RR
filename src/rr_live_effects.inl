// Included inside Controller. Transparent effects responsivity test
// (rr_transparent_effects.h): RR reads a mask that makes it lean on the current
// frame where a see-through layer added light. The mask is a development test
// texture (UseTest) and replaces the water or SSS mask, which it carries along.
private:
DevTest fx_test;
rrfx::Pipeline fx_pipeline;
// A failed setup keeps the test off until a setting changes.
bool fx_unavailable{};
unsigned fx_unavailable_changes{};
// Why the latest evaluation did not send the mask (None: it did).
rrfx::Hold fx_hold{rrfx::Hold::Off};
// 10 s summary while on.
UINT64 fx_report{};
unsigned fx_frames{}, fx_merged{};

void FxReconfigured() {
    fx_test = {};
    fx_pipeline = {};
    fx_unavailable = false;
    fx_unavailable_changes = 0;
    fx_hold = rrfx::Hold::Off;
    fx_report = 0;
    fx_frames = fx_merged = 0;
}
// Whether this evaluation sends RR the mask. Everything that can fail happens
// here, before any command is recorded; a failure only keeps the test off.
bool FxUse(Slot &s, const rrfx::Tuning &t, bool sky_edge, bool preset_known, unsigned preset,
           bool color_test) {
    fx_test.active = false;
    fx_hold = rrfx::Check(t, sky_edge, preset_known, preset,
                          s.transparency_used && s.gpu.transparency.guide, color_test);
    if (fx_hold != rrfx::Hold::None) {
        fx_report = 0;
        fx_frames = fx_merged = 0;
        return false;
    }
    const auto changes = rrfx::changes.load(std::memory_order_relaxed);
    if (fx_unavailable && fx_unavailable_changes == changes)
        return false;
    fx_unavailable = false;
    try {
        fx_pipeline.Initialize(pipelines.device.Get());
        s.gpu.fx.Prepare(fx_pipeline);
    } catch (const std::exception &e) {
        fx_unavailable = true;
        fx_unavailable_changes = changes;
        char line[200];
        snprintf(line, sizeof(line), "Transparent effects mask unavailable: %s", e.what());
        Log(line);
        return false;
    }
    return UseTest(fx_test, true, changes, s.gpu.w, s.gpu.h, DXGI_FORMAT_R16_FLOAT,
                   "Transparent effects mask");
}
// After the transparency Resolve and the water and SSS passes, so the color
// before transparency and the mask it carries along are written, and before the
// refraction dropout, which may change the color before transparency in place.
// A frame whose transparency is not admitted at submission keeps the existing mask. existing: the
// mask RR would otherwise get (NON_PIXEL_SHADER_RESOURCE), or null.
void FxRecord(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Options &o,
              const rrfx::Tuning &t, ID3D12Resource *existing) {
    s.gpu.fx.Record(cl, fx_pipeline,
                    {s.gpu.sr[0].Get(), s.gpu.transparency.guide.Get(), existing,
                     fx_test.texture.Get(), s.gpu.header->GetGPUVirtualAddress()},
                    rrfx::Make(t, s.gpu.w, s.gpu.h, o.color_x, o.color_y, existing != nullptr));
    ++fx_frames;
    fx_merged += existing != nullptr;
    const auto now = GetTickCount64();
    if (!fx_report)
        fx_report = now;
    else if (now - fx_report >= 10000) {
        char line[200];
        snprintf(line, sizeof(line),
                 "Transparent effects mask (%.1fs): %u evaluations, %u merged with the water or "
                 "SSS mask; strength %.2f",
                 double(now - fx_report) / 1000, fx_frames, fx_merged,
                 rrfx::ClampStrength(t.strength));
        Log(line);
        fx_report = now;
        fx_frames = fx_merged = 0;
    }
}
public:
rrfx::Report FxStatus() {
    std::lock_guard lock(mutex);
    const auto t = rrfx::Read();
    if (!t.enabled || rrfx::ClampStrength(t.strength) == 0)
        return rrfx::Report::Off;
    const bool running = enabled && !failed && !retired && feature;
    if (!running)
        return rrfx::Report::Waiting;
    if (fx_hold != rrfx::Hold::None && fx_hold != rrfx::Hold::Off)
        return rrfx::ReportOf(fx_hold);
    const auto changes = rrfx::changes.load(std::memory_order_relaxed);
    if (fx_unavailable && fx_unavailable_changes == changes)
        return rrfx::Report::Unavailable;
    switch (Test(fx_test, changes, true)) {
    case TestState::Blocked:
        return rrfx::Report::Blocked;
    case TestState::Active:
        return rrfx::Report::Active;
    default:
        return rrfx::Report::Waiting;
    }
}
