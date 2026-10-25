#pragma once
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <array>
#include <cmath>
#include <climits>
#include "rr_resolution_limits.h"
#include "rr_presets.h"
// Older local NGX headers may predate the RR responsivity mask key.
#ifdef NVSDK_NGX_Parameter_DLSSD_ResponsivityMask
#define RR_RESPONSIVITY_MASK_KEY NVSDK_NGX_Parameter_DLSSD_ResponsivityMask
#else
#define RR_RESPONSIVITY_MASK_KEY "DLSSD.ResponsivityMask"
#endif
// Optional masks used by the running water tests (rr_water.h).
#ifdef NVSDK_NGX_Parameter_DLSS_DisocclusionMask
#define RR_DISOCCLUSION_MASK_KEY NVSDK_NGX_Parameter_DLSS_DisocclusionMask
#define RR_DISOCCLUSION_MASK_X_KEY NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X
#define RR_DISOCCLUSION_MASK_Y_KEY NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y
#else
#define RR_DISOCCLUSION_MASK_KEY "DLSS.DisocclusionMask"
#define RR_DISOCCLUSION_MASK_X_KEY "DLSS.DisocclusionMask.Subrect.Base.X"
#define RR_DISOCCLUSION_MASK_Y_KEY "DLSS.DisocclusionMask.Subrect.Base.Y"
#endif
#ifdef NVSDK_NGX_Parameter_AnimatedTextureMask
#define RR_ANIMATED_TEXTURE_MASK_KEY NVSDK_NGX_Parameter_AnimatedTextureMask
#else
#define RR_ANIMATED_TEXTURE_MASK_KEY "AnimatedTextureMask"
#endif
namespace rreval {
struct Options {
    unsigned width = UINT_MAX, height = UINT_MAX, out_width = UINT_MAX, out_height = UINT_MAX,
             color_x = UINT_MAX, color_y = UINT_MAX, depth_x = UINT_MAX, depth_y = UINT_MAX,
             motion_x = UINT_MAX, motion_y = UINT_MAX;
    float jitter_x = NAN, jitter_y = NAN, motion_x_scale = NAN, motion_y_scale = NAN,
          pre_exposure = NAN, exposure_scale = NAN;
    int reset = -1, flags = -1, quality = -1;
};
inline constexpr bool SameCreation(const Options &a, const Options &b) {
    return a.width == b.width && a.height == b.height && a.out_width == b.out_width &&
           a.out_height == b.out_height && a.flags == b.flags && a.quality == b.quality;
}
inline bool Valid(const Options &v) {
    // Native input may include padding beyond the output extent. Creation and
    // evaluation forward independent dimensions; actual resources are checked
    // for subrect containment by the caller before private evaluation.
    if (!rrresolution::Texture(v.width, v.height) ||
        !rrresolution::Texture(v.out_width, v.out_height) || v.flags < 0 || v.quality < 0 ||
        v.quality > 5)
        return false;
    for (const auto origin : {std::array<unsigned, 2>{v.color_x, v.color_y},
                              {v.depth_x, v.depth_y},
                              {v.motion_x, v.motion_y}})
        if (!rrresolution::Fits(RR_TEXTURE2D_LIMIT, RR_TEXTURE2D_LIMIT, origin[0], origin[1],
                                v.width, v.height))
            return false;
    return std::isfinite(v.jitter_x) && std::isfinite(v.jitter_y) && std::abs(v.jitter_x) <= 1 &&
           std::abs(v.jitter_y) <= 1 && std::isfinite(v.motion_x_scale) &&
           std::isfinite(v.motion_y_scale) && v.motion_x_scale != 0 && v.motion_y_scale != 0 &&
           std::isfinite(v.pre_exposure) && v.pre_exposure > 0 && std::isfinite(v.exposure_scale) &&
           v.exposure_scale > 0;
}

// linear_depth: RR reads view-space depth (the weapon depth test, src/rr_weapon_depth.h)
// instead of the game's hardware depth. Nearer is then smaller, so the game's inverted-depth
// flag is cleared for RR.
inline int CreateFlags(int flags, bool linear_depth) {
    return linear_depth ? flags & ~int(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) : flags;
}
inline void CreateParameters(NVSDK_NGX_Parameter &p, const Options &v, unsigned preset = 6,
                             bool linear_depth = false) {
    rrpreset::Set(p, preset);
    p.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p.Set(NVSDK_NGX_Parameter_Width, v.width);
    p.Set(NVSDK_NGX_Parameter_Height, v.height);
    p.Set(NVSDK_NGX_Parameter_OutWidth, v.out_width);
    p.Set(NVSDK_NGX_Parameter_OutHeight, v.out_height);
    p.Set(NVSDK_NGX_Parameter_PerfQualityValue, v.quality);
    p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, CreateFlags(v.flags, linear_depth));
    p.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    p.Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, int(NVSDK_NGX_DLSS_Denoise_Mode_DLUnified));
    p.Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, unsigned(NVSDK_NGX_DLSS_Roughness_Mode_Packed));
    p.Set(NVSDK_NGX_Parameter_Use_HW_Depth, unsigned(linear_depth ? NVSDK_NGX_DLSS_Depth_Type_Linear
                                                                 : NVSDK_NGX_DLSS_Depth_Type_HW));
}
struct Resources {
    ID3D12Resource *color{};
    ID3D12Resource *depth{};
    ID3D12Resource *motion{};
    ID3D12Resource *output{};
    ID3D12Resource *normal{};
    ID3D12Resource *diffuse{};
    ID3D12Resource *specular{};
    ID3D12Resource *sss{};
    ID3D12Resource *before_transparency{};
    ID3D12Resource *responsivity{};
    ID3D12Resource *specular_hit{};
    ID3D12Resource *disocclusion{};
    ID3D12Resource *animated{};
};
inline void EvaluateParameters(NVSDK_NGX_Parameter &p, const Options &v, const Resources &r,
                               float *world_to_view, float *view_to_clip) {
    p.Set(NVSDK_NGX_Parameter_Color, r.color);
    p.Set(NVSDK_NGX_Parameter_Output, r.output);
    p.Set(NVSDK_NGX_Parameter_Depth, r.depth);
    p.Set(NVSDK_NGX_Parameter_MotionVectors, r.motion);
    p.Set(NVSDK_NGX_Parameter_DiffuseAlbedo, r.diffuse);
    p.Set(NVSDK_NGX_Parameter_SpecularAlbedo, r.specular);
    p.Set(NVSDK_NGX_Parameter_GBuffer_Normals, r.normal);
    p.Set(NVSDK_NGX_Parameter_GBuffer_Roughness, static_cast<ID3D12Resource *>(nullptr));
    // Set every frame, including null: never reuse a prior frame's optional guide.
    p.Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, r.specular_hit);
    p.Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide, r.sss);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_Y, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, r.before_transparency);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_Y, 0u);
    p.Set(RR_RESPONSIVITY_MASK_KEY, r.responsivity);
