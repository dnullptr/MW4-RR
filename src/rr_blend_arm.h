#pragma once
#include <type_traits>
#include <utility>

namespace rrblend {
// A replayed draw must not add samples to a game counting query: the query
// hooks must be proven (stencil_tracking: Reset marks queries known even when
// they never installed) and no query may be open.
template <class Proof> bool QueriesClear(const Proof &p) noexcept {
    return p.stencil_tracking && p.queries_known && !p.query_count;
}
// One ExecuteIndirect's arm for the coverage capture: the ReShade pre-event
// proves the bound state and arms it; the native detour's observer, which runs
// next on the same recording thread inside the same call, takes it. Take
// always disarms, so an arm whose native call was skipped never reaches a
// later call, and a call on another list (an inner hooked layer) takes nothing.
template <class Payload> class ArmLatch {
    static_assert(std::is_nothrow_default_constructible_v<Payload> &&
                  std::is_nothrow_move_assignable_v<Payload>);
    const void *list{};
    Payload payload{};
    bool valid{};

  public:
    bool Armed() const noexcept {
        return valid;
    }
    // Drops the payload's references.
    void Clear() noexcept {
        list = nullptr;
        payload = Payload{};
        valid = false;
    }
    // A null list never arms.
    void Set(const void *cl, Payload armed) noexcept {
        Clear();
        if (!cl)
            return;
        list = cl;
        payload = std::move(armed);
        valid = true;
    }
    // True, with the payload in out, only when armed for cl. out is empty on
    // false; the latch is empty either way.
    bool Take(const void *cl, Payload &out) noexcept {
        const bool match = valid && cl && list == cl;
        if (match)
            out = std::move(payload);
        else
            out = Payload{};
        Clear();
        return match;
    }
};
} // namespace rrblend
