#pragma once
#include "rr_sss_runtime_shader.h"
#include "rr_vrrt_apply_patch.h"
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rrapply {
// Pinned native inputs: cs_vrrt_apply 6db012ccd3387024 and its older build 53958092126ea74a.
// Their function bodies are identical; only the scene constant size differs.
inline constexpr int Inputs = 2;
inline int InputIndex(const void *data, size_t size) noexcept {
    if (size == 7880 &&
        rrsss::ShaderDigest(data, size,
                            "bc0f4944731d0221201e6f5ddcd40a4104d00fceeb0475d722c951b2a698df36"))
        return 0;
    if (size == 7848 &&
        rrsss::ShaderDigest(data, size,
                            "f3a822565b2442d5c4913e96134a1602b8cb610fc391e5c7c8c681f7b11b2fb7"))
        return 1;
    return -1;
}
inline std::string Plain(const void *data, size_t size) {
    std::string text;
    text.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        const char c = static_cast<const char *>(data)[i];
        if (c != '\r' && c != '\0')
            text += c;
    }
    return text;
}
// The disassembly without its comment lines (signature and binding tables, shader hash).
inline std::string CodeLines(const std::string &text) {
    std::string out;
    size_t at = 0;
    while (at < text.size()) {
        auto end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        if (text[at] != ';')
            out.append(text, at, end - at).push_back('\n');
        at = end + 1;
    }
    return out;
}
inline bool Sha256Hex(const void *data, size_t size, char (&hex)[65]) noexcept {
    std::array<unsigned char, 32> digest{};
    if (!data || !size || size > 4 * 1024 * 1024 ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size), digest.data(),
                   ULONG(digest.size())) < 0)
        return false;
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return true;
}
struct Prepared {
    std::array<std::shared_ptr<const std::vector<unsigned char>>, Variants> code;
    std::array<const char *, Variants> status{};
    // Why each variant was refused: the missing anchor line, the compiler's first error line,
    // or the first round-trip difference.
    std::array<std::string, Variants> detail;
};
// The first line where two listings' code lines differ, for the log.
inline std::string FirstDifference(const std::string &expected, const std::string &actual) {
    size_t at = 0;
    while (at < expected.size() && at < actual.size() && expected[at] == actual[at])
        ++at;
    const auto start = expected.rfind('\n', at == 0 ? 0 : at - 1);
    const auto from = start == std::string::npos ? 0 : start + 1;
    auto line = [&](const std::string &s) {
        return s.substr(from, (std::min)(s.find('\n', from), s.size()) - from).substr(0, 160);
    };
    return "expected \"" + line(expected) + "\", got \"" + line(actual) + "\"";
}
// Builds every variant from the game's own bytecode: disassemble, apply the exact edits,
// assemble, validate (which signs it), then disassemble the result again and require its code
// lines to equal the edited text, so what runs is exactly the reviewed edit of the input.
// Build pins the input; BuildUnpinned is for offline checks against a stand-in shader.
inline Prepared BuildUnpinned(const void *native, size_t size, HMODULE module) noexcept {
    Prepared result;
    result.status.fill("unsupported-ray-selection-shader");
    result.status[0] = "native";
    if (!native || !size || size > 4 * 1024 * 1024)
        return result;
    auto fail = [&](const char *why) {
        for (unsigned v = 1; v < Variants; ++v)
            if (!result.code[v])
                result.status[v] = why;
    };
    try {
        using Microsoft::WRL::ComPtr;
        fail("shader-compiler-unavailable");
        auto create = module ? reinterpret_cast<DxcCreateInstanceProc>(
                                   GetProcAddress(module, "DxcCreateInstance"))
                             : nullptr;
        if (!create)
            return result;
        ComPtr<IDxcLibrary> library;
        ComPtr<IDxcCompiler> compiler;
        ComPtr<IDxcAssembler> assembler;
        ComPtr<IDxcValidator> validator;
        if (FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
            FAILED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) ||
            FAILED(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator))))
            return result;
        auto disassemble = [&](const void *code, size_t bytes, std::string &text) {
            ComPtr<IDxcBlobEncoding> source, listing;
            if (FAILED(
                    library->CreateBlobWithEncodingFromPinned(code, UINT32(bytes), 0, &source)) ||
                FAILED(compiler->Disassemble(source.Get(), &listing)) || !listing ||
                listing->GetBufferSize() > 4 * 1024 * 1024)
                return false;
            text = Plain(listing->GetBufferPointer(), listing->GetBufferSize());
            return true;
        };
        auto succeeded = [](IDxcOperationResult *op) {
            HRESULT status = E_FAIL;
            return op && SUCCEEDED(op->GetStatus(&status)) && SUCCEEDED(status);
        };
        // The first non-empty line of a failed operation's messages.
        auto first_error = [](IDxcOperationResult *op) {
            ComPtr<IDxcBlobEncoding> errors;
            if (!op || FAILED(op->GetErrorBuffer(&errors)) || !errors)
                return std::string();
            const auto text = Plain(errors->GetBufferPointer(),
                                    (std::min)(errors->GetBufferSize(), size_t(4096)));
            const auto from = text.find_first_not_of("\n ");
            if (from == std::string::npos)
                return std::string();
            return text.substr(from, (std::min)(text.find('\n', from), text.size()) - from)
                .substr(0, 200);
        };
        std::string text;
        fail("shader-disassembly-failed");
        if (!disassemble(native, size, text))
            return result;
        for (unsigned v = 1; v < Variants; ++v) {
            auto &status = result.status[v];
            auto edited = text;
            status = "shader-transformation-rejected";
            if (!Transform(edited, v)) {
                if (const char *anchor = MissingAnchor(text))
                    result.detail[v] = std::string("missing line \"") + anchor + '"';
                continue;
            }
            ComPtr<IDxcBlobEncoding> modified;
            ComPtr<IDxcOperationResult> assembled, validated;
            ComPtr<IDxcBlob> code;
            status = "shader-assembly-failed";
            if (FAILED(library->CreateBlobWithEncodingOnHeapCopy(
                    edited.data(), UINT32(edited.size()), CP_UTF8, &modified)) ||
                FAILED(assembler->AssembleToContainer(modified.Get(), &assembled)) ||
                !succeeded(assembled.Get()) || FAILED(assembled->GetResult(&code)) || !code) {
                result.detail[v] = first_error(assembled.Get());
                continue;
            }
            status = "shader-validation-failed";
            if (FAILED(
                    validator->Validate(code.Get(), DxcValidatorFlags_InPlaceEdit, &validated)) ||
                !succeeded(validated.Get())) {
                result.detail[v] = first_error(validated.Get());
                continue;
            }
            std::string again;
            status = "shader-round-trip-mismatch";
            if (!disassemble(code->GetBufferPointer(), code->GetBufferSize(), again))
                continue;
            if (const auto expected = CodeLines(edited), actual = CodeLines(again);
                expected != actual) {
                result.detail[v] = FirstDifference(expected, actual);
                continue;
            }
            const auto *bytes = static_cast<const unsigned char *>(code->GetBufferPointer());
            result.code[v] = std::make_shared<const std::vector<unsigned char>>(
                bytes, bytes + code->GetBufferSize());
            status = "ready";
        }
    } catch (...) {
        for (unsigned v = 1; v < Variants; ++v) {
            result.code[v].reset();
            result.status[v] = "shader-preparation-exception";
            result.detail[v].clear();
        }
    }
    return result;
}
inline Prepared Build(const void *native, size_t size, HMODULE module) noexcept {
    if (InputIndex(native, size) < 0)
        return BuildUnpinned(nullptr, 0, nullptr);
    return BuildUnpinned(native, size, module);
}
// Pipeline creation only keeps a copy of a supported input, so the game's own path does no
// compiler work. The variants are built the first time the test asks for one, once per input;
// every pipeline built from that input shares the result.
class ShaderCache {
    std::mutex mutex;
    std::array<std::vector<unsigned char>, Inputs> source;
    std::array<bool, Inputs> attempted{};
    std::array<Prepared, Inputs> prepared;

  public:
    // The input index of a supported native shader, or -1.
    int Remember(const void *native, size_t size) {
        const int input = InputIndex(native, size);
        if (input < 0)
            return -1;
        std::lock_guard lock(mutex);
        if (source[input].empty()) {
            const auto *bytes = static_cast<const unsigned char *>(native);
            source[input].assign(bytes, bytes + size);
        }
        return input;
    }
    Prepared Get(int input, HMODULE module) {
        if (input < 0 || input >= Inputs)
            return Build(nullptr, 0, nullptr);
        std::lock_guard lock(mutex);
        if (source[input].empty())
            return Build(nullptr, 0, nullptr);
        if (!attempted[input]) {
            attempted[input] = true;
            prepared[input] = Build(source[input].data(), source[input].size(), module);
        }
        return prepared[input];
    }
};
inline ShaderCache &Shaders() {
    static ShaderCache cache;
    return cache;
}
} // namespace rrapply
