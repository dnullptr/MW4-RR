#pragma once
#include <climits>
namespace rrlive {
class HistoryPolicy {
    unsigned frame = UINT_MAX, generation{};
    bool rejected = true;

  public:
    bool Next(unsigned current, unsigned enable_generation, bool cut) {
        const bool reset = rejected || cut || generation != enable_generation ||
                           frame == UINT_MAX || frame == UINT_MAX - 1 || current != frame + 1;
        frame = current;
        generation = enable_generation;
        rejected = false;
        return reset;
    }
    void Reject() {
        rejected = true;
    }
};
} // namespace rrlive
