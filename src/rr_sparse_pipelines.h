#pragma once
#include <reshade.hpp>
#include "feed_fg_identity.h"
#include "rr_shader_versions.h"
#include "rr_lighting_source.h"
#include "rr_sss_vertex_proof.h"
#include "rr_vrrt_apply_shader.h"
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
    // Same preparation per non-raw lighting source, indexed by source - 1.
    std::array<ID3D12PipelineState *, rrlighting::Count - 1> native_lobe{};
    // VRRT apply only, for the ray selection test: its patched copies by rrapply variant, each
    // made the first time it is selected from the game's root signature and remembered input.
    std::array<ID3D12PipelineState *, rrapply::Variants> apply{};
    std::array<bool, rrapply::Variants> apply_failed{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> apply_root;
    int apply_input = -1;
};
// One ray selection copy is made at a time; requests for it wait rather than fall back.
inline std::mutex apply_creation;
// rrlighting::Source applied by the first spatial pass replacement.
inline std::atomic<unsigned> lobe_source{};
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
                    reshade::api::pipeline alternate = {}, ID3D12PipelineState *scale = nullptr,
                    std::array<ID3D12PipelineState *, rrlighting::Count - 1> lobes = {},
                    ID3D12RootSignature *apply_root = nullptr, int apply_input = -1) {
    // Root signatures are released after the lock: their destruction runs ReShade's events.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> released;
    std::lock_guard lock(mutex);
    if (auto at = pipelines.find(p.handle); at != pipelines.end()) {
        released = std::move(at->second.apply_root);
        pipelines.erase(at);
    }
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
    pipelines[p.handle] = {d,     h,     valid, false, alternate,  false,
                           scale, lobes, {},    {},    apply_root, apply_input};
    if (!valid)
        ++unsafe_revision[d];
}
inline void Removed(reshade::api::pipeline p) {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> released;
    std::lock_guard lock(mutex);
    if (auto at = pipelines.find(p.handle); at != pipelines.end()) {
        released = std::move(at->second.apply_root);
        pipelines.erase(at);
    }
}
inline void Forget(reshade::api::device *d) {
    std::vector<Microsoft::WRL::ComPtr<ID3D12RootSignature>> released;
    std::lock_guard lock(mutex);
    ++unsafe_revision[d];
    failed_devices.erase(d);
    corrected_devices.erase(d);
    for (auto i = pipelines.begin(); i != pipelines.end();)
        if (i->second.device == d) {
            if (i->second.apply_root)
                released.push_back(std::move(i->second.apply_root));
            i = pipelines.erase(i);
        } else
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
// The native root has no t2 for the SSS guide, so SSS-masked sources get no
// native-root variant and prepare raw lighting on that path.
inline constexpr unsigned NativeRootSources = rrlighting::SssNative2 - 1;
inline constexpr std::array<GUID, NativeRootSources> NativeLobeScaleId = {
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x03}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x04}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x05}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x06}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x07}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x08}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x09}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x0a}}};
// Native-root bytecode per non-raw, unmasked lighting source, indexed by source - 1.
inline constexpr std::array<D3D12_SHADER_BYTECODE, NativeRootSources> NativeLobeScaleCode = {
    D3D12_SHADER_BYTECODE{RRVrrtScaleNative1, sizeof(RRVrrtScaleNative1)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleNative2, sizeof(RRVrrtScaleNative2)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleFill2, sizeof(RRVrrtScaleFill2)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleFill3, sizeof(RRVrrtScaleFill3)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleCap8, sizeof(RRVrrtScaleCap8)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleCap4, sizeof(RRVrrtScaleCap4)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleWFill, sizeof(RRVrrtScaleWFill)},
    D3D12_SHADER_BYTECODE{RRVrrtScaleWNative, sizeof(RRVrrtScaleWNative)}};
