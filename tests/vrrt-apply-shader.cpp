#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/rr_vrrt_apply_shader.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <thread>
static std::vector<unsigned char> Read(const wchar_t *path) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
static size_t Count(const std::string &text, const std::string &part) {
    size_t n = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
        ++n;
    return n;
}
// A listing that holds every line the edits rely on once, between unrelated lines.
static std::string Synthetic() {
    std::string text = "; comment\ntarget triple = \"dxil-ms-dx\"\n\n";
    text += std::string(rrapply::Entry) + "\n";
    for (const char *g : rrapply::Guards)
        text += std::string(g) + "\n  %other = add i32 0, 0\n";
    for (const char *a : {rrapply::EntryAnchor, rrapply::FirstTap, rrapply::TapLoad, rrapply::Draw})
        text += std::string(a) + "\n";
    return text + "  ret void\n}\n";
}
// Runs the instructions an edit adds (every %rrsel line, then the draw's %145 compare) in text
// order, so the tests check what the edited IR computes rather than how it is spelled.
struct Evaluator {
    std::map<std::string, uint32_t> ints; // i32 and i1 values
    std::map<std::string, float> floats;
    std::vector<std::string> lines;
    explicit Evaluator(const std::string &text) {
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
            if (line.rfind("  %rrsel.", 0) == 0 || line.rfind("  %145 = fcmp", 0) == 0)
                lines.push_back(line);
    }
    uint32_t Int(const std::string &t) const {
        return t[0] == '%' ? ints.at(t) : uint32_t(std::stoll(t));
    }
    float Float(const std::string &t) const {
        if (t[0] == '%')
            return floats.at(t);
        if (t.rfind("0x", 0) == 0) { // LLVM writes float constants as hex doubles
            const uint64_t bits = std::stoull(t.substr(2), nullptr, 16);
            double d;
            std::memcpy(&d, &bits, sizeof(d));
            assert(double(float(d)) == d);
            return float(d);
        }
        return std::stof(t);
    }
    void Run() {
        for (auto line : lines) {
            for (auto &c : line)
                c = c == ',' ? ' ' : c;
            std::istringstream in(line);
            std::string out, eq, op, type, a, b;
            in >> out >> eq >> op;
            assert(eq == "=");
            if (op == "fcmp") {
                std::string condition;
                in >> condition;
                if (condition == "fast")
                    in >> condition;
                in >> type >> a >> b;
                assert(condition == "ogt" && type == "float");
                ints[out] = Float(a) > Float(b);
            } else if (op == "icmp") {
                std::string condition;
                in >> condition >> type >> a >> b;
                assert(type == "i32" && (condition == "ne" || condition == "eq"));
                ints[out] = (Int(a) == Int(b)) == (condition == "eq");
            } else if (op == "select") {
                std::string ctype, c, btype;
                in >> ctype >> c >> type >> a >> btype >> b;
                assert(ctype == "i1" && type == "float" && btype == "float");
                floats[out] = Int(c) ? Float(a) : Float(b);
            } else if (op == "fsub" || op == "fmul") {
                in >> type >> a >> b;
                assert(type == "float");
                floats[out] = op == "fsub" ? Float(a) - Float(b) : Float(a) * Float(b);
            } else {
                in >> type >> a >> b;
                assert(type == "i32");
                const uint32_t x = Int(a), y = Int(b);
                ints[out] = op == "lshr"  ? x >> y
                            : op == "shl" ? x << y
                            : op == "or"  ? x | y
                            : op == "xor" ? x ^ y
                            : op == "mul" ? x * y
                            : op == "and" ? x & y
                            : op == "add" ? x + y
                                          : (assert(!"unexpected instruction"), 0u);
            }
        }
    }
};
static uint32_t Lowbias32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static void CheckTransform() {
    using namespace rrapply;
    const auto base = Synthetic();
    assert(!MissingAnchor(base));
    for (unsigned v = 0; v < Variants; ++v)
        assert(Variant(NoiseOf(v), UntileOf(v)) == v);
    assert(Variant(NoiseCount, false) == 0 && Variant(99, true) == NoiseCount);
    auto native = base;
    assert(!Transform(native, 0) && native == base);
    auto beyond = base;
    assert(!Transform(beyond, Variants));
    for (unsigned v = 1; v < Variants; ++v) {
        auto text = base;
        assert(Transform(text, v));
        const auto n = NoiseOf(v);
        const bool untile = UntileOf(v);
        assert((FindLine(text, FirstTap) == std::string::npos) == (n == Separate));
        assert(Count(text, "[4 x float]* %15, i32 0, i32 %34\n") == (n == Separate ? 1u : 0u));
        assert((FindLine(text, Draw) == std::string::npos) == (n == Opposite));
        assert((FindLine(text, TapLoad) == std::string::npos) == untile);
        assert(Count(text, "i32 %rrsel.texelx, i32 %rrsel.texely, i32 %136,") ==
               (untile ? 1u : 0u));
        // The tile offset is computed once at entry, after the thread ids it reads.
        assert(FindLine(text, EntryAnchor) != std::string::npos);
        if (untile)
            assert(text.find("%rrsel.offsety = and") < text.find("%rrsel.shiftx = add") &&
                   text.find(EntryAnchor) < text.find("%rrsel.tilex"));
        // Every %rrsel value is defined exactly once.
        for (size_t at = text.find("  %rrsel."); at != std::string::npos;
             at = text.find("  %rrsel.", at + 1)) {
            const auto name = text.substr(at + 2, text.find(' ', at + 2) - at - 2);
            assert(Count(text, "  " + name + " = ") == 1);
        }
        // Lines the edits do not own are left alone.
        for (const char *g : Guards)
            assert(FindLine(text, g) != std::string::npos);
        assert(!Transform(text, v));
        auto again = text;
        for (unsigned w = 1; w < Variants; ++w)
            assert(!Transform(again, w) && again == text);
    }
    // A missing or repeated line refuses every variant, and names the line.
    std::vector<const char *> anchors{Entry};
    anchors.insert(anchors.end(), std::begin(Guards), std::end(Guards));
    anchors.insert(anchors.end(), {Draw, FirstTap, TapLoad, EntryAnchor});
    for (const char *a : anchors) {
        const auto line = std::string(a) + "\n";
        auto missing = base, repeated = base;
        missing.erase(missing.find(line), line.size());
        repeated.insert(repeated.find("  ret void"), line);
        assert(MissingAnchor(missing) == a && MissingAnchor(repeated) == a);
        for (unsigned v = 1; v < Variants; ++v) {
            auto m = missing, r = repeated;
            assert(!Transform(m, v) && m == missing && !Transform(r, v) && r == repeated);
        }
    }
    // A line must match whole: a prefix of a longer line does not count.
    auto longer = base;
    longer.replace(longer.find(Draw), std::string(Draw).size(), std::string(Draw) + ", !dbg !1");
    assert(MissingAnchor(longer) == Draw);
}
// The draw each variant's IR computes, on the 8-bit noise (u = level/255) at every Bernoulli
// rate 1/k the shader can produce (k = 16 - code, code -1..14). Lobe 1 is always the native
// draw. With opposite draws, lobe 2 hits as many levels as the native draw and none of the
// levels lobe 1 hits; otherwise lobe 2 keeps the native compare.
static void CheckDraws() {
    using namespace rrapply;
    for (unsigned v = 1; v < Variants; ++v) {
        auto text = Synthetic();
        assert(Transform(text, v));
        Evaluator e(text);
        e.ints["%12"] = e.ints["%13"] = e.ints["%131"] = e.ints["%132"] = 0;
        auto draw = [&](unsigned lobe, float rate, float u) {
            e.ints["%34"] = lobe;
            e.floats["%106"] = rate;
            e.floats["%144"] = u;
            e.Run();
            return e.ints.at("%145") != 0;
        };
        for (int k = 2; k <= 17; ++k) {
            const float rate = 1.0f / float(k);
            unsigned native = 0, first = 0, second = 0, both = 0;
            for (int level = 0; level < 256; ++level) {
                const float u = float(level) / 255.0f;
                const bool lobe1 = draw(0, rate, u), lobe2 = draw(1, rate, u);
                assert(lobe1 == (rate > u));
                if (NoiseOf(v) != Opposite)
                    assert(lobe2 == lobe1);
                native += rate > u;
                first += lobe1;
                second += lobe2;
                both += lobe1 && lobe2;
            }
            assert(native > 0 && first == native && second == native);
            if (NoiseOf(v) == Opposite) {
                assert(!both);
                // A fast reciprocal 1 ulp high moves the native count by at most one level.
                const float high = std::nextafter(rate, 1.0f);
                unsigned native_high = 0, second_high = 0;
                for (int level = 0; level < 256; ++level) {
                    const float u = float(level) / 255.0f;
                    native_high += high > u;
                    second_high += draw(1, high, u);
                }
                assert(native_high - second_high <= 1);
            }
        }
    }
}
// The repeat break's texel: (native texel + lowbias32 offset of the 128-px tile) wrapped to 128.
static void CheckRepeatBreak() {
    using namespace rrapply;
    for (unsigned v = 1; v < Variants; ++v) {
        if (!UntileOf(v))
            continue;
        auto text = Synthetic();
        assert(Transform(text, v));
        Evaluator e(text);
        e.ints["%34"] = 0;
        e.floats["%106"] = e.floats["%144"] = 0.5f;
        std::map<uint32_t, uint32_t> offsets;
        for (uint32_t x : {0u, 1u, 127u, 128u, 255u, 1919u, 3839u})
            for (uint32_t y : {0u, 127u, 128u, 1080u, 2159u})
                for (uint32_t texel : {0u, 1u, 64u, 127u}) {
                    e.ints["%12"] = x;
                    e.ints["%13"] = y;
                    e.ints["%131"] = texel;
                    e.ints["%132"] = 127 - texel;
                    e.Run();
                    const auto hash = Lowbias32(((y >> 7) << 16) | (x >> 7));
                    assert(e.ints.at("%rrsel.texelx") == ((texel + (hash & 127)) & 127));
                    assert(e.ints.at("%rrsel.texely") ==
                           ((127 - texel + ((hash >> 7) & 127)) & 127));
                    offsets[((y >> 7) << 16) | (x >> 7)] = hash & 0x3fff;
                }
        // Distinct tiles get distinct offsets here, so the repeat is really broken.
        std::map<uint32_t, int> seen;
        for (const auto &[tile, offset] : offsets)
            ++seen[offset];
        assert(seen.size() == offsets.size());
    }
}
static void CheckBookkeeping() {
    using namespace rrapply;
    assert(Current(0) == State::Off && last_variant == 0);
    Dispatched(2, true);
    assert(last_variant == 2 && Current(2) == State::Active && Current(1) != State::Active);
    // Turned back off: the native shader ran, and a capture must say so.
    Dispatched(0, false);
    assert(last_variant == 0 && Current(0) == State::Off && Current(2) != State::Active);
    Dispatched(3, false);
    assert(last_variant == 0 && Current(3) == State::Unavailable);
    Dispatched(3, true);
    assert(last_variant == 3 && Current(3) == State::Active);
    Dispatched(0, false);
}
static void CheckRejection() {
    const unsigned char junk[16]{};
    rrapply::ShaderCache cache;
    assert(cache.Remember(nullptr, 0) < 0 && cache.Remember(junk, sizeof(junk)) < 0);
    for (const auto &p :
         {rrapply::Build(nullptr, 0, nullptr), rrapply::Build(junk, sizeof(junk), nullptr),
          cache.Get(-1, nullptr), cache.Get(0, nullptr), cache.Get(1, nullptr),
          cache.Get(rrapply::Inputs, nullptr)}) {
        assert(std::string(p.status[0]) == "native");
        for (unsigned v = 1; v < rrapply::Variants; ++v)
            assert(!p.code[v] && std::string(p.status[v]) == "unsupported-ray-selection-shader");
    }
}
static void Print(const rrapply::Prepared &p) {
    for (unsigned v = 1; v < rrapply::Variants; ++v) {
        char hex[65] = "-";
        if (p.code[v])
            rrapply::Sha256Hex(p.code[v]->data(), p.code[v]->size(), hex);
        std::cout << "  " << rrapply::Name(v) << ": " << p.status[v];
        if (p.code[v])
            std::cout << ", " << p.code[v]->size() << " bytes, SHA-256 " << hex;
        if (!p.detail[v].empty())
            std::cout << " (" << p.detail[v] << ')';
        std::cout << '\n';
    }
}
static bool Ready(const rrapply::Prepared &p) {
    for (unsigned v = 1; v < rrapply::Variants; ++v)
        if (!p.code[v] || std::string(p.status[v]) != "ready")
            return false;
    return true;
}
// Usage: vrrt-apply-shader.exe [--unpinned] mw4-rr-dxcompiler.dll cs_vrrt_apply.cso...
// --unpinned skips the input pin, for checks against a stand-in shader.
int wmain(int argc, wchar_t **argv) {
    CheckTransform();
    CheckDraws();
    CheckRepeatBreak();
    CheckBookkeeping();
    CheckRejection();
    int first = 1;
    const bool unpinned = argc > 1 && std::wstring(argv[1]) == L"--unpinned";
    first += unpinned;
    if (argc < first + 2) {
        std::cout << "PASS: ray selection edits, their draws and tile offsets, status bookkeeping "
                     "and input rejection verified; shader fixture not supplied\n";
        return 0;
    }
    auto module = LoadLibraryExW(argv[first], nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(module);
    bool passed = true;
    for (int i = first + 1; i < argc; ++i) {
        auto native = Read(argv[i]);
        const int input = rrapply::InputIndex(native.data(), native.size());
        std::wcout << argv[i] << L": " << native.size() << L" bytes, pinned input " << input
                   << L'\n';
        const auto p = unpinned ? rrapply::BuildUnpinned(native.data(), native.size(), module)
                                : rrapply::Build(native.data(), native.size(), module);
        Print(p);
        if (!Ready(p)) {
            std::cerr << "FAIL: not every variant was prepared\n";
            passed = false;
            continue;
        }
        const auto none = rrapply::BuildUnpinned(native.data(), native.size(), nullptr);
        for (unsigned v = 1; v < rrapply::Variants; ++v)
            assert(!none.code[v] && std::string(none.status[v]) == "shader-compiler-unavailable");
        if (unpinned)
            continue;
        assert(input >= 0);
        // The add-on's path: pipeline creation remembers the input, the first use builds it.
        rrapply::ShaderCache cache;
        assert(cache.Remember(native.data(), native.size()) == input);
        rrapply::Prepared a, b;
        std::thread one([&] { a = cache.Get(input, module); });
        std::thread two([&] { b = cache.Get(input, module); });
        one.join();
        two.join();
        for (unsigned v = 1; v < rrapply::Variants; ++v)
            assert(a.code[v] && a.code[v] == b.code[v] && *a.code[v] == *p.code[v]);
        native.back() ^= 1;
        assert(rrapply::InputIndex(native.data(), native.size()) < 0);
        assert(!rrapply::Build(native.data(), native.size(), module).code[1]);
        rrapply::ShaderCache altered;
        assert(altered.Remember(native.data(), native.size()) < 0);
    }
    FreeLibrary(module);
    if (!passed)
        return 1;
    std::cout
        << "PASS: every ray selection variant assembled, validated and round-tripped from the "
           "supplied shader; no GPU work\n";
    return 0;
}
