#include "rr_guide_math.hlsli"

// Inputs must be private copies from the same validated prepass/SR submission.
// Caller must establish the current per-pixel parity addressing. This shader does
// not prove frame association or manage resource states or RR evaluation.
Texture2DArray<float3> MaterialF0 : register(t0);
Texture2DArray<float3> MaterialDiffuse : register(t1);
Texture2DArray<uint> NormalGloss : register(t2);
Texture2D<uint4> PackedSurface : register(t3);
cbuffer PrepassSettings : register(b0) { uint4 Settings[8]; }
cbuffer SceneConstants : register(b1) { float4 Scene[776]; }
RWTexture2D<float4> NormalRoughness : register(u0);
RWTexture2D<float4> DiffuseAlbedo : register(u1);
RWTexture2D<float4> SpecularAlbedo : register(u2);
// 0=padding, 1=ordinary, 2=sky, 3=special, 4=invalid/unsupported.
// A runtime consumer must reject a packet containing status4.
RWTexture2D<uint> Status : register(u3);

// FXC's default floating-point optimization can remove isfinite checks on
// otherwise-unused camera values (e.g. sky). Integer exponent tests retain the
// validation even without /Gis. NaN/Inf inputs must reject the whole packet.
bool RRFinite(float x) { return (asuint(x)&0x7f800000u)!=0x7f800000u; }
bool RRFinite3(float3 x) { return all((asuint(x)&0x7f800000u)!=0x7f800000u); }
bool RRFinite4(float4 x) { return all((asuint(x)&0x7f800000u)!=0x7f800000u); }

bool RRConversionLayout(uint2 outputSize)
{
    uint w,h,l;MaterialF0.GetDimensions(w,h,l);
    if(l!=4 || w*2!=outputSize.x || h*2!=outputSize.y)return false;
    MaterialDiffuse.GetDimensions(w,h,l);
    if(l!=4 || w*2!=outputSize.x || h*2!=outputSize.y)return false;
    NormalGloss.GetDimensions(w,h,l);
    if(l!=4 || w*2!=outputSize.x || h*2!=outputSize.y)return false;
    PackedSurface.GetDimensions(w,h);
    if(any(uint2(w,h)!=outputSize))return false;
    if(Settings[2].z!=1 || Settings[0].z!=1 || Settings[7].z!=0)return false;
    float4 extent=Scene[86];
    if(!RRFinite4(extent) || any(extent.xy<=0) || any(extent.xy>float2(outputSize)) ||
       any(floor(extent.xy)!=extent.xy) || any(abs(extent.xy*extent.zw-1)>1e-5))return false;
    return RRFinite3(Scene[148].xyz) && RRFinite3(Scene[149].xyz) && RRFinite3(Scene[150].xyz) &&
           RRFinite3(Scene[167].xyz) && RRFinite3(Scene[168].xyz) && RRFinite3(Scene[169].xyz);
}

