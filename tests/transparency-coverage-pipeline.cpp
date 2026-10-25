#include "../src/rr_transparency_coverage_pipeline.h"
#include "../src/rr_pipeline_state.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace rrblend;
namespace api = reshade::api;
using T = api::pipeline_subobject_type;

static const Target Oit{0xa36d02f6245a4129ull, 11856, Kind::Oit};
static const Target Refraction{0xb72393e05df90a78ull, 4812, Kind::Refraction};

static void TestTargetHash() {
    // The registry hashes pixel shaders exactly as OnPipeline's table does.
    std::vector<unsigned char> bytes(4812);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(i * 37 + 11);
    assert(rrsss::PipelineProof::ByteHash(bytes.data(), bytes.size()) ==
           rrshader::Hash(bytes.data(), bytes.size()));
    // FNV-1a64 of "a".
    assert(rrsss::PipelineProof::ByteHash("a", 1) == 0xaf63dc4c8601ec8cull);

    api::shader_desc ps{bytes.data(), bytes.size()};
    api::pipeline_subobject objects[] = {{T::pixel_shader, 1, &ps}};
    uint64_t hash = 1;
    // Right size, wrong bytes: not a target, and the hash is left alone.
    assert(!PixelTarget(1, objects, hash) && hash == 1);
    ps.code_size = 4811;
    assert(!PixelTarget(1, objects, hash));
    ps.code = nullptr;
    ps.code_size = 4812;
    assert(!PixelTarget(1, objects, hash));
    assert(!PixelTarget(0, objects, hash));
    assert(!PixelTarget(1, nullptr, hash));
    assert(!PixelTarget(65, objects, hash));

    // A table whose entry is these bytes: PixelTarget returns that entry and its hash.
    const auto h = rrsss::PipelineProof::ByteHash(bytes.data(), bytes.size());
    const Target table[] = {{0x0123456789abcdefull, 100, Kind::Oit},
                            {h, 4812, Kind::Refraction},
                            {h ^ 1, 4812, Kind::Oit}};
    ps = {bytes.data(), bytes.size()};
    hash = 1;
    assert(PixelTarget(1, objects, hash, table) == &table[1] && hash == h);
    // Only the pixel shader is matched, wherever it is among the subobjects.
    api::shader_desc vs{bytes.data(), bytes.size()}, other{bytes.data(), 100};
    api::pipeline_subobject many[] = {{T::vertex_shader, 1, &vs},
                                      {T::compute_shader, 1, &vs},
                                      {T::pixel_shader, 1, &ps},
                                      {T::sample_mask, 0, nullptr}};
    hash = 1;
    assert(PixelTarget(4, many, hash, table) == &table[1] && hash == h);
    many[2].data = &other;
    hash = 1;
    assert(!PixelTarget(4, many, hash, table) && hash == 1);
    // The sizes come from the given table too, not from the shipped one.
    const auto small_hash = rrsss::PipelineProof::ByteHash(bytes.data(), 100);
    const Target one_target[] = {{small_hash, 100, Kind::Oit}};
    assert(PixelTarget(4, many, hash, one_target) == &one_target[0] && hash == small_hash);
    hash = 1;
    // Right bytes, wrong size: the prefix of the target is not the target.
    ps.code_size = 4811;
    hash = 1;
    assert(!PixelTarget(1, objects, hash, table) && hash == 1);
    // Right hash, wrong size: the hash and the size must match the same entry.
    const auto prefix = rrsss::PipelineProof::ByteHash(bytes.data(), 4811);
    const Target sized[] = {{prefix, 4812, Kind::Oit}, {0x42, 4811, Kind::Refraction}};
    hash = 1;
    assert(!PixelTarget(1, objects, hash, sized) && hash == 1);
    ps.code_size = 4812;
    assert(!PixelTarget(1, objects, hash, sized) && hash == 1);
    // An empty table matches nothing.
    assert(!PixelTarget(1, objects, hash, std::span<const Target>{}) && hash == 1);
}

static void TestFactors() {
    using F = api::blend_factor;
    const std::pair<D3D12_BLEND, F> pairs[] = {
        {D3D12_BLEND_ZERO, F::zero},
        {D3D12_BLEND_ONE, F::one},
        {D3D12_BLEND_SRC_COLOR, F::source_color},
        {D3D12_BLEND_INV_SRC_COLOR, F::one_minus_source_color},
        {D3D12_BLEND_SRC_ALPHA, F::source_alpha},
        {D3D12_BLEND_INV_SRC_ALPHA, F::one_minus_source_alpha},
        {D3D12_BLEND_SRC1_COLOR, F::source1_color},
        {D3D12_BLEND_INV_SRC1_COLOR, F::one_minus_source1_color},
        {D3D12_BLEND_SRC1_ALPHA, F::source1_alpha},
        {D3D12_BLEND_INV_SRC1_ALPHA, F::one_minus_source1_alpha}};
    for (const auto &[d3d, expected] : pairs) {
        F out = F::constant_color;
        assert(ReShadeFactor(d3d, out) && out == expected);
    }
    // The variant never uses the others.
    for (auto d3d : {D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_COLOR,
                     D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA_SAT,
                     D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR,
                     D3D12_BLEND_ALPHA_FACTOR, D3D12_BLEND_INV_ALPHA_FACTOR, D3D12_BLEND(0)}) {
        F out = F::constant_color;
        assert(!ReShadeFactor(d3d, out));
    }
}

// The native blend as D3D12 holds it (observation) and as ReShade reports it
// (all eight slots copied from RT0, since IndependentBlendEnable is FALSE).
static D3D12_BLEND_DESC NativeBlend(D3D12_BLEND src, D3D12_BLEND dest, UINT8 mask = 15) {
    D3D12_BLEND_DESC d{};
    for (auto &rt : d.RenderTarget) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = src;
        rt.DestBlend = dest;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = mask;
    }
    return d;
}
static api::blend_desc ReShadeNative(api::blend_factor src, api::blend_factor dest,
                                     uint8_t mask = 15) {
    api::blend_desc d{};
    for (int i = 0; i < 8; ++i) {
        d.blend_enable[i] = true;
        d.source_color_blend_factor[i] = src;
        d.dest_color_blend_factor[i] = dest;
        d.source_alpha_blend_factor[i] = api::blend_factor::zero;
        d.dest_alpha_blend_factor[i] = api::blend_factor::one;
        d.render_target_write_mask[i] = mask;
    }
    d.blend_constant[0] = 0.25f;
    return d;
}

