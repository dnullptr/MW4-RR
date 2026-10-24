#pragma once
#include <cstdint>
#include <limits>
#include <initializer_list>
namespace rrtrans {
#define RRTRANS_PRODUCER_ROUTE 1
inline bool ProducerReadState(uint32_t state) {
    return (state & 192u) != 0 && (state & ~192u) == 0;
}
// The producer owns its earlier snapshot. Later source writes/aliases do not
// change that private image; only original execution order is relevant here.
inline bool AdmitProducer(int prepass, int producer, int anchor, int consumer,
                          bool producer_submitted, bool anchor_submitted, bool baseline) {
    return baseline && prepass >= 0 && producer >= prepass && anchor >= producer &&
           consumer >= anchor && !producer_submitted && !anchor_submitted;
}
inline bool Extent(uint64_t source_width, uint64_t source_height, uint64_t render_width,
                   uint64_t render_height) {
    return source_width && source_height && source_width <= UINT32_MAX / 2 &&
           source_height <= UINT32_MAX / 2 && render_width == 2 * source_width &&
           render_height == 2 * source_height;
}
inline bool Admit(int prepass, int preparation, int anchor, int consumer,
                  bool preparation_submitted, bool anchor_submitted, bool baseline) {
    return baseline && prepass >= 0 && preparation >= prepass &&
           preparation < (std::numeric_limits<int>::max)() && anchor == preparation + 1 &&
           consumer > anchor && !preparation_submitted && !anchor_submitted;
}
// This policy is used only after the first composition callback has recorded
// the final copy on the anchor itself. Older preparation lists still refer to
// the snapshot, so their unique original submission and lifetime remain proven.
inline bool AdmitAtDraw(int prepass, int preparation, int anchor, int consumer,
                        bool preparation_submitted, bool anchor_submitted, bool baseline) {
    return baseline && prepass >= 0 && preparation >= prepass && anchor >= preparation &&
           anchor > prepass && consumer >= anchor && !preparation_submitted && !anchor_submitted;
}
struct CompositionLatch {
    uint32_t frame{UINT32_MAX};
    bool Claim(uint32_t current, uint32_t view) {
        if (current == UINT32_MAX || view == UINT32_MAX || frame == current)
            return false;
        frame = current;
        return true;
    }
};
inline bool FitsSlots(uint64_t count, uint64_t base, uint64_t depth, uint64_t sss, uint64_t glass,
                      uint64_t hit, uint64_t persistent, uint64_t limit) {
    if (!count || persistent > limit)
        return false;
    const auto per = (limit - persistent) / count;
    uint64_t remaining = per;
    for (auto bytes : {base, depth, sss, glass, hit}) {
        if (bytes > remaining)
            return false;
        remaining -= bytes;
    }
    return true;
}
inline bool RequiredSlots(uint64_t count, uint64_t base, uint64_t depth, uint64_t sss,
                          uint64_t glass, uint64_t hit, uint64_t persistent, uint64_t &result) {
    result = 0;
    if (!count)
        return false;
    const auto per = (UINT64_MAX - persistent) / count;
    uint64_t remaining = per, total{};
    for (auto bytes : {base, depth, sss, glass, hit}) {
        if (bytes > remaining)
            return false;
        remaining -= bytes;
        total += bytes;
    }
    result = persistent + count * total;
    return result != 0;
}
inline bool FitsSix(uint64_t base, uint64_t depth, uint64_t sss, uint64_t glass, uint64_t hit,
                    uint64_t persistent, uint64_t limit) {
    return FitsSlots(6, base, depth, sss, glass, hit, persistent, limit);
}
inline bool RequiredSix(uint64_t base, uint64_t depth, uint64_t sss, uint64_t glass, uint64_t hit,
                        uint64_t persistent, uint64_t &result) {
    return RequiredSlots(6, base, depth, sss, glass, hit, persistent, result);
}
} // namespace rrtrans
