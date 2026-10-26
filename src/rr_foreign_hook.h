#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
// Whether another add-on hooked an NGX export before RR did. Such a hook sits beneath RR's: it
// sees only the calls RR passes down, so RR must not skip the game's SR call (DLSS5 NR takes
// its depth and motion from that call).
namespace rrforeign {
// The destination of a jump at code: E9 rel32, FF 25 [rip+disp32], or mov rax, imm64; jmp rax.
// Returns 0 for anything else.
inline uintptr_t JumpTarget(const uint8_t *code, uintptr_t at) {
    if (code[0] == 0xE9) {
        int32_t rel;
        std::memcpy(&rel, code + 1, 4);
        return at + 5 + intptr_t(rel);
    }
    if (code[0] == 0xFF && code[1] == 0x25) {
        int32_t disp;
        std::memcpy(&disp, code + 2, 4);
        return at + 6 + intptr_t(disp); // address of the 8-byte destination
    }
    if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
        uint64_t value;
        std::memcpy(&value, code + 2, 8);
        return uintptr_t(value);
    }
    return 0;
}
inline bool Readable(uintptr_t at, size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<void *>(at), &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return at + size <= uintptr_t(m.BaseAddress) + m.RegionSize;
}
// Follows up to 4 jumps from target, the way detours and their relay stubs chain. The hook is
// foreign when the chain ends in a module other than core; a chain that cannot be read counts
// as foreign. owner names the module it ends in.
inline bool Beneath(void *target, HMODULE core, char *owner, size_t owner_size) {
    if (owner_size)
        owner[0] = 0;
    uintptr_t at = uintptr_t(target);
    bool jumped = false;
    for (unsigned hop = 0; hop < 4; ++hop) {
        if (!Readable(at, 12))
            return jumped;
        const auto *code = reinterpret_cast<const uint8_t *>(at);
        auto next = JumpTarget(code, at);
        if (!next)
            break;
        if (code[0] == 0xFF) { // indirect: read the destination pointer
            if (!Readable(next, 8))
                return true;
            std::memcpy(&next, reinterpret_cast<const void *>(next), 8);
        }
        jumped = true;
        at = next;
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(at), &module)) {
            if (module == core)
                return false; // a jump within NGX itself
            if (owner_size) {
                char path[MAX_PATH]{};
                if (GetModuleFileNameA(module, path, MAX_PATH)) {
                    const char *leaf = std::strrchr(path, '\\');
                    strncpy_s(owner, owner_size, leaf ? leaf + 1 : path, _TRUNCATE);
                }
            }
            return true;
        }
        // Not in a module: a relay stub. Follow it.
    }
    return jumped;
}
} // namespace rrforeign
