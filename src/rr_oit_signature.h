#pragma once
#include "feed_fg_identity.h"
#include "rr_compute_state.h"
#include "rr_transparency_blend_policy.h"
#include <d3d12.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

// ReShade reports the OIT resolve's ExecuteIndirect without its command
// signature, and only the signature tells whether replaying that call is exact.
// The native CreateCommandSignature is hooked so that every signature carries
// its rrblend::ClassifySignature result as private data.
namespace rrsig {
using rrblend::SignatureClass;
inline constexpr GUID SignatureMarkerId = {
    0xa29ef2fa, 0xb359, 0x4951, {0xb4, 0xac, 0x05, 0xab, 0xe7, 0xf1, 0x7e, 0xa0}};
struct Marker {
    uintptr_t owner;
    UINT version;
    SignatureClass cls;
    UINT stride;
    UINT arguments;
    D3D12_INDIRECT_ARGUMENT_TYPE draw; // the last argument: DRAW or DRAW_INDEXED when Replayable
};
// Hooks live for the process, like rrsss::PipelineProof's. A signature created
// before Attach, or on a device whose hook failed, has no marker: Unknown.
class CreationHook {
    // No member initializers: clang refuses them before the class is complete
    // (CWG1397), and slots{} value-initializes both members anyway.
    struct Slot {
        void *target;
        std::atomic<void *> original;
    };
    static inline std::array<Slot, 8> slots{};
    static inline std::mutex install_mutex;
    static inline unsigned char cookie;
    static inline constexpr UINT version = 1;
    static inline std::atomic<rrcompute::Tracker *> layouts{};
    struct Error {
        DWORD value = GetLastError();
        ~Error() {
            SetLastError(value);
        }
    };
    using Create = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *,
                                                const D3D12_COMMAND_SIGNATURE_DESC *,
                                                ID3D12RootSignature *, REFIID, void **);
    static Marker Describe(const D3D12_COMMAND_SIGNATURE_DESC &desc, ID3D12RootSignature *root) {
        auto *tracker = layouts.load(std::memory_order_acquire);
        Marker marker{};
        marker.owner = reinterpret_cast<uintptr_t>(&cookie);
        marker.version = version;
        // Root constants are only known through a root signature the tracker
        // has a layout for; a desc without them needs no root signature.
        marker.cls = rrblend::ClassifySignature(desc, [&](UINT parameter) {
            return root && tracker ? tracker->RootConstantWords(root, parameter) : -1;
        });
        marker.stride = desc.ByteStride;
        marker.arguments = desc.NumArgumentDescs;
        if (desc.NumArgumentDescs && desc.pArgumentDescs)
            marker.draw = desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type;
        return marker;
    }
    // The detour body once the native creation has returned.
    static void Mark(HRESULT result, const D3D12_COMMAND_SIGNATURE_DESC *desc,
                     ID3D12RootSignature *root, void **out) noexcept {
        if (FAILED(result) || !desc || !out || !*out)
            return;
        Error error;
        try {
            auto p = fg::NativeObject<ID3D12CommandSignature>(static_cast<IUnknown *>(*out));
            if (!p)
                return;
            const auto marker = Describe(*desc, root);
            // Failure leaves no marker. Clear an earlier one in case the runtime
            // returned an object it already had.
            if (FAILED(p->SetPrivateData(SignatureMarkerId, sizeof(marker), &marker)))
                p->SetPrivateData(SignatureMarkerId, 0, nullptr);
        } catch (...) {
        }
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnCreate(ID3D12Device *d,
                                              const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                              ID3D12RootSignature *root, REFIID iid, void **out) {
        const auto hr = reinterpret_cast<Create>(slots[N].original.load(
            std::memory_order_acquire))(d, desc, root, iid, out);
        Mark(hr, desc, root, out);
        return hr;
    }
    template <size_t... N> static auto Addresses(std::index_sequence<N...>) {
        return std::array<void *, 8>{reinterpret_cast<void *>(&OnCreate<N>)...};
    }
    static bool Hook(void *target) {
        if (!target)
            return false;
        for (auto &s : slots)
            if (s.target == target)
                return s.original.load() != nullptr;
        for (size_t k = 0; k < slots.size(); ++k)
            if (!slots[k].target) {
                void *original{};
                if (MH_CreateHook(target, Addresses(std::make_index_sequence<8>{})[k],
                                  &original) != MH_OK)
                    return false;
                slots[k].target = target;
                slots[k].original.store(original, std::memory_order_release);
                if (MH_EnableHook(target) != MH_OK) {
                    MH_RemoveHook(target);
                    slots[k].original = nullptr;
                    return false;
                }
                return true;
            }
        return false;
    }

    static void Log(void (*log)(const char *), const char *text) noexcept {
        if (log)
            try {
                log(text);
            } catch (...) {
            }
    }

  public:
    // tracker supplies the root-constant word counts from its Define layouts. The
    // first tracker passed is kept; Attach refuses any other. log may be null.
    static bool Attach(ID3D12Device *object, rrcompute::Tracker &tracker,
                       void (*log)(const char *)) noexcept {
        Error error;
        try {
            auto device = fg::NativeObject<ID3D12Device>(object);
            if (!device)
                return false;
            std::lock_guard lock(install_mutex);
            rrcompute::Tracker *bound{};
            if (!layouts.compare_exchange_strong(bound, &tracker) && bound != &tracker) {
                Log(log, "RR command signature hook refused: a different root signature tracker");
                return false;
            }
            const auto initialized = MH_Initialize();
            if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
                return false;
            // The detours stay installed for the process, so this module must too.
            HMODULE self{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&OnCreate<0>), &self)) {
                Log(log, "RR command signature hook not installed: self pin failed");
                return false;
            }
            // SDK ID3D12DeviceVtbl: IUnknown 0-2, ID3D12Object 3-6, then
            // ID3D12Device from GetNodeCount (7); CreateCommandSignature is 41.
            return Hook((*reinterpret_cast<void ***>(device.Get()))[41]);
        } catch (...) {
            return false;
        }
    }
    static bool Attached() noexcept {
        for (const auto &s : slots)
            if (s.original.load(std::memory_order_acquire))
                return true;
        return false;
    }
    static bool ReadMarker(ID3D12CommandSignature *object, Marker &marker) noexcept {
        Error error;
        try {
            auto p = fg::NativeObject<ID3D12CommandSignature>(object);
            if (!p)
                return false;
            UINT size = sizeof(marker);
            return SUCCEEDED(p->GetPrivateData(SignatureMarkerId, &size, &marker)) &&
                   size == sizeof(marker) && marker.owner == reinterpret_cast<uintptr_t>(&cookie) &&
                   marker.version == version;
        } catch (...) {
            return false;
        }
    }
};
inline bool Attach(ID3D12Device *native, rrcompute::Tracker &layouts,
                   void (*log)(const char *)) noexcept {
    return CreationHook::Attach(native, layouts, log);
}
// False means status "signature hook not installed".
inline bool Attached() noexcept {
    return CreationHook::Attached();
}
inline bool ReadMarker(ID3D12CommandSignature *signature, Marker &marker) noexcept {
    return CreationHook::ReadMarker(signature, marker);
}
// A signature without this add-on's marker is Unknown, which means refuse.
inline SignatureClass Read(ID3D12CommandSignature *signature) noexcept {
    Marker marker{};
    return ReadMarker(signature, marker) ? marker.cls : SignatureClass::Unknown;
}
} // namespace rrsig
