#include "../src/rr_sss_runtime_shader.h"
#include <fstream>
#include <iostream>
#include <cassert>
#include <iterator>
#include <thread>
static std::vector<unsigned char> Read(const wchar_t *path) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
int wmain(int argc, wchar_t **argv) {
    std::vector<unsigned char> out{1}; const char *status = "";
    assert(!rrsss::BuildRuntimeShader(nullptr, 0, nullptr, out, status) && out.empty());
    if (argc != 4) { std::cout << "PASS: invalid shader rejected; private equivalence fixture not supplied\n"; return 0; }
    auto native = Read(argv[1]), expected = Read(argv[2]);
    auto module=LoadLibraryExW(argv[3], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(module);
    if (!rrsss::BuildRuntimeShader(native.data(), native.size(), module, out, status)) {
        std::cerr << "FAIL: runtime transformation did not produce the existing guide: " << status << '\n'; return 1;
    }
    assert(out == expected);
    rrsss::RuntimeShaderCache cache;
    std::shared_ptr<const std::vector<unsigned char>> a, b;
    std::thread one([&]{const char *s=""; a=cache.Get(native.data(),native.size(),module,s);});
    std::thread two([&]{const char *s=""; b=cache.Get(native.data(),native.size(),module,s);});
    one.join(); two.join();
    assert(a && a==b && *a==expected);
    rrsss::RuntimeShaderCache failed;
    assert(!failed.Get(native.data(),native.size(),nullptr,status));
    assert(!failed.Get(native.data(),native.size(),module,status));
    out={1};
    assert(!rrsss::BuildRuntimeShader(native.data(), native.size(), nullptr, out, status) && out.empty());
    native.back() ^= 1;
    assert(!cache.Get(native.data(),native.size(),module,status));
    assert(!rrsss::BuildRuntimeShader(native.data(), native.size(), module, out, status) && out.empty());
    FreeLibrary(module);
    std::cout << "PASS: runtime guide is byte-identical; concurrent cache reuse, altered input and missing compiler rejection verified; no GPU work\n";
}
