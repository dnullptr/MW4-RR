#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/rr_capture_model.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace rrcapture;
using Seq = Sequence<Frame<int>>;

static Frame<int> Make(unsigned serial, int buffer, bool delivered = true, unsigned w = 4,
                       unsigned h = 3) {
    Frame<int> f;
    f.buffer = buffer;
    f.width = w;
    f.height = h;
    f.meta.serial = serial;
    f.meta.delivered = delivered;
    return f;
}
static Seq Collecting() {
    Seq s;
    assert(s.Request());
    s.Begin({1, 2, 3, 4, 5, 6, 7, 8});
    assert(s.state == State::Collecting);
    return s;
}
static bool Contains(const std::string &text, const char *part) {
    return text.find(part) != std::string::npos;
}
// Brackets and braces nest and balance, outside strings; strings and escapes terminate.
static bool Balanced(const std::string &text) {
    std::string open;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\')
                ++i;
            else if (c == '"')
                in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            open += c;
        } else if (c == '}' || c == ']') {
            if (open.empty() || open.back() != (c == '}' ? '{' : '['))
                return false;
            open.pop_back();
        }
    }
    return open.empty() && !in_string;
}

static void Lifecycle() {
    Seq s;
    assert(s.state == State::Idle && !s.Active());
    assert(!s.Wants(0));
    assert(s.Request() && s.state == State::Armed && s.Active());
    assert(!s.Request()); // Ignored while Armed.
    assert(!s.Wants(0));  // Armed does not collect yet.
    s.Begin({1, 2, 3, 4, 5, 6, 7, 8});
    assert(s.state == State::Collecting && s.Active() && s.pool.size() == 8);
    assert(!s.Request()); // Ignored while Collecting.
    assert(s.state == State::Collecting && s.pool.size() == 8);
    s.run.assign(3, Make(1, 0));
    s.attempts = 5;
    s.state = State::Writing;
    assert(!s.Active() && !s.Request() && s.attempts == 5); // Ignored while Writing.
    s.Finished(true, "folder");
    assert(s.state == State::Saved && s.message == "folder" && !s.Active());
    s.pool = {9};
    assert(s.Request() && s.state == State::Armed); // Saved -> Armed clears everything.
    assert(s.attempts == 0 && s.message.empty() && s.pool.empty() && s.run.empty());
    s.Fail("boom");
    assert(s.state == State::Failed && s.message == "boom" && !s.Active());
    assert(s.Request() && s.state == State::Armed && s.message.empty());
    // Begin only leaves Armed.
    Seq idle;
    idle.Begin({1});
    assert(idle.state == State::Idle && idle.pool.empty());
}

static void WantsInFlight() {
    Seq s = Collecting();
    assert(s.Wants(0) && s.Wants(7) && !s.Wants(8) && !s.Wants(9));
    s.run.assign(3, Make(1, 0));
    assert(s.Wants(4) && !s.Wants(5));
    s.run.assign(8, Make(1, 0));
    assert(!s.Wants(0));
}

static void PoolTakeRecycle() {
    Seq s = Collecting();
    for (int i = 8; i >= 1; --i)
        assert(s.Take() == i);
    assert(s.Take() == 0); // Empty pool gives an empty buffer.
    s.Recycle(7);
    s.Recycle(0); // An empty buffer is not pooled.
    assert(s.pool.size() == 1 && s.Take() == 7);
    s.state = State::Writing;
    s.Recycle(3); // Dropped unless Collecting.
    assert(s.pool.empty());
}

static void InOrderAfterShuffle() {
    const unsigned order[8] = {13, 10, 17, 11, 15, 12, 16, 14};
    Seq t = Collecting();
    for (int i = 0; i < 8; ++i)
        (void)t.Take(); // The eight buffers are all in flight.
    for (unsigned serial : order)
        t.Complete(Make(serial, int(serial)));
    const auto done = t.Settle();
    assert(done.size() == Frames && t.state == State::Writing && t.run.empty());
    for (unsigned i = 0; i < Frames; ++i)
        assert(done[i].meta.serial == 10 + i && done[i].buffer == int(10 + i));
    assert(t.completed.empty());
    // Writing drops later completions.
    t.Complete(Make(18, 99));
    assert(t.Settle().empty() && t.state == State::Writing && t.pool.empty());
}

