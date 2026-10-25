// Transparency albedo blend (NVIDIA RR guide 3.4.1). One thread per render pixel.
// Coverage is the owned 4x MSAA transmittance T (cleared to 1, multiplied by each
// captured composite's dest-blend factor). Parity mapping matches the glass unpack.
// The CPU mirror is rrblend::Reference in src/rr_transparency_blend_policy.h.
cbuffer Control : register(b0) {
    uint Admitted, Width, Height, Flags; // Flags bit0 blend sky (status 2), bit1 blend roughness,
                                         // bit2 3x3 tent filter, bit3 debug (opacity as diffuse),
                                         // bit4 pattern (diffuse target from Color; set only
                                         // when t2 is a real SRV of RR's input colour)
    float4 Diffuse;                      // rgb = diffuse target
    float4 Params;                       // x specular target, y roughness target, z strength, w floor
    uint4 Reserved;                      // unused
    uint4 Zero;                          // always 0: source for clearing Stats
};
Texture2DMS<float4, 4> Coverage : register(t0);
Texture2D<uint> Status : register(t1);
Texture2D<float4> Color : register(t2); // RR's render-resolution input colour, origin (0, 0)
RWTexture2D<float4> NormalRoughness : register(u0);
RWTexture2D<float4> DiffuseAlbedo : register(u1);
RWTexture2D<float4> SpecularAlbedo : register(u2);
RWByteAddressBuffer Stats : register(u3); // [0] ran [4] eligible [8] covered [12] opacity*64
groupshared uint g_eligible, g_covered, g_opacity;
static const float3 kLuma = float3(0.299, 0.587, 0.114);
// Opacity of render pixel q. Saturated per tap: NaN reads as clear, and a value
// outside [0, 1] cannot leak through the weighted sum. The subtraction is precise
// so that it carries no fast-math (no-NaN) flag into the saturate that maps NaN to 0.
float3 Opacity(int2 q) {
    precise float3 o = 1.0 - Coverage.Load(q >> 1, (q.x & 1) | ((q.y & 1) << 1)).rgb;
    return saturate(o);
}
// Pattern diffuse target of render pixel p: Diffuse * colour / M, saturated, where M is the
// mean luma of the 5x5 colour box around p over the taps inside the extent. A non-finite
// colour or M, or M below 1e-4 (too dark to scale by), gives Diffuse. Precise, so that no
// fast-math (no-NaN/no-Inf) flag lets the compiler drop the finiteness test.
float3 PatternTarget(int2 p) {
    precise float sum = 0.0;
    float count = 0.0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) [unroll] for (int dx = -2; dx <= 2; ++dx) {
        const int2 q = p + int2(dx, dy);
        if (q.x >= 0 && q.y >= 0 && q.x < int(Width) && q.y < int(Height)) {
            sum += dot(Color.Load(int3(q, 0)).rgb, kLuma);
            count += 1.0;
        }
    }
    precise float m = sum / count;
    precise float3 color = Color.Load(int3(p, 0)).rgb;
    if (!all(isfinite(color)) || !isfinite(m) || m < 1e-4)
        return Diffuse.rgb;
    precise float3 target = Diffuse.rgb * color / m;
    return saturate(target);
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    // Submission-time gate. Admitted lives in an upload-heap CBV that the CPU writes, and every
    // wave loads it on its own, so both barriers stay unconditional: a word rewritten while the
    // dispatch is in flight must not leave part of a group waiting at a barrier the rest skipped.
    // Unadmitted, no thread reads or writes a resource (inside is false, Stats stays untouched).
    const bool admitted = Admitted == 1u;
    if (gi == 0) {
        g_eligible = 0;
        g_covered = 0;
        g_opacity = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const bool inside = admitted && id.x < Width && id.y < Height;
    const uint status = inside ? Status[id.xy] : 0u;
    const bool eligible = status == 1u || status == 3u || (status == 2u && (Flags & 1u));
    float3 c = 0;
    if (eligible) {
        const int2 p = int2(id.xy);
        float3 o;
        if (Flags & 4u) {
            // 1-2-1 x 1-2-1 tent over p + [-1, 1]^2, normalized over the taps inside the extent.
            o = 0;
            float total = 0.0;
            [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) {
                const int2 q = p + int2(dx, dy);
                if (q.x >= 0 && q.y >= 0 && q.x < int(Width) && q.y < int(Height)) {
                    const float w = (dx != 0 ? 1.0 : 2.0) * (dy != 0 ? 1.0 : 2.0);
                    o += w * Opacity(q);
                    total += w;
                }
            }
            o /= total;
        } else {
            o = Opacity(p);
        }
        c = saturate(saturate((o - Params.w) / (1.0 - Params.w)) * Params.z);
    }
    const bool covered = eligible && max(c.r, max(c.g, c.b)) > 0.0;
    const bool debug = eligible && (Flags & 8u);
    if (covered || debug) {
        float4 d = DiffuseAlbedo[id.xy];
        float4 s = SpecularAlbedo[id.xy];
        if (covered) {
            d.rgb = lerp(d.rgb, (Flags & 16u) ? PatternTarget(int2(id.xy)) : Diffuse.rgb, c);
            s.rgb = lerp(s.rgb, Params.xxx, c);
            if (Flags & 2u) {
                float4 n = NormalRoughness[id.xy];
                n.w = lerp(n.w, Params.y, dot(c, kLuma));
                NormalRoughness[id.xy] = n;
            }
        }
        // Debug: opacity as diffuse, specular black; uncovered eligible pixels show black. It runs
        // after the normal blend, so roughness is still blended here when the roughness flag is
        // set, as in rrblend::Reference.
        if (debug) {
            d.rgb = c;
            s.rgb = 0;
        }
        DiffuseAlbedo[id.xy] = d;
        SpecularAlbedo[id.xy] = s;
    }
    if (eligible)
        InterlockedAdd(g_eligible, 1u);
    if (covered) {
        InterlockedAdd(g_covered, 1u);
        // At most 64 per pixel: the uint32 sum is exact up to 2^26 covered pixels (8192x8192),
        // well above 4K (3840x2160).
        InterlockedAdd(g_opacity, uint(round(dot(c, kLuma) * 64.0)));
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        if (admitted && id.x == 0 && id.y == 0)
            Stats.Store(0, 1u);
        if (g_eligible) Stats.InterlockedAdd(4, g_eligible);
        if (g_covered) Stats.InterlockedAdd(8, g_covered);
        if (g_opacity) Stats.InterlockedAdd(12, g_opacity);
    }
}
