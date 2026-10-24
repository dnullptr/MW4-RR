#pragma once
#include "rr_bindings.h"
#include "rr_resolution_limits.h"
namespace rrmaterial {
inline bool ValidAlbedoPair(const rrbindings::Value &a, const rrbindings::Value &b) {
    auto valid = [](const auto &v) {
        using namespace reshade::api;
        return v.known && v.resource && v.dimension == unsigned(resource_type::texture_2d) &&
               v.format == 26 && v.view_format == 26 &&
               v.view_type == unsigned(resource_view_type::texture_2d_array) &&
               v.first_level == 0 && v.levels == 1 && v.first_layer == 0 && v.layers == 4 &&
               rrresolution::HalfInput(v.width, v.height);
    };
    return valid(a) && valid(b) && a.resource != b.resource && a.width == b.width &&
           a.height == b.height;
}
} // namespace rrmaterial
