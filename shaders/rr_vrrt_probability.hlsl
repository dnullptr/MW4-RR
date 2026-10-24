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

uint PixelInverse(uint2 p,uint2 size,out bool supported){
 uint request=Requests.Load(int3(p,0));
 uint2 blocks=(size+uint2(31,15))/uint2(32,16);
 uint2 tile=p/uint2(32,16);
 uint2 mirror=blocks-tile-1;
 uint2 edge=min(tile,mirror);
 int ring=int(min(edge.x,edge.y));
 uint packed=0;supported=true;
 for(uint lobe=0;lobe<2;++lobe){
  int2 limits=asint(Budget.Load2(lobe*16));
  rrvrrt::Rate rate=rrvrrt::InverseRate(request>>(lobe*16),asint(Settings[1].x),asint(Settings[1].y),asint(Settings[1].z),limits.x,limits.y,ring);
  supported=supported&&rate.supported;
  packed|=rate.denominator<<(lobe*8);
 }
 return packed;
}
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID){
 uint w,h,bytes;Requests.GetDimensions(w,h);Probability.GetDimensions(bytes);
 uint x=id.x*2,y=id.y;if(x>=w||y>=h)return;
 uint rows=w>0?min(32u,(131072u-16u)/(w*2u)):0;
 bool dimensions=w>0&&w<=RR_TEXTURE2D_LIMIT&&(w&1)==0&&h>0&&h<=RR_TEXTURE2D_LIMIT&&rows>0;
 if(dimensions)dimensions=bytes>=((h+rows-1)/rows)*131072;
 dimensions=dimensions&&all(asfloat(Scene[86].xy)==float2(w,h));
 if(!dimensions){GuideStatus[uint2(x,y)]=0;if(x+1<w)GuideStatus[uint2(x+1,y)]=0;return;}
 uint chunk=y/rows,base=chunk*131072;
 if(x==0&&(y%rows)==0)Probability.Store4(base,uint4(w,h,y,min(rows,h-y)));
 bool valid0,valid1;
 uint a=PixelInverse(uint2(x,y),uint2(w,h),valid0);
 uint b=PixelInverse(uint2(x+1,y),uint2(w,h),valid1);
 if(!valid0)GuideStatus[uint2(x,y)]=0;
 if(!valid1)GuideStatus[uint2(x+1,y)]=0;
 uint offset=base+16+((y%rows)*w+x)*2;
 Probability.Store(offset,a|(b<<16));
}
