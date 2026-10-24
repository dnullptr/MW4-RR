#pragma once
#include <cstddef>
#include <cstdint>
namespace rrversion {
// Shader variants with verified resource, constant, and output contracts.
inline constexpr uint64_t Canonical(uint64_t hash) {
    switch (hash) {
    case 0x977c372cff89beecull:
        return 0xc8be52d1ee156111ull;
    case 0x440013c08ca66b96ull:
        return 0x6ef368e46fa21120ull;
    case 0x4deaee132c7a7d1bull:
        return 0xd63e707761558424ull;
    case 0x6db012ccd3387024ull:
        return 0x53958092126ea74aull;
    default:
        return hash;
    }
}
inline constexpr size_t SourceBytes(uint64_t hash) {
    switch (hash) {
    case 0xc8be52d1ee156111ull:
        return 19744;
    case 0x977c372cff89beecull:
        return 19748;
    case 0x6ef368e46fa21120ull:
        return 15044;
    case 0x440013c08ca66b96ull:
        return 15080;
    case 0xd63e707761558424ull:
        return 11680;
    case 0x4deaee132c7a7d1bull:
        return 12604;
    case 0x53958092126ea74aull:
        return 7848;
    case 0x6db012ccd3387024ull:
        return 7880;
    default:
        return 0;
    }
}
inline constexpr bool NativeTemporal(uint64_t hash) {
    return hash == 0x6ef368e46fa21120ull || hash == 0x440013c08ca66b96ull;
}
inline constexpr bool ReplacementSize(size_t bytes) {
    return bytes == 11680 || bytes == 12604 || bytes == 7848 || bytes == 7880;
}
} // namespace rrversion
