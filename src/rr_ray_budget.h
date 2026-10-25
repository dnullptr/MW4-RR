#pragma once
#include "rr_ray_density.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
namespace rrraybudget {
// CPU side of the more-rays experiment: the gate's proof of the game's ray lists, the caps for a
// raised per-lobe ray target, and the session stop rule that watches how full the lists get.
inline constexpr std::uint64_t Resolve = 0x293995352d1f8220ull; // cs_vrrt_resolve
inline constexpr std::uint64_t ListHeader = 144, EntryBytes = 4, PixelEntryBytes = 4;
inline constexpr double Fill = 0.5;      // expected rays may use at most this share of a list
inline constexpr double StopFill = 0.75; // a raised frame above this stops raising
inline constexpr double MaxTarget = 1.0; // rays per pixel per lobe
inline constexpr unsigned SettingsRegisters = 38, TargetRegister = 2, SceneRegister = 86;
inline constexpr std::uint64_t PrivateBytes = 1024; // private cb1 copy
inline constexpr std::uint64_t ProofAge = 500;      // ms; resolve uses the previous apply's proof

// Session option, never saved.
enum Choice : unsigned { Off, Scale15, Scale20, Scale25, Count };
inline constexpr const char *labels[Count] = {"Off", "1.5x", "2x", "2.5x"};
inline constexpr float scales[Count] = {1.0f, 1.5f, 2.0f, 2.5f};
inline constexpr unsigned Valid(unsigned c) {
    return c < Count ? c : Off;
}
inline std::atomic<unsigned> choice{};

enum class Binding : unsigned { Unknown, Table, Root };
struct View {
    Binding binding{};
    bool raw{};                        // R32_TYPELESS raw buffer view with a known byte range
    std::uint64_t resource{}, bytes{}; // opaque resource identity, view size in bytes
};
// Native apply UAVs: u2 pixel counter, u3 lobe-1 pixel list, u5 lobe-1 list, u6 lobe-2 list.
enum ListIndex : unsigned { PixelCounter, PixelList, Lobe1, Lobe2, ListCount };
inline constexpr unsigned Registers[ListCount] = {2, 3, 5, 6};
struct Lists {
    bool valid{};
    const char *reason = "not observed"; // string literal; compared by pointer
    unsigned w{}, h{};
    std::array<View, ListCount> views{};
    std::array<std::uint64_t, 2> entries{}; // usable ray entries per lobe
    std::uint64_t pixel_entries{};
};
// The first reason a view cannot be trusted as a list of at least `least` bytes, or null.
// Only a table-bound raw view with a known size counts: out-of-range writes through it are
// discarded, while a root UAV has no size.
inline const char *Reject(const View &v, const char *const (&why)[4], std::uint64_t least) {
    if (v.binding == Binding::Root)
        return why[0];
    if (v.binding != Binding::Table)
        return why[1];
    if (!v.raw)
        return why[2];
    return v.bytes < least ? why[3] : nullptr;
}
inline Lists Prove(const std::array<View, ListCount> &views, unsigned w, unsigned h) {
    static constexpr const char *why[3][4] = {
        {"lobe 1 list is a root UAV", "lobe 1 list binding unknown", "lobe 1 list size unknown",
         "lobe 1 list too small"},
        {"lobe 2 list is a root UAV", "lobe 2 list binding unknown", "lobe 2 list size unknown",
         "lobe 2 list too small"},
        {"pixel list is a root UAV", "pixel list binding unknown", "pixel list size unknown",
         "pixel list too small"}};
    constexpr unsigned order[3] = {Lobe1, Lobe2, PixelList};
    constexpr std::uint64_t least[3] = {ListHeader + 1, ListHeader + 1, PixelEntryBytes};
    Lists r;
    r.w = w;
    r.h = h;
    r.views = views;
    if (!w || !h) {
        r.reason = "render size unknown";
        return r;
    }
    for (unsigned i = 0; i < 3; ++i)
        if (const char *bad = Reject(views[order[i]], why[i], least[i])) {
            r.reason = bad;
            return r;
        }
    r.valid = true;
    r.reason = "proven";
    for (unsigned lobe = 0; lobe < 2; ++lobe)
        r.entries[lobe] = (views[Lobe1 + lobe].bytes - ListHeader) / EntryBytes;
    r.pixel_entries = views[PixelList].bytes / PixelEntryBytes;
    if (r.pixel_entries < std::uint64_t(w) * h)
        r.entries[0] = std::min(r.entries[0], r.pixel_entries);
    return r;
}
// Cap for a lobe's raised target, in rays per pixel: min(MaxTarget, Fill * entries / (w*h)); 0 if
// unknown.
inline float Cap(std::uint64_t entries, unsigned w, unsigned h) {
    if (!entries || !w || !h)
        return 0;
    return float(std::min(MaxTarget, Fill * double(entries) / (double(w) * h)));
}
// Same rule the GPU applies (the shader uses a shared copy in src/rr_vrrt_target.h): only a
// positive target below the cap is raised, to min(target * scale, cap); never lowered. Zero,
// negative, NaN and infinite targets and scale <= 1 return target unchanged.
inline float Raised(float target, float scale, float cap) {
    if (!(target > 0 && target < cap && scale > 1))
        return target;
    return std::min(target * scale, cap);
}
inline std::string ViewText(const View &v) {
    if (v.binding == Binding::Root)
        return "root";
    if (v.binding != Binding::Table)
        return "unknown";
    if (!v.raw)
        return "table (size unknown)";
    char text[48];
    std::snprintf(text, sizeof(text), "table %lluB", static_cast<unsigned long long>(v.bytes));
    return text;
}
// "RR ray lists 1920x1080: lobe1=table 8294544B lobe2=table 8294544B pixels=table 8294400B
//  counter=root | rays/px lobe1=1.00 lobe2=1.00 | proven"
inline std::string Describe(const Lists &lists) {
    const double area = double(lists.w) * lists.h;
    const bool known = lists.valid && area > 0;
    char text[512];
    std::snprintf(text, sizeof(text),
                  "RR ray lists %ux%u: lobe1=%s lobe2=%s pixels=%s counter=%s | rays/px lobe1=%.2f "
                  "lobe2=%.2f | %s%s",
                  lists.w, lists.h, ViewText(lists.views[Lobe1]).c_str(),
                  ViewText(lists.views[Lobe2]).c_str(), ViewText(lists.views[PixelList]).c_str(),
                  ViewText(lists.views[PixelCounter]).c_str(),
                  known ? double(lists.entries[0]) / area : 0,
                  known ? double(lists.entries[1]) / area : 0,
                  lists.valid ? "" : "unproven: ", lists.valid ? "proven" : lists.reason);
    return text;
}

// One completed frame as the stop rule sees it.
struct FrameSample {
    std::array<double, 2> expected{}, worst{}; // rays this frame per lobe
    std::array<std::uint64_t, 2> entries{};    // proven capacity; 0 = unproven
    std::array<float, 2> target{};             // apply cb1[2].x/.y; 0 = not read
    unsigned w{}, h{};
    bool raised{}; // this frame's resolve used a raised target
};
// From a completed frame's readback: per lobe, expected = sum over buckets 1..16 of count / i
// plus the extra rays, worst = sum of those counts plus the extra rays. Bucket 0 (not requested)
// is ignored.
inline FrameSample Sample(const rrdensity::Frame &f, const Lists &lists, bool raised) {
    FrameSample s;
    s.w = lists.w;
    s.h = lists.h;
    s.raised = raised;
    for (unsigned lobe = 0; lobe < 2; ++lobe) {
        const unsigned base = lobe * rrdensity::Buckets;
        s.expected[lobe] = s.worst[lobe] = double(f[rrdensity::Extra + lobe]);
        for (unsigned i = 1; i < rrdensity::Buckets; ++i) {
            s.expected[lobe] += double(f[base + i]) / i;
            s.worst[lobe] += double(f[base + i]);
        }
        s.entries[lobe] = lists.valid ? lists.entries[lobe] : 0;
        std::memcpy(&s.target[lobe], &f[rrdensity::Target + lobe], sizeof(float));
    }
    return s;
}

// Counts the frames of a session and stops raising when a raised frame is too full or has no
// proven list. A stop lasts until Restart (the option is picked again).
class Monitor {
    struct Lobe {
        double rays{}, worst{}, fill{}, capacity{};
        float target{};
    };
    struct Window {
        std::array<Lobe, 2> lobes{};
        unsigned frames{}, sized{}, raised{};
    };
    Window window;
    std::string stop;
    bool stopped{}, started{};
    std::uint64_t since{};

