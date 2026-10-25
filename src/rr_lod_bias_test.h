#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <reshade_api_pipeline.hpp>
namespace rrlodbias {
// Development test (saved, off by default): adds an offset to the mip LOD bias of the game's
// texture samplers as they are created, so textures sample blurrier mips. Alpha-tested tree
// needles then cover pixels more evenly from frame to frame; shows whether RR's tree noise is
// texture aliasing in its input. The game makes its samplers at load, so a change applies
// after a restart. Comparison (shadow) samplers and clamped or bordered ones (screen-space
// passes) are left alone; material textures wrap.
enum Mode : unsigned { Off, Half, One, Two, Count };
inline constexpr const char *labels[Count] = {"Off", "+0.5", "+1", "+2"};
inline constexpr float offsets[Count] = {0.0f, 0.5f, 1.0f, 2.0f};
inline constexpr unsigned Valid(unsigned mode) {
    return mode < Count ? mode : Off;
}
inline constexpr float Offset(unsigned mode) {
    return offsets[Valid(mode)];
}
// D3D12's MipLODBias range.
inline constexpr float MinBias = -16.0f, MaxBias = 15.99f;
using reshade::api::filter_mode;
using reshade::api::sampler_desc;
using reshade::api::texture_address_mode;
inline constexpr uint32_t CompareBit = 0x80; // filter_mode's comparison flag
inline bool Biases(const sampler_desc &desc) {
    using A = texture_address_mode;
    const auto screen = [](A a) { return a == A::clamp || a == A::border || a == A::mirror_once; };
    return (static_cast<uint32_t>(desc.filter) & CompareBit) == 0 && !screen(desc.address_u) &&
           !screen(desc.address_v);
}
inline float Biased(float bias, float offset) {
    return std::fmin(std::fmax(bias + offset, MinBias), MaxBias);
}
// Counted for the panel and the log: samplers seen, changed, and the game's own bias range on
// the changed ones (hundredths, so it fits an atomic int).
struct Stats {
    std::atomic<unsigned> heap_seen{}, heap_changed{}, static_seen{}, static_changed{};
    std::atomic<int> min_bias{INT32_MAX}, max_bias{INT32_MIN};
    void Bias(float bias) {
        const int b = static_cast<int>(std::lround(std::fmin(std::fmax(bias, -100.0f), 100.0f) * 100));
        for (int v = min_bias; b < v && !min_bias.compare_exchange_weak(v, b);)
            ;
        for (int v = max_bias; b > v && !max_bias.compare_exchange_weak(v, b);)
            ;
    }
};
// Returns whether desc changed.
inline bool Apply(sampler_desc &desc, float offset, Stats &stats, bool is_static) {
    ++(is_static ? stats.static_seen : stats.heap_seen);
    if (!(offset != 0) || !Biases(desc))
        return false;
    stats.Bias(desc.mip_lod_bias);
    desc.mip_lod_bias = Biased(desc.mip_lod_bias, offset);
    ++(is_static ? stats.static_changed : stats.heap_changed);
    return true;
}
// Pipeline layouts (D3D12 root signatures) carry static samplers inside their sampler ranges.
// The params are const, so a changed layout is copied into storage, which must outlive the
// create_pipeline_layout call. Returns whether any static sampler changed; params then point
// into storage.
struct LayoutCopy {
    std::vector<reshade::api::pipeline_layout_param> params;
    std::vector<std::vector<reshade::api::descriptor_range_with_flags>> ranges;
    std::vector<std::vector<sampler_desc>> samplers;
};
inline bool HasStaticSamplers(const reshade::api::descriptor_range_with_flags &r) {
    return r.type == reshade::api::descriptor_type::sampler && r.count && r.static_samplers;
}
inline bool WithFlags(reshade::api::pipeline_layout_param_type t) {
    using T = reshade::api::pipeline_layout_param_type;
    return t == T::descriptor_table_with_flags || t == T::push_descriptors_with_ranges_and_flags;
}
inline bool ApplyLayout(uint32_t count, reshade::api::pipeline_layout_param *&params, float offset,
                        Stats &stats, LayoutCopy &copy) {
    if (!(offset != 0) || !params || !count)
        return false;
    copy.params.assign(params, params + count);
    copy.ranges.clear();
    copy.samplers.clear();
    bool changed = false;
    for (auto &p : copy.params) {
        if (!WithFlags(p.type) || !p.descriptor_table_with_flags.ranges)
            continue;
        const auto &table = p.descriptor_table_with_flags;
        auto &ranges = copy.ranges.emplace_back(table.ranges, table.ranges + table.count);
        for (auto &r : ranges) {
            if (!HasStaticSamplers(r))
                continue;
            auto &s = copy.samplers.emplace_back(r.static_samplers, r.static_samplers + r.count);
            for (auto &d : s)
                changed |= Apply(d, offset, stats, true);
            r.static_samplers = s.data(); // inner buffers keep their address when outer moves
        }
        p.descriptor_table_with_flags.ranges = ranges.data();
    }
    if (changed)
        params = copy.params.data();
    return changed;
}
inline std::atomic<unsigned> mode{}; // read at sampler creation
inline Stats stats;
} // namespace rrlodbias