// A native pipeline's creation subobjects as ReShade's D3D12 layer reports them.
struct Native {
    std::vector<unsigned char> vs = std::vector<unsigned char>(96, 0x5a), ps;
    api::shader_desc vs_desc{}, ps_desc{}, none{};
    api::blend_desc blend{};
    api::rasterizer_desc raster{};
    api::depth_stencil_desc depth{};
    api::format dsv = api::format::unknown, rt = api::format::r11g11b10_float;
    uint32_t samples = 4, mask = ~0u, viewports = 1;
    api::primitive_topology topology = api::primitive_topology::triangle_list;
    api::stream_output_desc so{};
    api::dynamic_state dynamic[2] = {api::dynamic_state::primitive_topology,
                                     api::dynamic_state::blend_constant};
    api::pipeline_flags flags = api::pipeline_flags::none;
    std::vector<api::pipeline_subobject> objects;
    Native(const Target &target, const api::blend_desc &b) : ps(target.bytes, 0xa5), blend(b) {
        vs_desc = {vs.data(), vs.size()};
        ps_desc = {ps.data(), ps.size()};
        raster.cull_mode = api::cull_mode::none;
        depth.depth_enable = false;
        depth.depth_write_mask = false;
        objects = {{T::vertex_shader, 1, &vs_desc},
                   {T::hull_shader, 1, &none},
                   {T::domain_shader, 1, &none},
                   {T::geometry_shader, 1, &none},
                   {T::pixel_shader, 1, &ps_desc},
                   {T::stream_output_state, 1, &so},
                   {T::blend_state, 1, &blend},
                   {T::sample_mask, 1, &mask},
                   {T::rasterizer_state, 1, &raster},
                   {T::depth_stencil_state, 1, &depth},
                   {T::input_layout, 0, nullptr},
                   {T::primitive_topology, 1, &topology},
                   {T::render_target_formats, 1, &rt},
                   {T::depth_stencil_format, 1, &dsv},
                   {T::sample_count, 1, &samples},
                   {T::viewport_count, 1, &viewports},
                   {T::dynamic_pipeline_states, 2, dynamic},
                   {T::flags, 1, &flags}};
    }
    Native(const Native &) = delete;
    unsigned Count() const {
        return unsigned(objects.size());
    }
    const api::pipeline_subobject *Find(const CoveragePatch &patch, T type) const {
        for (unsigned i = 0; i < patch.count; ++i)
            if (patch.objects[i].type == type)
                return &patch.objects[i];
        return nullptr;
    }
};

// Every subobject is kept in place and unchanged except the blend state, the
// render target formats and (without a DSV) the DSV format.
static void KeptOthers(const Native &n, const CoveragePatch &patch, bool dsv_kept) {
    assert(patch.count == n.Count());
    for (unsigned i = 0; i < patch.count; ++i) {
        const auto &o = patch.objects[i];
        assert(o.type == n.objects[i].type);
        if (o.type == T::blend_state || o.type == T::render_target_formats ||
            (o.type == T::depth_stencil_format && !dsv_kept))
            continue;
        assert(o.count == n.objects[i].count && o.data == n.objects[i].data);
    }
    const auto *ps = static_cast<const api::shader_desc *>(n.Find(patch, T::pixel_shader)->data);
    assert(ps->code == n.ps.data() && ps->code_size == n.ps.size());
    const auto *formats = n.Find(patch, T::render_target_formats);
    assert(formats->count == 1 &&
           *static_cast<const api::format *>(formats->data) == api::format::r10g10b10a2_unorm);
    const auto *dsv = n.Find(patch, T::depth_stencil_format);
    assert(dsv->count == 1);
    if (dsv_kept)
        assert(dsv->data == &n.dsv);
    else
        assert(*static_cast<const api::format *>(dsv->data) == api::format::unknown);
    assert(n.Find(patch, T::blend_state)->data == &patch.blend);
    assert(patch.vertex == &n.vs_desc && patch.pixel_shader == &n.ps_desc);
}

static void DisabledSlots(const api::blend_desc &b) {
    for (int i = 1; i < 8; ++i) {
        assert(!b.blend_enable[i] && !b.logic_op_enable[i]);
        assert(b.source_color_blend_factor[i] == api::blend_factor::one &&
               b.dest_color_blend_factor[i] == api::blend_factor::zero);
        assert(b.source_alpha_blend_factor[i] == api::blend_factor::one &&
               b.dest_alpha_blend_factor[i] == api::blend_factor::zero);
        assert(b.color_blend_op[i] == api::blend_op::add &&
               b.alpha_blend_op[i] == api::blend_op::add);
        assert(b.render_target_write_mask[i] == 0);
    }
}

static void TestPatchOit() {
    // a36d: dual source, scene' = T0 + scene * (1 - T1), into one RT, no depth.
    Native n(Oit,
             ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    n.dsv = api::format::d32_float; // depth and stencil are off, so the DSV goes
    const auto native = NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR);
    CoveragePatch patch;
    assert(patch.Prepare(n.Count(), n.objects.data(), Oit, native, DepthClass::NoDsv));
    KeptOthers(n, patch, false);
    const auto &b = patch.blend;
    assert(!b.alpha_to_coverage_enable);
    assert(b.blend_enable[0] && !b.logic_op_enable[0]);
    assert(b.source_color_blend_factor[0] == api::blend_factor::zero);
    assert(b.dest_color_blend_factor[0] == api::blend_factor::one_minus_source1_color);
    assert(b.color_blend_op[0] == api::blend_op::add);
    assert(b.source_alpha_blend_factor[0] == api::blend_factor::zero);
    assert(b.dest_alpha_blend_factor[0] == api::blend_factor::one);
    assert(b.alpha_blend_op[0] == api::blend_op::add);
    assert(b.logic_op[0] == api::logic_op::noop);
    assert(b.render_target_write_mask[0] == 7);
    assert(b.blend_constant[0] == 0.25f);
    DisabledSlots(b);
    // The source subobjects are never written.
    assert(n.dsv == api::format::d32_float && n.rt == api::format::r11g11b10_float);
    assert(n.blend.dest_alpha_blend_factor[0] == api::blend_factor::one &&
           n.blend.source_color_blend_factor[0] == api::blend_factor::one);
}

static void TestPatchRefraction() {
    // b723: ONE / INV_SRC_ALPHA, write mask 15. Kept DSV: depth tested, never written.
    Native n(Refraction,
             ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source_alpha));
    n.depth.depth_enable = true;
    n.dsv = api::format::d32_float;
    const auto native = NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA);
    CoveragePatch patch;
    assert(patch.Prepare(n.Count(), n.objects.data(), Refraction, native, DepthClass::KeepDsv));
    KeptOthers(n, patch, true);
    const auto &b = patch.blend;
    assert(b.blend_enable[0] && !b.logic_op_enable[0] && !b.alpha_to_coverage_enable);
    assert(b.source_color_blend_factor[0] == api::blend_factor::zero);
    assert(b.dest_color_blend_factor[0] == api::blend_factor::one_minus_source_alpha);
    assert(b.source_alpha_blend_factor[0] == api::blend_factor::zero);
    // The refraction form carries 1 - opacity in alpha too.
    assert(b.dest_alpha_blend_factor[0] == api::blend_factor::one_minus_source_alpha);
    assert(b.render_target_write_mask[0] == 15);
    DisabledSlots(b);
    // The same native as an OIT target would leave alpha alone.
    CoveragePatch oit;
    Native same(Refraction,
                ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source_alpha));
    const Target as_oit{Refraction.hash, Refraction.bytes, Kind::Oit};
    assert(oit.Prepare(same.Count(), same.objects.data(), as_oit, native, DepthClass::NoDsv));
    assert(oit.blend.dest_alpha_blend_factor[0] == api::blend_factor::one);
    assert(oit.blend.render_target_write_mask[0] == 7);
    // A colour-type refraction dest factor has no single-channel transmittance.
    Native colour(Refraction,
                  ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source_color));
    CoveragePatch c;
    assert(c.Prepare(colour.Count(), colour.objects.data(), Refraction,
                     NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_COLOR), DepthClass::NoDsv));
    assert(c.blend.dest_color_blend_factor[0] == api::blend_factor::one_minus_source_color);
    assert(c.blend.dest_alpha_blend_factor[0] == api::blend_factor::one);
    assert(c.blend.render_target_write_mask[0] == 7);
}

