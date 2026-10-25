// Sheet motion test (development, src/rr_sheet_motion.h). Where the glass capture's
// coverage says the transparency is at least Threshold opaque, and a surface at view
// depth Distance would be in front of the depth RR has, RR gets that surface's depth and
// its motion rebuilt from this frame's and the previous frame's camera. Elsewhere, and
// whenever the coverage was not admitted, RR's motion is copied unchanged and its depth
// is left as it is. The game's motion is only read.
cbuffer Control:register(b0){
 uint Width,Height,MotionX,MotionY,Flags,Pad0,Pad1,Pad2;
 float Distance,Threshold,InvScaleX,InvScaleY,AX,BX,AY,BY;
 float4 R0,R1,R2,R3; // view position (current) -> previous clip, row-vector
}
// The glass capture's control buffer: word 0 is 1 only when this frame's coverage was
// captured in submission order (written at submission).
cbuffer Coverage:register(b1){uint Admitted;}
Texture2D<float2> Motion:register(t0);
// Transparency transmittance in rgb. Each texel holds 2x2 render pixels as its four samples.
Texture2DMS<float4,4> Transparency:register(t1);
RWTexture2D<float2> Output:register(u0);
// RR's linear depth (weapon depth copy), changed in place.
RWTexture2D<float> Depth:register(u1);
static const uint CoverageFlag=1;
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float2 mv=Motion.Load(int3(id.xy+uint2(MotionX,MotionY),0));
 [branch]if((Flags&CoverageFlag)!=0&&Admitted==1){
  uint w,h,n;
  Transparency.GetDimensions(w,h,n);
  uint2 texel=id.xy>>1;
  if(all(texel<uint2(w,h))){
   float3 t=saturate(Transparency.Load(int2(texel),(id.x&1)|((id.y&1)<<1)).rgb);
   float opacity=1-(t.r+t.g+t.b)/3;
   float z=Depth[id.xy];
   [branch]if(opacity>=Threshold&&Distance<z){
    float2 ndc=float2((id.x+0.5)/Width*2-1,1-(id.y+0.5)/Height*2);
    float3 v=Distance*float3(ndc.x*AX+BX,ndc.y*AY+BY,1);
    float4 c=v.x*R0+v.y*R1+v.z*R2+R3;
    if(c.w>0){
     float2 p=c.xy/c.w;
     float2 moved=float2((p.x*0.5+0.5)*Width,(0.5-p.y*0.5)*Height)-(float2(id.xy)+0.5);
     mv=moved*float2(InvScaleX,InvScaleY);
     Depth[id.xy]=Distance;
    }
   }
  }
 }
 Output[id.xy]=mv;
}
