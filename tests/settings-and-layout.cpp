#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_specular_distance.h"
#include <cassert>
#include <string_view>

constexpr rrsettings::Preferences defaults;
static_assert(defaults.rr && defaults.sss && defaults.glass && defaults.hit &&
              defaults.preset == 6);
static_assert(std::string_view(rrsettings::QualityLabel(0)) == "Performance");
static_assert(std::string_view(rrsettings::QualityLabel(5)) == "Native / DLAA");
static_assert(std::string_view(rrsettings::QualityLabel(-1)) == "Not observed yet");

int main() {
    assert(rrvrsconfig::Allowed("VRS@0;123 = true // preserved\n", true, true));
    assert(rrvrsconfig::Allowed("VRS@0;123 = false\n", true, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = true\n", false, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = true\nVRS@1;123 = true\n", true, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = invalid\n", true, true));
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 4;
    for (auto size : {std::pair{640u, 368u}, {1280u, 720u}, {3440u, 1440u}, {5120u, 2880u}}) {
        d.Width = size.first;
        d.Height = size.second;
        assert(rrglass::Shape(d, 1));
        assert(!rrglass::Shape(d, 0));
    }
    // Specular hit distance follows the runtime guide extent, not a fixed resolution.
    D3D12_RESOURCE_DESC h{};
    h.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    h.DepthOrArraySize = h.MipLevels = 1;
    h.SampleDesc.Count = 1;
    h.Format = DXGI_FORMAT_R16_FLOAT;
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (auto size : {std::pair{640u, 368u}, {1280u, 720u}, {1920u, 1088u}, {2560u, 1440u},
                      {3440u, 1440u}}) {
        h.Width = size.first;
        h.Height = size.second;
        assert(rrspecular::ValidSource(h, size.first, size.second, read));
        assert(rrspecular::ValidSource(h, size.first, size.second,
                                       read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        assert(!rrspecular::ValidSource(h, size.first + 2, size.second, read));
        assert(!rrspecular::ValidSource(h, size.first, size.second,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        assert(rrspecular::DispatchShape(size.first, size.second, (size.first + 15) / 16,
                                         (size.second + 15) / 16, 1));
        assert(!rrspecular::DispatchShape(size.first, size.second, (size.first + 7) / 8,
                                          (size.second + 15) / 16, 1));
    }
    h.Format = DXGI_FORMAT_R32_FLOAT;
    assert(!rrspecular::ValidSource(h, UINT(h.Width), h.Height, read));
    rrspecular::Diagnostics stats;
    char line[320];
    assert(!stats.Summary(line, sizeof(line)));
    assert(std::string_view(line).find("blurDispatches=0") != std::string_view::npos);
    stats.Dispatch();
    stats.Observe(rrspecular::Stage::Shape);
    assert(stats.Summary(line, sizeof(line)));
    assert(std::string_view(line).find("lastReject=shape") != std::string_view::npos);
    assert(!stats.Summary(line, sizeof(line)));
    assert(stats.Scan(1) && !stats.Scan(1) && stats.Candidate(1) && !stats.Candidate(1));
    d.Width = 16385;
    assert(!rrglass::Shape(d, 1));
    d.Width = 1920;
    d.SampleDesc.Count = 1;
    assert(!rrglass::Shape(d, 1));
}
