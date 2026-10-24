#pragma once
#include "rr_binding_probe.h"
#include "rr_compute_state.h"
#include "rr_evaluation_params.h"
namespace rrinputs {
inline bool NativeConstantMatches(const rrbindings::Tracker &tracker, UINT64 list, unsigned reg,
                                  const rrbindings::Value &value,
                                  const rrcompute::Snapshot &native) {
    if (!native.valid || !value.known || !value.buffer || !value.resource ||
        tracker.CurrentLayout(list) != UINT64(native.root.Get()))
        return false;
    auto root = tracker.FindRootConstant(list, reg);
    if (root.known) {
        const auto &argument = native.args[root.parameter];
        if (argument.kind != rrcompute::Kind::CBV)
            return false;
        auto r =
            fg::NativeObject<ID3D12Resource>(reinterpret_cast<ID3D12Resource *>(value.resource));
        if (!r || r->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
            return false;
        const auto address = r->GetGPUVirtualAddress();
        return address && value.byte_offset <= UINT64_MAX - address &&
               address + value.byte_offset == argument.address;
    }
    auto table = tracker.FindConstant(list, reg);
    return table.known && native.args[table.parameter].kind == rrcompute::Kind::Table &&
           native.args[table.parameter].address == table.table;
}
using Evaluation = rreval::Options;
template <class T>
inline void ReadEvalValue(const NVSDK_NGX_Parameter *p, const char *key, T &output) {
    T value{};
    if (NVSDK_NGX_SUCCEED(p->Get(key, &value)))
        output = value;
}
inline Evaluation ReadEvaluation(const NVSDK_NGX_Parameter *p) {
    Evaluation v;
    if (!p)
        return v;
    __try {
        ReadEvalValue(p, NVSDK_NGX_Parameter_OutWidth, v.out_width);
        ReadEvalValue(p, NVSDK_NGX_Parameter_OutHeight, v.out_height);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, v.flags);
        ReadEvalValue(p, NVSDK_NGX_Parameter_PerfQualityValue, v.quality);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, v.width);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, v.height);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, v.color_x);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, v.color_y);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, v.depth_x);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, v.depth_y);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, v.motion_x);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, v.motion_y);
        ReadEvalValue(p, NVSDK_NGX_Parameter_Jitter_Offset_X, v.jitter_x);
        ReadEvalValue(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, v.jitter_y);
        ReadEvalValue(p, NVSDK_NGX_Parameter_MV_Scale_X, v.motion_x_scale);
        ReadEvalValue(p, NVSDK_NGX_Parameter_MV_Scale_Y, v.motion_y_scale);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, v.pre_exposure);
        ReadEvalValue(p, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, v.exposure_scale);
        ReadEvalValue(p, NVSDK_NGX_Parameter_Reset, v.reset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
    return v;
}
} // namespace rrinputs
