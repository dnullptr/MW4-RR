#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
namespace rrtrace {
enum class Kind { Unknown, SR, RR, Other, FG };
// NVSDK_NGX_Feature: SuperSampling 1, FrameGeneration 11, RayReconstruction 13.
inline Kind FeatureKind(unsigned feature) {
    return feature == 1    ? Kind::SR
           : feature == 13 ? Kind::RR
           : feature == 11 ? Kind::FG
                           : Kind::Other;
}
inline const char *Name(Kind kind) {
    switch (kind) {
    case Kind::SR:
        return "SR";
    case Kind::RR:
        return "RR";
    case Kind::Other:
        return "other";
    case Kind::FG:
        return "FG";
    default:
        return "unknown";
    }
}
class Handles {
    std::map<uintptr_t, Kind> values;

  public:
    void Created(uintptr_t handle, unsigned feature, bool ok) {
        if (!ok || !handle)
            return;
        if (values.size() >= 256 && !values.count(handle))
            values.erase(values.begin());
        values[handle] = FeatureKind(feature);
    }
    void Released(uintptr_t handle, bool ok) {
        if (ok)
            values.erase(handle);
    }
    Kind Lookup(uintptr_t handle) const {
        auto i = values.find(handle);
        return i == values.end() ? Kind::Unknown : i->second;
    }
    size_t Size() const {
        return values.size();
    }
};
} // namespace rrtrace
