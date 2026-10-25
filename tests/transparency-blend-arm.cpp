#include "../src/rr_blend_arm.h"
#include <cassert>
#include <cstdio>
#include <memory>

// The transparency albedo blend's indirect arm (src/rr_blend_arm.h): the
// ReShade pre-event arms one ExecuteIndirect and the native detour's observer
// on the same thread takes it. Plain assert: never built with NDEBUG.
struct Payload {
    std::shared_ptr<int> held; // stands in for the snapshot's and entry's references
    int value{};
};
using Latch = rrblend::ArmLatch<Payload>;
static int a, b; // two command lists, by address

// The armed list takes the payload once; the latch is then empty.
static void TakeOnce() {
    Latch latch;
    assert(!latch.Armed());
    auto held = std::make_shared<int>(7);
    latch.Set(&a, {held, 3});
    assert(latch.Armed() && held.use_count() == 2);
    Payload out;
    assert(latch.Take(&a, out) && out.value == 3 && out.held == held);
    assert(!latch.Armed() && held.use_count() == 2); // moved, not copied
    Payload again;
    assert(!latch.Take(&a, again) && !again.held && again.value == 0);
}

// Another list (an inner hooked layer, or a stale arm) never takes it, and
// that call disarms: the references are released and the armed list cannot
// take it afterward either.
static void OtherListDisarms() {
    Latch latch;
    auto held = std::make_shared<int>(1);
    latch.Set(&a, {held, 5});
    Payload out;
    assert(!latch.Take(&b, out) && !out.held && out.value == 0);
    assert(!latch.Armed() && held.use_count() == 1);
    assert(!latch.Take(&a, out) && !out.held);
}

// A null list never arms, and a null list never takes.
static void NullList() {
    Latch latch;
    auto held = std::make_shared<int>(1);
    latch.Set(nullptr, {held, 1});
    assert(!latch.Armed() && held.use_count() == 1);
    Payload out;
    assert(!latch.Take(nullptr, out) && !out.held);
    latch.Set(&a, {held, 1});
    assert(!latch.Take(nullptr, out) && !latch.Armed() && held.use_count() == 1);
    // A later event on a null list disarms an earlier arm.
    latch.Set(&a, {held, 1});
    latch.Set(nullptr, {held, 2});
    assert(!latch.Armed() && held.use_count() == 1 && !latch.Take(&a, out) && !out.held);
}

// A new event replaces the previous arm; Clear drops it.
static void ReplaceAndClear() {
    Latch latch;
    auto first = std::make_shared<int>(1), second = std::make_shared<int>(2);
    latch.Set(&a, {first, 1});
    latch.Set(&b, {second, 2});
    assert(first.use_count() == 1 && second.use_count() == 2);
    Payload out;
    assert(!latch.Take(&a, out));
    latch.Set(&b, {second, 2});
    latch.Clear();
    assert(!latch.Armed() && second.use_count() == 1);
    assert(!latch.Take(&b, out) && !out.held);
}

// A taken arm overwrites whatever the caller's payload held.
static void TakeOverwrites() {
    Latch latch;
    auto armed = std::make_shared<int>(1), stale = std::make_shared<int>(2);
    latch.Set(&a, {armed, 9});
    Payload out{stale, 4};
    assert(latch.Take(&a, out) && out.held == armed && out.value == 9);
    assert(stale.use_count() == 1);
    // A refused take leaves the caller's payload empty, never a stale value.
    Payload refused{stale, 4};
    assert(!latch.Take(&a, refused) && !refused.held && refused.value == 0);
}

int main() {
    TakeOnce();
    OtherListDisarms();
    NullList();
    ReplaceAndClear();
    TakeOverwrites();
    printf("transparency-blend-arm: all tests passed\n");
    return 0;
}