static void SettleAcrossCalls() {
    // Consecutive frames arriving in separate polls still build one run.
    Seq s = Collecting();
    for (unsigned i = 0; i < 7; ++i) {
        s.Complete(Make(20 + i, int(i + 1)));
        assert(s.Settle().empty() && s.run.size() == i + 1);
    }
    s.Complete(Make(27, 8));
    const auto done = s.Settle();
    assert(done.size() == Frames && s.state == State::Writing && s.run.empty());
    assert(done.front().meta.serial == 20 && done.back().meta.serial == 27);
}

static void UndeliveredClearsRun() {
    Seq s = Collecting();
    for (int i = 0; i < 8; ++i)
        (void)s.Take(); // All eight buffers are in flight.
    for (unsigned i = 0; i < 3; ++i)
        s.Complete(Make(30 + i, int(i + 1)));
    assert(s.Settle().empty() && s.run.size() == 3 && s.pool.empty());
    s.Complete(Make(33, 4, false)); // Not delivered.
    assert(s.Settle().empty());
    assert(s.run.empty() && s.state == State::Collecting);
    assert(s.pool.size() == 4); // The three run buffers and the bad frame's own.
    int sum = 0;
    for (int b : s.pool)
        sum += b;
    assert(sum == 1 + 2 + 3 + 4);
    // An undelivered frame inside one batch clears the frames sorted before it.
    s.Complete(Make(41, 6));
    s.Complete(Make(40, 5));
    s.Complete(Make(42, 7, false));
    s.Complete(Make(43, 8));
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 43 && s.pool.size() == 7);
}

static void GapAndSizeRestart() {
    Seq s = Collecting();
    s.Complete(Make(50, 1));
    s.Complete(Make(51, 2));
    s.Complete(Make(53, 3)); // Serial gap.
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 53 && s.run[0].buffer == 3);
    assert(s.pool.size() == 10 && s.state == State::Collecting); // 8 + the two recycled.
    s.Complete(Make(54, 4));
    s.Complete(Make(55, 5, true, 8, 6)); // Size change.
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 55 && s.run[0].width == 8);
    s.Complete(Make(56, 6, true, 8, 5)); // Height alone differs.
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 56 && s.run[0].height == 5);
    s.Complete(Make(56, 7, true, 8, 5)); // Repeated serial is not consecutive.
    assert(s.Settle().empty() && s.run.size() == 1 && s.run[0].buffer == 7);
}

static void FailDropsLater() {
    Seq s = Collecting();
    s.run.assign(2, Make(1, 0));
    s.Complete(Make(5, 5));
    s.Fail("no good");
    assert(s.state == State::Failed && s.run.empty() && s.pool.empty() && s.completed.empty());
    s.Complete(Make(6, 6)); // A copy that finished after the failure.
    assert(s.Settle().empty());
    assert(s.state == State::Failed && s.run.empty() && s.pool.empty() && s.completed.empty());
    assert(s.message == "no good");
}

static void CountOverflow() {
    Seq s = Collecting();
    for (unsigned i = 0; i < MaxAttempts; ++i)
        assert(s.Count());
    assert(s.attempts == MaxAttempts && s.state == State::Collecting);
    assert(!s.Count());
    assert(s.state == State::Failed && s.message == "RR did not deliver 8 consecutive frames");
    assert(s.pool.empty() && s.run.empty());
}