#ifdef NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_X
    p.Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_Y, 0u);
#endif
    p.Set(RR_DISOCCLUSION_MASK_KEY, r.disocclusion);
    p.Set(RR_DISOCCLUSION_MASK_X_KEY, 0u);
    p.Set(RR_DISOCCLUSION_MASK_Y_KEY, 0u);
    p.Set(RR_ANIMATED_TEXTURE_MASK_KEY, r.animated);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, v.jitter_x);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, v.jitter_y);
    p.Set(NVSDK_NGX_Parameter_Reset, 1);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_X, v.motion_x_scale);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_Y, v.motion_y_scale);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, v.width);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, v.height);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, v.color_x);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, v.color_y);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, v.depth_x);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, v.depth_y);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, v.motion_x);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, v.motion_y);
    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
    const char *zero[] = {NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y,
                          NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y,
                          NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y,
                          NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y};
    for (auto key : zero)
        p.Set(key, 0u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, v.pre_exposure);
    p.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, v.exposure_scale);
    p.Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, static_cast<void *>(world_to_view));
    p.Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, static_cast<void *>(view_to_clip));
}
inline std::array<float, 16> WorldToView(const float *position, const float *right, const float *up,
                                         const float *forward) {
    std::array<float, 16> result{};
    const float *axes[] = {right, up, forward};
    for (unsigned col = 0; col < 3; ++col) {
        double translation = 0;
        for (unsigned row = 0; row < 3; ++row) {
            result[row * 4 + col] = axes[col][row];
            translation -= double(position[row]) * axes[col][row];
        }
        result[12 + col] = float(translation);
    }
    result[15] = 1;
    return result;
}
} // namespace rreval
