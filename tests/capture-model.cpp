#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/rr_capture_model.h"
#include <cassert>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
// Completes a frame the way production does: stamped with the session it was recorded in.
static void Done(Seq &s, Frame<int> f) {
    f.session = s.session;
    s.Complete(std::move(f));
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
static std::size_t Count(const std::string &text, const char *part) {
    std::size_t n = 0;
    for (std::size_t p = text.find(part); p != std::string::npos; p = text.find(part, p + 1))
        ++n;
    return n;
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
    assert(s.session == 0);
    assert(s.Request() && s.state == State::Armed && s.Active() && s.session == 1);
    assert(!s.Request() && s.session == 1); // Ignored while Armed.
    assert(!s.Wants(0));                    // Armed does not collect yet.
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
    assert(s.session == 2);
    assert(s.attempts == 0 && s.message.empty() && s.pool.empty() && s.run.empty());
    s.Fail("boom");
    assert(s.state == State::Failed && s.message == "boom" && !s.Active());
    assert(s.Request() && s.state == State::Armed && s.message.empty() && s.session == 3);
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
        Done(t, Make(serial, int(serial)));
    const auto done = t.Settle();
    assert(done.size() == Frames && t.state == State::Writing && t.run.empty());
    for (unsigned i = 0; i < Frames; ++i)
        assert(done[i].meta.serial == 10 + i && done[i].buffer == int(10 + i));
    assert(t.completed.empty());
    // Writing drops later completions.
    Done(t, Make(18, 99));
    assert(t.Settle().empty() && t.state == State::Writing && t.pool.empty());
}

static void SettleAcrossCalls() {
    // Consecutive frames arriving in separate polls still build one run.
    Seq s = Collecting();
    for (unsigned i = 0; i < 7; ++i) {
        Done(s, Make(20 + i, int(i + 1)));
        assert(s.Settle().empty() && s.run.size() == i + 1);
    }
    Done(s, Make(27, 8));
    const auto done = s.Settle();
    assert(done.size() == Frames && s.state == State::Writing && s.run.empty());
    assert(done.front().meta.serial == 20 && done.back().meta.serial == 27);
}

static void UndeliveredClearsRun() {
    Seq s = Collecting();
    for (int i = 0; i < 8; ++i)
        (void)s.Take(); // All eight buffers are in flight.
    for (unsigned i = 0; i < 3; ++i)
        Done(s, Make(30 + i, int(i + 1)));
    assert(s.Settle().empty() && s.run.size() == 3 && s.pool.empty());
    Done(s, Make(33, 4, false)); // Not delivered.
    assert(s.Settle().empty());
    assert(s.run.empty() && s.state == State::Collecting);
    assert(s.pool.size() == 4); // The three run buffers and the bad frame's own.
    int sum = 0;
    for (int b : s.pool)
        sum += b;
    assert(sum == 1 + 2 + 3 + 4);
    // An undelivered frame inside one batch clears the frames sorted before it.
    Done(s, Make(41, 6));
    Done(s, Make(40, 5));
    Done(s, Make(42, 7, false));
    Done(s, Make(43, 8));
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 43 && s.pool.size() == 7);
}

static void GapAndSizeRestart() {
    Seq s = Collecting();
    Done(s, Make(50, 1));
    Done(s, Make(51, 2));
    Done(s, Make(53, 3)); // Serial gap.
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 53 && s.run[0].buffer == 3);
    assert(s.pool.size() == 10 && s.state == State::Collecting); // 8 + the two recycled.
    Done(s, Make(54, 4));
    Done(s, Make(55, 5, true, 8, 6)); // Size change.
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 55 && s.run[0].width == 8);
    Done(s, Make(56, 6, true, 8, 5)); // Height alone differs.
    assert(s.Settle().empty());
    assert(s.run.size() == 1 && s.run[0].meta.serial == 56 && s.run[0].height == 5);
    Done(s, Make(56, 7, true, 8, 5)); // Repeated serial is not consecutive.
    assert(s.Settle().empty() && s.run.size() == 1 && s.run[0].buffer == 7);
}

