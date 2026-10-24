#pragma once
#include <cstdint>
namespace rrsparsebudget {
// CurrentUsage already includes this pool and the game. Credit the completed
// slot being released only when it belongs to the queried adapter.
inline bool Replacement(uint64_t allocated, uint64_t previous, uint64_t needed, bool same_adapter,
                        uint64_t budget, uint64_t usage, uint64_t &next) {
    next = 0;
    if (previous > allocated || !needed || needed == UINT64_MAX || usage > budget)
        return false;
    const auto retained = allocated - previous;
    if (needed > UINT64_MAX - retained)
        return false;
    const auto credit = same_adapter ? previous : 0;
    const auto growth = needed > credit ? needed - credit : 0;
    if (growth > budget - usage)
        return false;
    next = retained + needed;
    return true;
}
} // namespace rrsparsebudget