static void FinishedTransitions() {
    Seq s = Collecting();
    s.Finished(true, "ignored"); // Only Writing finishes.
    assert(s.state == State::Collecting && s.message.empty());
    s.state = State::Writing;
    s.Finished(false, "disk full");
    assert(s.state == State::Failed && s.message == "disk full");
    s.Finished(true, "late"); // Already settled.
    assert(s.state == State::Failed && s.message == "disk full");
}

// Like a ComPtr that is moved around: explicit bool, released when the last owner goes.
struct Handle {
    static inline int live = 0;
    bool held = false;
    Handle() = default;
    explicit Handle(int) : held(true) {
        ++live;
    }
    Handle(Handle &&o) noexcept : held(std::exchange(o.held, false)) {}
    Handle &operator=(Handle &&o) noexcept {
        if (this != &o) {
            live -= held;
            held = std::exchange(o.held, false);
        }
        return *this;
    }
    ~Handle() {
        live -= held;
    }
    explicit operator bool() const {
        return held;
    }
};
static void MoveOnlyBuffers() {
    {
        Sequence<Frame<Handle>> s;
        std::vector<Handle> buffers;
        for (int i = 0; i < 8; ++i)
            buffers.emplace_back(i);
        assert(s.Request());
        s.Begin(std::move(buffers));
        assert(Handle::live == 8);
        for (unsigned i = 0; i < 2; ++i) {
            Frame<Handle> f;
            f.buffer = s.Take();
            f.meta.serial = 5 + i;
            f.meta.delivered = i == 0;
            s.Complete(std::move(f));
        }
        assert(s.Settle().empty());
        assert(s.pool.size() == 8 && s.run.empty()); // Both buffers came back.
        s.Recycle(Handle());
        assert(s.pool.size() == 8); // An empty handle is not pooled.
        s.Fail("done");
        assert(Handle::live == 0 && s.pool.empty());
    }
    assert(Handle::live == 0);
}

static void Packing() {
    // 3 x 2 texels of 2 bytes in a buffer with 8-byte rows, starting 5 bytes in.
    unsigned char buffer[32];
    for (unsigned i = 0; i < sizeof buffer; ++i)
        buffer[i] = static_cast<unsigned char>(0xE0 + i % 16); // Padding looks like 0xE?.
    Region r;
    r.present = true;
    r.width = 3;
    r.height = 2;
    r.bytes_per_pixel = 2;
    r.offset = 5;
    r.pitch = 8;
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned i = 0; i < 6; ++i)
            buffer[5 + y * 8 + i] = static_cast<unsigned char>(y * 16 + i);
    std::vector<unsigned char> packed;
    unsigned rows = 0;
    PackRows(buffer, r, [&](const unsigned char *row, std::uint64_t bytes) {
        assert(bytes == 6);
        packed.insert(packed.end(), row, row + bytes);
        ++rows;
    });
    assert(rows == 2 && packed.size() == 12);
    const unsigned char want[12] = {0, 1, 2, 3, 4, 5, 16, 17, 18, 19, 20, 21};
    for (unsigned i = 0; i < 12; ++i)
        assert(packed[i] == want[i]);
    r.height = 0;
    PackRows(buffer, r, [&](const unsigned char *, std::uint64_t) { assert(false); });
}

static void Names() {
    assert(FileName(3, Status) == "frame3_status.bin");
    assert(FileName(0, Color) == "frame0_color.bin");
    assert(FileName(7, SpecularHitDistance) == "frame7_specular_hit_distance.bin");
    assert(FileName(12, ColorBeforeTransparency) == "frame12_color_before_transparency.bin");
    assert(std::string(names[NormalRoughness]) == "normal_roughness");
    assert(std::string(names[InputCount - 1]) == "specular_hit_distance");
    assert(InputCount == 11 && Frames == 8 && MaxAttempts == 600);

    assert(Format(26) && std::string(Format(26)->name) == "R11G11B10_FLOAT");
    assert(Format(26)->bytes == 4);
    assert(Format(2)->bytes == 16 && Format(10)->bytes == 8 && Format(16)->bytes == 8);
    assert(Format(34)->bytes == 4 && Format(41)->bytes == 4 && Format(42)->bytes == 4);
    assert(Format(54)->bytes == 2);
    assert(!Format(0) && !Format(87));
    assert(FormatName(54) == "R16_FLOAT" && FormatName(42) == "R32_UINT");
    assert(FormatName(87) == "DXGI_87");
    for (const FormatInfo &f : formats)
        assert(Format(f.dxgi) == &f);
}

