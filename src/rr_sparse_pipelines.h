#pragma once
#include <reshade.hpp>
#include "feed_fg_identity.h"
#include "rr_shader_versions.h"
#include "rr_lighting_source.h"
#include "rr_sss_vertex_proof.h"
#include "../build/rr_sparse.generated.h"
#include "../build/rr_vrrt_native.generated.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <atomic>
#include <vector>
namespace rrbypass {
inline constexpr uint64_t Temporal = 0x6ef368e46fa21120ull, Spatial = 0xd63e707761558424ull,
                          Vrrt = 0x53958092126ea74aull;
inline void (*logger)(const char *){};
inline uint64_t Hash(const void *data, size_t bytes) {
    uint64_t value = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i)
        value = (value ^ static_cast<const unsigned char *>(data)[i]) * 1099511628211ull;
    return value;
}
inline uint64_t OriginalHash(uint64_t h) {
    return h == RRSpatialPassthroughHash ? Spatial : rrversion::Canonical(h);
}
struct Entry {
    reshade::api::device *device{};
    uint64_t hash{};
    bool replacement{}, used{};
    reshade::api::pipeline alternate{};
    bool bound{};
    ID3D12PipelineState *scale{};
};
inline std::mutex mutex, preparation;
inline std::atomic<bool> global_failed{};
inline void GlobalFail(const char *why) {
    if (!global_failed.exchange(true) && logger)
        logger(why);
}
// Never destroyed: entries hold root signatures, which must not be released at process exit.
inline auto &pipelines = *new std::unordered_map<uint64_t, Entry>;
inline std::unordered_set<reshade::api::device *> failed_devices, corrected_devices;
inline std::unordered_map<reshade::api::device *, uint64_t> unsafe_revision;
struct Status {
    bool temporal{}, spatial{}, sampling{}, corrected{}, unreplaced{}, failed{};
    uint64_t revision{};
};
inline bool Target(uint64_t h) {
    return h == Temporal || h == Spatial || h == Vrrt;
}
inline void Fail(reshade::api::device *d, const char *why) {
    std::lock_guard lock(mutex);
    if (failed_devices.insert(d).second) {
        ++unsafe_revision[d];
        if (logger)
            logger(why);
    }
}
inline Status Health(reshade::api::device *d) {
    std::lock_guard lock(mutex);
    Status s;
    s.failed = global_failed || failed_devices.contains(d);
    s.corrected = corrected_devices.contains(d);
    s.revision = unsafe_revision[d];
    for (auto &[_, e] : pipelines)
        if (e.device == d) {
            s.unreplaced |= !e.replacement;
            if (e.replacement && e.used) {
                s.temporal |= e.hash == Temporal;
                s.spatial |= e.hash == Spatial;
                s.sampling |= e.hash == Vrrt;
            }
        }
    return s;
}
inline bool Ready(reshade::api::device *d) {
    auto s = Health(d);
    return s.temporal && s.spatial && s.sampling && s.corrected && !s.unreplaced && !s.failed;
}
inline void Corrected(reshade::api::device *d) {
    std::lock_guard lock(mutex);
    if (corrected_devices.insert(d).second && logger)
        logger(
            "RR fresh lighting copied and importance weighted; native temporal history retained");
}
inline void Created(reshade::api::device *d, reshade::api::pipeline p, uint64_t actual,
                    reshade::api::pipeline alternate = {}, ID3D12PipelineState *scale = nullptr) {
    std::lock_guard lock(mutex);
    pipelines.erase(p.handle);
    auto h = OriginalHash(actual);
    if (!Target(h))
        return;
    if (pipelines.size() >= 65536) {
        ++unsafe_revision[d];
        failed_devices.insert(d);
        return;
    }
    bool valid = rrversion::NativeTemporal(actual) ||
                 (h == Vrrt && rrversion::SourceBytes(actual) != 0) ||
                 (actual == RRSpatialPassthroughHash && scale);
    pipelines[p.handle] = {d, h, valid, false, alternate, false, scale};
    if (!valid)
        ++unsafe_revision[d];
}
inline void Removed(reshade::api::pipeline p) {
    std::lock_guard lock(mutex);
    pipelines.erase(p.handle);
}
inline void Forget(reshade::api::device *d) {
    std::lock_guard lock(mutex);
    ++unsafe_revision[d];
    failed_devices.erase(d);
    corrected_devices.erase(d);
    for (auto i = pipelines.begin(); i != pipelines.end();)
        if (i->second.device == d)
            i = pipelines.erase(i);
        else
            ++i;
}
inline void Used(reshade::api::device *d, uint64_t pipeline, uint64_t observed = 0) {
    std::lock_guard lock(mutex);
    auto at = pipelines.find(pipeline);
    if (at == pipelines.end() || at->second.device != d) {
        if (Target(observed)) {
            ++unsafe_revision[d];
            failed_devices.insert(d);
        }
        return;
    }
    auto &e = at->second;
    e.used = true;
}
inline bool Create(reshade::api::device *d, reshade::api::pipeline_layout, uint32_t count,
                   const reshade::api::pipeline_subobject *objects) {
    if (!d || d->get_api() != reshade::api::device_api::d3d12 || !objects || count > 64)
        return false;
    reshade::api::shader_desc *candidate = nullptr;
    for (unsigned i = 0; i < count; ++i) {
        using T = reshade::api::pipeline_subobject_type;
        auto &o = objects[i];
        if (o.type == T::compute_shader) {
            if (candidate || o.count != 1 || !o.data)
                return false;
            candidate = (reshade::api::shader_desc *)o.data;
        } else if (o.type == T::pixel_shader || o.type == T::vertex_shader)
            return false;
    }
    if (!candidate || !candidate->code)
        return false;
    auto n = candidate->code_size;
    if (!rrversion::ReplacementSize(n))
        return false;
    const auto actual = Hash(candidate->code, n);
    if (rrversion::SourceBytes(actual) != n)
        return false;
    auto h = rrversion::Canonical(actual);
    if (h == Spatial) {
        candidate->code = RRSpatialPassthrough;
        candidate->code_size = sizeof(RRSpatialPassthrough);
        return true;
    }
    return false; // Temporal is intentionally never replaced.
}
inline constexpr GUID AlternateId = {
    0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x01}};
