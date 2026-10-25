#include "../src/rr_transparency_blend_policy.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

using namespace rrblend;

static bool Near(float a, float b) {
    return std::fabs(a - b) <= 1e-6f;
}

static void Fill(float (&t)[3][3][3], float value) {
    for (auto &row : t)
        for (auto &tap : row)
            for (auto &channel : tap)
                channel = value;
}

static Guides Sample() {
    return {{0.9f, 0.8f, 0.7f}, {0.04f, 0.05f, 0.06f}, 0.6f};
}

static bool Same(const Guides &a, const Guides &b) {
    return !memcmp(&a, &b, sizeof(Guides));
}

static void TestAdmit() {
    assert(Admit(true, 0, 0, 0, false));
    assert(Admit(true, 5, 5, 5, false));
    assert(Admit(true, 0, 2, 5, false));
    assert(Admit(true, 1, 1, 3, false));
    assert(!Admit(false, 0, 2, 5, false));
    assert(!Admit(true, 0, 2, 5, true));
    assert(!Admit(true, -1, 2, 5, false));
    assert(!Admit(true, -2, 2, 5, false));
    assert(!Admit(true, 0, -1, 5, false));
    assert(!Admit(true, 0, -2, 5, false));
    assert(!Admit(true, 0, 2, -1, false));
    assert(!Admit(true, 0, 2, -2, false));
    assert(!Admit(true, 3, 2, 5, false));
    assert(!Admit(true, 0, 5, 4, false));
    assert(!Admit(true, -1, -1, -1, false));
}

static void TestTargets() {
    auto oit = FindTarget(0xa36d02f6245a4129ull, 11856);
    assert(oit && oit->kind == Kind::Oit && oit->hash == 0xa36d02f6245a4129ull);
    auto refraction = FindTarget(0xb72393e05df90a78ull, 4812);
    assert(refraction && refraction->kind == Kind::Refraction && refraction->bytes == 4812);
    assert(!FindTarget(0xa36d02f6245a4129ull, 11855));
    assert(!FindTarget(0xa36d02f6245a4129ull, 4812));
    assert(!FindTarget(0xb72393e05df90a78ull, 11856));
    assert(!FindTarget(0xa36d02f6245a4128ull, 11856));
    assert(!FindTarget(0, 0));
}

static D3D12_BLEND_DESC Native(D3D12_BLEND dest) {
    D3D12_BLEND_DESC d{};
    auto &rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = dest;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return d;
}

static void TestClassify() {
    for (auto dest : {D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR, D3D12_BLEND_SRC_COLOR,
                      D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
                      D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA})
        assert(Classify(Native(dest), 1) == BlendClass::Accept);
    // Observed b723 state (programs.json): ONE / INV_SRC_ALPHA, write mask 15.
    assert(Classify(Native(D3D12_BLEND_INV_SRC_ALPHA), 1) == BlendClass::Accept);
    auto rgb_only = Native(D3D12_BLEND_INV_SRC1_COLOR);
    rgb_only.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    assert(Classify(rgb_only, 1) == BlendClass::Accept);

    assert(Classify(Native(D3D12_BLEND_INV_SRC1_COLOR), 2) == BlendClass::Targets);
    assert(Classify(Native(D3D12_BLEND_INV_SRC1_COLOR), 0) == BlendClass::Targets);
    auto off = Native(D3D12_BLEND_INV_SRC1_COLOR);
    off.RenderTarget[0].BlendEnable = FALSE;
    assert(Classify(off, 1) == BlendClass::NoBlend);
    for (auto dest :
         {D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
          D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_BLEND_FACTOR,
          D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_SRC_ALPHA_SAT})
        assert(Classify(Native(dest), 1) == BlendClass::Factor);
    auto op = Native(D3D12_BLEND_INV_SRC1_COLOR);
    op.RenderTarget[0].BlendOp = D3D12_BLEND_OP_SUBTRACT;
    assert(Classify(op, 1) == BlendClass::Op);
    op.RenderTarget[0].BlendOp = D3D12_BLEND_OP_MAX;
    assert(Classify(op, 1) == BlendClass::Op);
    auto logic = Native(D3D12_BLEND_INV_SRC1_COLOR);
    logic.RenderTarget[0].LogicOpEnable = TRUE;
    assert(Classify(logic, 1) == BlendClass::Logic);
    auto mask = Native(D3D12_BLEND_INV_SRC1_COLOR);
    mask.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALPHA;
    assert(Classify(mask, 1) == BlendClass::Mask);
    mask.RenderTarget[0].RenderTargetWriteMask = 0;
    assert(Classify(mask, 1) == BlendClass::Mask);
    // Alpha to coverage changes which samples are written: refused as a mask.
    auto coverage = Native(D3D12_BLEND_INV_SRC1_COLOR);
    coverage.AlphaToCoverageEnable = TRUE;
    assert(Classify(coverage, 1) == BlendClass::Mask);
}

