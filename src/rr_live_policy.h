#pragma once
#include <climits>
namespace rrlive {
class HistoryPolicy {
    unsigned frame = UINT_MAX, generation{}, reason{};
    bool rejected = true;

  public:
    // Reset causes, reported for diagnostics only. Several may apply at once.
    enum Reason : unsigned {
        Rejected = 1,  // a frame, toggle or GPU check rejected history
        Cut = 2,       // the game or camera requested a reset
        Generation = 4, // RR was re-enabled or reconfigured
        Gap = 8,       // first frame, or frames were not consecutive
    };
    bool Next(unsigned current, unsigned enable_generation, bool cut) {
        reason = (rejected ? Rejected : 0u) | (cut ? Cut : 0u) |
                 (generation != enable_generation ? Generation : 0u) |
                 (frame == UINT_MAX || frame == UINT_MAX - 1 || current != frame + 1 ? Gap : 0u);
        frame = current;
        generation = enable_generation;
        rejected = false;
        return reason != 0;
    }
    unsigned LastReason() const {
        return reason;
    }
    void Reject() {
        rejected = true;
    }
};
} // namespace rrlive
