#pragma once
#include <cstdint>
namespace rrstatus {
enum class State { Off, Active, Waiting, Fallback, ParentOff, Error, Unavailable };
struct Flags {
    bool rr{}, sss{}, glass{}, failed{}, retired{}, sss_failed{}, glass_failed{};
};
enum class Route { Unknown, NativeSR, Replacement, SpatialFallback };
struct Snapshot {
    State rr{}, sss{}, glass{};
    Route route{};
};
// UI-only telemetry: consumes results already read by the normal completion
// poll. It never waits for the GPU or reads a GPU resource itself.
class Telemetry {
    std::uint32_t generation{}, serial{};
    std::uint64_t completed_at{}, fallback_at{};
    bool completed{}, fallback{}, displayed{}, sss{}, glass{};
    Route route{};

  public:
    void Reset(std::uint32_t value) {
        *this = {};
        generation = value;
    }
    void Fallback(std::uint32_t gen, std::uint64_t now) {
        if (gen == generation) {
            fallback = true;
            fallback_at = now;
        }
    }
    void Completed(std::uint32_t number, std::uint32_t gen, std::uint64_t now, bool good,
                   bool used_sss, bool used_glass, bool replaced_sr = false) {
        if (gen != generation)
            return;
        if (!good)
            Fallback(gen, now);
        if (completed && (number < serial || (number == serial && good)))
            return;
        completed = true;
        serial = number;
        completed_at = now;
        displayed = good;
        route =
            replaced_sr ? (good ? Route::Replacement : Route::SpatialFallback) : Route::NativeSR;
        sss = good && used_sss;
        glass = good && used_glass;
        if (!good)
            Fallback(gen, now);
    }
    Snapshot Read(const Flags &f, std::uint64_t now) const {
        const bool fresh = completed && now >= completed_at && now - completed_at <= 1000;
        const bool recent_fallback = fallback && now >= fallback_at && now - fallback_at <= 1000;
        const State rr = f.failed             ? State::Error
                         : f.retired          ? State::Unavailable
                         : !f.rr              ? State::Off
                         : recent_fallback    ? State::Fallback
                         : fresh && displayed ? State::Active
                                              : State::Waiting;
        auto guide = [&](bool on, bool failed, bool delivered) {
            return failed                           ? State::Error
                   : !on                            ? State::Off
                   : !f.rr || f.failed || f.retired ? State::ParentOff
                   : fresh && delivered             ? State::Active
                                                    : State::Waiting;
        };
        return {rr, guide(f.sss, f.sss_failed, sss), guide(f.glass, f.glass_failed, glass),
                fresh && f.rr && !f.failed && !f.retired ? route : Route::Unknown};
    }
};
inline const char *RouteLabel(Route route) {
    switch (route) {
    case Route::Replacement:
        return "RR replaces SR";
    case Route::SpatialFallback:
        return "Spatial fallback (SR skipped)";
    case Route::NativeSR:
        return "Native SR retained";
    default:
        return "Awaiting a completed frame";
    }
}
inline const char *Label(State state) {
    switch (state) {
    case State::Off:
        return "OFF";
    case State::Active:
        return "ON";
    case State::Waiting:
        return "ON / waiting";
    case State::Fallback:
        return "ON / fallback";
    case State::ParentOff:
        return "ON / RR off";
    case State::Error:
        return "ERROR";
    default:
        return "unavailable";
    }
}
} // namespace rrstatus