static void TestPatchBlend() {
    auto native = Native(D3D12_BLEND_INV_SRC1_COLOR);
    native.AlphaToCoverageEnable = TRUE;
    native.IndependentBlendEnable = TRUE;
    native.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
    native.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    native.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_MAX;
    native.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
    for (UINT i = 1; i < 8; ++i)
        native.RenderTarget[i] = native.RenderTarget[0];
    const auto p = PatchBlend(native);
    assert(p.AlphaToCoverageEnable == FALSE);
    assert(p.IndependentBlendEnable == FALSE);
    const auto &rt = p.RenderTarget[0];
    assert(rt.BlendEnable == TRUE);
    assert(rt.LogicOpEnable == FALSE);
    assert(rt.SrcBlend == D3D12_BLEND_ZERO);
    assert(rt.DestBlend == D3D12_BLEND_INV_SRC1_COLOR);
    assert(rt.BlendOp == D3D12_BLEND_OP_ADD);
    assert(rt.SrcBlendAlpha == D3D12_BLEND_ZERO);
    assert(rt.DestBlendAlpha == D3D12_BLEND_ONE);
    assert(rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
    assert(rt.LogicOp == D3D12_LOGIC_OP_NOOP);
    assert(rt.RenderTargetWriteMask == 7);
    for (UINT i = 1; i < 8; ++i) {
        const auto &rest = p.RenderTarget[i];
        assert(rest.BlendEnable == FALSE && rest.LogicOpEnable == FALSE);
        assert(rest.SrcBlend == D3D12_BLEND_ONE && rest.DestBlend == D3D12_BLEND_ZERO);
        assert(rest.BlendOp == D3D12_BLEND_OP_ADD);
        assert(rest.SrcBlendAlpha == D3D12_BLEND_ONE && rest.DestBlendAlpha == D3D12_BLEND_ZERO);
        assert(rest.BlendOpAlpha == D3D12_BLEND_OP_ADD);
        assert(rest.LogicOp == D3D12_LOGIC_OP_NOOP);
        assert(rest.RenderTargetWriteMask == 0);
    }
    auto refraction = Native(D3D12_BLEND_INV_SRC_ALPHA);
    refraction.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_ALPHA;
    const auto q = PatchBlend(refraction);
    assert(q.RenderTarget[0].DestBlend == D3D12_BLEND_INV_SRC_ALPHA);
    assert(q.RenderTarget[0].RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_GREEN);
}

static void TestDepthStencil() {
    D3D12_DEPTH_STENCIL_DESC d{};
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    assert(DepthStencilReplayable(d) == DepthClass::NoDsv);

    d.DepthEnable = TRUE;
    assert(DepthStencilReplayable(d) == DepthClass::Refuse);
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    assert(DepthStencilReplayable(d) == DepthClass::KeepDsv);

    d.StencilEnable = TRUE;
    d.StencilReadMask = 0xff;
    d.StencilWriteMask = 0xff;
    D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                    D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
    d.FrontFace = d.BackFace = keep;
    assert(DepthStencilReplayable(d) == DepthClass::KeepDsv);
    D3D12_STENCIL_OP D3D12_DEPTH_STENCILOP_DESC::*ops[] = {
        &D3D12_DEPTH_STENCILOP_DESC::StencilFailOp, &D3D12_DEPTH_STENCILOP_DESC::StencilDepthFailOp,
        &D3D12_DEPTH_STENCILOP_DESC::StencilPassOp};
    for (auto face : {&D3D12_DEPTH_STENCIL_DESC::FrontFace, &D3D12_DEPTH_STENCIL_DESC::BackFace})
        for (auto op : ops) {
            auto writes = d;
            (writes.*face).*op = D3D12_STENCIL_OP_REPLACE;
            assert(DepthStencilReplayable(writes) == DepthClass::Refuse);
            writes.StencilWriteMask = 0;
            assert(DepthStencilReplayable(writes) == DepthClass::KeepDsv);
            writes.StencilWriteMask = 0x80;
            writes.StencilEnable = FALSE;
            assert(DepthStencilReplayable(writes) == DepthClass::KeepDsv);
        }

    // Stencil test only (no depth test) that never writes keeps the DSV.
    d.DepthEnable = FALSE;
    assert(DepthStencilReplayable(d) == DepthClass::KeepDsv);
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    assert(DepthStencilReplayable(d) == DepthClass::Refuse);
}

static D3D12_INDIRECT_ARGUMENT_DESC Arg(D3D12_INDIRECT_ARGUMENT_TYPE type) {
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = type;
    return a;
}

static D3D12_INDIRECT_ARGUMENT_DESC Constant(UINT parameter, UINT offset, UINT count) {
    auto a = Arg(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT);
    a.Constant.RootParameterIndex = parameter;
    a.Constant.DestOffsetIn32BitValues = offset;
    a.Constant.Num32BitValuesToSet = count;
    return a;
}

template <size_t N> static SignatureClass Signature(const D3D12_INDIRECT_ARGUMENT_DESC (&args)[N]) {
    D3D12_COMMAND_SIGNATURE_DESC d{};
    d.ByteStride = 64;
    d.NumArgumentDescs = UINT(N);
    d.pArgumentDescs = args;
    // Root parameter 2 holds 4 constants, 5 holds 1; the others are not known.
    return ClassifySignature(d, [](UINT parameter) {
        return parameter == 2 ? 4 : parameter == 5 ? 1 : -1;
    });
}

static void TestSignature() {
    using T = D3D12_INDIRECT_ARGUMENT_TYPE;
    const D3D12_INDIRECT_ARGUMENT_DESC draw[] = {Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(draw) == SignatureClass::Replayable);
    const D3D12_INDIRECT_ARGUMENT_DESC full[] = {
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW),
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW),
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW),
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW),
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW),
        Constant(2, 0, 4),
        Constant(5, 0, 1),
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)};
    assert(Signature(full) == SignatureClass::Replayable);

    const D3D12_INDIRECT_ARGUMENT_DESC offset[] = {Constant(2, 1, 3),
                                                   Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(offset) == SignatureClass::PartialConstants);
    // An offset alone is partial, even with a full count or an unknown parameter.
    const D3D12_INDIRECT_ARGUMENT_DESC shifted[] = {Constant(2, 1, 4),
                                                    Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(shifted) == SignatureClass::PartialConstants);
    const D3D12_INDIRECT_ARGUMENT_DESC shifted_unknown[] = {Constant(3, 1, 4),
                                                            Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(shifted_unknown) == SignatureClass::PartialConstants);
    const D3D12_INDIRECT_ARGUMENT_DESC count[] = {Constant(2, 0, 2),
                                                  Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(count) == SignatureClass::PartialConstants);
    const D3D12_INDIRECT_ARGUMENT_DESC unknown[] = {Constant(3, 0, 4),
                                                    Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(unknown) == SignatureClass::Unknown);
    // A known partial write is reported even when another parameter is unknown.
    const D3D12_INDIRECT_ARGUMENT_DESC mixed[] = {Constant(3, 0, 4), Constant(2, 0, 2),
                                                  Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(mixed) == SignatureClass::PartialConstants);

    const D3D12_INDIRECT_ARGUMENT_DESC dispatch[] = {Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)};
    assert(Signature(dispatch) == SignatureClass::Unsupported);
    const D3D12_INDIRECT_ARGUMENT_DESC two[] = {Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW),
                                                Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)};
    assert(Signature(two) == SignatureClass::Unsupported);
    const D3D12_INDIRECT_ARGUMENT_DESC last[] = {
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW),
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW)};
    assert(Signature(last) == SignatureClass::Unsupported);
    const D3D12_INDIRECT_ARGUMENT_DESC none[] = {
        Arg(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW)};
    assert(Signature(none) == SignatureClass::Unsupported);
    const D3D12_INDIRECT_ARGUMENT_DESC strange[] = {Arg(T(1000)),
                                                    Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
    assert(Signature(strange) == SignatureClass::Unsupported);
    // A structural refusal wins over constants that would also be refused.
    const D3D12_INDIRECT_ARGUMENT_DESC both[] = {Constant(3, 1, 4),
                                                 Arg(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)};
    assert(Signature(both) == SignatureClass::Unsupported);

    D3D12_COMMAND_SIGNATURE_DESC empty{};
    auto words = [](UINT) { return 4; };
    assert(ClassifySignature(empty, words) == SignatureClass::Unsupported);
    empty.NumArgumentDescs = 1;
    assert(ClassifySignature(empty, words) == SignatureClass::Unsupported);
    empty.NumArgumentDescs = 0;
    empty.pArgumentDescs = draw;
    assert(ClassifySignature(empty, words) == SignatureClass::Unsupported);
}

