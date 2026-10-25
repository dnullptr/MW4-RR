#pragma once
#include <d3d12.h>
#include <climits>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_set>
namespace rrspecular {
// Native cs_ray_hit_distance_blur: t10 is the cs_raygen_sum_up specular hit
// distance scalar, t13 is the prepass PackedSurface of the same frame.
// 0x4894e5e7e717b468 is the build shipped with the 2026-10-25 game update.
inline constexpr bool DistanceBlur(uint64_t hash) {
    return hash == 0xc9ca6daa80593f3aull || hash == 0x4894e5e7e717b468ull;
}
// The copy is sized from the current prepass guides, not a fixed resolution.
inline bool ValidSource(const D3D12_RESOURCE_DESC &d, UINT w, UINT h, D3D12_RESOURCE_STATES state) {
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_COPY_SOURCE;
    return w && h && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == w &&
           d.Height == h && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
           d.SampleDesc.Count == 1 && d.Format == DXGI_FORMAT_R16_FLOAT &&
           !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) &&
           (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && !(state & ~read);
}
// Dispatch shape of the native blur: one 16x16 group per tile of the guide.
inline bool DispatchShape(UINT64 w, UINT h, UINT x, UINT y, UINT z) {
    return w && h && w <= UINT_MAX && x == (UINT(w) + 15) / 16 && y == (h + 15) / 16 && z == 1;
}
enum class Stage : unsigned { Context, Table, View, Shape, Layout, Slot, Copied, Count };
inline const char *StageName(Stage s) {
    constexpr const char *names[] = {"context", "table",           "view",  "shape",
                                     "layout",  "no-prepass-slot", "copied"};
    return unsigned(s) < unsigned(Stage::Count) ? names[unsigned(s)] : "none";
}
// Log-only counters for the native distance blur. A structurally matching
// shader with an unrecognized hash is reported but never copied.
class Diagnostics {
    std::mutex mutex;
    std::array<uint64_t, unsigned(Stage::Count)> counts{};
    uint64_t dispatches{};
    Stage last = Stage::Count;
    std::unordered_set<uint64_t> scanned, candidates;

  public:
    void Dispatch() {
        std::lock_guard lock(mutex);
        ++dispatches;
    }
    void Observe(Stage s) {
        std::lock_guard lock(mutex);
        if (unsigned(s) >= counts.size())
            return;
        ++counts[unsigned(s)];
        if (s != Stage::Copied)
            last = s;
    }
    // True once per unknown hash: examine its bindings a single time.
    bool Scan(uint64_t hash) {
        std::lock_guard lock(mutex);
        return scanned.size() < 4096 && scanned.insert(hash).second;
    }
    bool Candidate(uint64_t hash) {
        std::lock_guard lock(mutex);
        return candidates.size() < 16 && candidates.insert(hash).second;
    }
    // Summarizes and clears the window; returns false when nothing happened. rejects gets one
    // bit per rejection stage seen in the window.
    bool Summary(char *text, size_t size, unsigned *rejects = nullptr) {
        std::lock_guard lock(mutex);
        uint64_t total = dispatches;
        for (auto c : counts)
            total += c;
        snprintf(
            text, size,
            "Specular hit distance (10s): blurDispatches=%llu copied=%llu lastReject=%s "
            "[context=%llu table=%llu view=%llu shape=%llu layout=%llu slot=%llu]",
            static_cast<unsigned long long>(dispatches),
            static_cast<unsigned long long>(counts[unsigned(Stage::Copied)]), StageName(last),
            static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]),
            static_cast<unsigned long long>(counts[2]), static_cast<unsigned long long>(counts[3]),
            static_cast<unsigned long long>(counts[4]), static_cast<unsigned long long>(counts[5]));
        if (rejects) {
            *rejects = 0;
            for (unsigned i = 0; i < counts.size(); ++i)
                if (i != unsigned(Stage::Copied) && counts[i])
                    *rejects |= 1u << i;
        }
        dispatches = 0;
        counts = {};
        last = Stage::Count;
        return total != 0;
    }
};
} // namespace rrspecular