void RRWriteEmpty(uint2 p,uint status,float roughness)
{
    NormalRoughness[p]=float4(0,0,0,roughness);
    DiffuseAlbedo[p]=0;SpecularAlbedo[p]=0;Status[p]=status;
}

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w,h;NormalRoughness.GetDimensions(w,h);
    uint2 size=uint2(w,h),p=id.xy;
    if(any(p>=size))return;
    uint dw,dh,sw,sh,tw,th;DiffuseAlbedo.GetDimensions(dw,dh);SpecularAlbedo.GetDimensions(sw,sh);Status.GetDimensions(tw,th);
    // No out-of-bounds UAV writes, even for a malformed caller. The caller must
    // additionally reject unequal output dimensions before dispatch.
    if(p.x>=dw || p.y>=dh || p.x>=sw || p.y>=sh || p.x>=tw || p.y>=th)return;
    RRWriteEmpty(p,4,0);
    if(!RRConversionLayout(size))return;
    if(any(float2(p)>=Scene[86].xy)){RRWriteEmpty(p,0,0);return;}
    uint4 surface=PackedSurface.Load(int3(p,0));float depth=asfloat(surface.x);
    // NVIDIA's RR guide: sky diffuse albedo 0.5, specular albedo and normal 0.
    // Zero sky albedo smears thin foliage and wire edges against the sky.
    if(depth==0 || surface.x==0x7f7fffffu){
        RRWriteEmpty(p,2,1);DiffuseAlbedo[p]=float4(0.5,0.5,0.5,1);
#if RR_GUIDE_TEST==7 || RR_GUIDE_TEST==8
        // Sky as a surface: a normal facing the camera instead of 0, so RR cannot
        // single sky pixels out by their normal. Sky uses the primary camera rows.
        float2 skyClip=1-2*(float2(p)+0.5)*Scene[86].zw;
        float3 skyRay=Scene[148].xyz*skyClip.x+Scene[149].xyz*skyClip.y+Scene[150].xyz;
        float skyLength2=dot(skyRay,skyRay);
        if(RRFinite(skyLength2) && skyLength2>=1e-20)NormalRoughness[p]=float4(-skyRay*rsqrt(skyLength2),1);
#endif
        return;
    }
    if(!RRFinite(depth) || depth<=0 || depth>1)return;
    bool alternate=(surface.w&1u)!=0, special=(surface.w&2u)!=0, depthOverride=(surface.w&8u)!=0;
    if(alternate!=(depth>63.0/64.0) || (depthOverride&&!special))return;
    float roughness=asfloat(surface.z);
    if(!RRFinite(roughness) || roughness<0 || roughness>1 || (special&&roughness!=0))return;
    uint3 address=RRMaterialArrayAddress(p);
    float3 normal=depthOverride?RRDecodeSurfaceNormal(surface.y):RRDecodeNormalGloss(NormalGloss.Load(int4(address,0))).xyz;
    float3 diffuse=0,materialF0=0;
    if(special){materialF0=1;}
    else{
        if(Settings[1].y!=0 || Settings[0].x!=0)diffuse=MaterialDiffuse.Load(int4(address,0));
        if(Settings[1].x!=0)materialF0=MaterialF0.Load(int4(address,0));
    }
    if(!RRFinite3(diffuse) || !RRFinite3(materialF0) || any(diffuse<0) || any(materialF0<0))return;
    float2 clip=1-2*(float2(p)+0.5)*Scene[86].zw;
    float3 rx=alternate?Scene[167].xyz:Scene[148].xyz;
    float3 ry=alternate?Scene[168].xyz:Scene[149].xyz;
    float3 rz=alternate?Scene[169].xyz:Scene[150].xyz;
    float3 ray=rx*clip.x+ry*clip.y+rz;
    float length2=dot(ray,ray);
    if(!RRFinite(length2) || length2<1e-20)return;
    ray*=rsqrt(length2);
    // Development guide tests for thin-geometry noise (rr_guide_test.h). 0 = normal guides.
#ifndef RR_GUIDE_TEST
#define RR_GUIDE_TEST 0
#endif
#if RR_GUIDE_TEST==1
    if(dot(normal,ray)>0)normal=-normal; // back faces (e.g. foliage undersides) face the camera
#elif RR_GUIDE_TEST==3
    if(!special)diffuse=0.5;
#elif RR_GUIDE_TEST==5
    if(!special)roughness=0.5;
#elif RR_GUIDE_TEST==6
    // Every guide flat: only depth, motion and color still vary per pixel.
    if(!special){diffuse=0.5;roughness=0.5;}
#endif
    float3 specular=RRGameSpecularReflectance(materialF0,roughness,dot(normal,ray));
    if(!RRFinite3(specular))return;
#if RR_GUIDE_TEST==4 || RR_GUIDE_TEST==6
    specular=0;
#endif
#if RR_GUIDE_TEST==2 || RR_GUIDE_TEST==6
    normal=-ray; // after the specular guide, so it keeps the real view angle
#endif
    NormalRoughness[p]=float4(normal,roughness);
    DiffuseAlbedo[p]=float4(diffuse,1);
    SpecularAlbedo[p]=float4(specular,1);
    Status[p]=special?3u:1u;
}
