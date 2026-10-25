// Included inside Controller. Refraction dropout test (rr_refraction_dropout.h):
// RR reads a copy of its color with pixels inside this frame's refraction
// coverage dropped at random. The copy is a development test texture (UseTest).
private:
DevTest dropout_test;
rrdropout::Pipeline dropout_pipeline;
// A failed setup keeps the test off until a dropout setting changes.
bool dropout_unavailable{};
unsigned dropout_unavailable_changes{};
// The latest evaluation that used the copy had the glass capture's coverage
// target for its frame (source) and refraction coverage in it (covered).
bool dropout_source{}, dropout_covered{};
// 10 s summary while on: evaluations with the copy, with coverage, and with
// the color before transparency dropped too.
UINT64 dropout_report{};
unsigned dropout_frames{}, dropout_covered_frames{}, dropout_before_frames{};

// The glass capture's refraction coverage for this slot's frame. Empty until
// the transparency albedo blend's coverage capture is present.
struct DropoutCoverage {
    ID3D12Resource *target{};
    D3D12_GPU_VIRTUAL_ADDRESS admitted{};
    bool source{}, ready{};
};
DropoutCoverage DropoutCoverageOf(const Slot &) const {
    return {};
}
void DropoutSummaryReset() {
    dropout_report = 0;
    dropout_frames = dropout_covered_frames = dropout_before_frames = 0;
}
void DropoutReconfigured() {
    dropout_test = {};
    dropout_pipeline = {};
    dropout_unavailable = dropout_source = dropout_covered = false;
    dropout_unavailable_changes = 0;
    DropoutSummaryReset();
}
// Whether this evaluation sends RR the copy. Everything that can fail happens
// here, before any command is recorded; a failure only keeps the test off.
bool DropoutUse(Slot &s, const rrdropout::Tuning &t, bool color_test, DXGI_FORMAT format) {
    dropout_test.active = false;
    if (!rrdropout::Wanted(t, color_test)) {
        DropoutSummaryReset();
        return false;
    }
    const auto changes = rrdropout::changes.load(std::memory_order_relaxed);
    if (dropout_unavailable && dropout_unavailable_changes == changes)
        return false;
    dropout_unavailable = false;
    try {
        dropout_pipeline.Initialize(pipelines.device.Get());
        Require(dropout_pipeline.Writable(format), "no typed UAV store for the color format");
        s.gpu.dropout.Prepare(dropout_pipeline);
    } catch (const std::exception &e) {
        dropout_unavailable = true;
        dropout_unavailable_changes = changes;
        char line[200];
        snprintf(line, sizeof(line), "Refraction dropout unavailable: %s", e.what());
        Log(line);
        return false;
    }
    return UseTest(dropout_test, true, changes, s.gpu.w, s.gpu.h, format, "Refraction dropout");
}
// After the transparency Resolve, so the color before transparency is written.
void DropoutRecord(Slot &s, ID3D12GraphicsCommandList *cl, const rreval::Options &o,
                   const rrdropout::Tuning &t) {
    const auto coverage = DropoutCoverageOf(s);
    const bool before = rrdropout::DropBeforeTransparency(
        t, s.transparency_used && s.gpu.transparency.guide, dropout_pipeline.typed_load);
    rrdropout::Inputs in{s.gpu.sr[0].Get(), dropout_test.texture.Get(),
                         coverage.ready ? coverage.target : nullptr,
                         coverage.ready ? coverage.admitted : s.gpu.header->GetGPUVirtualAddress(),
                         before ? s.gpu.transparency.guide.Get() : nullptr};
    const auto format = dropout_test.texture->GetDesc().Format;
    s.gpu.dropout.Record(
        cl, dropout_pipeline, in,
        rrdropout::Make(t, s.gpu.w, s.gpu.h, o.color_x, o.color_y, s.serial, coverage.ready, before,
                        rrdropout::FormatMax(format == DXGI_FORMAT_R11G11B10_FLOAT,
                                             format == DXGI_FORMAT_R16G16B16A16_FLOAT)));
    dropout_source = coverage.source;
    dropout_covered = coverage.ready;
    ++dropout_frames;
    dropout_covered_frames += coverage.ready;
    dropout_before_frames += before;
    const auto now = GetTickCount64();
    if (!dropout_report)
        dropout_report = now;
    else if (now - dropout_report >= 10000) {
        char line[240];
        snprintf(line, sizeof(line),
                 "Refraction dropout (%.1fs): %u evaluations, %u with refraction coverage, %u "
                 "also dropped the color before transparency; strength %.2f%s",
                 double(now - dropout_report) / 1000, dropout_frames, dropout_covered_frames,
                 dropout_before_frames, rrdropout::ClampStrength(t.strength),
                 t.show_mask ? ", mask view" : "");
        Log(line);
        dropout_report = now;
        dropout_frames = dropout_covered_frames = dropout_before_frames = 0;
    }
}
public:
rrdropout::Report DropoutStatus() {
    std::lock_guard lock(mutex);
    const auto t = rrdropout::Read();
    if (!t.enabled)
        return rrdropout::Report::Off;
    const auto running = enabled && !failed && !retired && feature;
    if (color_test.active && running)
        return rrdropout::Report::ColorTest;
    const auto changes = rrdropout::changes.load(std::memory_order_relaxed);
    if (dropout_unavailable && dropout_unavailable_changes == changes)
        return rrdropout::Report::Unavailable;
    switch (Test(dropout_test, changes, true)) {
    case TestState::Blocked:
        return rrdropout::Report::Blocked;
    case TestState::Active:
        return dropout_covered  ? rrdropout::Report::Active
               : dropout_source ? rrdropout::Report::NoRefraction
                                : rrdropout::Report::NoSource;
    default:
        return rrdropout::Report::Waiting;
    }
}
