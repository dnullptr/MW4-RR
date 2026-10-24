#pragma once
namespace rrinternal {
inline thread_local bool active{};
struct Scope {
    bool previous = active;
    Scope() {
        active = true;
    }
    ~Scope() {
        active = previous;
    }
};
} // namespace rrinternal
