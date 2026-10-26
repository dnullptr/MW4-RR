#pragma once
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "rr_ngx_api.h"
#include "rr_evaluation_params.h"
#include "rr_settings_model.h"
#include "rr_trace_model.h"
// What RR was created with, which runtime ran it, and NGX's result codes. Nothing here runs per
// frame: creation attempts, retirement and inference failures only.
namespace rrngxlog {
// Keyed on numeric values: an older SDK may lack the newer enum names.
inline const char *ResultName(unsigned code) {
    switch (code) {
    case 0x1u:
        return "Success";
    case 0xBAD00000u:
        return "Fail";
    case 0xBAD00000u | 1:
        return "FAIL_FeatureNotSupported";
    case 0xBAD00000u | 2:
        return "FAIL_PlatformError";
    case 0xBAD00000u | 3:
        return "FAIL_FeatureAlreadyExists";
    case 0xBAD00000u | 4:
        return "FAIL_FeatureNotFound";
    case 0xBAD00000u | 5:
        return "FAIL_InvalidParameter";
    case 0xBAD00000u | 6:
        return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00000u | 7:
        return "FAIL_NotInitialized";
    case 0xBAD00000u | 8:
        return "FAIL_UnsupportedInputFormat";
    case 0xBAD00000u | 9:
        return "FAIL_RWFlagMissing";
    case 0xBAD00000u | 10:
        return "FAIL_MissingInput";
    case 0xBAD00000u | 11:
        return "FAIL_UnableToInitializeFeature";
    case 0xBAD00000u | 12:
        return "FAIL_OutOfDate";
    case 0xBAD00000u | 13:
        return "FAIL_OutOfGPUMemory";
    case 0xBAD00000u | 14:
        return "FAIL_UnsupportedFormat";
    case 0xBAD00000u | 15:
        return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00000u | 16:
        return "FAIL_UnsupportedParameter";
    case 0xBAD00000u | 17:
        return "FAIL_Denied";
    case 0xBAD00000u | 18:
        return "FAIL_NotImplemented";
    default:
        return "unknown";
    }
}
// "0xBAD0000D FAIL_OutOfGPUMemory"
struct Code {
    char text[48]{};
    explicit Code(unsigned code) {
        snprintf(text, sizeof(text), "0x%08X %s", code, ResultName(code));
    }
};
// A call that hands back an object: NGX's code, or "null handle" when it succeeded without one.
inline Code Returned(unsigned code, const void *object) {
    Code c(code);
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Result(code)) && !object)
        snprintf(c.text, sizeof(c.text), "null handle");
    return c;
}
// rrprivate::Require with the call's outcome appended: "<what>: <outcome>".
inline void Require(bool valid, const char *what, const Code &outcome) {
    if (valid)
        return;
    char line[160];
    snprintf(line, sizeof(line), "%s: %s", what, outcome.text);
    throw std::runtime_error(line);
}

// "a.b.c.d"
struct Version {
    char text[24]{};
    explicit Version(const unsigned (&v)[4]) {
        snprintf(text, sizeof(text), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    }
};
// VS_FIXEDFILEINFO (13 DWORDs) inside a VS_VERSIONINFO resource: its signature sits on a 4-byte
// boundary, and dwFileVersionMS/LS follow dwStrucVersion.
inline bool FixedFileVersion(const void *data, size_t bytes, unsigned (&v)[4]) {
    constexpr size_t size = 52;
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t at = 0; p && bytes >= size && at <= bytes - size; at += 4) {
        uint32_t f[4];
        memcpy(f, p + at, sizeof(f));
        if (f[0] != 0xFEEF04BDu)
            continue;
        v[0] = f[2] >> 16;
        v[1] = f[2] & 0xFFFF;
        v[2] = f[3] >> 16;
        v[3] = f[3] & 0xFFFF;
        return true;
    }
    return false;
}
// The module's VS_VERSION_INFO (RT_VERSION) resource, read without version.lib.
inline bool ModuleVersion(HMODULE m, unsigned (&v)[4]) {
    const HRSRC r = m ? FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16)) : nullptr;
    const DWORD bytes = r ? SizeofResource(m, r) : 0;
    const HGLOBAL g = bytes ? LoadResource(m, r) : nullptr;
    const void *data = g ? LockResource(g) : nullptr;
    return data && FixedFileVersion(data, bytes, v);
}
// The probe's rule (rrtrace::RrModelPath), for a path in any case.
inline bool ModelPath(const wchar_t *path) {
    wchar_t lower[MAX_PATH]{};
    for (size_t i = 0; path[i] && i + 1 < MAX_PATH; ++i)
        lower[i] = static_cast<wchar_t>(towlower(path[i]));
    return rrtrace::RrModelPath(lower);
}

