#pragma once
#include <reshade_api_device.hpp>
#include <mutex>
#include <unordered_map>
namespace rrshader {
inline uint64_t Hash(const void *p, size_t n, uint64_t h = 14695981039346656037ull) {
    auto *b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
struct Binding {
    uint64_t pipeline{}, shader{};
};
class DispatchTracker {
    std::mutex mutex;
    std::unordered_map<uint64_t, Binding> bindings;

  public:
    void BindStages(uint64_t list, reshade::api::pipeline_stage stages, uint64_t pipeline,
                    uint64_t hash, bool compute) {
        if ((uint32_t(stages) & uint32_t(reshade::api::pipeline_stage::compute_shader)) != 0)
            Bind(list, pipeline, hash, compute);
        else if ((uint32_t(stages) & uint32_t(reshade::api::pipeline_stage::ray_tracing_shader)) !=
                 0)
            Reset(list);
    }
    void Bind(uint64_t list, uint64_t pipeline, uint64_t hash, bool compute) {
        std::lock_guard lock(mutex);
        if (!compute) {
            bindings.erase(list);
            return;
        }
        if (bindings.size() < 8192 || bindings.contains(list))
            bindings[list] = {pipeline, hash};
    }
    Binding Get(uint64_t list) {
        std::lock_guard lock(mutex);
        auto it = bindings.find(list);
        return it == bindings.end() ? Binding{} : it->second;
    }
    void Reset(uint64_t list) {
        std::lock_guard lock(mutex);
        bindings.erase(list);
    }
    void Forget(uint64_t list) {
        Reset(list);
    }
};
} // namespace rrshader