static void TestPatchDsvAbsent() {
    // Without a DSV format subobject the variant has none either; a kept DSV
    // needs the native format, so it is refused.
    Native n(Oit,
             ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    const auto native = NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR);
    n.objects.erase(n.objects.begin() + 13);
    assert(n.objects[12].type == T::render_target_formats);
    CoveragePatch patch;
    assert(patch.Prepare(n.Count(), n.objects.data(), Oit, native, DepthClass::NoDsv));
    for (unsigned i = 0; i < patch.count; ++i)
        assert(patch.objects[i].type != T::depth_stencil_format);
    CoveragePatch kept;
    assert(!kept.Prepare(n.Count(), n.objects.data(), Oit, native, DepthClass::KeepDsv));
    assert(!strcmp(kept.status, "kept-dsv-format-unknown") && kept.count == 0);
    Native unknown(
        Oit, ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    assert(
        !kept.Prepare(unknown.Count(), unknown.objects.data(), Oit, native, DepthClass::KeepDsv));
    assert(!strcmp(kept.status, "kept-dsv-format-unknown"));
}

// Prepare refuses with this status and leaves no subobjects to create from.
template <class Edit> static void Refused(const char *status, Edit &&edit) {
    Native n(Oit,
             ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    edit(n);
    CoveragePatch patch;
    const bool ok =
        patch.Prepare(n.Count(), n.objects.data(), Oit,
                      NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR), DepthClass::NoDsv);
    if (ok || strcmp(patch.status, status))
        printf("expected %s, got %s (%d)\n", status, patch.status, int(ok));
    assert(!ok && !strcmp(patch.status, status) && patch.count == 0);
}
static void Replace(Native &n, T type, api::pipeline_subobject with) {
    for (auto &o : n.objects)
        if (o.type == type)
            o = with;
}
static void Remove(Native &n, T type) {
    for (auto i = n.objects.begin(); i != n.objects.end(); ++i)
        if (i->type == type) {
            n.objects.erase(i);
            return;
        }
}

static void TestPatchRefusals() {
    // Baseline accepted, so each refusal below comes from its one change.
    {
        Native n(Oit,
                 ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
        CoveragePatch patch;
        assert(patch.Prepare(n.Count(), n.objects.data(), Oit,
                             NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR),
                             DepthClass::NoDsv));
        assert(!strcmp(patch.status, "prepared"));
        assert(!patch.Prepare(n.Count(), n.objects.data(), Oit,
                              NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR),
                              DepthClass::Refuse));
        assert(!strcmp(patch.status, "depth-stencil-not-replayable") && patch.count == 0);
        assert(!patch.Prepare(0, n.objects.data(), Oit, {}, DepthClass::NoDsv));
        assert(!patch.Prepare(n.Count(), nullptr, Oit, {}, DepthClass::NoDsv));
        assert(!patch.Prepare(65, n.objects.data(), Oit, {}, DepthClass::NoDsv));
        assert(!strcmp(patch.status, "subobject-count"));
    }
    Refused("subobject-duplicate-or-null",
            [](Native &n) { n.objects.push_back({T::sample_mask, 1, &n.mask}); });
    Refused("subobject-duplicate-or-null",
            [](Native &n) { Replace(n, T::rasterizer_state, {T::rasterizer_state, 1, nullptr}); });
    Refused("vertex-shader", [](Native &n) { n.vs_desc.code_size = 0; });
    Refused("vertex-shader",
            [](Native &n) { Replace(n, T::vertex_shader, {T::vertex_shader, 2, &n.vs_desc}); });
    Refused("pixel-shader", [](Native &n) { n.ps_desc.code_size = 11855; });
    Refused("pixel-shader", [](Native &n) { n.ps_desc.code = nullptr; });
    Refused("blend-state",
            [](Native &n) { Replace(n, T::blend_state, {T::blend_state, 2, &n.blend}); });
    Refused("render-target-count", [](Native &n) {
        Replace(n, T::render_target_formats, {T::render_target_formats, 2, &n.rt});
    });
    Refused("sample-count", [](Native &n) { n.samples = 1; });
    Refused("sample-mask",
            [](Native &n) { Replace(n, T::sample_mask, {T::sample_mask, 2, &n.mask}); });
    // A partial sample mask is mirrored, not refused.
    for (uint32_t mask : {0xffffu, 0x5u}) {
        Native n(Oit,
                 ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
        n.mask = mask;
        CoveragePatch patch;
        assert(patch.Prepare(n.Count(), n.objects.data(), Oit,
                             NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR),
                             DepthClass::NoDsv));
        const auto *kept = n.Find(patch, T::sample_mask);
        assert(kept->count == 1 && kept->data == &n.mask);
    }
    Refused("primitive-topology",
            [](Native &n) { n.topology = api::primitive_topology::triangle_strip; });
    Refused("extra-shader-stage",
            [](Native &n) { Replace(n, T::geometry_shader, {T::geometry_shader, 1, &n.vs_desc}); });
    Refused("extra-shader-stage",
            [](Native &n) { n.objects.push_back({T::compute_shader, 1, &n.vs_desc}); });
    Refused("stream-output", [](Native &n) { n.so.rasterized_stream = 1; });
    Refused("viewport-count", [](Native &n) { n.viewports = 2; });
    Refused("dynamic-state", [](Native &n) { n.dynamic[1] = api::dynamic_state::blend_enable; });
    Refused("pipeline-flags", [](Native &n) { n.flags = api::pipeline_flags::library; });
    Refused("unsupported-subobject",
            [](Native &n) { n.objects.push_back({T::amplification_shader, 1, &n.vs_desc}); });
    Refused("unsupported-subobject",
            [](Native &n) { n.objects.push_back({T::max_vertex_count, 1, &n.samples}); });
    for (auto type : {T::vertex_shader, T::pixel_shader, T::blend_state, T::rasterizer_state,
                      T::depth_stencil_state, T::render_target_formats, T::sample_count,
                      T::sample_mask, T::primitive_topology})
        Refused("required-subobject-missing", [type](Native &n) { Remove(n, type); });
    // Optional subobjects may be absent or empty.
    for (auto type : {T::hull_shader, T::stream_output_state, T::input_layout, T::viewport_count,
                      T::dynamic_pipeline_states, T::flags, T::depth_stencil_format}) {
        Native n(Oit,
                 ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
        Remove(n, type);
        n.objects.push_back({T::mesh_shader, 0, nullptr});
        CoveragePatch patch;
        assert(patch.Prepare(n.Count(), n.objects.data(), Oit,
                             NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR),
                             DepthClass::NoDsv));
    }
    // The unsupported subobject's type is reported.
    {
        Native n(Oit,
                 ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
        n.objects.push_back({T::libraries, 1, &n.samples});
        CoveragePatch patch;
        assert(!patch.Prepare(n.Count(), n.objects.data(), Oit,
                              NativeBlend(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR),
                              DepthClass::NoDsv));
        assert(patch.problem == unsigned(T::libraries));
    }
}

// The native PSO desc ReShade received: one R11G11B10_FLOAT RT, 4x standard pattern.
struct NativeDesc {
    unsigned char vs[96]{}, ps[128]{}, cached[16]{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    NativeDesc(const D3D12_BLEND_DESC &blend, DXGI_FORMAT dsv, bool depth_test, UINT mask = ~0u) {
        for (size_t i = 0; i < sizeof(ps); ++i)
            ps[i] = static_cast<unsigned char>(i * 7 + 3);
        vs[0] = 0x44;
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature *>(uintptr_t(0x1000));
        desc.VS = {vs, sizeof(vs)};
        desc.PS = {ps, sizeof(ps)};
        desc.BlendState = blend;
        desc.SampleMask = mask;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = depth_test;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
        desc.DSVFormat = dsv;
        desc.SampleDesc = {4, D3D12_STANDARD_MULTISAMPLE_PATTERN};
        desc.CachedPSO = {cached, sizeof(cached)};
    }
};

static D3D12_BLEND D3d(api::blend_factor f) {
    using F = api::blend_factor;
    switch (f) {
    case F::zero:
        return D3D12_BLEND_ZERO;
    case F::one:
        return D3D12_BLEND_ONE;
    case F::source_color:
        return D3D12_BLEND_SRC_COLOR;
    case F::one_minus_source_color:
        return D3D12_BLEND_INV_SRC_COLOR;
    case F::source_alpha:
        return D3D12_BLEND_SRC_ALPHA;
    case F::one_minus_source_alpha:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case F::source1_color:
        return D3D12_BLEND_SRC1_COLOR;
    case F::one_minus_source1_color:
        return D3D12_BLEND_INV_SRC1_COLOR;
    case F::source1_alpha:
        return D3D12_BLEND_SRC1_ALPHA;
    case F::one_minus_source1_alpha:
        return D3D12_BLEND_INV_SRC1_ALPHA;
    default:
        assert(!"unexpected ReShade blend factor");
        return D3D12_BLEND(0);
    }
}
// The variant's legacy desc as ReShade creates it from the patched subobjects
// and as ApplyQuality forwards it: native sample desc, no cached PSO.
static D3D12_GRAPHICS_PIPELINE_STATE_DESC Created(const NativeDesc &native,
                                                  const CoveragePatch &patch, BOOL independent) {
    auto d = native.desc;
    d.CachedPSO = {};
    d.BlendState = {};
    d.BlendState.AlphaToCoverageEnable = patch.blend.alpha_to_coverage_enable;
    d.BlendState.IndependentBlendEnable = independent;
    for (int i = 0; i < 8; ++i) {
        auto &rt = d.BlendState.RenderTarget[i];
        const auto &b = patch.blend;
        assert(b.color_blend_op[i] == api::blend_op::add &&
               b.alpha_blend_op[i] == api::blend_op::add && b.logic_op[i] == api::logic_op::noop);
        rt.BlendEnable = b.blend_enable[i];
        rt.LogicOpEnable = b.logic_op_enable[i];
        rt.SrcBlend = D3d(b.source_color_blend_factor[i]);
        rt.DestBlend = D3d(b.dest_color_blend_factor[i]);
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3d(b.source_alpha_blend_factor[i]);
        rt.DestBlendAlpha = D3d(b.dest_alpha_blend_factor[i]);
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = b.render_target_write_mask[i];
    }
    d.NumRenderTargets = 0;
    for (unsigned i = 0; i < patch.count; ++i) {
        const auto &o = patch.objects[i];
        if (o.type == T::render_target_formats) {
            d.NumRenderTargets = o.count;
            for (UINT j = 0; j < 8; ++j)
                d.RTVFormats[j] = j < o.count
                                      ? DXGI_FORMAT(static_cast<const api::format *>(o.data)[j])
                                      : DXGI_FORMAT_UNKNOWN;
        }
        if (o.type == T::depth_stencil_format)
            d.DSVFormat = DXGI_FORMAT(*static_cast<const api::format *>(o.data));
        if (o.type == T::sample_mask)
            d.SampleMask = *static_cast<const uint32_t *>(o.data);
    }
    return d;
}

static void TestExpected(const Target &target, D3D12_BLEND src, D3D12_BLEND dest,
                         api::blend_factor rs_src, api::blend_factor rs_dest, DepthClass depth,
                         UINT mask = ~0u) {
    using rrsss::PipelineProof;
    const auto blend = NativeBlend(src, dest);
    const NativeDesc native(blend, DXGI_FORMAT_D32_FLOAT, depth == DepthClass::KeepDsv, mask);
    const auto observed = PipelineProof::ObserveLegacy(&native.desc);
    assert(Classify(observed.blend, observed.rt_count) == BlendClass::Accept);
    assert(DepthStencilReplayable(observed.depth.desc) == depth);
    const auto e = Expected(observed, target.kind, depth);
    // Only the target, its blend, the cached PSO and (no DSV) the DSV format change.
    for (unsigned i = 0; i < 8; ++i)
        assert(e.shaders[i].bytes == observed.shaders[i].bytes &&
               e.shaders[i].code == observed.shaders[i].code &&
               e.shaders[i].hash == observed.shaders[i].hash);
    assert(e.shaders[1].hash == PipelineProof::ByteHash(native.ps, sizeof(native.ps)));
    assert(!e.cached_present && !e.cached_bytes);
    assert(observed.cached_present && observed.cached_bytes == 16);
    assert(e.rt_count == 1 && e.rt_formats[0] == DXGI_FORMAT_R10G10B10A2_UNORM);
    for (unsigned i = 1; i < 8; ++i)
        assert(e.rt_formats[i] == DXGI_FORMAT_UNKNOWN);
    const auto patched = PatchBlend(blend, target.kind);
    assert(!memcmp(&e.blend, &patched, sizeof(patched)));
    assert(e.blend.IndependentBlendEnable == FALSE);
    assert(e.depth.format ==
           (depth == DepthClass::NoDsv ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_D32_FLOAT));
    assert(!memcmp(&e.depth.desc, &observed.depth.desc, sizeof(e.depth.desc)));
    assert(e.sample.Count == 4 && e.sample.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN);
    assert(e.sample_mask == mask && observed.sample_mask == mask);
    assert(e.root_identity == observed.root_identity && e.input_hash == observed.input_hash);
    assert(!memcmp(&e.raster, &observed.raster, sizeof(e.raster)));
    // The variant created from the patched subobjects is Equivalent to it,
    // whichever IndependentBlendEnable ReShade picks for one RT.
    Native n(target, ReShadeNative(rs_src, rs_dest));
    n.mask = mask;
    if (depth == DepthClass::KeepDsv)
        n.dsv = api::format::d32_float;
    CoveragePatch patch;
    assert(patch.Prepare(n.Count(), n.objects.data(), target, blend, depth));
    for (BOOL independent : {FALSE, TRUE}) {
        const auto created = Created(native, patch, independent);
        const auto actual = PipelineProof::ObserveLegacy(&created);
        assert(PipelineProof::Equivalent(e, actual));
        // Any other variant state is a mismatch.
        auto other = created;
        for (auto &rt : other.BlendState.RenderTarget)
            rt.DestBlend = dest == D3D12_BLEND_INV_SRC_COLOR ? D3D12_BLEND_SRC_COLOR
                                                             : D3D12_BLEND_INV_SRC_COLOR;
        assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&other)));
        other = created;
        other.BlendState.RenderTarget[0].DestBlendAlpha =
            other.BlendState.RenderTarget[0].DestBlendAlpha == D3D12_BLEND_ONE
                ? D3D12_BLEND_INV_SRC_ALPHA
                : D3D12_BLEND_ONE;
        assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&other)));
        other = created;
        other.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
        assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&other)));
        other = created;
        other.DSVFormat = depth == DepthClass::NoDsv ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&other)));
        other = created;
        other.SampleDesc.Quality = 0;
        assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&other)));
        other = created;
        other.SampleMask = mask ^ 1;
        assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&other)));
    }
    // The native PSO itself is not the variant.
    auto uncached = native.desc;
    uncached.CachedPSO = {};
    assert(!PipelineProof::Equivalent(e, PipelineProof::ObserveLegacy(&uncached)));
}

