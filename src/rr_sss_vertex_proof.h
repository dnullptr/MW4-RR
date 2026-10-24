#pragma once
#include <windows.h>
#include <iterator>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <wrl/client.h>
#include <dxcapi.h>
namespace rrsss {
inline bool ReplayBinding(D3D_SHADER_INPUT_TYPE type) noexcept {
    switch (type) {
    case D3D_SIT_CBUFFER:
    case D3D_SIT_TBUFFER:
    case D3D_SIT_TEXTURE:
    case D3D_SIT_SAMPLER:
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_BYTEADDRESS:
        return true;
    default:
        return false;
    }
}
inline HMODULE ReplayCompiler() noexcept {
    static HMODULE module = []() -> HMODULE {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ReplayCompiler), &self))
            return nullptr;
        wchar_t path[32768]{};
        const auto n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
        if (!n || n >= std::size(path))
            return nullptr;
        auto *last = wcsrchr(path, L'\\');
        if (!last || size_t(last - path) + 25 >= std::size(path))
            return nullptr;
        wcscpy_s(last + 1, std::size(path) - size_t(last + 1 - path), L"mw4-rr-dxcompiler.dll");
        return LoadLibraryExW(path, nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}
inline bool VertexReplayProof(const void *code, size_t size, const char *&status) noexcept {
    status = "vertex-bytecode-invalid";
    if (!code || !size || size > 4 * 1024 * 1024)
        return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12ShaderReflection> reflection;
    using Reflect = HRESULT(WINAPI *)(LPCVOID, SIZE_T, REFIID, void **);
    static HMODULE legacy =
        LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto reflect =
        legacy ? reinterpret_cast<Reflect>(GetProcAddress(legacy, "D3DReflect")) : nullptr;
    if (!reflect || FAILED(reflect(code, size, IID_PPV_ARGS(&reflection)))) {
        status = "vertex-dxc-companion-unavailable";
        auto module = ReplayCompiler();
        if (!module)
            return false;
        auto create =
            reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
        if (!create)
            return false;
        ComPtr<IDxcLibrary> library;
        ComPtr<IDxcContainerReflection> container;
        ComPtr<IDxcBlobEncoding> blob;
        UINT part{};
        status = "vertex-dxil-reflection-unavailable";
        if (FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
            FAILED(create(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container))) ||
            FAILED(library->CreateBlobWithEncodingFromPinned(code, UINT32(size), 0, &blob)) ||
            FAILED(container->Load(blob.Get())) ||
            FAILED(container->FindFirstPartKind(0x4c495844u, &part)) ||
            FAILED(container->GetPartReflection(part, IID_PPV_ARGS(&reflection))))
            return false;
    }
    D3D12_SHADER_DESC desc{};
    status = "vertex-stage-or-description-unsupported";
    if (FAILED(reflection->GetDesc(&desc)) ||
        D3D12_SHVER_GET_TYPE(desc.Version) != D3D12_SHVER_VERTEX_SHADER ||
        desc.BoundResources > 128)
        return false;
    // Explicit numerical capabilities only. Reject waves, bindless heaps, atomics,
    // viewport/view IDs and future unknown requirements before replaying geometry.
    constexpr UINT64 allowed = D3D_SHADER_REQUIRES_DOUBLES | D3D_SHADER_REQUIRES_MINIMUM_PRECISION |
                               D3D_SHADER_REQUIRES_11_1_DOUBLE_EXTENSIONS |
                               D3D_SHADER_REQUIRES_INT64_OPS | D3D_SHADER_REQUIRES_NATIVE_16BIT_OPS;
    status = "vertex-replay-requires-unsupported-capabilities";
    if (reflection->GetRequiresFlags() & ~allowed)
        return false;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC binding{};
        status = "vertex-replay-writable-or-unknown-binding";
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding)) || !ReplayBinding(binding.Type))
            return false;
    }
    status = "vertex-read-only-reflection-proven";
    return true;
}
} // namespace rrsss