struct Adapter {
    bool found{}, versioned{};
    unsigned version[4]{}, vendor{}; // UMD version, DXGI_ADAPTER_DESC::VendorId
    char name[384]{};                // DXGI_ADAPTER_DESC::Description as UTF-8
};
inline Adapter ReadAdapter(ID3D12Device *device) {
    Adapter a;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (!device || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->GetDesc(&desc)))
        return a;
    a.found = true;
    a.vendor = desc.VendorId;
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, a.name, sizeof(a.name), nullptr, nullptr);
    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        a.versioned = true;
        a.version[0] = HIWORD(umd.HighPart);
        a.version[1] = LOWORD(umd.HighPart);
        a.version[2] = HIWORD(umd.LowPart);
        a.version[3] = LOWORD(umd.LowPart);
    }
    return a;
}
// NVIDIA's driver number from the UMD version: 32.0.15.6094 -> 560.94.
inline void NvidiaVersion(const unsigned (&v)[4], char *out, size_t size) {
    snprintf(out, size, "%u.%02u", ((v[2] % 10) * 10000 + v[3]) / 100, v[3] % 100);
}
// "32.0.15.6094 (NVIDIA 560.94) NVIDIA GeForce RTX 4090", or "?" for an unknown adapter.
inline void DescribeDriver(const Adapter &a, char *out, size_t size) {
    if (!a.found) {
        snprintf(out, size, "?");
        return;
    }
    char nvidia[32]{};
    if (a.versioned && a.vendor == 0x10DE) {
        char number[16];
        NvidiaVersion(a.version, number, sizeof(number));
        snprintf(nvidia, sizeof(nvidia), " (NVIDIA %s)", number);
    }
    snprintf(out, size, "%s%s %s", a.versioned ? Version(a.version).text : "?", nvidia, a.name);
}
// "<path> <a.b.c.d>", with "?" for what cannot be read.
inline void AppendModule(std::string &line, HMODULE m) {
    wchar_t path[MAX_PATH]{};
    char utf8[MAX_PATH * 3]{};
    unsigned v[4]{};
    const bool named =
        m && GetModuleFileNameW(m, path, MAX_PATH) &&
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    line += named ? utf8 : "?";
    line += ' ';
    line += ModuleVersion(m, v) ? Version(v).text : "?";
}
// "RR runtime: NGX core <path> <ver>; model <path> <ver>; driver <ver> (NVIDIA x.y) <GPU>".
// Module and DXGI queries: the controller runs this only after releasing its mutex.
inline std::string RuntimeLine(HMODULE core, ID3D12Device *device) {
    std::string line = "RR runtime: NGX core ";
    AppendModule(line, core);
    bool model = false;
    HMODULE modules[2048]{};
    DWORD bytes{};
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
        const size_t count = (std::min)(size_t(bytes / sizeof(HMODULE)), std::size(modules));
        for (size_t i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH]{};
            HMODULE held{}; // Keeps the model mapped while its version resource is read.
            if (!GetModuleFileNameW(modules[i], path, MAX_PATH) || !ModelPath(path) ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    reinterpret_cast<LPCWSTR>(modules[i]), &held))
                continue;
            line += "; model ";
            AppendModule(line, held);
            FreeLibrary(held);
            model = true;
        }
    }
    if (!model)
        line += "; model not loaded";
    char driver[512];
    DescribeDriver(ReadAdapter(device), driver, sizeof(driver));
    line += "; driver ";
    line += driver;
    return line;
}

// RR's entries in NGX's capability map; a field stays empty when its Get fails.
struct Capability {
    std::optional<int> available, needs_driver, init;
    std::optional<unsigned> major, minor;
};
inline Capability ReadCapability(const NVSDK_NGX_Parameter &p) {
    Capability c;
    auto read = [&p](const char *key, auto &field) {
        typename std::remove_reference_t<decltype(field)>::value_type v{};
        if (NVSDK_NGX_SUCCEED(p.Get(key, &v)))
            field = v;
    };
    read(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, c.available);
    read(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, c.needs_driver);
    read(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, c.major);
    read(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, c.minor);
    read(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, c.init);
    return c;
}
// "RR capability: available=1 needsUpdatedDriver=0 minDriver=0.0 featureInit=0x00000001 Success"
inline void DescribeCapability(const Capability &c, char *out, size_t size) {
    const std::optional<long long> number[] = {c.available, c.needs_driver, c.major, c.minor};
    char field[4][24];
    for (unsigned i = 0; i < 4; ++i)
        if (number[i])
            snprintf(field[i], sizeof(field[i]), "%lld", *number[i]);
        else
            snprintf(field[i], sizeof(field[i]), "?");
    const Code init(c.init ? unsigned(*c.init) : 0u);
    snprintf(out, size,
             "RR capability: available=%s needsUpdatedDriver=%s minDriver=%s.%s featureInit=%s",
             field[0], field[1], field[2], field[3], c.init ? init.text : "?");
}
// Queries, reads and destroys the capability map; a failed query or a missing export has its own
// line, so the capability flags are never left unread without a trace.
inline void CapabilityLine(const rreval::Api &api, char *out, size_t size) {
    if (!api.capabilities) {
        snprintf(out, size, "RR capability: export missing");
        return;
    }
    NVSDK_NGX_Parameter *map{};
    const auto result = api.capabilities(&map);
    if (NVSDK_NGX_FAILED(result) || !map)
        snprintf(out, size, "RR capability query failed: %s", Returned(result, map).text);
    else
        DescribeCapability(ReadCapability(*map), out, size);
    // The caller owns the map, even one handed back with a failure.
    if (map && api.destroy)
        api.destroy(map);
}
// "RR created: preset F, quality 1 (Balanced), flags 75 -> 11, render 2224x1248, output
// 3840x2160": the game's flags, then the ones RR got.
inline void DescribeCreation(const rreval::Options &o, unsigned preset, char *out, size_t size) {
    const char *name = "unknown";
    for (size_t i = 0; i < std::size(rrpreset::values); ++i)
        if (rrpreset::values[i] == preset)
            name = rrpreset::names[i];
    // The panel's names; NGX has no quality past DLAA (5).
    const char *quality =
        o.quality >= 0 && o.quality <= 5 ? rrsettings::QualityLabel(o.quality) : "unknown";
    snprintf(out, size,
             "RR created: preset %s, quality %d (%s), flags %d -> %d, render %ux%u, output %ux%u",
             name, o.quality, quality, o.flags, rreval::CreateFlags(o.flags), o.width,
             o.height, o.out_width, o.out_height);
}
} // namespace rrngxlog