// The native PSO retains each alternate, so the game's PSO lifetime covers it.
inline ID3D12PipelineState *MakeScaleVariant(reshade::api::device *d,
                                             reshade::api::pipeline_layout root,
                                             reshade::api::pipeline p, const GUID &id,
                                             D3D12_SHADER_BYTECODE code) {
    auto native = fg::NativeObject<ID3D12Device>((ID3D12Device *)d->get_native());
    if (!native || !root.handle || !p.handle)
        return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = (ID3D12RootSignature *)root.handle;
    desc.CS = code;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scale;
    if (FAILED(native->CreateComputePipelineState(&desc, IID_PPV_ARGS(&scale))) ||
        FAILED(((ID3D12PipelineState *)p.handle)->SetPrivateDataInterface(id, scale.Get())))
        return nullptr;
    return scale.Get();
}
inline ID3D12PipelineState *MakeScale(reshade::api::device *d, reshade::api::pipeline_layout root,
                                      reshade::api::pipeline p) {
    return MakeScaleVariant(d, root, p, ScaleId, {RRVrrtScale, sizeof(RRVrrtScale)});
}
inline constexpr std::array<GUID, rrapply::Variants> ApplyVariantId = {
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x10}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x11}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x12}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x13}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x14}},
    GUID{0xf1406a29, 0xcadc, 0x4ec7, {0x94, 0xfa, 0xf9, 0xee, 0x49, 0xc2, 0x17, 0x15}}};
