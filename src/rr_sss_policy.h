#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <initializer_list>
namespace rrsss {
template <class T> int RecordingIndex(size_t count, T *const *lists, T *command) {
    int index = -1;
    for (size_t i = 0; i < count; ++i)
        if (lists[i] == command) {
            if (index >= 0 || i > size_t((std::numeric_limits<int>::max)()))
                return -2;
            index = int(i);
        }
    return index;
}
inline bool FitsOwnedBudget(uint64_t base, uint64_t depth, uint64_t persistent, uint64_t sss,
                            uint64_t hit, uint64_t limit, uint64_t count = 6) {
    if (!count || persistent > limit || depth > limit || base > limit - depth || hit > limit ||
        sss > limit - hit)
        return false;
    const auto per = (limit - persistent) / count;
    return base + depth <= per && sss + hit <= per - base - depth;
}
inline bool Present(int index) {
    return index != -1;
}
inline bool Admit(int pre, int producer, int consumer, bool pre_submitted, bool producer_submitted,
                  bool baseline) {
    return baseline && pre >= 0 && producer >= pre && consumer > producer && !pre_submitted &&
           !producer_submitted;
}
struct Admission {
    bool sealed{}, accepted{};
    bool Seal(int consumer, bool ready) {
        if (sealed || !Present(consumer))
            return false;
        sealed = true;
        accepted = ready;
        return true;
    }
};
inline unsigned ParitySample(unsigned x, unsigned y) {
    return (x & 1) + 2 * (y & 1);
}
inline bool ValidScalar(float value) {
    return std::isfinite(value) && value != -65504.0f && std::abs(value) < 65504.0f;
}
struct DrawResult {
    bool issued{}, failed{};
};
template <class Prepare, class Bind, class Draw, class Restore>
DrawResult DrawOnce(Prepare prepare, Bind bind, Draw draw, Restore restore) noexcept {
    DrawResult result;
    bool touched = false;
    try {
        prepare();
        touched = true;
        bind();
        result.issued = true;
        draw();
    } catch (...) {
        result.failed = true;
    }
    if (touched)
        try {
            restore();
        } catch (...) {
            result.failed = true;
        }
    return result;
}
} // namespace rrsss