static void TestCoverage() {
    D3D12_RESOURCE_DESC scene{};
    scene.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    scene.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    scene.Width = 960;
    scene.Height = 544;
    scene.DepthOrArraySize = 1;
    scene.MipLevels = 1;
    scene.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    scene.SampleDesc = {4, D3D12_STANDARD_MULTISAMPLE_PATTERN};
    scene.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    scene.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto d = CoverageDesc(scene);
    assert(d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    assert(d.Alignment == 0);
    assert(d.Width == 960 && d.Height == 544);
    assert(d.DepthOrArraySize == 1 && d.MipLevels == 1);
    assert(d.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    assert(d.SampleDesc.Count == 4 && d.SampleDesc.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN);
    assert(d.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    assert(d.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    scene.SampleDesc = {4, 0};
    scene.DepthOrArraySize = 2;
    scene.MipLevels = 3;
    const auto q = CoverageDesc(scene);
    assert(q.SampleDesc.Count == 4 && q.SampleDesc.Quality == 0);
    assert(q.DepthOrArraySize == 1 && q.MipLevels == 1);

    assert(Extent(d, 1920, 1088));
    assert(Extent(q, 1920, 1088));
    assert(!Extent(d, 1920, 1087));
    assert(!Extent(d, 1921, 1088));
    assert(!Extent(d, 960, 544));
    auto quality = d;
    quality.SampleDesc.Quality = 1;
    assert(!Extent(quality, 1920, 1088));
    auto single = d;
    single.SampleDesc = {1, 0};
    assert(!Extent(single, 1920, 1088));
    auto eight = d;
    eight.SampleDesc.Count = 8;
    assert(!Extent(eight, 1920, 1088));
    auto zero = d;
    zero.Width = 0;
    assert(!Extent(zero, 0, 1088));
    zero = d;
    zero.Height = 0;
    assert(!Extent(zero, 1920, 0));
    auto large = d;
    large.Width = 1ull << 32;
    assert(!Extent(large, 0, 1088));
}

static void TestControl() {
    static_assert(sizeof(Control) == 256);
    static_assert(offsetof(Control, admitted) == 0);
    static_assert(offsetof(Control, width) == 4);
    static_assert(offsetof(Control, height) == 8);
    static_assert(offsetof(Control, flags) == 12);
    static_assert(offsetof(Control, diffuse) == 16);
    static_assert(offsetof(Control, params) == 32);
    static_assert(offsetof(Control, reserved) == 48);
    static_assert(offsetof(Control, zero) == 64);
    static_assert(SkyFlag == 1 && RoughnessFlag == 2 && FilterFlag == 4 && DebugFlag == 8);

    const auto c = Make(Tuning{}, 1920, 1088);
    assert(c.admitted == 0 && c.width == 1920 && c.height == 1088);
    assert(c.flags == (SkyFlag | FilterFlag));
    assert(c.diffuse[0] == 0.5f && c.diffuse[1] == 0.5f && c.diffuse[2] == 0.5f);
    assert(c.diffuse[3] == 0.f);
    assert(c.params[0] == 0.f && c.params[1] == 0.f && c.params[2] == 1.f);
    assert(c.params[3] == 2.f / 1023.f && c.params[3] == OpacityFloor);
    unsigned char bytes[sizeof(Control)];
    memcpy(bytes, &c, sizeof(bytes));
    for (size_t i = 48; i < sizeof(bytes); ++i)
        assert(bytes[i] == 0);

    Tuning t;
    t.diffuse = 2.f;
    t.specular = 0.04f;
    t.roughness = 0.25f;
    t.strength = 0.5f;
    t.sky = false;
    t.filter = false;
    t.debug = true;
    const auto e = Make(t, 640, 368);
    assert(e.flags == (RoughnessFlag | DebugFlag));
    assert(e.diffuse[0] == 1.f && e.diffuse[1] == 1.f && e.diffuse[2] == 1.f);
    assert(e.diffuse[3] == 0.f);
    assert(e.params[0] == 0.04f && e.params[1] == 0.25f && e.params[2] == 0.5f);
    assert(e.params[3] == OpacityFloor);
    t.roughness = 0.f;
    assert(Make(t, 640, 368).flags & RoughnessFlag);
    assert(Make(t, 640, 368).params[1] == 0.f);
}

static void TestClamp() {
    const Tuning defaults;
    assert(defaults.diffuse == 0.5f && defaults.specular == 0.f && defaults.roughness == -1.f);
    assert(defaults.strength == 1.f && defaults.sky && defaults.filter && !defaults.debug);
    const auto same = Clamp(defaults);
    assert(same.diffuse == 0.5f && same.specular == 0.f && same.roughness == -1.f);
    assert(same.strength == 1.f && same.sky && same.filter && !same.debug);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Tuning t;
    t.diffuse = nan;
    t.specular = nan;
    t.roughness = nan;
    t.strength = nan;
    auto c = Clamp(t);
    assert(c.diffuse == 0.5f && c.specular == 0.f && c.roughness == -1.f && c.strength == 1.f);

    t.diffuse = -0.25f;
    t.specular = 3.f;
    t.roughness = -0.5f;
    t.strength = inf;
    t.sky = false;
    t.filter = false;
    t.debug = true;
    c = Clamp(t);
    assert(c.diffuse == 0.f && c.specular == 1.f && c.roughness == -1.f && c.strength == 1.f);
    assert(!c.sky && !c.filter && c.debug);

    t.diffuse = inf;
    t.specular = -inf;
    t.roughness = 7.f;
    t.strength = -2.f;
    c = Clamp(t);
    assert(c.diffuse == 1.f && c.specular == 0.f && c.roughness == 1.f && c.strength == 0.f);
    t.roughness = -inf;
    assert(Clamp(t).roughness == -1.f);
    t.roughness = 0.f;
    assert(Clamp(t).roughness == 0.f);
    t.roughness = 0.3f;
    assert(Clamp(t).roughness == 0.3f);
    t.diffuse = 0.2f;
    t.specular = 0.7f;
    t.strength = 0.6f;
    c = Clamp(t);
    assert(c.diffuse == 0.2f && c.specular == 0.7f && c.strength == 0.6f);
}

static Control Admitted(const Tuning &t, uint32_t w = 8, uint32_t h = 8) {
    auto c = Make(t, w, h);
    c.admitted = 1;
    return c;
}

static void TestReference() {
    Tuning plain;
    plain.filter = false;
    float t[3][3][3];
    Fill(t, 0.f); // fully opaque coverage: o = 1, c = 1

    // Not admitted: nothing changes, even for fully covered eligible pixels.
    auto control = Make(plain, 8, 8);
    for (uint32_t admitted : {0u, 2u}) {
        control.admitted = admitted;
        auto g = Sample();
        Reference(control, 3, 3, 1, t, g);
        assert(Same(g, Sample()));
    }

    // Full coverage replaces the albedo guides with the targets.
    control = Admitted(plain);
    for (uint32_t status : {1u, 3u}) {
        auto g = Sample();
        Reference(control, 3, 3, status, t, g);
        for (int i = 0; i < 3; ++i) {
            assert(Near(g.diffuse[i], 0.5f));
            assert(Near(g.specular[i], 0.f));
        }
        assert(g.roughness == 0.6f);
    }
    // Status 0 and 4 are never touched; pixels outside the extent are skipped.
    for (uint32_t status : {0u, 4u, 5u}) {
        auto g = Sample();
        Reference(control, 3, 3, status, t, g);
        assert(Same(g, Sample()));
    }
    for (auto p : {std::pair{8u, 3u}, {3u, 8u}}) {
        auto g = Sample();
        Reference(control, p.first, p.second, 1, t, g);
        assert(Same(g, Sample()));
    }

    // Sky (status 2) follows the sky flag.
    {
        auto g = Sample();
        Reference(control, 3, 3, 2, t, g);
        assert(Near(g.diffuse[0], 0.5f));
        Tuning no_sky = plain;
        no_sky.sky = false;
        g = Sample();
        Reference(Admitted(no_sky), 3, 3, 2, t, g);
        assert(Same(g, Sample()));
        g = Sample();
        Reference(Admitted(no_sky), 3, 3, 1, t, g);
        assert(Near(g.diffuse[0], 0.5f));
    }

    // Floor: opacity at or below 2/1023 is quantization noise, c = 0.
    {
        Fill(t, 1.f - 1.f / 1023.f);
        auto g = Sample();
        Reference(control, 3, 3, 1, t, g);
        assert(Same(g, Sample()));
        Fill(t, 1.f);
        g = Sample();
        Reference(control, 3, 3, 1, t, g);
        assert(Same(g, Sample()));
        // Above the floor, opacity is rescaled to [0, 1].
        Fill(t, 0.5f);
        g = Sample();
        Reference(control, 3, 3, 1, t, g);
        const float c = (0.5f - OpacityFloor) / (1.f - OpacityFloor);
        assert(Near(g.diffuse[0], 0.9f + c * (0.5f - 0.9f)));
        assert(Near(g.diffuse[1], 0.8f + c * (0.5f - 0.8f)));
        assert(Near(g.specular[2], 0.06f + c * (0.f - 0.06f)));
    }

    // Strength scales the rescaled opacity.
    {
        Tuning half = plain;
        half.strength = 0.5f;
        Fill(t, 0.f);
        auto g = Sample();
        Reference(Admitted(half), 3, 3, 1, t, g);
        assert(Near(g.diffuse[0], 0.7f));
        assert(Near(g.specular[0], 0.02f));
        Tuning none = plain;
        none.strength = 0.f;
        g = Sample();
        Reference(Admitted(none), 3, 3, 1, t, g);
        assert(Same(g, Sample()));
    }

    // Per-channel opacity lerps each channel separately; specular target applies.
    {
        Tuning tinted = plain;
        tinted.diffuse = 0.25f;
        tinted.specular = 0.04f;
        Fill(t, 1.f);
        t[1][1][0] = 0.f; // red fully covered
        t[1][1][1] = 1.f; // green clear
        t[1][1][2] = 0.5f;
        auto g = Sample();
        Reference(Admitted(tinted), 3, 3, 1, t, g);
        const float c = (0.5f - OpacityFloor) / (1.f - OpacityFloor);
        assert(Near(g.diffuse[0], 0.25f));
        assert(g.diffuse[1] == 0.8f);
        assert(Near(g.diffuse[2], 0.7f + c * (0.25f - 0.7f)));
        assert(Near(g.specular[0], 0.04f));
        assert(g.specular[1] == 0.05f);
        assert(Near(g.specular[2], 0.06f + c * (0.04f - 0.06f)));
        assert(g.roughness == 0.6f);
    }

    // Roughness moves toward its target by luma(c) only when enabled.
    {
        Tuning rough = plain;
        rough.roughness = 0.2f;
        Fill(t, 1.f);
        t[1][1][0] = 0.f;
        t[1][1][1] = 1.f;
        t[1][1][2] = 0.f;
        auto g = Sample();
        Reference(Admitted(rough), 3, 3, 1, t, g);
        const float luma = 0.299f + 0.114f;
        assert(Near(g.roughness, 0.6f + luma * (0.2f - 0.6f)));
        g = Sample();
        Reference(Admitted(plain), 3, 3, 1, t, g);
        assert(g.roughness == 0.6f);
        // No coverage: roughness is untouched even when enabled.
        Fill(t, 1.f);
        g = Sample();
        Reference(Admitted(rough), 3, 3, 1, t, g);
        assert(Same(g, Sample()));
    }

    // Filter: 1-2-1 x 1-2-1 tent over the in-bounds taps, normalized.
    {
        Tuning filtered;
        Fill(t, 1.f);
        t[1][1][0] = t[1][1][1] = t[1][1][2] = 0.f; // only the centre is covered
        auto g = Sample();
        Reference(Admitted(filtered), 3, 3, 1, t, g);
        float c = (4.f / 16.f - OpacityFloor) / (1.f - OpacityFloor);
        assert(Near(g.diffuse[0], 0.9f + c * (0.5f - 0.9f)));
        // Without the filter the centre tap is used alone.
        g = Sample();
        Reference(Admitted(plain), 3, 3, 1, t, g);
        assert(Near(g.diffuse[0], 0.5f));

        // Corner pixel: only the centre, right, below and diagonal taps are
        // in bounds (weights 4, 2, 2, 1). Out-of-bounds taps claim full
        // coverage and must be ignored.
        Fill(t, 0.f);
        t[1][2][0] = t[1][2][1] = t[1][2][2] = 1.f;
        t[2][1][0] = t[2][1][1] = t[2][1][2] = 1.f;
        t[2][2][0] = t[2][2][1] = t[2][2][2] = 1.f;
        g = Sample();
        Reference(Admitted(filtered), 0, 0, 1, t, g);
        c = (4.f / 9.f - OpacityFloor) / (1.f - OpacityFloor);
        assert(Near(g.diffuse[0], 0.9f + c * (0.5f - 0.9f)));
        // Opposite corner of a 4x2 extent mirrors it.
        Fill(t, 0.f);
        t[1][0][0] = t[1][0][1] = t[1][0][2] = 1.f;
        t[0][1][0] = t[0][1][1] = t[0][1][2] = 1.f;
        t[0][0][0] = t[0][0][1] = t[0][0][2] = 1.f;
        g = Sample();
        Reference(Admitted(filtered, 4, 2), 3, 1, 1, t, g);
        assert(Near(g.diffuse[0], 0.9f + c * (0.5f - 0.9f)));
        // Edge pixel (top row): six taps in bounds, weights 2-4-2 and 1-2-1.
        Fill(t, 0.f);
        for (int x = 0; x < 3; ++x)
            for (int ch = 0; ch < 3; ++ch)
                t[1][x][ch] = t[2][x][ch] = 1.f;
        t[1][1][0] = t[1][1][1] = t[1][1][2] = 0.f;
        g = Sample();
        Reference(Admitted(filtered), 3, 0, 1, t, g);
        c = (4.f / 12.f - OpacityFloor) / (1.f - OpacityFloor);
        assert(Near(g.diffuse[0], 0.9f + c * (0.5f - 0.9f)));
        // A one-pixel extent keeps only the centre.
        Fill(t, 1.f);
        t[1][1][0] = t[1][1][1] = t[1][1][2] = 0.f;
        g = Sample();
        Reference(Admitted(filtered, 1, 1), 0, 0, 1, t, g);
        assert(Near(g.diffuse[0], 0.5f));
    }

    // Debug: eligible pixels show c in diffuse and zero specular.
    {
        Tuning debug = plain;
        debug.debug = true;
        Fill(t, 1.f);
        t[1][1][0] = 0.f;
        t[1][1][2] = 0.5f;
        auto g = Sample();
        Reference(Admitted(debug), 3, 3, 1, t, g);
        const float c = (0.5f - OpacityFloor) / (1.f - OpacityFloor);
        assert(Near(g.diffuse[0], 1.f) && g.diffuse[1] == 0.f && Near(g.diffuse[2], c));
        assert(g.specular[0] == 0.f && g.specular[1] == 0.f && g.specular[2] == 0.f);
        assert(g.roughness == 0.6f);
        // Uncovered but eligible pixels show black.
        Fill(t, 1.f);
        g = Sample();
        Reference(Admitted(debug), 3, 3, 3, t, g);
        assert(g.diffuse[0] == 0.f && g.diffuse[1] == 0.f && g.diffuse[2] == 0.f);
        assert(g.specular[0] == 0.f && g.roughness == 0.6f);
        // Ineligible pixels stay untouched.
        Fill(t, 0.f);
        g = Sample();
        Reference(Admitted(debug), 3, 3, 0, t, g);
        assert(Same(g, Sample()));
        g = Sample();
        auto off = Admitted(debug);
        off.admitted = 0;
        Reference(off, 3, 3, 1, t, g);
        assert(Same(g, Sample()));
    }
}

static void TestReport() {
    Counters k{};
    k.frames = 600;
    k.captures_oit = 590;
    k.captures_refr = 12;
    k.admitted = 598;
    k.ran = 597;
    k.pixels = 1000;
    k.eligible = 250;
    k.covered = 50;
    k.opacity64 = 1600;
    k.last = Reason::Split;
    char text[256];
    const char *expected =
        "Transparency albedo blend (10s): frames=600 captures=602 oit=590 refr=12 admitted=598 "
        "eligible=25.0% covered=20.0% meanOpacity=0.500 lastReject=split";
    const int n = Report(k, text, sizeof(text));
    assert(!strcmp(text, expected));
    assert(n == int(strlen(expected)));

    char small[16];
    assert(Report(k, small, sizeof(small)) == n);
    assert(strlen(small) == sizeof(small) - 1 && !strncmp(small, expected, sizeof(small) - 1));

    const Counters zero{};
    assert(zero.last == Reason::None);
    Report(zero, text, sizeof(text));
    assert(!strcmp(text, "Transparency albedo blend (10s): frames=0 captures=0 oit=0 refr=0 "
                         "admitted=0 eligible=0.0% covered=0.0% meanOpacity=0.000 "
                         "lastReject=none"));

    const char *labels[] = {"none",   "disabled", "no-slot", "no-pipeline", "signature", "extent",
                            "memory", "order",    "split",   "typed-load",  "failed"};
    for (int i = 0; i <= int(Reason::Failed); ++i)
        assert(!strcmp(Label(Reason(i)), labels[i]));
    assert(!strcmp(Label(Reason(99)), "none"));
}

static void TestStatus() {
    using rrstatus::State;
    // enabled, failed, unavailable, rr_on, rr_failed, rr_retired, fresh_ran
    assert(StatusOf(false, true, true, true, false, false, true) == State::Off);
    assert(StatusOf(false, false, false, false, false, false, false) == State::Off);
    assert(StatusOf(true, true, true, false, false, false, true) == State::ParentOff);
    assert(StatusOf(true, true, false, true, true, false, true) == State::ParentOff);
    assert(StatusOf(true, false, false, true, false, true, true) == State::ParentOff);
    assert(StatusOf(true, true, true, true, false, false, true) == State::Error);
    assert(StatusOf(true, false, true, true, false, false, true) == State::Unavailable);
    assert(StatusOf(true, false, false, true, false, false, true) == State::Active);
    assert(StatusOf(true, false, false, true, false, false, false) == State::Waiting);
    assert(!strcmp(rrstatus::Label(StatusOf(true, false, true, true, false, false, false)),
                   "unavailable"));
    assert(!strcmp(rrstatus::Label(StatusOf(true, false, false, false, false, false, false)),
                   "ON / RR off"));
}

int main() {
    TestAdmit();
    TestTargets();
    TestClassify();
    TestPatchBlend();
    TestDepthStencil();
    TestSignature();
    TestCoverage();
    TestControl();
    TestClamp();
    TestReference();
    TestReport();
    TestStatus();
    puts("PASS: transparency blend admission, targets, blend/depth/signature classification, "
         "coverage extent, control layout, tuning, reference math, report and status");
}
