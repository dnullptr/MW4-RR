// Refraction dropout (development test, src/rr_refraction_dropout.h). NVIDIA RR
// guide 3.5, "Resampling a noisy image": inside the game's refraction coverage
// a pixel of RR's color is set to 0 with probability P = Strength * opacity and
// the survivors are divided by 1 - P, so the average radiance is unchanged. A
// blue-noise threshold, shifted every frame, picks the survivors. Elsewhere, and
// whenever the coverage was not admitted, the color is copied unchanged. The
// game's color is only read.
// Limit: the copy format's largest finite value; scaled survivors are clamped to it.
cbuffer Control:register(b0){uint Width,Height,ColorX,ColorY,Offset,Flags;float Strength,Limit;}
// The glass capture's control buffer: word 0 is 1 only when this frame's
// coverage was captured in submission order (written at submission).
cbuffer Coverage:register(b1){uint Admitted;}
// Alpha is passed through unchanged.
Texture2D<float4> Color:register(t0);
// Refraction transmittance in alpha. Each texel holds 2x2 render pixels as its
// four samples.
Texture2DMS<float4,4> Refraction:register(t1);
RWTexture2D<float4> Output:register(u0);
// RR's color before transparency, changed in place with the same pixels.
RWTexture2D<float3> BeforeTransparency:register(u1);
static const uint BlueNoise[8192]={
#include "rr_blue_noise.inc"
};
static const uint CoverageFlag=1,BeforeTransparencyFlag=2,MaskFlag=4;
float Opacity(int2 q,uint2 extent){
 uint2 texel=uint2(q)>>1;
 if(any(texel>=extent))return 0;
 uint s=(uint(q.x)&1)|((uint(q.y)&1)<<1);
 return 1-saturate(Refraction.Load(int2(texel),s).a);
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float4 color=Color.Load(int3(id.xy+uint2(ColorX,ColorY),0));
 float3 c=color.rgb;
 // 3x3 tent of the opacity: a smooth probability across the 2-bit alpha steps.
 float m=0;
 [branch]if((Flags&CoverageFlag)!=0&&Admitted==1){
  uint w,h,n;
  Refraction.GetDimensions(w,h,n);
  float sum=0,weight=0;
  [unroll]for(int y=-1;y<=1;++y)
   [unroll]for(int x=-1;x<=1;++x){
    int2 q=int2(id.xy)+int2(x,y);
    if(any(q<0)||q.x>=int(Width)||q.y>=int(Height))continue;
    float k=float((2-abs(x))*(2-abs(y)));
    sum+=k*Opacity(q,uint2(w,h));
    weight+=k;
   }
  m=sum/weight;
 }
 if((Flags&MaskFlag)!=0){
  float y=dot(c,float3(0.2126,0.7152,0.0722));
  Output[id.xy]=float4(lerp(c,float3(2*y,0,0),m),color.a);
  return;
 }
 float p=m*clamp(Strength,0.0,0.875);
 [branch]if(!(p>0)){
  Output[id.xy]=color;
  return;
 }
 uint2 t=(id.xy+uint2(Offset&0xffff,Offset>>16))&127;
 uint i=t.y*128+t.x;
 uint rank=(BlueNoise[i>>1]>>((i&1)*16))&0xffff;
 float scale=(float(rank)+0.5)/16384.0>=p?1/(1-p):0;
 // Dropped: 0 (also for an infinite input). Kept: clamped below overflow.
 Output[id.xy]=float4(scale>0?min(c*scale,Limit):0,color.a);
 if((Flags&BeforeTransparencyFlag)!=0){
  float3 b=BeforeTransparency[id.xy];
  BeforeTransparency[id.xy]=scale>0?min(b*scale,65024.0):0;
 }
}
