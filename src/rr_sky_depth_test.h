#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
namespace rrskydepth {
// Development test: give RR a finite far plane so sky depth 0 reads as very far
// instead of infinitely far. Only an infinite reverse-Z projection is changed:
// depth = near / z becomes depth = a + b / z with depth(near) = 1, depth(far) = 0.
inline std::atomic<bool> enabled{};
inline constexpr float FarScale = 1e6f;
enum class Layout { Unknown, RowVector, ColumnVector };
// Row-vector (pos * M): [11] = 1, [14] = near. Column-vector (M * pos): transposed.
inline Layout Detect(const std::array<float, 16> &m) {
    if (std::abs(m[10]) > 1e-6f || m[15] != 0)
        return Layout::Unknown;
    if (m[11] == 1 && m[14] > 0)
        return Layout::RowVector;
    if (m[14] == 1 && m[11] > 0)
        return Layout::ColumnVector;
    return Layout::Unknown;
}
inline bool Apply(std::array<float, 16> &m) {
    const auto layout = Detect(m);
    if (layout == Layout::Unknown)
        return false;
    const unsigned b_index = layout == Layout::RowVector ? 14 : 11;
    const double n = m[b_index], f = n * FarScale;
    m[10] = float(-n / (f - n));
    m[b_index] = float(n * f / (f - n));
    return true;
}
inline void Describe(const std::array<float, 16> &m, char *out, size_t size) {
    const auto layout = Detect(m);
    std::snprintf(out, size,
                  "Sky depth test: projection layout=%s m10=%g m11=%g m14=%g m15=%g",
                  layout == Layout::RowVector      ? "row-vector infinite reverse-Z"
                  : layout == Layout::ColumnVector ? "column-vector infinite reverse-Z"
                                                   : "not infinite reverse-Z (left unchanged)",
                  m[10], m[11], m[14], m[15]);
}
} // namespace rrskydepth