static void Json() {
    std::string s;
    JsonNumber(s, 1.0);
    JsonNumber(s, -0.5);
    assert(s == "1-0.5");
    s.clear();
    JsonNumber(s, 0.1f); // A float widened to double keeps nine digits.
    assert(s == "0.100000001");
    s.clear();
    JsonNumber(s, 123456.789);
    assert(s == "123456.789");
    s.clear();
    JsonNumber(s, 1e30);
    assert(s == "1e+30");
    s.clear();
    JsonNumber(s, std::numeric_limits<double>::quiet_NaN());
    JsonNumber(s, std::numeric_limits<double>::infinity());
    JsonNumber(s, -std::numeric_limits<float>::infinity());
    assert(s == "nullnullnull");
    s.clear();
    JsonString(s, "a\"b\\c\n\r\t\x01\x1f ok");
    assert(s == "\"a\\\"b\\\\c\\n\\r\\t\\u0001\\u001f ok\"");
    assert(Balanced(s));
    s.clear();
    JsonString(s, "");
    assert(s == "\"\"");
}

static Frame<int> Rich(unsigned serial) {
    Frame<int> f = Make(serial, 0);
    f.meta.sl_frame = serial + 1000;
    f.meta.stats = {0, 1, 2, 3};
    f.meta.jitter[0] = 0.25f;
    f.meta.jitter[1] = -0.125f;
    f.meta.cpu_time_ms = 123456.789;
    f.meta.camera.far_plane = std::numeric_limits<float>::infinity(); // Not finite: null.
    f.meta.camera.near_plane = 0.1f;
    Region &c = f.regions[Color];
    c.present = true;
    c.format = 26;
    c.width = 4;
    c.height = 3;
    c.bytes_per_pixel = 4;
    c.source_width = 8;
    c.source_height = 6;
    Region &d = f.regions[SpecularHitDistance];
    d.present = true;
    d.format = 87; // Unknown, still named.
    d.width = 4;
    d.height = 3;
    d.bytes_per_pixel = 3;
    return f;
}