  public:
    static constexpr std::uint64_t Interval = 10000;
    bool Stopped() const {
        return stopped;
    }
    const char *StopReason() const {
        return stop.c_str();
    }
    void Restart() {
        stopped = false;
        stop.clear();
    }
    // True only on the call that stops raising.
    bool Add(const FrameSample &s) {
        const double area = double(s.w) * s.h;
        ++window.frames;
        window.raised += s.raised;
        window.sized += area > 0;
        for (unsigned lobe = 0; lobe < 2; ++lobe) {
            Lobe &l = window.lobes[lobe];
            if (area > 0) {
                l.rays += s.expected[lobe] / area;
                l.worst = std::max(l.worst, s.worst[lobe] / area);
            }
            if (s.entries[lobe]) {
                l.fill = std::max(l.fill, s.expected[lobe] / double(s.entries[lobe]));
                if (area > 0)
                    l.capacity = double(s.entries[lobe]) / area;
            }
            if (s.target[lobe])
                l.target = s.target[lobe];
        }
        if (!s.raised || stopped)
            return false;
        for (unsigned lobe = 0; lobe < 2; ++lobe) {
            const bool proven = s.entries[lobe] != 0;
            const double fill = proven ? s.expected[lobe] / double(s.entries[lobe]) : 0;
            if (proven && fill <= StopFill)
                continue;
            char text[96];
            if (!proven)
                std::snprintf(text, sizeof(text), "a raised frame had no proven ray list");
            else
                std::snprintf(text, sizeof(text), "lobe %u expected %.0f rays, %.0f%% of its list",
                              lobe + 1, s.expected[lobe], 100 * fill);
            stop = text;
            stopped = true;
            return true;
        }
        return false;
    }
    // Once per interval with activity; the stop and its reason survive the reset.
    bool Report(std::uint64_t now, std::string &out) {
        if (!started) {
            started = true;
            since = now;
            return false;
        }
        if (now < since || now - since < Interval)
            return false;
        const bool active = window.frames != 0;
        if (active) {
            char text[640];
            int at = std::snprintf(text, sizeof(text), "RR ray budget (%llus): frames=%u raised=%u",
                                   static_cast<unsigned long long>((now - since) / 1000),
                                   window.frames, window.raised);
            for (unsigned lobe = 0; lobe < 2 && at > 0 && at < int(sizeof(text)); ++lobe) {
                const Lobe &l = window.lobes[lobe];
                at += std::snprintf(text + at, sizeof(text) - at,
                                    " | lobe%u rays/px=%.3f worst/px=%.2f peakFill=%.1f%% "
                                    "capacity/px=%.2f target=%.3f",
                                    lobe + 1, window.sized ? l.rays / window.sized : 0, l.worst,
                                    100 * l.fill, l.capacity, double(l.target));
            }
            if (at > 0 && at < int(sizeof(text)))
                std::snprintf(text + at, sizeof(text) - at, " | %s%s", stopped ? "stopped: " : "",
                              stopped ? stop.c_str() : "running");
            out = text;
        }
        window = {};
        started = true;
        since = now;
        return active;
    }
};

// Limits how often the resolve gate's verdict is logged.
class Verdicts {
    const char *last{};
    std::uint64_t at{};
    bool logged{};

  public:
    static constexpr std::uint64_t Gap = 2000;
    // True when `reason` (a string literal, compared by pointer) differs from the last logged
    // one and at least Gap ms passed since the last log (the first call always logs). On true,
    // remembers reason and time. Suppressed changes are logged on a later call.
    bool Changed(const char *reason, std::uint64_t now) {
        if (logged && (reason == last || now < at || now - at < Gap))
            return false;
        logged = true;
        last = reason;
        at = now;
        return true;
    }
};
} // namespace rrraybudget