static void ResetOnlyStartsRun() {
    // RR's history restarts at 13, so 10..12 cannot lead into it.
    Seq s = Collecting();
    for (unsigned i = 10; i <= 20; ++i) {
        Frame<int> f = Make(i, int(i));
        f.meta.reset = i == 13;
        Done(s, f);
    }
    const auto done = s.Settle();
    assert(done.size() == Frames && s.state == State::Writing && s.run.empty());
    for (unsigned i = 0; i < Frames; ++i)
        assert(done[i].meta.serial == 13 + i && done[i].meta.reset == (i == 0));
    assert(s.pool.size() == 11); // 8 + the buffers of 10..12, as for a gap.
    // A reset frame that comes first starts a run like any other.
    Seq t = Collecting();
    Frame<int> first = Make(30, 1);
    first.meta.reset = true;
    Done(t, first);
    assert(t.Settle().empty() && t.run.size() == 1 && t.run[0].meta.serial == 30);
    for (unsigned i = 31; i < 38; ++i)
        Done(t, Make(i, int(i)));
    const auto run = t.Settle();
    assert(run.size() == Frames && run.front().meta.serial == 30 && t.state == State::Writing);
}

static void StaleSessions() {
    Seq s = Collecting();
    assert(s.session == 1);
    // Eight copies of session 1 are still in flight when the capture fails and is asked for again.
    std::vector<Frame<int>> stale;
    for (unsigned i = 0; i < 8; ++i) {
        (void)s.Take();
        stale.push_back(Make(60 + i, 70 + int(i)));
        stale.back().session = s.session;
    }
    s.Fail("RR did not deliver 8 consecutive frames");
    assert(s.Request() && s.session == 2 && s.state == State::Armed);
    for (Frame<int> &f : stale) // They finish while the new session is still Armed.
        s.Complete(f);
    assert(s.Settle().empty() && s.completed.empty() && s.state == State::Armed);
    s.Begin({11, 12, 13, 14, 15, 16, 17, 18});
    for (Frame<int> &f : stale) // And again once it is Collecting.
        s.Complete(f);
    assert(s.Settle().empty());
    assert(s.state == State::Collecting && s.run.empty() && s.completed.empty());
    assert(s.pool == (std::vector<int>{11, 12, 13, 14, 15, 16, 17, 18})); // Nothing recycled.
    // In one batch, stale frames neither join nor break the new run, whatever their serials.
    for (Frame<int> &f : stale)
        s.Complete(f);
    Frame<int> bad = Make(103, 90, false), twin = Make(101, 91);
    bad.session = twin.session = 1;
    s.Complete(bad);
    s.Complete(twin);
    for (unsigned i = 0; i < 8; ++i)
        Done(s, Make(100 + i, 20 + int(i)));
    const auto done = s.Settle();
    assert(done.size() == Frames && s.state == State::Writing);
    for (unsigned i = 0; i < Frames; ++i)
        assert(done[i].meta.serial == 100 + i && done[i].session == 2);
    assert(s.pool.size() == 8);
    // The same holds after a save: a straggler from the finished session is dropped.
    Frame<int> late = Make(200, 95);
    late.session = s.session;
    s.Finished(true, "folder");
    assert(s.Request() && s.session == 3);
    s.Complete(late);
    assert(s.Settle().empty() && s.state == State::Armed && s.completed.empty());
    s.Begin({1});
    s.Complete(late);
    assert(s.Settle().empty() && s.run.empty() && s.pool == std::vector<int>{1});
}

