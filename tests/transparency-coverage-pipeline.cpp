#include "../src/rr_transparency_coverage_pipeline.h"
#include "../src/rr_pipeline_state.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
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
    assert(patch.vertex == &n.vs_desc);
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
    Refused("sample-mask", [](Native &n) { n.mask = 0xffff; });
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
    NativeDesc(const D3D12_BLEND_DESC &blend, DXGI_FORMAT dsv, bool depth_test) {
        for (size_t i = 0; i < sizeof(ps); ++i)
            ps[i] = static_cast<unsigned char>(i * 7 + 3);
        vs[0] = 0x44;
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature *>(uintptr_t(0x1000));
        desc.VS = {vs, sizeof(vs)};
        desc.PS = {ps, sizeof(ps)};
        desc.BlendState = blend;
        desc.SampleMask = ~0u;
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
    }
    return d;
}

static void TestExpected(const Target &target, D3D12_BLEND src, D3D12_BLEND dest,
                         api::blend_factor rs_src, api::blend_factor rs_dest, DepthClass depth) {
    using rrsss::PipelineProof;
    const auto blend = NativeBlend(src, dest);
    const NativeDesc native(blend, DXGI_FORMAT_D32_FLOAT, depth == DepthClass::KeepDsv);
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
    assert(e.root_identity == observed.root_identity && e.input_hash == observed.input_hash);
    assert(!memcmp(&e.raster, &observed.raster, sizeof(e.raster)));
    // The variant created from the patched subobjects is Equivalent to it,
    // whichever IndependentBlendEnable ReShade picks for one RT.
    Native n(target, ReShadeNative(rs_src, rs_dest));
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

static std::vector<std::string> logged;
static void Log(const char *text) {
    logged.push_back(text);
}

static void TestRegistry() {
    CoveragePipelineRegistry registry;
    registry.logger = &Log;
    auto *device = reinterpret_cast<api::device *>(uintptr_t(0x2000));
    auto *other = reinterpret_cast<api::device *>(uintptr_t(0x3000));
    assert(!registry.AnyAccepted() && !registry.AllRefused() && !registry.LastRefusal());
    assert(!registry.Find(device, 0x10));
    // A null device is ignored before anything is read.
    Native n(Oit,
             ReShadeNative(api::blend_factor::one, api::blend_factor::one_minus_source1_color));
    registry.Initialize(nullptr, {0x1000}, n.Count(), n.objects.data(), {0x10});
    assert(logged.empty() && !registry.AllRefused());
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
    TestExpected(Refraction, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_COLOR,
                 api::blend_factor::source_alpha, api::blend_factor::one_minus_source_color,
                 DepthClass::NoDsv);
    TestFormatSupport();
    TestRegistry();
    puts("PASS: transparency coverage variant target hash, blend factor mapping, subobject "
         "patch and refusals, expected observation equivalence, format support, registry");
}
