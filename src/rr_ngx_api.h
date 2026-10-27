#pragma once
#include <nvsdk_ngx.h>
#include <windows.h>
namespace rreval {
struct Api {
    decltype(&NVSDK_NGX_D3D12_AllocateParameters) allocate{};
    decltype(&NVSDK_NGX_D3D12_DestroyParameters) destroy{};
    decltype(&NVSDK_NGX_D3D12_CreateFeature) create{};
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) evaluate{};
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) release{};
    // Optional: only the creation log reads NGX's capability map.
    decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters) capabilities{};
    bool Valid() const {
        return allocate && destroy && create && evaluate && release;
    }
    static Api Load(HMODULE module) {
        Api a;
        if (!module)
            return a;
        a.allocate = reinterpret_cast<decltype(a.allocate)>(
            GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
        a.destroy = reinterpret_cast<decltype(a.destroy)>(
            GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
        a.create = reinterpret_cast<decltype(a.create)>(
            GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
        a.evaluate = reinterpret_cast<decltype(a.evaluate)>(
            GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        a.release = reinterpret_cast<decltype(a.release)>(
            GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        a.capabilities = reinterpret_cast<decltype(a.capabilities)>(
            GetProcAddress(module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        return a;
    }
};
} // namespace rreval
