// Water responsivity mask (src/rr_water.h). Runs at SR time on the water
// classification the guide shader wrote at this frame's prepass.
cbuffer Control:register(b0){uint Width,Height,OutputW,OutputH;uint Serial,ResetHistory,Accepted,SssAccepted;uint Replacement,ColorX,ColorY,Flags;float Responsivity,Unused,WaterResponsivity;uint WaterFlags;}
// RR responsivity mask: WaterResponsivity on water, else 0.
RWTexture2D<float> ResponsivityOut:register(u0);
// The guide shader's water code/255 (RRWaterCode in rr_convert_guides.hlsl).
RWTexture2D<float> Water:register(u1);
static const uint FlagResponsivity=1;
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 uint code=0;
 // A rejected submission never loads texels that may not have been written.
 [branch]if(Accepted!=0){
  code=(uint)round(saturate(Water[id.xy])*255.0f);
 }
 bool water=(code&1u)!=0;
 float responsive=(WaterFlags&FlagResponsivity)!=0&&water?clamp(WaterResponsivity,0.0f,1.0f):0;
 ResponsivityOut[id.xy]=responsive;
}
