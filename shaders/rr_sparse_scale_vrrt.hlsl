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
// RR_NATIVE_LOBE 1 or 2 takes that lobe from native temporal output (t0, the
// native spatial input) instead of raw current-frame lighting. One lobe is
// diffuse and one specular; native diffuse keeps the game's SSS blur from
// spreading sparse raw samples into blotches. Its weight bits come along too,
// matching the temporal-only passthrough comparison.
#ifndef RR_NATIVE_LOBE
#define RR_NATIVE_LOBE 0
#endif
uint2 ScaleLobe(uint2 packed,bool sampled,uint inverse,out bool valid){
 float4 v=float4(f16tof32(packed.x&65535),f16tof32(packed.x>>16),f16tof32(packed.y&65535),f16tof32(packed.y>>16));
 valid=sampled&&inverse!=0&&all(v>=0)&&all(v<=65504);
 if(!valid)return 0;
 float f=float(inverse);v=min(v*float4(f,f,f,f*f),65504);
 uint4 h=f32tof16(v);return uint2(h.x|(h.y<<16),h.z|(h.w<<16));
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
 bool a,b;
 uint2 ca=ScaleLobe(c.xy,(wt>>20)!=0,code&255,a);
 uint2 cb=ScaleLobe(c.zw,((wt>>8)&4095)!=0,code>>8,b);
#ifdef RR_PACKED_FLOAT
 uint4 nc=asuint(nativeLighting.Load(int3(p,0)));uint nw=asuint(nativeWeights.Load(int3(p,0)));
#else
 uint4 nc=nativeLighting.Load(int3(p,0));uint nw=nativeWeights.Load(int3(p,0));
#endif
 uint outWeights=(wt&((a?0xfff00000:0)|(b?0x000fff00:0)))|(nw&255);
#if RR_NATIVE_LOBE==1
 ca=nc.xy;outWeights=(outWeights&~0xfff00000u)|(nw&0xfff00000u);
#elif RR_NATIVE_LOBE==2
 cb=nc.zw;outWeights=(outWeights&~0x000fff00u)|(nw&0x000fff00u);
#endif
#ifdef RR_PACKED_FLOAT
 color[p]=asfloat(uint4(ca,cb));
 weights[p]=asfloat(outWeights);
#else
 color[p]=uint4(ca,cb);
 weights[p]=outWeights;
#endif
}