static void FailDropsLater() {
    Seq s = Collecting();
    s.run.assign(2, Make(1, 0));
    Done(s, Make(5, 5));
    s.Fail("no good");
    assert(s.state == State::Failed && s.run.empty() && s.pool.empty() && s.completed.empty());
    Done(s, Make(6, 6)); // A copy that finished after the failure.
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

static void FailIgnoredWhileWriting() {
    Seq s = Collecting();
    for (unsigned i = 0; i < Frames; ++i)
        Done(s, Make(40 + i, s.Take()));
    assert(s.Settle().size() == Frames && s.state == State::Writing);
    Done(s, Make(48, 9)); // Queued while the writer runs.
    s.Fail("too late");   // The writer owns this session's outcome.
    assert(s.state == State::Writing && s.message.empty() && s.completed.size() == 1);
    s.attempts = MaxAttempts;
    assert(!s.Count() && s.state == State::Writing && s.message.empty());
    s.Finished(false, "disk full");
    assert(s.state == State::Failed && s.message == "disk full");
    s.Fail("again"); // Outside Writing, Fail still applies.
    assert(s.state == State::Failed && s.message == "again" && s.completed.empty());
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
            f.session = s.session;
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

static void RegionBounds() {
    // 3 x 2 texels of 2 bytes, 8-byte rows, 5 bytes in: PackRows reads bytes 5..18.
    Region r;
    r.present = true;
    r.width = 3;
    r.height = 2;
    r.bytes_per_pixel = 2;
    r.offset = 5;
    r.pitch = 8;
    assert(Fits(r, 19) && Fits(r, 1000) && !Fits(r, 18) && !Fits(r, 0));
    r.height = 1; // One row: the pitch is never stepped.
    assert(Fits(r, 11) && !Fits(r, 10));
    r.pitch = 6; // Pitch equal to the row is tight packing.
    r.height = 2;
    assert(Fits(r, 17) && !Fits(r, 16));
    r.pitch = 5; // Rows would overlap.
    assert(!Fits(r, 1000));
    r.pitch = 8;
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    Region empty = r;
    empty.present = false;
    assert(!Fits(empty, 1000) && !Fits(empty, max));
    empty = r;
    empty.width = 0;
    assert(!Fits(empty, 1000) && !Fits(empty, max));
    empty = r;
    empty.height = 0;
    assert(!Fits(empty, 1000) && !Fits(empty, max));
    empty = r;
    empty.bytes_per_pixel = 0;
    assert(!Fits(empty, 1000) && !Fits(empty, max));
    // Sums that would wrap around 64 bits never fit.
    Region big = r;
    big.offset = max - 2;
    assert(!Fits(big, max));
    big.offset = max - 6; // The first row fits exactly; the second wraps.
    big.height = 1;
    assert(Fits(big, max));
    big.height = 2;
    assert(!Fits(big, max));
    big = r;
    big.pitch = max / 2 + 1; // pitch * (height - 1) alone wraps.
    big.height = 3;
    assert(!Fits(big, max));
    big.height = 2; // Reads up to 5 + pitch + 6, which is below 2^64.
    assert(Fits(big, max) && Fits(big, 11 + big.pitch) && !Fits(big, 10 + big.pitch));
    big.width = 0xffffffffu;
    big.bytes_per_pixel = 0xffffffffu;
    big.pitch = max;
    big.height = 1;
    big.offset = 0;
    assert(Fits(big, max) && !Fits(big, std::uint64_t(0xffffffffu) * 0xffffffffu - 1));
}

static void Names() {
    assert(FileName(3, Status) == "frame3_status.bin");
    assert(FileName(0, Color) == "frame0_color.bin");
    assert(FileName(7, SpecularHitDistance) == "frame7_specular_hit_distance.bin");
    assert(FileName(12, ColorBeforeTransparency) == "frame12_color_before_transparency.bin");
    assert(FileName(7, RrOutput) == "frame7_rr_output.bin");
    assert(std::string(names[NormalRoughness]) == "normal_roughness");
    assert(std::string(names[SpecularHitDistance]) == "specular_hit_distance");
    assert(std::string(names[InputCount - 1]) == "rr_output");
    assert(RrOutput == 11 && InputCount == 12 && Frames == 8 && MaxAttempts == 600);

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
    JsonNumber(s, -0.0);
    JsonNumber(s, 100000000.0);
    JsonNumber(s, 1234567890.0); // Ten digits no longer fit in nine.
    JsonNumber(s, 0.00001);
    assert(s == "-01000000001.23456789e+091e-05");
    s.clear();
    JsonNumber(s, std::numeric_limits<double>::quiet_NaN());
    JsonNumber(s, std::numeric_limits<double>::infinity());
    JsonNumber(s, -std::numeric_limits<float>::infinity());
    assert(s == "nullnullnull");
    s.clear();
    JsonFixed(s, 1e8 + 0.125); // Milliseconds since boot keep their microseconds.
    assert(s == "100000000.125");
    s.clear();
    JsonFixed(s, 123456.789);
    JsonFixed(s, -0.5);
    assert(s == "123456.789-0.500");
    s.clear();
    JsonFixed(s, -std::numeric_limits<double>::max()); // Every finite value prints in full.
    assert(s.size() == 314 && s.front() == '-' && s.compare(s.size() - 4, 4, ".000") == 0);
    s.clear();
    JsonFixed(s, std::numeric_limits<double>::quiet_NaN());
    JsonFixed(s, std::numeric_limits<double>::infinity());
    JsonFixed(s, -std::numeric_limits<double>::infinity());
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

// JsonNumber must print what printf("%.9g") prints in the C locale.
static void JsonNumberMatchesPrintf() {
    std::uint64_t x = 88172645463325252ull; // xorshift64, a fixed sequence
    auto next = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    unsigned checked = 0;
    for (unsigned i = 0; i < 50000; ++i) {
        const std::uint64_t bits = next();
        const std::uint32_t f32 = static_cast<std::uint32_t>(bits >> 32);
        float f;
        std::memcpy(&f, &f32, sizeof f);
        double d;
        std::memcpy(&d, &bits, sizeof d);
        for (double v : {double(f), d, double(i) * 0.37, double(i) * 1e-7, double(i) * 1e7}) {
            if (!std::isfinite(v))
                continue;
            char want[32];
            std::snprintf(want, sizeof want, "%.9g", v);
            std::string got;
            JsonNumber(got, v);
            assert(got == want);
            ++checked;
        }
    }
    assert(checked > 200000);
}
// printf follows the process locale, so a comma-decimal one would corrupt a manifest. The test
// only has something to check where such a locale is installed.
static void LocaleIndependent() {
    bool ran = false;
    for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR"}) {
        if (!std::setlocale(LC_ALL, name) || *std::localeconv()->decimal_point != ',')
            continue;
        char printed[32];
        std::snprintf(printed, sizeof printed, "%.9g", 0.5);
        assert(std::string(printed) == "0,5"); // The locale is really in effect.
        std::string s;
        JsonNumber(s, 0.5);
        JsonNumber(s, -1234.5);
        JsonNumber(s, 1e-7);
        assert(s == "0.5-1234.51e-07");
        Header h;
        h.responsivity_strength = -0.5f;
        const std::string m = Manifest(h, std::vector<Frame<int>>{Rich(1)});
        assert(Contains(m, "\"responsivity_strength\": -0.5") &&
               Contains(m, "\"jitter\": [0.25, -0.125]"));
        ran = true;
        break;
    }
    std::setlocale(LC_ALL, "C");
    std::puts(ran ? "locale check ran with a comma-decimal locale"
                  : "locale check skipped: no comma-decimal locale installed");
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
    frames[1].meta.cpu_time_ms = 1e8 + 0.125;
    frames[0].meta.hit_distance_test = 2;
    const std::string m = Manifest(h, frames);
    assert(Balanced(m));
    assert(Contains(m, "\"cpu_time_ms\": 100000000.125")); // Fixed, where %.9g would round.
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
                                "\"hit_distance_test\": 2",
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
    // Ten absent inputs per frame, and "far".
    assert(Count(m, "\": null") == 2 * (InputCount - 2) + 2);
    // No frames at all is still valid.
    const std::string empty = Manifest(h, std::vector<Frame<int>>{});
    assert(Balanced(empty) && Contains(empty, "\"frame_count\": 0"));
}

// rr_output is the output size: each input's own width and height go in the manifest.
static void ManifestOutputSize() {
    Header h;
    h.render_width = 4;
    h.render_height = 3;
    h.output_width = 8;
    h.output_height = 6;
    Frame<int> f = Make(9, 0);
    f.regions[Color] = Region{true, 26, 4, 3, 4, 0, 256, 0, 0, 8, 6};
    f.regions[RrOutput] = Region{true, 10, 8, 6, 8, 1024, 256, 0, 0, 8, 6};
    const std::string m = Manifest(h, std::vector<Frame<int>>{f});
    assert(Balanced(m));
    assert(Contains(m, "\"color\": {\"file\": \"frame0_color.bin\", \"format\": "
                       "\"R11G11B10_FLOAT\", \"dxgi_format\": 26, \"width\": 4, \"height\": 3, "
                       "\"bytes_per_pixel\": 4"));
    assert(Contains(m, "\"rr_output\": {\"file\": \"frame0_rr_output.bin\", \"format\": "
                       "\"R16G16B16A16_FLOAT\", \"dxgi_format\": 10, \"width\": 8, \"height\": 6, "
                       "\"bytes_per_pixel\": 8, \"source_offset\": [0, 0], "
                       "\"source_size\": [8, 6]}"));
    // All 12 inputs in table order, and no others: two present, ten null.
    std::size_t at = m.find("\"inputs\": {");
    assert(at != std::string::npos);
    for (unsigned i = 0; i < InputCount; ++i) {
        at = m.find("\"" + std::string(names[i]) + "\": ", at);
        assert(at != std::string::npos);
    }
    assert(InputCount == 12 && Count(m, "\": null") == 10 && Count(m, "{\"file\": ") == 2);
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
    ResetOnlyStartsRun();
    StaleSessions();
    FailDropsLater();
    CountOverflow();
    FinishedTransitions();
    FailIgnoredWhileWriting();
    MoveOnlyBuffers();
    Packing();
    RegionBounds();
    Names();
    Json();
    JsonNumberMatchesPrintf();
    LocaleIndependent();
    ManifestShape();
    ManifestOutputSize();
    std::puts("capture model tests passed");
    return argc > 1 ? WriteSample(argv[1]) : 0;
}
