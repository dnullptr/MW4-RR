#include "../src/rr_resolution_limits.h"
// Same native root, heaps and tables. Only proven b1/b3 GPUVAs are temporarily
// changed by the caller and restored after all strips; unknown b2 is untouched.
cbuffer RateFirst:register(b1){uint4 RateHeader;uint4 RateWords0[4095];}
cbuffer RateSecond:register(b3){uint4 RateWords1[4096];}
#ifdef RR_PACKED_FLOAT
Texture2D<float4> nativeLighting:register(t0);
Texture2D<float> nativeWeights:register(t1);
RWTexture2D<float4> color:register(u0);
RWTexture2D<float> weights:register(u1);
#else
Texture2D<uint4> nativeLighting:register(t0);
Texture2D<uint> nativeWeights:register(t1);
RWTexture2D<uint4> color:register(u0);
RWTexture2D<uint> weights:register(u1);
#endif

uint PixelCode(uint2 p){
 uint byteOffset=16+(p.y*RateHeader.x+p.x)*2;
 uint word;
 if(byteOffset<65536){uint i=(byteOffset-16)/4;word=RateWords0[i/4][i%4];}
 else{uint i=(byteOffset-65536)/4;word=RateWords1[i/4][i%4];}
 return (word>>((byteOffset&2)*8))&65535;
}
// RR_NATIVE_LOBE is a lobe mask (1 = first, 2 = second, 3 = both) taken from
// native temporal output (t0, the native spatial input) instead of raw
// current-frame lighting. The game's SSS blur spreads sparse, boosted raw
// diffuse samples into blotches. Its weight bits come along too, matching the
// temporal-only passthrough comparison.
// RR_NATIVE_FILL keeps a lobe's fresh sample, without the inverse-probability
// boost, wherever a ray landed this frame; only unsampled pixels take native.
#ifndef RR_NATIVE_LOBE
#define RR_NATIVE_LOBE 0
#endif
#ifndef RR_NATIVE_FILL
#define RR_NATIVE_FILL 0
#endif
// RR_CAP_RATIO caps a raw sample's luminance, before the inverse-probability
// boost, at that many times the native temporal luminance of its lobe, so a rare
// bright sample cannot be boosted into a splat the SSS blur spreads. Pixels with
// no native history for that lobe are left uncapped.
#ifndef RR_CAP_RATIO
#define RR_CAP_RATIO 0
#endif
float4 UnpackLobe(uint2 packed){
 return float4(f16tof32(packed.x&65535),f16tof32(packed.x>>16),f16tof32(packed.y&65535),f16tof32(packed.y>>16));
}
float4 CapSample(float4 v,uint2 native,bool history){
#if RR_CAP_RATIO
 float3 luma=float3(0.2126,0.7152,0.0722);
 float limit=RR_CAP_RATIO*dot(UnpackLobe(native).rgb,luma),l=dot(v.rgb,luma);
 // NaN or infinite native estimates fail these comparisons and leave v uncapped.
 if(history&&limit>0&&limit<65504&&l>limit){float s=limit/l;v*=float4(s,s,s,s*s);}
#endif
 return v;
}
uint2 ScaleLobe(uint2 packed,bool sampled,uint inverse,uint2 native,bool history,out bool valid){
 float4 v=UnpackLobe(packed);
 valid=sampled&&inverse!=0&&all(v>=0)&&all(v<=65504);
 if(!valid)return 0;
 v=CapSample(v,native,history);
 float f=float(inverse);v=min(v*float4(f,f,f,f*f),65504);
 uint4 h=f32tof16(v);return uint2(h.x|(h.y<<16),h.z|(h.w<<16));
}
// RR_W_SOURCE leaves a lobe's colour raw and boosted but replaces its 4th half,
// which the game appears to use as an occlusion term after this pass, so the
// boost cannot wash it out: 1 = the fresh unboosted value where a ray landed and
// native temporal elsewhere, 2 = native temporal everywhere.
#ifndef RR_W_SOURCE
#define RR_W_SOURCE 0
#endif
uint2 KeepW(uint2 scaled,uint2 raw,bool fresh,uint2 native){
#if RR_W_SOURCE
 uint w=RR_W_SOURCE==1&&fresh?raw.y>>16:native.y>>16;
 return uint2(scaled.x,(scaled.y&65535)|(w<<16));
#else
 return scaled;
#endif
}
uint2 SelectLobe(uint2 raw,bool sampled,uint2 native,out bool fresh){
#if RR_NATIVE_FILL
 float4 v=float4(f16tof32(raw.x&65535),f16tof32(raw.x>>16),f16tof32(raw.y&65535),f16tof32(raw.y>>16));
 fresh=sampled&&all(v>=0)&&all(v<=65504);
#else
 fresh=false;
#endif
 return fresh?raw:native;
}
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID){
 uint w,h;color.GetDimensions(w,h);uint2 p=uint2(id.x,id.y+RateHeader.z);
 if(id.x>=w||id.y>=RateHeader.w||p.y>=h)return;
 // A malformed header cannot be interpreted as a native weighting profile.
 uint rows=w>0?min(32u,(131072u-16u)/(w*2u)):0;
 if(w==0||w>RR_TEXTURE2D_LIMIT||(w&1)!=0||RateHeader.x!=w||RateHeader.y!=h||RateHeader.w==0||RateHeader.w>rows)return;
 uint code=PixelCode(id.xy);
#ifdef RR_PACKED_FLOAT
 uint4 c=asuint(color[p]);uint wt=asuint(weights[p]);
#else
 uint4 c=color[p];uint wt=weights[p];
#endif
#ifdef RR_PACKED_FLOAT
 uint4 nc=asuint(nativeLighting.Load(int3(p,0)));uint nw=asuint(nativeWeights.Load(int3(p,0)));
#else
 uint4 nc=nativeLighting.Load(int3(p,0));uint nw=nativeWeights.Load(int3(p,0));
#endif
 bool a,b;
 uint2 ca=ScaleLobe(c.xy,(wt>>20)!=0,code&255,nc.xy,(nw>>20)!=0,a);
 uint2 cb=ScaleLobe(c.zw,((wt>>8)&4095)!=0,code>>8,nc.zw,((nw>>8)&4095)!=0,b);
 ca=KeepW(ca,c.xy,a,nc.xy);cb=KeepW(cb,c.zw,b,nc.zw);
 uint outWeights=(wt&((a?0xfff00000:0)|(b?0x000fff00:0)))|(nw&255);
#if RR_NATIVE_LOBE&1
 bool fa;ca=SelectLobe(c.xy,(wt>>20)!=0,nc.xy,fa);
 outWeights=(outWeights&~0xfff00000u)|((fa?wt:nw)&0xfff00000u);
#endif
#if RR_NATIVE_LOBE&2
 bool fb;cb=SelectLobe(c.zw,((wt>>8)&4095)!=0,nc.zw,fb);
 outWeights=(outWeights&~0x000fff00u)|((fb?wt:nw)&0x000fff00u);
#endif
#ifdef RR_PACKED_FLOAT
 color[p]=asfloat(uint4(ca,cb));
 weights[p]=asfloat(outWeights);
#else
 color[p]=uint4(ca,cb);
 weights[p]=outWeights;
#endif
}
