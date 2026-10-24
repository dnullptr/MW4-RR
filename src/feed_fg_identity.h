#pragma once
#include <unknwn.h>
#include <wrl/client.h>
#include <utility>
#include <initializer_list>

namespace fg {
// Public Streamline native-interface query, documented for third-party overlays.
// https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md#53-how-to-check-if-sl-proxies-are-used
inline constexpr GUID NativeInterfaceId = {
    0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
// ReShade returns its proxy from resource GetDevice even when a command list
// exposes the original device. Its public source defines IID_UnwrappedObject:
// https://github.com/crosire/reshade/blob/main/source/com_utils.hpp
inline constexpr GUID ReShadeUnwrappedId = {
    0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
template <class T> Microsoft::WRL::ComPtr<T> NativeObject(IUnknown *object) {
    Microsoft::WRL::ComPtr<T> result;
    if (!object)
        return result;
    Microsoft::WRL::ComPtr<IUnknown> current = object;
    // QueryInterface owns one reference on success. Keep that reference while
    // obtaining the requested interface, and never rely on adapter/LUID equality.
    for (unsigned layer = 0; layer < 4; ++layer) {
        bool advanced = false;
        for (const auto &id : {NativeInterfaceId, ReShadeUnwrappedId}) {
            Microsoft::WRL::ComPtr<IUnknown> native;
            if (FAILED(
                    current->QueryInterface(id, reinterpret_cast<void **>(native.GetAddressOf()))))
                continue;
            if (!native)
                return {};
            // A self-returning query must not hide the other wrapper protocol.
            if (native.Get() == current.Get())
                continue;
            current = std::move(native);
            advanced = true;
            break;
        }
        if (!advanced) {
            current.As(&result);
            return result;
        }
    }
    return {}; // Unknown/cyclic wrapper chain is not an identity proof.
}
inline bool SameNativeObject(IUnknown *a, IUnknown *b) {
    auto left = NativeObject<IUnknown>(a), right = NativeObject<IUnknown>(b);
    return left && right && left.Get() == right.Get();
}
} // namespace fg
