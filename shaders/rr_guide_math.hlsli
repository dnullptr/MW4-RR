#ifndef MW4_RR_GUIDE_MATH_HLSLI
#define MW4_RR_GUIDE_MATH_HLSLI

// Conversion primitives only. The caller must establish matching frame,
// camera/normal space, source formats, resource lifetime and special-material
// routing. This include does not access textures or invoke NGX/Streamline.

uint3 RRMaterialArrayAddress(uint2 pixel)
{
    return uint3(pixel >> 1, (pixel.x & 1u) | ((pixel.y & 1u) << 1u));
}

float3 RRDecodeOct(uint2 encoded, float maximum)
{
    float2 xy = float2(encoded) * (2.0 / maximum) - 1.0;
    float z = 1.0 - abs(xy.x) - abs(xy.y);
    float fold = max(-z, 0.0);
    xy += float2(xy.x > 0.0 ? -fold : fold, xy.y > 0.0 ? -fold : fold);
    return normalize(float3(xy, z));
}

float3 RRDecodeSurfaceNormal(uint packed)
{
    return RRDecodeOct(uint2(packed & 255u, (packed >> 8u) & 255u), 255.0);
}

float4 RRDecodeNormalGloss(uint packed)
{
    float3 normal = RRDecodeOct(uint2(packed & 4095u, (packed >> 12u) & 4095u), 4095.0);
    float roughness = 1.0 - float((packed >> 24u) & 127u) / 127.0;
    return float4(normal, roughness);
}

bool RRHasSpecialNormalFlag(uint packed)
{
    // cs_prepass takes a separate branch when bit31 is set (or another
    // depth-dependent condition holds). Decoding alone does not handle it.
    return (packed & 0x80000000u) != 0u;
}

bool RRValidSpecularInputs(float3 materialF0, float roughness, float noV)
{
    return all(isfinite(materialF0)) && all(materialF0 >= 0.0) &&
        isfinite(roughness) && roughness >= 0.0 && roughness <= 1.0 &&
        isfinite(noV) && abs(noV) <= 1.0;
}

float3 RRGameSpecularReflectance(float3 materialF0, float roughness, float noV)
{
    // c8be52d1ee156111 instructions762..796. Same rational fit family as
    // NVIDIA's RR guide, but the observed game omits bias*=saturate(F0.g*50).
    // Preserve that behavior, including nonzero grazing bias for zero F0.
    // Caller must check RRValidSpecularInputs. Do not clamp HDR material RGB.
    float v = abs(noV);
    float v2 = v * v, v3 = v2 * v;
    float alpha = roughness * roughness;
    float alpha3 = alpha * alpha * alpha;
    float biasNumerator = (0.99044 - 1.28514 * v) + (1.29678 - 0.755907 * v) * alpha;
    float biasDenominator = (1.0 + 2.92338 * v + 59.4188 * v3) +
        (20.3225 - 27.0302 * v + 222.592 * v3) * alpha +
        (121.563 + 626.13 * v + 316.627 * v3) * alpha3;
    float scaleNumerator = (0.0365463 + 3.32707 * v) + (9.0632 - 9.04756 * v) * alpha;
    float scaleDenominator = (1.0 + 3.59685 * v2 - 1.36772 * v3) +
        (9.04401 - 16.3174 * v2 + 9.22949 * v3) * alpha +
        (5.56589 + 19.7886 * v2 - 20.2123 * v3) * alpha3;
    return materialF0 * max(scaleNumerator / scaleDenominator, 0.0) + max(biasNumerator / biasDenominator, 0.0);
}

#endif