static void ManifestShape() {
    Header h;
    h.created = "2026-10-25T10:31:02";
    h.render_width = 4;
    h.render_height = 3;
    h.output_width = 8;
    h.output_height = 6;
    h.quality = -1;
    h.preset = 6;
    h.sss_enabled = true;
    h.responsivity_strength = -0.5f;
    std::vector<Frame<int>> frames{Rich(7), Rich(8)};
    const std::string m = Manifest(h, frames);
    assert(Balanced(m));
    const char *top[] = {"\"format\": \"mw4-rr-capture\"",
                         "\"version\": 1",
                         "\"created\": \"2026-10-25T10:31:02\"",
                         "\"frame_count\": 2",
                         "\"render_width\": 4",
                         "\"render_height\": 3",
                         "\"output_width\": 8",
                         "\"output_height\": 6",
                         "\"quality\": -1",
                         "\"preset\": 6",
                         "\"settings\"",
                         "\"status_values\"",
                         "\"reset_reason_bits\"",
                         "\"formats\"",
                         "\"frames\""};
    std::size_t at = 0;
    for (const char *key : top) { // Present, and in the plan's order.
        const std::size_t found = m.find(key, at);
        assert(found != std::string::npos);
        at = found;
    }
    const char *settings[] = {"\"sss_enabled\": true",           "\"transparency_enabled\": false",
                              "\"specular_hit_enabled\": false", "\"tolerate_isolated\": false",
                              "\"lighting_input\": 0",           "\"responsivity_enabled\": false",
                              "\"responsivity_strength\": -0.5", "\"guide_test\": 0",
                              "\"sky_depth_test\": false"};
    for (const char *s : settings)
        assert(Contains(m, s));
    assert(Contains(m, "\"2\": \"sky\""));
    assert(Contains(m, "\"8\": \"gap\""));
    for (const FormatInfo &f : formats) {
        assert(Contains(m, (std::string("\"") + f.name + "\"").c_str()));
        assert(Contains(m, ("\"dxgi_format\": " + std::to_string(f.dxgi)).c_str()));
    }
    assert(Contains(m, "\"layout\": \"4 x float32 RGBA\""));
    assert(!Contains(m, "DXGI_87\": {")); // Unknown formats are not listed under formats.
    const char *frame_keys[] = {"\"index\": 0",
                                "\"index\": 1",
                                "\"serial\": 7",
                                "\"sl_frame\": 1007",
                                "\"viewport\"",
                                "\"delivered\": true",
                                "\"reset\": false",
                                "\"reset_reasons\"",
                                "\"replaced_sr\"",
                                "\"stats\"",
                                "\"bad_frame\": 0",
                                "\"copied_frame\": 1",
                                "\"isolated_guide_pixels\": 2",
                                "\"sanitized_sss_samples\": 3",
                                "\"jitter\": [0.25, -0.125]",
                                "\"motion_vector_scale\"",
                                "\"pre_exposure\"",
                                "\"exposure_scale\"",
                                "\"color_subrect\"",
                                "\"depth_subrect\"",
                                "\"motion_subrect\"",
                                "\"world_to_view\"",
                                "\"view_to_clip\"",
                                "\"camera\"",
                                "\"position\"",
                                "\"right\"",
                                "\"up\"",
                                "\"forward\"",
                                "\"near\": 0.100000001",
                                "\"far\": null",
                                "\"fov\"",
                                "\"aspect\"",
                                "\"clip_to_view\"",
                                "\"depth_inverted\"",
                                "\"camera_motion_included\"",
                                "\"motion_vectors_3d\"",
                                "\"orthographic\"",
                                "\"motion_vectors_dilated\"",
                                "\"motion_vectors_jittered\"",
                                "\"cpu_time_ms\": 123456.789",
                                "\"inputs\""};
    for (const char *key : frame_keys)
        assert(Contains(m, key));
    for (unsigned i = 0; i < InputCount; ++i)
        assert(Contains(m, (std::string("\"") + names[i] + "\":").c_str()));
    assert(Contains(m, "\"sss_guide\": null"));
    assert(Contains(m, "\"status\": null"));
    assert(Contains(m, "\"file\": \"frame0_color.bin\""));
    assert(Contains(m, "\"file\": \"frame1_color.bin\""));
    assert(Contains(m, "\"file\": \"frame1_specular_hit_distance.bin\""));
    assert(Contains(m, "\"format\": \"R11G11B10_FLOAT\", \"dxgi_format\": 26"));
    assert(Contains(m, "\"format\": \"DXGI_87\", \"dxgi_format\": 87"));
    assert(Contains(m, "\"source_size\": [8, 6]"));
    assert(Contains(m, "\"source_offset\": [0, 0]"));
    // 16 matrix numbers per matrix, 11 inputs per frame.
    std::size_t nulls = 0;
    for (std::size_t p = m.find("\": null"); p != std::string::npos; p = m.find("\": null", p + 1))
        ++nulls;
    assert(nulls == 2 * (InputCount - 2) + 2); // Nine absent inputs per frame, and "far".
    // No frames at all is still valid.
    const std::string empty = Manifest(h, std::vector<Frame<int>>{});
    assert(Balanced(empty) && Contains(empty, "\"frame_count\": 0"));
}

