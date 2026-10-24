#include "../src/rr_sr_eligibility.h"
#include "../src/rr_spatial_fallback.h"
#include "../src/rr_status.h"
#include <cassert>
#include <cmath>
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