inline constexpr GUID ScaleId = {
    0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x02}};
inline ID3D12PipelineState *MakeScale(reshade::api::device *d, reshade::api::pipeline_layout root,
                                      reshade::api::pipeline p) {
    auto native = fg::NativeObject<ID3D12Device>((ID3D12Device *)d->get_native());
    if (!native || !root.handle || !p.handle)
        return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = (ID3D12RootSignature *)root.handle;
    desc.CS = {RRVrrtScale, sizeof(RRVrrtScale)};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scale;
    if (FAILED(native->CreateComputePipelineState(&desc, IID_PPV_ARGS(&scale))) ||
        FAILED(((ID3D12PipelineState *)p.handle)->SetPrivateDataInterface(ScaleId, scale.Get())))
        return nullptr;
    return scale.Get();
}
inline void Initialize(reshade::api::device *d, reshade::api::pipeline_layout root, uint32_t count,
                       const reshade::api::pipeline_subobject *objects, reshade::api::pipeline p,
                       uint64_t actual) {
    auto h = OriginalHash(actual);
    if (!Target(h)) {
        Created(d, p, actual);
        return;
    }
    if (h == Temporal) {
        std::lock_guard lock(mutex);
        auto at = pipelines.find(p.handle);
        if (at != pipelines.end() && at->second.hash == Temporal && at->second.device == d)
            return;
    }
    if (rrversion::NativeTemporal(actual)) {
        Created(d, p, actual);
        return;
    }
    if (h == Vrrt && rrversion::SourceBytes(actual)) {
        Created(d, p, actual);
        return;
    }
    {
        std::lock_guard lock(mutex);
        auto at = pipelines.find(p.handle);
        if (at != pipelines.end() && at->second.device == d && at->second.hash == h &&
            at->second.replacement)
            return;
    }
    if (actual == RRSpatialPassthroughHash) {
        auto *scale = h == Spatial ? MakeScale(d, root, p) : nullptr;
        Created(d, p, actual, {}, scale);
        return;
    }
    std::lock_guard prepare(preparation);
    std::array<reshade::api::pipeline_subobject, 64> copy{};
    reshade::api::shader_desc shader{};
    bool valid = root.handle && p.handle && count && count <= 64 && objects;
    if (valid) {
        std::copy_n(objects, count, copy.begin());
        for (unsigned i = 0; i < count; ++i)
            if (copy[i].type == reshade::api::pipeline_subobject_type::compute_shader &&
                copy[i].count == 1 && copy[i].data) {
                shader = *(const reshade::api::shader_desc *)copy[i].data;
                copy[i].data = &shader;
            }
        valid = Create(d, root, count, copy.data());
    }
    reshade::api::pipeline alt{};
    if (valid)
        valid = d->create_pipeline(root, count, copy.data(), &alt) && alt.handle;
    if (valid)
        valid = SUCCEEDED(
            ((ID3D12PipelineState *)p.handle)
                ->SetPrivateDataInterface(AlternateId, (ID3D12PipelineState *)alt.handle));
    auto *scale = valid && h == Spatial ? MakeScale(d, root, p) : nullptr;
    if (valid && h == Spatial && !scale) {
        valid = false;
        ((ID3D12PipelineState *)p.handle)->SetPrivateDataInterface(AlternateId, nullptr);
    }
    if (alt.handle)
        d->destroy_pipeline(alt);
    Created(d, p, valid ? RRSpatialPassthroughHash : actual, valid ? alt : reshade::api::pipeline{},
            scale);
}
inline uint64_t Effective(reshade::api::device *, uint64_t original) {
    return original;
}
inline bool SpatialWriteContract(reshade::api::device *d, uint64_t original, uint64_t effective) {
    std::lock_guard lock(mutex);
    auto at = pipelines.find(original);
    if (at == pipelines.end())
        return false;
    auto &e = at->second;
    return e.device == d && e.hash == Spatial && e.replacement && e.scale && original == effective;
}
inline Microsoft::WRL::ComPtr<ID3D12PipelineState> Scale(reshade::api::device *d,
                                                         uint64_t original) {
    std::lock_guard lock(mutex);
    auto at = pipelines.find(original);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> result;
    if (at != pipelines.end() && at->second.device == d)
        result = at->second.scale;
    return result;
}
inline reshade::api::pipeline Bind(reshade::api::command_list *list,
                                   reshade::api::pipeline_stage stage, reshade::api::pipeline p) {
    using S = reshade::api::pipeline_stage;
    if (stage != S::all && stage != S::all_compute)
        return p;
    std::lock_guard lock(mutex);
    auto at = pipelines.find(p.handle);
    if (at == pipelines.end() || at->second.device != list->get_device() ||
        !at->second.replacement || !at->second.alternate.handle)
        return p;
    list->bind_pipeline(S::all_compute, at->second.alternate);
    at->second.bound = true;
    return at->second.alternate;
}
} // namespace rrbypass