// Writes two 4 x 3 frames: color (R11G11B10_FLOAT, every texel R=1 G=0.5 B=2) and status
// (R32_UINT, (x + y) % 5), packed from a padded fake readback buffer.
static int WriteSample(const char *folder) {
    std::filesystem::create_directories(folder);
    constexpr unsigned W = 4, H = 3, Pitch = 256, StatusAt = 1024;
    std::vector<unsigned char> buffer(2048, 0xEE); // 0xEE marks padding.
    const std::uint32_t texel = 0x3C0u | (0x380u << 11) | (0x200u << 22);
    auto put = [&](std::size_t at, std::uint32_t v) {
        for (unsigned b = 0; b < 4; ++b)
            buffer[at + b] = static_cast<unsigned char>(v >> (8 * b));
    };
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            put(y * Pitch + x * 4, texel);
            put(StatusAt + y * Pitch + x * 4, (x + y) % 5);
        }
    Header h;
    h.created = "2026-10-25T10:31:02";
    h.render_width = W;
    h.render_height = H;
    h.output_width = 8;
    h.output_height = 6;
    h.quality = 2;
    h.preset = 6;
    h.sss_enabled = true;
    h.transparency_enabled = true;
    h.tolerate_isolated = true;
    h.responsivity_strength = -0.5f;
    std::vector<Frame<int>> frames;
    for (unsigned i = 0; i < 2; ++i) {
        Frame<int> f = Make(1234 + i, 0, true, W, H);
        f.meta.sl_frame = 5678 + i;
        f.meta.replaced_sr = true;
        f.meta.stats = {0, 1, 0, 0};
        f.meta.jitter[0] = 0.25f;
        f.meta.jitter[1] = -0.125f;
        f.meta.motion_scale[0] = f.meta.motion_scale[1] = 1;
        f.meta.pre_exposure = f.meta.exposure_scale = 1;
        f.meta.cpu_time_ms = 123456.789 + i;
        f.meta.world_to_view[0] = f.meta.world_to_view[5] = f.meta.world_to_view[10] = 1;
        f.meta.world_to_view[15] = 1;
        f.meta.view_to_clip[0] = 1.5f;
        f.meta.view_to_clip[5] = 2;
        f.meta.view_to_clip[11] = 1;
        f.meta.view_to_clip[14] = 0.1f;
        f.meta.camera.near_plane = 0.1f;
        f.meta.camera.far_plane = std::numeric_limits<float>::infinity();
        f.meta.camera.fov = 1.2f;
        f.meta.camera.aspect = 1.78f;
        f.meta.camera.depth_inverted = 1;
        Region &c = f.regions[Color];
        c = Region{true, 26, W, H, 4, 0, Pitch, 0, 0, 8, 6};
        Region &s = f.regions[Status];
        s = Region{true, 42, W, H, 4, StatusAt, Pitch, 0, 0, W, H};
        for (unsigned in : {unsigned(Color), unsigned(Status)}) {
            std::ofstream out(std::filesystem::path(folder) / FileName(i, in), std::ios::binary);
            PackRows(buffer.data(), f.regions[in], [&](const unsigned char *row, std::uint64_t n) {
                out.write(reinterpret_cast<const char *>(row), std::streamsize(n));
            });
            if (!out)
                return 1;
        }
        frames.push_back(f);
    }
    std::ofstream manifest(std::filesystem::path(folder) / "manifest.json", std::ios::binary);
    manifest << Manifest(h, frames);
    return manifest ? 0 : 1;
}

int main(int argc, char **argv) {
    Lifecycle();
    WantsInFlight();
    PoolTakeRecycle();
    InOrderAfterShuffle();
    SettleAcrossCalls();
    UndeliveredClearsRun();
    GapAndSizeRestart();
    FailDropsLater();
    CountOverflow();
    FinishedTransitions();
    MoveOnlyBuffers();
    Packing();
    Names();
    Json();
    ManifestShape();
    std::puts("capture model tests passed");
    return argc > 1 ? WriteSample(argv[1]) : 0;
}