// Fake device: only CheckFeatureSupport (ID3D12Device slot 13) is called.
struct FakeDevice {
    void **vtable{};
    std::array<void *, 14> table{};
    UINT support1 = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE |
                    D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
    UINT levels = 1;
    HRESULT format_result = S_OK, quality_result = S_OK;
    std::vector<std::pair<D3D12_FEATURE, DXGI_FORMAT>> queries;
    UINT samples = 0;
    static HRESULT STDMETHODCALLTYPE Check(FakeDevice *self, D3D12_FEATURE feature, void *data,
                                           UINT size) {
        if (feature == D3D12_FEATURE_FORMAT_SUPPORT) {
            assert(size == sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT));
            auto *d = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(data);
            self->queries.push_back({feature, d->Format});
            d->Support1 = D3D12_FORMAT_SUPPORT1(self->support1);
            d->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
            return self->format_result;
        }
        assert(feature == D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS);
        assert(size == sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS));
        auto *d = static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(data);
        self->queries.push_back({feature, d->Format});
        self->samples = d->SampleCount;
        d->NumQualityLevels = self->levels;
        return self->quality_result;
    }
    static void Trap() {
        assert(!"only CheckFeatureSupport is called");
    }
    FakeDevice() {
        table.fill(reinterpret_cast<void *>(&Trap));
        table[13] = reinterpret_cast<void *>(&Check);
        vtable = table.data();
    }
    ID3D12Device *Device() {
        return reinterpret_cast<ID3D12Device *>(this);
    }
};

