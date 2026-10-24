#pragma once
#include <array>
#include <cstdint>
namespace rrsss {
// Only graphics root-CBV addresses. No ownership and no resource dereferences.
class RootCBVs {
    std::array<uint64_t, 64> addresses{};

  public:
    bool Bind(unsigned parameter, uint64_t address) {
        if (parameter >= addresses.size())
            return false;
        addresses[parameter] = address;
        return true;
    }
    void Invalidate() {
        addresses = {};
    }
    bool Matches(unsigned parameter, uint64_t address) const {
        return parameter < addresses.size() && address && !(address & 255) &&
               addresses[parameter] == address;
    }
};
} // namespace rrsss
