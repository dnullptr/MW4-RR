#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/rr_vrrt_apply_shader.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
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
        assert(Count(text, "fcmp fast ogt float %rrsel.rate, %rrsel.noise\n") ==
               (n == Opposite ? 1u : 0u));
        assert(Count(text, "0x3FEFFFE000000000") == (n == Opposite ? 1u : 0u));
        assert((FindLine(text, TapLoad) == std::string::npos) == untile);
        assert(Count(text, "i32 %rrsel.texelx, i32 %rrsel.texely, i32 %136,") == (untile ? 1u : 0u));
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
// Opposite draws on the 8-bit noise, in the shader's float arithmetic: for every Bernoulli rate
// 1/(16-code) the second lobe hits as many of the 256 levels as the native draw does, and none
// of the levels the first lobe hits.
static void CheckOppositeDraws() {
    const float shrink = float(1.0 - std::ldexp(1.0, -16));
    assert(double(shrink) == 1.0 - std::ldexp(1.0, -16));
    for (int code = 0; code <= 14; ++code) {
        const float rate = 1.0f / float(16 - code);
        unsigned native = 0, opposite = 0, both = 0;
        for (int k = 0; k < 256; ++k) {
            const volatile float u = float(k) / 255.0f;
            const bool first = rate > u, second = rate * shrink > 1.0f - u;
            native += first;
            opposite += second;
            both += first && second;
        }
        assert(native == opposite && native > 0 && !both);
    }
}
static void CheckRejection() {
    const unsigned char junk[16]{};
    for (const auto &p : {rrapply::Build(nullptr, 0, nullptr), rrapply::Build(junk, sizeof(junk), nullptr),
                          rrapply::Shaders().Get(junk, sizeof(junk), nullptr)}) {
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
        std::cout << '\n';
    }
    if (!p.detail.empty())
        std::cout << "  detail: " << p.detail << '\n';
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
    CheckOppositeDraws();
    CheckRejection();
    int first = 1;
    const bool unpinned = argc > 1 && std::wstring(argv[1]) == L"--unpinned";
    first += unpinned;
    if (argc < first + 2) {
        std::cout << "PASS: ray selection edits, opposite-draw counts and input rejection verified; "
                     "shader fixture not supplied\n";
        return 0;
    }
    auto module = LoadLibraryExW(argv[first], nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(module);
    bool passed = true;
    for (int i = first + 1; i < argc; ++i) {
        auto native = Read(argv[i]);
        const int input = rrapply::InputIndex(native.data(), native.size());
        std::wcout << argv[i] << L": " << native.size() << L" bytes, pinned input " << input << L'\n';
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
        rrapply::ShaderCache cache;
        rrapply::Prepared a, b;
        std::thread one([&] { a = cache.Get(native.data(), native.size(), module); });
        std::thread two([&] { b = cache.Get(native.data(), native.size(), module); });
        one.join();
        two.join();
        for (unsigned v = 1; v < rrapply::Variants; ++v)
            assert(a.code[v] && a.code[v] == b.code[v] && *a.code[v] == *p.code[v]);
        native.back() ^= 1;
        assert(rrapply::InputIndex(native.data(), native.size()) < 0);
        assert(!rrapply::Build(native.data(), native.size(), module).code[1]);
    }
    FreeLibrary(module);
    if (!passed)
        return 1;
    std::cout << "PASS: every ray selection variant assembled, validated and round-tripped from the "
                 "supplied shader; no GPU work\n";
    return 0;
}