static void TestFormatSupport() {
    const char *status = nullptr;
    {
        FakeDevice d;
        assert(CoverageFormatSupported(d.Device(), status));
        assert(d.queries.size() == 2 && d.samples == 4);
        for (const auto &q : d.queries)
            assert(q.second == DXGI_FORMAT_R10G10B10A2_UNORM);
    }
    for (UINT missing :
         {UINT(D3D12_FORMAT_SUPPORT1_RENDER_TARGET), UINT(D3D12_FORMAT_SUPPORT1_BLENDABLE),
          UINT(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET)}) {
        FakeDevice d;
        d.support1 &= ~missing;
        assert(!CoverageFormatSupported(d.Device(), status));
        assert(!strcmp(status, "coverage-format-unsupported"));
    }
    FakeDevice none;
    none.levels = 0;
    assert(!CoverageFormatSupported(none.Device(), status));
    assert(!strcmp(status, "coverage-four-sample-target-unavailable"));
    FakeDevice failed;
    failed.format_result = E_FAIL;
    assert(!CoverageFormatSupported(failed.Device(), status));
    assert(!strcmp(status, "coverage-format-query-failed"));
    FakeDevice quality;
    quality.quality_result = E_FAIL;
    assert(!CoverageFormatSupported(quality.Device(), status));
    assert(!strcmp(status, "coverage-format-query-failed"));
    assert(!CoverageFormatSupported(nullptr, status));
}

