#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <vector>
#include <string>
#include <regex>
#include <memory>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")

namespace rrsss {
// Only transformation rules and fingerprints are shipped. The input comes
// from the game's pipeline creation callback; generated code stays in memory.
inline bool ShaderDigest(const void *data, size_t size, const char *expected) noexcept {
    if (!data || !size || size > 4 * 1024 * 1024)
        return false;
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size),
                   digest.data(), ULONG(digest.size())) < 0)
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i)
        if (hex[digest[i] >> 4] != expected[2*i] || hex[digest[i] & 15] != expected[2*i+1])
            return false;
    return true;
}
inline bool SupportedRuntimeShader(const void *data, size_t size) noexcept {
    return size == 56388 && ShaderDigest(data, size,
        "3632196273fba431d92b8ae659cb9dac09444c43c6244205ba4b4fefab02e591");
}
inline bool ReplaceShaderOnce(std::string &text, const std::string &from,
                               const std::string &to) {
    const auto at = text.find(from);
    if (at == std::string::npos || text.find(from, at + from.size()) != std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    return true;
}
inline bool TransformRuntimeShader(std::string &text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.find("%sss.") != std::string::npos || text.find("@dx.op.discard") != std::string::npos)
        return false;
    if (!ReplaceShaderOnce(text, "%608 = and i32 %607, 16384", "%608 = and i32 %607, 16384"))
        return false;
    std::string added;
    const std::regex ids(R"(%(61[1-9]|62[0-3])\b)");
    for (int i = 611; i <= 623; ++i) {
        const auto prefix = "\n  %" + std::to_string(i) + " = ";
        const auto at = text.find(prefix);
        if (at == std::string::npos || text.find(prefix, at + prefix.size()) != std::string::npos)
            return false;
        const auto end = text.find('\n', at + 1);
        if (end == std::string::npos)
            return false;
        auto line = text.substr(at + 1, end - at - 1);
        if (i == 623 && line != "  %623 = fmul fast float %622, 2.500000e-01")
            return false;
        added += std::regex_replace(line, ids, "%sss.$1") + '\n';
    }
    const std::string first = "  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %3584)";
    if (!ReplaceShaderOnce(text, first, added + first) ||
        !ReplaceShaderOnce(text, "  ret void",
            "  call void @dx.op.storeOutput.f32(i32 5, i32 1, i32 0, i8 0, float %sss.623)\n  ret void"))
        return false;
    int next = 0;
    const std::regex metadata(R"((?:^|\n)!(\d+) = )");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), metadata);
         it != std::sregex_iterator(); ++it)
        next = (std::max)(next, std::stoi((*it)[1].str()) + 1);
    if (!next || !ReplaceShaderOnce(text, "!52 = !{!53}", "!52 = !{!53, !" + std::to_string(next) + "}"))
        return false;
    text += "\n!" + std::to_string(next) + " = !{i32 1, !\"SV_Target\", i8 9, i8 16, !" +
        std::to_string(next+1) + ", i8 0, i32 1, i8 1, i32 1, i8 0, !" + std::to_string(next+2) + "}\n!" +
        std::to_string(next+1) + " = !{i32 1}\n!" + std::to_string(next+2) + " = !{i32 3, i32 1}\n";
    return true;
}
inline bool BuildRuntimeShader(const void *native, size_t size, HMODULE module,
                               std::vector<unsigned char> &out, const char *&status) noexcept {
    out.clear();
    status = "unsupported-subsurface-scattering-shader";
    if (!SupportedRuntimeShader(native, size))
        return false;
    try {
        using Microsoft::WRL::ComPtr;
        status = "shader-compiler-unavailable";
        auto create = module ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance")) : nullptr;
        if (!create)
            return false;
        ComPtr<IDxcLibrary> library;
        ComPtr<IDxcCompiler> compiler;
        ComPtr<IDxcAssembler> assembler;
        ComPtr<IDxcValidator> validator;
        if (FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
            FAILED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) ||
            FAILED(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator))))
            return false;
        ComPtr<IDxcBlobEncoding> source, disassembly, modified;
        status = "shader-disassembly-failed";
        if (FAILED(library->CreateBlobWithEncodingFromPinned(native, UINT32(size), 0, &source)) ||
            FAILED(compiler->Disassemble(source.Get(), &disassembly)) || !disassembly ||
            disassembly->GetBufferSize() > 4 * 1024 * 1024)
            return false;
        std::string text(static_cast<const char *>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
        while (!text.empty() && text.back() == '\0') text.pop_back();
        status = "shader-transformation-rejected";
        if (!TransformRuntimeShader(text))
            return false;
        status = "shader-assembly-failed";
        if (FAILED(library->CreateBlobWithEncodingOnHeapCopy(text.data(), UINT32(text.size()), CP_UTF8, &modified)))
            return false;
        auto succeeded = [](IDxcOperationResult *op) {
            HRESULT result = E_FAIL;
            return op && SUCCEEDED(op->GetStatus(&result)) && SUCCEEDED(result);
        };
        ComPtr<IDxcOperationResult> assembled, validated;
        ComPtr<IDxcBlob> code;
        if (FAILED(assembler->AssembleToContainer(modified.Get(), &assembled)) || !succeeded(assembled.Get()) ||
            FAILED(assembled->GetResult(&code)) || !code)
            return false;
        status = "shader-validation-failed";
        if (FAILED(validator->Validate(code.Get(), DxcValidatorFlags_InPlaceEdit, &validated)) || !succeeded(validated.Get()))
            return false;
        // Pins the complete validated output, including original instructions,
        // bindings and output signatures, to the tested transformation result.
        status = "shader-output-mismatch";
        if (code->GetBufferSize() != 63128 || !ShaderDigest(code->GetBufferPointer(), code->GetBufferSize(),
                "bfa44f35ca4c29500ff5f629ba3504c86eb30d7ea38c92067cb8a788780aac8b"))
            return false;
        const auto *bytes = static_cast<const unsigned char *>(code->GetBufferPointer());
        out.assign(bytes, bytes + code->GetBufferSize());
        status = "ready";
        return true;
    } catch (...) {
        out.clear();
        status = "shader-preparation-exception";
        return false;
    }
}
class RuntimeShaderCache {
    std::mutex mutex;
    bool attempted{};
    const char *result = "not-prepared";
    std::shared_ptr<const std::vector<unsigned char>> code;
public:
    std::shared_ptr<const std::vector<unsigned char>> Get(const void *native, size_t size,
                                                         HMODULE module, const char *&status) {
        if (!SupportedRuntimeShader(native, size)) {
            status = "unsupported-subsurface-scattering-shader";
            return {};
        }
        std::lock_guard lock(mutex);
        if (!attempted) {
            attempted = true;
            auto prepared = std::make_shared<std::vector<unsigned char>>();
            if (BuildRuntimeShader(native, size, module, *prepared, result))
                code = std::move(prepared);
        }
        status = result;
        return code;
    }
};
inline RuntimeShaderCache &RuntimeShader() {
    static RuntimeShaderCache cache;
    return cache;
}
} // namespace rrsss