// Optional ray selection test: the game's apply shader with rrapply's edits, on the game's
// root signature. Pipeline creation only keeps a copy of a supported input, with no compiler
// work; each variant's pipeline is made the first time it is selected. Any failure leaves that
// variant missing and the native shader in use.
inline int ApplySource(uint32_t count, const reshade::api::pipeline_subobject *objects) noexcept {
    try {
        for (uint32_t i = 0; objects && i < count; ++i)
            if (objects[i].type == reshade::api::pipeline_subobject_type::compute_shader &&
                objects[i].count == 1 && objects[i].data) {
                const auto *shader =
                    static_cast<const reshade::api::shader_desc *>(objects[i].data);
                return shader->code ? rrapply::Shaders().Remember(shader->code, shader->code_size)
                                    : -1;
            }
    } catch (...) {
    }
    return -1;
}
// The copy a game PSO already owns under id, or null; the game's PSO keeps owning it.
inline ID3D12PipelineState *Attached(uint64_t p, const GUID &id) noexcept {
    IUnknown *stored = nullptr;
    UINT size = sizeof(stored);
    if (!p || FAILED(((ID3D12PipelineState *)p)->GetPrivateData(id, &size, &stored)) ||
        size != sizeof(stored) || !stored)
        return nullptr;
    Microsoft::WRL::ComPtr<IUnknown> owned;
    owned.Attach(stored);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    return SUCCEEDED(owned.As(&state)) ? state.Get() : nullptr;
}
inline ID3D12PipelineState *MakeApplyVariant(reshade::api::device *d, ID3D12RootSignature *root,
                                             uint64_t p, int input, unsigned v) noexcept {
    static std::array<std::atomic<bool>, rrapply::Variants> reported{};
    ID3D12PipelineState *result = nullptr;
    try {
        // A repeated init_pipeline for the same game PSO (a pipeline library handing it out
        // again) resets its Entry. Reuse the copy the PSO already owns: attaching a new one
        // would release a pipeline the GPU may still be running.
        if ((result = Attached(p, ApplyVariantId[v])))
            return result;
        rrapply::Prepared prepared;
        if (root && input >= 0)
            prepared = rrapply::Shaders().Get(input, rrsss::ReplayCompiler());
        const char *status = input < 0 ? "unsupported-ray-selection-shader"
                             : !root   ? "no-root-signature"
                                       : prepared.status[v];
        if (const auto &code = prepared.code[v]) {
            result = MakeScaleVariant(d, {reinterpret_cast<uint64_t>(root)}, {p}, ApplyVariantId[v],
                                      {code->data(), code->size()});
            if (!result)
                status = "pipeline-creation-failed";
        }
        rrapply::variant_status[v].store(status, std::memory_order_relaxed);
        if (reported[v].exchange(true) || !logger)
            return result;
        char line[512], hex[65] = "-";
        if (result) {
            rrapply::Sha256Hex(prepared.code[v]->data(), prepared.code[v]->size(), hex);
            snprintf(line, sizeof(line),
                     "Ray selection test: %s shader ready (%zu bytes, SHA-256 %s)",
                     rrapply::Name(v), prepared.code[v]->size(), hex);
        } else {
            const auto &detail = prepared.detail[v];
            snprintf(line, sizeof(line),
                     "Ray selection test: %s unavailable (%s%s%s); native shader kept",
                     rrapply::Name(v), status ? status : "unknown", detail.empty() ? "" : ": ",
                     detail.c_str());
        }
        logger(line);
    } catch (...) {
        // A pipeline already made is owned by the game's, so it is still safe to use.
    }
    return result;
}
// Optional: a missing variant falls back to raw lighting.
inline std::array<ID3D12PipelineState *, rrlighting::Count - 1>
MakeNativeLobeScales(reshade::api::device *d, reshade::api::pipeline_layout root,
                     reshade::api::pipeline p) {
    std::array<ID3D12PipelineState *, rrlighting::Count - 1> result{};
    for (unsigned i = 0; i < NativeLobeScaleCode.size(); ++i)
        result[i] = MakeScaleVariant(d, root, p, NativeLobeScaleId[i], NativeLobeScaleCode[i]);
    return result;
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
        // Only a supported input keeps the root signature, for the test's copies.
        const int input = ApplySource(count, objects);
        Created(d, p, actual, {}, nullptr, {},
                input >= 0 ? reinterpret_cast<ID3D12RootSignature *>(root.handle) : nullptr, input);
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
        Created(d, p, actual, {}, scale,
                scale ? MakeNativeLobeScales(d, root, p)
                      : std::array<ID3D12PipelineState *, rrlighting::Count - 1>{});
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
    std::array<ID3D12PipelineState *, rrlighting::Count - 1> lobes{};
    if (valid && scale)
        lobes = MakeNativeLobeScales(d, root, p);
    if (alt.handle)
        d->destroy_pipeline(alt);
    Created(d, p, valid ? RRSpatialPassthroughHash : actual, valid ? alt : reshade::api::pipeline{},
            scale, lobes);
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
// lobe: an rrlighting::Source. A variant that failed creation, or an SSS-masked
// source, which has no native-root variant, falls back to raw.
inline Microsoft::WRL::ComPtr<ID3D12PipelineState> Scale(reshade::api::device *d,
                                                         uint64_t original, unsigned lobe = 0) {
    std::lock_guard lock(mutex);
    auto at = pipelines.find(original);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> result;
    if (at != pipelines.end() && at->second.device == d) {
        result = at->second.scale;
        if (lobe != rrlighting::Raw && lobe < rrlighting::Count) {
            if (auto *variant = at->second.native_lobe[lobe - 1])
                result = variant;
        }
    }
    return result;
}
// The ray selection test's patched copy of a recognized VRRT apply pipeline, if it was built.
// The ray selection test's copy of a VRRT apply pipeline, or null for the native one. The first
// request for a variant builds the copies (once per input) and creates its pipeline on this
// recording thread, a one-off pause when the test is first switched on.
inline Microsoft::WRL::ComPtr<ID3D12PipelineState> Apply(reshade::api::device *d, uint64_t original,
                                                         unsigned variant) {
    using Microsoft::WRL::ComPtr;
    if (!variant || variant >= rrapply::Variants)
        return {};
    ComPtr<ID3D12RootSignature> root; // Released after every lock below.
    int input = -1;
    auto find = [&]() -> Entry * {
        auto at = pipelines.find(original);
        return at != pipelines.end() && at->second.device == d && at->second.hash == Vrrt &&
                       at->second.replacement
                   ? &at->second
                   : nullptr;
    };
    {
        std::lock_guard lock(mutex);
        const auto *e = find();
        if (!e || e->apply_failed[variant])
            return {};
        if (e->apply[variant])
            return e->apply[variant];
    }
    std::lock_guard creation(apply_creation);
    {
        std::lock_guard lock(mutex);
        const auto *e = find();
        if (!e || e->apply_failed[variant])
            return {};
        if (e->apply[variant])
            return e->apply[variant];
        root = e->apply_root;
        input = e->apply_input;
    }
    auto *made = MakeApplyVariant(d, root.Get(), original, input, variant);
    std::lock_guard lock(mutex);
    auto *e = find();
    if (!e)
        return {};
    e->apply[variant] = made;
    e->apply_failed[variant] = !made;
    return made;
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
