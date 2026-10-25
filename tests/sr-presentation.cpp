#include "../src/rr_sr_eligibility.h"
#include "../src/rr_spatial_fallback.h"
#include "../src/rr_status.h"
#include <cassert>
#include <cmath>
#include <cstring>
int main() {
    rrstatus::Telemetry telemetry;
    rrstatus::Flags flags;
    flags.rr = true;
    telemetry.Reset(1);
    telemetry.Completed(1, 1, 100, true, false, false, true);
    assert(telemetry.Read(flags, 100).route == rrstatus::Route::Replacement);
    telemetry.Completed(2, 1, 101, false, false, false, true);
    assert(telemetry.Read(flags, 101).route == rrstatus::Route::SpatialFallback);
    telemetry.Completed(3, 1, 102, true, false, false, false);
    assert(telemetry.Read(flags, 102).route == rrstatus::Route::NativeSR);
    assert(telemetry.Read(flags, 2000).route == rrstatus::Route::Unknown);
    assert(telemetry.Read(flags, 102).hit == rrstatus::State::Off);
    flags.hit = true;
    assert(telemetry.Read(flags, 102).hit == rrstatus::State::Waiting);
    telemetry.Completed(4, 1, 103, true, false, false, true, true);
    assert(telemetry.Read(flags, 103).hit == rrstatus::State::Active);
    telemetry.Completed(5, 1, 104, true, false, false, true, false);
    assert(telemetry.Read(flags, 104).hit == rrstatus::State::Waiting);
    flags.rr = false;
    assert(telemetry.Read(flags, 104).hit == rrstatus::State::ParentOff);
    flags.rr = true;

    rrsr::StateProof p;
    p.known = {true, true, true, true};
    p.states = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    p.resources = {1, 2, 3, 4};
    assert(rrsr::Eligible(p));
    for (unsigned i = 0; i < 4; ++i) {
        auto q = p;
        q.known[i] = false;
        assert(!rrsr::Eligible(q));
    }
    auto q = p;
    q.states[0] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    assert(!rrsr::Eligible(q));
    q = p;
    q.states[3] = D3D12_RESOURCE_STATE_COMMON;
    assert(!rrsr::Eligible(q));
    q = p;
    q.resources[3] = q.resources[0];
    assert(!rrsr::Eligible(q));
    q = p;
    q.states[0] =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    assert(rrsr::Eligible(q));
    // The capture log's reason, one clause per SR resource, in Eligible's order of checks.
    char text[300];
    rrsr::Describe(p, text, sizeof(text));
    assert(!strcmp(text, "color 0x40 ok, depth 0x40 ok, motion 0x40 ok, output 0x8 ok"));
    rrsr::Describe(q, text, sizeof(text));
    assert(!strncmp(text, "color 0xc0 ok, ", 15));
    q = p;
    q.states[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    q.known[1] = false;
    q.resources[2] = q.resources[0];
    q.states[3] = D3D12_RESOURCE_STATE_COMMON;
    q.aliased[3] = true;
    assert(!rrsr::Eligible(q));
    rrsr::Describe(q, text, sizeof(text));
    assert(!strcmp(text,
                   "color 0x80 (wants NPSR, reads only), depth unknown, motion same as color, "
                   "output aliased 0x0 (wants UAV)"));
    q = p;
    q.resources[1] = 0; // Never known to the tracker either.
    q.known[1] = false;
    q.resources[3] = q.resources[2];
    q.states[2] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    rrsr::Describe(q, text, sizeof(text));
    assert(!strcmp(text, "color 0x40 ok, depth missing, motion 0x840 (wants NPSR, reads only), "
                         "output same as motion"));
    // Every clause "ok" exactly when Eligible accepts.
    for (unsigned i = 0; i < 4; ++i)
        for (auto s :
             {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_GENERIC_READ}) {
            q = p;
            q.states[i] = s;
            rrsr::Describe(q, text, sizeof(text));
            const char *at = text;
            unsigned ok = 0;
            while ((at = strstr(at, " ok")))
                ++ok, ++at;
            assert((ok == 4) == rrsr::Eligible(q) && ok >= 3);
        }
    // A short buffer keeps a terminated prefix; an empty one is left alone.
    char short_text[12];
    rrsr::Describe(p, short_text, sizeof(short_text));
    assert(!strcmp(short_text, "color 0x40 "));
    short_text[0] = 'x';
    rrsr::Describe(p, short_text, 0);
    assert(short_text[0] == 'x');
    auto a = rrspatial::Coordinate(0, 2, 4);
    assert(a.first == 0 && a.second == 1 && a.fraction == 0);
    a = rrspatial::Coordinate(1, 2, 4);
    assert(a.first == 0 && a.second == 1 && std::abs(a.fraction - .25f) < .00001f);
    a = rrspatial::Coordinate(3, 2, 4);
    assert(a.first == 1 && a.second == 1 && a.fraction == 0);
    for (auto in : {1u, 640u, 1088u, 1920u, 3440u})
        for (auto out : {1u, 720u, 1080u, 1440u, 2160u, 5120u})
            for (unsigned x = 0; x < out; ++x) {
                auto c = rrspatial::Coordinate(x, in, out);
                assert(c.first < in && c.second < in && c.fraction >= 0 && c.fraction <= 1);
            }
    assert(rrspatial::UseFallback(true, true, false));
    assert(rrspatial::UseFallback(true, false, true));
    assert(!rrspatial::UseFallback(false, true, true));
    assert(!rrspatial::UseFallback(true, false, false));
}