// Fake reflection of a pixel shader: only what PixelReplayGate reads.
struct FakeReflection final : ID3D12ShaderReflection {
    UINT version = UINT(D3D12_SHVER_PIXEL_SHADER) << 16 | 0x61; // ps_6_1
    UINT64 flags = 0;
    std::vector<D3D_SHADER_INPUT_TYPE> bindings{D3D_SIT_CBUFFER, D3D_SIT_SAMPLER, D3D_SIT_TEXTURE};
    std::vector<D3D_NAME> outputs{D3D_NAME_TARGET};
    UINT bound = ~0u; // BoundResources when not bindings.size()
    HRESULT desc_result = S_OK, binding_result = S_OK, output_result = S_OK;
    unsigned binding_reads = 0, output_reads = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) noexcept override {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return 2;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3D12_SHADER_DESC *desc) noexcept override {
        *desc = {};
        desc->Version = version;
        desc->BoundResources = bound != ~0u ? bound : UINT(bindings.size());
        desc->OutputParameters = UINT(outputs.size());
        return desc_result;
    }
    ID3D12ShaderReflectionConstantBuffer *STDMETHODCALLTYPE
    GetConstantBufferByIndex(UINT) noexcept override {
        return nullptr;
    }
    ID3D12ShaderReflectionConstantBuffer *STDMETHODCALLTYPE
    GetConstantBufferByName(LPCSTR) noexcept override {
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE
    GetResourceBindingDesc(UINT i, D3D12_SHADER_INPUT_BIND_DESC *desc) noexcept override {
        assert(i < bindings.size());
        ++binding_reads;
        *desc = {};
        desc->Name = "binding";
        desc->Type = bindings[i];
        desc->BindCount = 1;
        return binding_result;
    }
    HRESULT STDMETHODCALLTYPE
    GetInputParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC *) noexcept override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE
    GetOutputParameterDesc(UINT i, D3D12_SIGNATURE_PARAMETER_DESC *desc) noexcept override {
        assert(i < outputs.size());
        ++output_reads;
        *desc = {};
        desc->SemanticName = "SV_Target";
        desc->SemanticIndex = i;
        desc->Register = i;
        desc->SystemValueType = outputs[i];
        return output_result;
    }
    HRESULT STDMETHODCALLTYPE
    GetPatchConstantParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC *) noexcept override {
        return E_NOTIMPL;
    }
    ID3D12ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByName(LPCSTR) noexcept override {
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE
    GetResourceBindingDescByName(LPCSTR, D3D12_SHADER_INPUT_BIND_DESC *) noexcept override {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetMovInstructionCount() noexcept override {
        return 0;
    }
    UINT STDMETHODCALLTYPE GetMovcInstructionCount() noexcept override {
        return 0;
    }
    UINT STDMETHODCALLTYPE GetConversionInstructionCount() noexcept override {
        return 0;
    }
    UINT STDMETHODCALLTYPE GetBitwiseInstructionCount() noexcept override {
        return 0;
    }
    D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive() noexcept override {
        return D3D_PRIMITIVE_UNDEFINED;
    }
    BOOL STDMETHODCALLTYPE IsSampleFrequencyShader() noexcept override {
        return FALSE;
    }
    UINT STDMETHODCALLTYPE GetNumInterfaceSlots() noexcept override {
        return 0;
    }
    HRESULT STDMETHODCALLTYPE GetMinFeatureLevel(D3D_FEATURE_LEVEL *) noexcept override {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetThreadGroupSize(UINT *, UINT *, UINT *) noexcept override {
        return 0;
    }
    UINT64 STDMETHODCALLTYPE GetRequiresFlags() noexcept override {
        return flags;
    }
};

static bool Gate(FakeReflection &r, const char *&status, uint64_t &detail) {
    status = nullptr;
    detail = 0;
    return PixelReplayGate(&r, status, detail);
}

static void TestPixelProof() {
    const char *status = nullptr;
    uint64_t detail = 0;
    {
        // a36d: cb3, s1, five Texture2DMS and one Texture2D, dual-source
        // SV_Target0/1, [earlydepthstencil] (/tmp/claude-0/glass/a36d.txt).
        FakeReflection a36d;
        a36d.bindings = {D3D_SIT_CBUFFER, D3D_SIT_SAMPLER, D3D_SIT_TEXTURE, D3D_SIT_TEXTURE,
                         D3D_SIT_TEXTURE, D3D_SIT_TEXTURE, D3D_SIT_TEXTURE, D3D_SIT_TEXTURE};
        a36d.outputs = {D3D_NAME_TARGET, D3D_NAME_TARGET};
        a36d.flags = D3D_SHADER_REQUIRES_EARLY_DEPTH_STENCIL;
        assert(Gate(a36d, status, detail));
        assert(!strcmp(status, "pixel-read-only-reflection-proven") && detail == ~0ull);
        assert(a36d.binding_reads == 8 && a36d.output_reads == 2);
    }
    {
        // Every read-only binding type and every allowed requirement.
        FakeReflection r;
        r.bindings = {D3D_SIT_CBUFFER, D3D_SIT_TBUFFER,    D3D_SIT_TEXTURE,
                      D3D_SIT_SAMPLER, D3D_SIT_STRUCTURED, D3D_SIT_BYTEADDRESS};
        r.flags = D3D_SHADER_REQUIRES_DOUBLES | D3D_SHADER_REQUIRES_MINIMUM_PRECISION |
                  D3D_SHADER_REQUIRES_11_1_DOUBLE_EXTENSIONS | D3D_SHADER_REQUIRES_INT64_OPS |
                  D3D_SHADER_REQUIRES_NATIVE_16BIT_OPS | D3D_SHADER_REQUIRES_EARLY_DEPTH_STENCIL;
        assert(Gate(r, status, detail) && detail == ~0ull);
        // No bindings and no outputs is read-only too.
        FakeReflection empty;
        empty.bindings.clear();
        empty.outputs.clear();
        assert(Gate(empty, status, detail));
    }
    // UAVs and other non-read bindings, after read-only ones, are refused by type.
    for (auto type : {D3D_SIT_UAV_RWTYPED, D3D_SIT_UAV_RWSTRUCTURED, D3D_SIT_UAV_RWBYTEADDRESS,
                      D3D_SIT_UAV_APPEND_STRUCTURED, D3D_SIT_UAV_CONSUME_STRUCTURED,
                      D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER, D3D_SIT_RTACCELERATIONSTRUCTURE,
                      D3D_SIT_UAV_FEEDBACKTEXTURE, D3D_SHADER_INPUT_TYPE(99)}) {
        FakeReflection r;
        r.bindings.push_back(type);
        assert(!Gate(r, status, detail));
        assert(!strcmp(status, "pixel-replay-writable-or-unknown-binding") &&
               detail == uint64_t(type));
    }
    // Waves, UAV and bindless features, outputs by capability and unknown bits.
    for (UINT64 flag :
         {UINT64(D3D_SHADER_REQUIRES_UAVS_AT_EVERY_STAGE), UINT64(D3D_SHADER_REQUIRES_64_UAVS),
          UINT64(D3D_SHADER_REQUIRES_11_1_SHADER_EXTENSIONS),
          UINT64(D3D_SHADER_REQUIRES_STENCIL_REF), UINT64(D3D_SHADER_REQUIRES_INNER_COVERAGE),
          UINT64(D3D_SHADER_REQUIRES_TYPED_UAV_LOAD_ADDITIONAL_FORMATS),
          UINT64(D3D_SHADER_REQUIRES_ROVS), UINT64(D3D_SHADER_REQUIRES_WAVE_OPS),
          UINT64(D3D_SHADER_REQUIRES_VIEW_ID), UINT64(D3D_SHADER_REQUIRES_BARYCENTRICS),
          UINT64(D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING),
          UINT64(D3D_SHADER_REQUIRES_SAMPLER_DESCRIPTOR_HEAP_INDEXING),
          UINT64(D3D_SHADER_REQUIRES_WRITEABLE_MSAA_TEXTURES),
          UINT64(D3D_SHADER_REQUIRES_EXTENDED_COMMAND_INFO), UINT64(1) << 40}) {
        FakeReflection r;
        r.flags = flag | D3D_SHADER_REQUIRES_EARLY_DEPTH_STENCIL;
        assert(!Gate(r, status, detail));
        assert(!strcmp(status, "pixel-replay-requires-unsupported-capabilities") && detail == flag);
        assert(!r.binding_reads && !r.output_reads);
    }
    // Any output but SV_Target, after a target, is refused by system value.
    for (auto name : {D3D_NAME_DEPTH, D3D_NAME_COVERAGE, D3D_NAME_DEPTH_GREATER_EQUAL,
                      D3D_NAME_DEPTH_LESS_EQUAL, D3D_NAME_STENCIL_REF, D3D_NAME_UNDEFINED}) {
        FakeReflection r;
        r.outputs.push_back(name);
        assert(!Gate(r, status, detail));
        assert(!strcmp(status, "pixel-replay-output-not-render-target") &&
               detail == uint64_t(name));
    }
    {
        // Not a pixel stage, no description, too many bindings or no reflection.
        FakeReflection vertex;
        vertex.version = UINT(D3D12_SHVER_VERTEX_SHADER) << 16 | 0x61;
        assert(!Gate(vertex, status, detail));
        assert(!strcmp(status, "pixel-stage-or-description-unsupported") && detail == ~0ull);
        FakeReflection failed;
        failed.desc_result = E_FAIL;
        assert(!Gate(failed, status, detail));
        assert(!strcmp(status, "pixel-stage-or-description-unsupported"));
        FakeReflection many;
        many.bound = 129;
        assert(!Gate(many, status, detail));
        assert(!strcmp(status, "pixel-stage-or-description-unsupported") && !many.binding_reads);
        assert(!PixelReplayGate(nullptr, status, detail));
        assert(!strcmp(status, "pixel-stage-or-description-unsupported"));
    }
    {
        // A binding or output that cannot be read is refused.
        FakeReflection binding;
        binding.binding_result = E_FAIL;
        assert(!Gate(binding, status, detail));
        assert(!strcmp(status, "pixel-replay-writable-or-unknown-binding") && detail == ~0ull);
        FakeReflection output;
        output.output_result = E_FAIL;
        assert(!Gate(output, status, detail));
        assert(!strcmp(status, "pixel-replay-output-not-render-target") && detail == ~0ull);
    }
    // The bytecode itself: absent, empty or oversized, then not reflectable here.
    assert(!PixelReplayProof(nullptr, 64, status, detail));
    assert(!strcmp(status, "pixel-bytecode-invalid") && detail == ~0ull);
    const char garbage[] = "not a shader container, not DXBC and not DXIL at all.";
    assert(!PixelReplayProof(garbage, 0, status, detail));
    assert(!strcmp(status, "pixel-bytecode-invalid"));
    assert(!PixelReplayProof(garbage, 4 * 1024 * 1024 + 1, status, detail));
    assert(!strcmp(status, "pixel-bytecode-invalid"));
    assert(!PixelReplayProof(garbage, sizeof(garbage), status, detail));
    assert(!strcmp(status, "pixel-dxc-companion-unavailable") ||
           !strcmp(status, "pixel-dxil-reflection-unavailable"));
    // The vertex proof keeps its own statuses on the shared loader.
    assert(!rrsss::VertexReplayProof(nullptr, 64, status));
    assert(!strcmp(status, "vertex-bytecode-invalid"));
    assert(!rrsss::VertexReplayProof(garbage, sizeof(garbage), status));
    assert(!strcmp(status, "vertex-dxc-companion-unavailable") ||
           !strcmp(status, "vertex-dxil-reflection-unavailable"));
}

static std::vector<std::string> logged;
static void Log(const char *text) {
    logged.push_back(text);
}

// A ReShade device that only reports its API: the registry refuses before
// it reads anything else.
namespace fake {
using namespace reshade::api;
struct Device final : reshade::api::device {
    device_api api = device_api::d3d12;
    uint64_t get_native() const override {
        return 0;
    }
    void get_private_data(const uint8_t[16], uint64_t *data) const override {
        *data = 0;
    }
    void set_private_data(const uint8_t[16], const uint64_t) override {}
    device_api get_api() const override {
        return api;
    }
    bool check_capability(device_caps) const override {
        return {};
    }
    bool check_format_support(format, resource_usage) const override {
        return {};
    }
    bool create_sampler(const sampler_desc &, sampler *) override {
        return {};
    }
    void destroy_sampler(sampler) override {}
    bool create_resource(const resource_desc &, const subresource_data *, resource_usage,
                         resource *, void **) override {
        return {};
    }
    void destroy_resource(resource) override {}
    resource_desc get_resource_desc(resource) const override {
        return {};
    }
    bool create_resource_view(resource, resource_usage, const resource_view_desc &,
                              resource_view *) override {
        return {};
    }
    void destroy_resource_view(resource_view) override {}
    resource get_resource_from_view(resource_view) const override {
        return {};
    }
    resource_view_desc get_resource_view_desc(resource_view) const override {
        return {};
    }
    bool map_buffer_region(resource, uint64_t, uint64_t, map_access, void **) override {
        return {};
    }
    void unmap_buffer_region(resource) override {}
    bool map_texture_region(resource, uint32_t, const subresource_box *, map_access,
                            subresource_data *) override {
        return {};
    }
    void unmap_texture_region(resource, uint32_t) override {}
    void update_buffer_region(const void *, resource, uint64_t, uint64_t) override {}
    void update_texture_region(const subresource_data &, resource, uint32_t,
                               const subresource_box *) override {}
    bool create_pipeline(pipeline_layout, uint32_t, const pipeline_subobject *,
                         pipeline *) override {
        assert(!"a refused pipeline creates no variant");
        return false;
    }
    void destroy_pipeline(pipeline) override {}
    bool create_pipeline_layout(uint32_t, const pipeline_layout_param *,
                                pipeline_layout *) override {
        return {};
    }
    void destroy_pipeline_layout(pipeline_layout) override {}
    bool allocate_descriptor_tables(uint32_t, pipeline_layout, uint32_t,
                                    descriptor_table *) override {
        return {};
    }
    void free_descriptor_tables(uint32_t, const descriptor_table *) override {}
    void get_descriptor_heap_offset(descriptor_table, uint32_t, uint32_t, descriptor_heap *,
                                    uint32_t *) const override {}
    void copy_descriptor_tables(uint32_t, const descriptor_table_copy *) override {}
    void update_descriptor_tables(uint32_t, const descriptor_table_update *) override {}
    bool create_query_heap(query_type, uint32_t, query_heap *) override {
        return {};
    }
    void destroy_query_heap(query_heap) override {}
    bool get_query_heap_results(query_heap, query_type, uint32_t, uint32_t, void *,
                                uint32_t) override {
        return {};
    }
    void set_resource_name(resource, const char *) override {}
    void set_resource_view_name(resource_view, const char *) override {}
    bool create_fence(uint64_t, fence_flags, fence *, void **) override {
        return {};
    }
    void destroy_fence(fence) override {}
    uint64_t get_completed_fence_value(fence) const override {
        return {};
    }
    bool wait(fence, uint64_t, uint64_t) override {
        return {};
    }
    bool signal(fence, uint64_t) override {
        return {};
    }
    bool get_property(device_properties, void *) const override {
        return {};
    }
    uint64_t get_resource_view_gpu_address(resource_view) const override {
        return {};
    }
    void get_acceleration_structure_size(acceleration_structure_type,
                                         acceleration_structure_build_flags, uint32_t,
                                         const acceleration_structure_build_input *, uint64_t *,
                                         uint64_t *, uint64_t *) const override {}
    bool get_pipeline_shader_group_handles(pipeline, uint32_t, uint32_t, void *) override {
        return {};
    }
};
} // namespace fake

static void TestReportGate() {
    CoverageReportGate gate;
    // The first line of each kind for a shader, then the cap.
    for (int i = 0; i < 16; ++i)
        assert(gate.Admit(0xa, false));
    assert(!gate.Admit(0xa, false));
    assert(gate.Admit(0xa, true) && !gate.Admit(0xa, true));
    assert(gate.Admit(0xb, false) && gate.Admit(0xb, true));
    assert(!gate.Admit(0xb, false) && !gate.Admit(0xb, true));
    // Under the cap every line is logged, first or not.
    CoverageReportGate under;
    for (int i = 0; i < 8; ++i)
        assert(under.Admit(0xc, true) && under.Admit(0xd, false));
    assert(!under.Admit(0xc, true) && !under.Admit(0xd, false) && under.Admit(0xe, true));
}

static void TestStatus() {
    auto *a = reinterpret_cast<api::device *>(uintptr_t(0x2000));
    auto *b = reinterpret_cast<api::device *>(uintptr_t(0x3000));
    CoverageStatus status;
    auto none = status.Counts();
    assert(!none.oit.accepted && !none.oit.refused && !none.refraction.accepted &&
           !none.refraction.refused && !status.OitUnavailable());
    status.Record(a, Kind::Oit, false);
    status.Record(a, Kind::Oit, false);
    status.Record(a, Kind::Refraction, true);
    auto c = status.Counts(a);
    assert(c.oit.refused == 2 && !c.oit.accepted && c.refraction.accepted == 1 &&
           !c.refraction.refused);
    // Every OIT PSO was refused: unavailable, whatever refraction did.
    assert(status.OitUnavailable());
    status.Record(b, Kind::Oit, true);
    status.Record(b, Kind::Refraction, false);
    c = status.Counts(b);
    assert(c.oit.accepted == 1 && !c.oit.refused && c.refraction.refused == 1 &&
           !c.refraction.accepted);
    c = status.Counts();
    assert(c.oit.refused == 2 && c.oit.accepted == 1 && c.refraction.accepted == 1 &&
           c.refraction.refused == 1);
    assert(!status.OitUnavailable());
    // A forgotten device's counts are gone.
    status.Forget(b);
    c = status.Counts(b);
    assert(!c.oit.accepted && !c.refraction.refused);
    assert(status.Counts().oit.accepted == 0 && status.OitUnavailable());
    status.Forget(a);
    assert(!status.Counts().oit.refused && !status.OitUnavailable());
    // Only refused refraction PSOs do not make the OIT capture unavailable.
    status.Record(a, Kind::Refraction, false);
    assert(!status.OitUnavailable());
}

static void TestRegistry() {
    CoveragePipelineRegistry registry;
    registry.logger = &Log;
    auto *device = reinterpret_cast<api::device *>(uintptr_t(0x2000));
    auto *other = reinterpret_cast<api::device *>(uintptr_t(0x3000));
    assert(!registry.Counts().oit.refused && !registry.OitUnavailable() && !registry.LastRefusal());
    assert(!registry.Find(device, 0x10));
    // A null device is ignored before anything is read.
    Native n(Oit,
             ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    registry.Initialize(nullptr, {0x1000}, n.Count(), n.objects.data(), {0x10});
    assert(logged.empty() && !registry.OitUnavailable());
    // The command signature hook result is kept per device.
    assert(!registry.SignatureHooked(device) && !registry.SignatureHooked());
    registry.SignatureHook(device, true);
    assert(registry.SignatureHooked(device) && registry.SignatureHooked());
    registry.SignatureHook(other, false);
    assert(registry.SignatureHooked(device) && !registry.SignatureHooked(other));
    assert(!registry.SignatureHooked());
    registry.ForgetDevice(other);
    assert(!registry.SignatureHooked(other) && registry.SignatureHooked());
    registry.ForgetDevice(device);
    assert(!registry.SignatureHooked(device) && !registry.SignatureHooked());
    registry.Forget(0x10);
    assert(!registry.Find(device, 0x10));
}

static bool Contains(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}

// Refusals through Initialize, with targets built from the test's own shaders.
static void TestRegistryRefusals() {
    Native oit(Oit,
               ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    Native refraction(Refraction, ReShadeNative(api::blend_factor::one,
                                                api::blend_factor::one_minus_source_alpha));
    Native unknown(
        Oit, ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    unknown.ps[0] ^= 1;
    std::vector<unsigned char> third_ps(4780, 0x3c);
    const uint64_t oit_hash = rrsss::PipelineProof::ByteHash(oit.ps.data(), oit.ps.size());
    const uint64_t refraction_hash =
        rrsss::PipelineProof::ByteHash(refraction.ps.data(), refraction.ps.size());
    const uint64_t third_hash = rrsss::PipelineProof::ByteHash(third_ps.data(), third_ps.size());
    const Target targets[] = {{oit_hash, Oit.bytes, Kind::Oit},
                              {refraction_hash, Refraction.bytes, Kind::Refraction},
                              {third_hash, 4780, Kind::Refraction}};
    CoveragePipelineRegistry registry(targets);
    registry.logger = &Log;
    logged.clear();
    fake::Device device, second, vulkan;
    vulkan.api = api::device_api::vulkan;

    // Not a target, or not D3D12: nothing is counted or logged.
    registry.Initialize(&device, {0x1000}, unknown.Count(), unknown.objects.data(), {0});
    registry.Initialize(&vulkan, {0x1000}, oit.Count(), oit.objects.data(), {0});
    assert(logged.empty() && !registry.Counts().oit.refused && !registry.LastRefusal());

    // A refusal is counted for its device and kind and logged with its shader.
    registry.Initialize(&device, {0x1000}, oit.Count(), oit.objects.data(), {0});
    assert(logged.size() == 1);
    char hex[24];
    snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(oit_hash));
    assert(logged[0] == std::string("Transparency albedo blend refused PSO ") + hex +
                            ": no-pipeline: missing-pipeline-or-layout-handle "
                            "(nativeObservation{unobserved})");
    assert(!strcmp(registry.LastRefusal(), "missing-pipeline-or-layout-handle"));
    auto c = registry.Counts(&device);
    assert(c.oit.refused == 1 && !c.oit.accepted && !c.refraction.refused);
    assert(registry.OitUnavailable());
    registry.Initialize(&device, {0}, refraction.Count(), refraction.objects.data(), {0x20});
    c = registry.Counts(&device);
    assert(c.oit.refused == 1 && c.refraction.refused == 1);
    snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(refraction_hash));
    assert(logged.size() == 2 && Contains(logged[1], hex));
    // Per device, summed over the live ones, and reset when a device goes.
    registry.Initialize(&second, {0x1000}, oit.Count(), oit.objects.data(), {0});
    assert(registry.Counts(&second).oit.refused == 1 && registry.Counts().oit.refused == 2);
    registry.ForgetDevice(&device);
    c = registry.Counts(&device);
    assert(!c.oit.refused && !c.refraction.refused);
    assert(registry.Counts().oit.refused == 1 && registry.OitUnavailable());
    registry.ForgetDevice(&second);
    assert(!registry.Counts().oit.refused && !registry.OitUnavailable());

    // At most 16 lines, but the first refusal of another shader is still logged.
    for (int i = 0; i < 30; ++i)
        registry.Initialize(&device, {0x1000}, oit.Count(), oit.objects.data(), {0});
    assert(logged.size() == 16 && registry.Counts(&device).oit.refused == 30);
    refraction.ps_desc = {third_ps.data(), third_ps.size()};
    registry.Initialize(&device, {0x1000}, refraction.Count(), refraction.objects.data(), {0});
    snprintf(hex, sizeof(hex), "0x%016llx", static_cast<unsigned long long>(third_hash));
    assert(logged.size() == 17 && Contains(logged[16], hex));
    registry.Initialize(&device, {0x1000}, refraction.Count(), refraction.objects.data(), {0});
    assert(logged.size() == 17 && registry.Counts(&device).refraction.refused == 2);
    registry.ForgetDevice(&device);
}

int main() {
    TestTargetHash();
    TestFactors();
    TestPatchOit();
    TestPatchRefraction();
    TestPatchDsvAbsent();
    TestPatchRefusals();
    TestExpected(Oit, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR, api::blend_factor::one,
                 api::blend_factor::one_minus_source1_color, DepthClass::NoDsv);
    TestExpected(Oit, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR, api::blend_factor::one,
                 api::blend_factor::one_minus_source1_color, DepthClass::KeepDsv);
    TestExpected(Refraction, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, api::blend_factor::one,
                 api::blend_factor::one_minus_source_alpha, DepthClass::NoDsv);
    TestExpected(Refraction, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, api::blend_factor::one,
                 api::blend_factor::one_minus_source_alpha, DepthClass::KeepDsv);
    // A partial sample mask is mirrored into the variant and still Equivalent.
    TestExpected(Oit, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC1_COLOR, api::blend_factor::one,
                 api::blend_factor::one_minus_source1_color, DepthClass::NoDsv, 0x5);
    TestExpected(Refraction, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_COLOR,
                 api::blend_factor::source_alpha, api::blend_factor::one_minus_source_color,
                 DepthClass::NoDsv);
    TestFormatSupport();
    TestPixelProof();
    TestReportGate();
    TestStatus();
    TestRegistry();
    TestRegistryRefusals();
    puts("PASS: transparency coverage variant target hash, blend factor mapping, subobject "
         "patch and refusals, expected observation equivalence, format support, pixel replay "
         "proof, report cap, status counts, registry");
}
