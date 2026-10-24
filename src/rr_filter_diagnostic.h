#pragma once
#include <array>
#include <cstdint>
namespace rrfilter {
// CPU recording proof only. The caller owns resources and validates native
// pipeline, binding/state/shape, same-recording identity and submission order.
class Chain {
    std::array<uint64_t, 2> history{};
    bool seen{}, consumed{}, invalid{};
public:
    bool Temporal(std::array<uint64_t, 2> pair) {
        if (seen || invalid || !pair[0] || !pair[1] || pair[0] == pair[1]) {
            invalid = true;
            return false;
        }
        seen = true;
        history = pair;
        return true;
    }
    bool Matches(std::array<uint64_t, 2> pair) const {
        return seen && !invalid && !consumed && history == pair;
    }
    bool Spatial(std::array<uint64_t, 2> pair) {
        if (!Matches(pair)) return false;
        consumed = true;
        return true;
    }
    bool Accepted() const { return seen && consumed && !invalid; }
};
struct Status { bool desired{}, effective{}, pending{}; };
}
