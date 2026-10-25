#include "../src/rr_resolution_limits.h"
#include "../src/rr_vrrt_rate.h"
// Read exactly the native request/budget/settings/scene inputs, without
// changing native VRRT, its random choices, counters or work queues.
Texture2D<uint> Requests:register(t0);
ByteAddressBuffer Budget:register(t1);
cbuffer NativeSettings:register(b0){uint4 Settings[38];}
cbuffer NativeScene:register(b1){uint4 Scene[792];}
RWByteAddressBuffer Probability:register(u0);
RWTexture2D<uint> GuideStatus:register(u1);

uint PixelInverse(uint2 p,uint2 size,out bool supported,out uint2 extra,out uint2 raw){
 uint request=Requests.Load(int3(p,0));
 uint2 blocks=(size+uint2(31,15))/uint2(32,16);
 uint2 tile=p/uint2(32,16);
 uint2 mirror=blocks-tile-1;
 uint2 edge=min(tile,mirror);
 int ring=int(min(edge.x,edge.y));
 uint packed=0;supported=true;extra=0;raw=0;
 for(uint lobe=0;lobe<2;++lobe){
  int2 limits=asint(Budget.Load2(lobe*16));
  rrvrrt::Rate rate=rrvrrt::InverseRate(request>>(lobe*16),asint(Settings[1].x),asint(Settings[1].y),asint(Settings[1].z),limits.x,limits.y,ring);
  supported=supported&&rate.supported;
  packed|=rate.denominator<<(lobe*8);
  extra[lobe]=rate.extra;raw[lobe]=rate.raw;
 }
 return packed;
}
// Diagnostic only: per lobe, how many pixels got each inverse rate (bucket 0 =
// not requested, 16 = 16 or more), plus the budget limits and rate settings.
// The caller zeroes RateStats before this dispatch; nothing native reads it.
RWByteAddressBuffer RateStats:register(u2);
static const uint RateBuckets=17,RateBase=64,RateLimits=RateBase+2*RateBuckets*4,RateExtra=RateLimits+28;
groupshared uint rateCounts[2*RateBuckets];
// Per lobe: extra rays (final code-16), raw multi-ray potential (raw-16), max raw code.
groupshared uint rateExtra[6];
void CountRates(uint packed,uint2 extra,uint2 raw){
 for(uint lobe=0;lobe<2;++lobe){
  uint denominator=(packed>>(lobe*8))&255;
  InterlockedAdd(rateCounts[lobe*RateBuckets+min(denominator,16u)],1);
  if(denominator==0)continue;
  if(extra[lobe]!=0)InterlockedAdd(rateExtra[lobe],extra[lobe]);
  if(raw[lobe]>16)InterlockedAdd(rateExtra[2+lobe],raw[lobe]-16);
  InterlockedMax(rateExtra[4+lobe],raw[lobe]);
 }
}
bool Convert(uint2 id,uint w,uint h,uint bytes){
 uint x=id.x*2,y=id.y;if(x>=w||y>=h)return false;
 uint rows=w>0?min(32u,(131072u-16u)/(w*2u)):0;
 bool dimensions=w>0&&w<=RR_TEXTURE2D_LIMIT&&(w&1)==0&&h>0&&h<=RR_TEXTURE2D_LIMIT&&rows>0;
 if(dimensions)dimensions=bytes>=((h+rows-1)/rows)*131072;
 dimensions=dimensions&&all(asfloat(Scene[86].xy)==float2(w,h));
 if(!dimensions){GuideStatus[uint2(x,y)]=0;if(x+1<w)GuideStatus[uint2(x+1,y)]=0;return false;}
 uint chunk=y/rows,base=chunk*131072;
 if(x==0&&(y%rows)==0)Probability.Store4(base,uint4(w,h,y,min(rows,h-y)));
 bool valid0,valid1;uint2 extra0,extra1,raw0,raw1;
 uint a=PixelInverse(uint2(x,y),uint2(w,h),valid0,extra0,raw0);
 uint b=PixelInverse(uint2(x+1,y),uint2(w,h),valid1,extra1,raw1);
 if(!valid0)GuideStatus[uint2(x,y)]=0;
 if(!valid1)GuideStatus[uint2(x+1,y)]=0;
 uint offset=base+16+((y%rows)*w+x)*2;
 Probability.Store(offset,a|(b<<16));
 CountRates(a,extra0,raw0);
 if(x+1<w)CountRates(b,extra1,raw1);
 return true;
}
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID,uint index:SV_GroupIndex){
 uint w,h,bytes;Requests.GetDimensions(w,h);Probability.GetDimensions(bytes);
 if(index<2*RateBuckets)rateCounts[index]=0;
 if(index<6)rateExtra[index]=0;
 GroupMemoryBarrierWithGroupSync();
 bool first=Convert(id.xy,w,h,bytes)&&all(id.xy==0);
 GroupMemoryBarrierWithGroupSync();
 if(index<2*RateBuckets&&rateCounts[index]!=0){uint old;RateStats.InterlockedAdd(RateBase+index*4,rateCounts[index],old);}
 if(index<4&&rateExtra[index]!=0){uint old;RateStats.InterlockedAdd(RateExtra+index*4,rateExtra[index],old);}
 if(index>=4&&index<6&&rateExtra[index]!=0){uint old;RateStats.InterlockedMax(RateExtra+index*4,rateExtra[index],old);}
 if(first){
  int4 limits=int4(asint(Budget.Load2(0)),asint(Budget.Load2(16)));
  RateStats.Store4(RateLimits,asuint(limits));
  RateStats.Store3(RateLimits+16,Settings[1].xyz);
 }
}
