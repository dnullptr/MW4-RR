// Development masks for running water (src/rr_water.h). Runs at SR time on
// the water classification the guide shader wrote at this frame's prepass.
cbuffer Control:register(b0){uint Width,Height,OutputW,OutputH;uint Serial,ResetHistory,Accepted,SssAccepted;uint Replacement,ColorX,ColorY,Flags;float Responsivity,OutputScale,WaterResponsivity;uint WaterFlags;}
Texture2D<float> Water:register(t0); // 1 on water pixels, 0 elsewhere
// The SSS responsivity mask this frame's SSS resolve wrote; a null view
// unless WaterFlags bit2 says it is merged.
Texture2D<float> SssMask:register(t1);
// RR responsivity mask: SSS values where nonzero, else WaterResponsivity on water.
RWTexture2D<float> ResponsivityOut:register(u0);
// RR disocclusion mask. RR reads it in log scale; 10000 makes it drop history,
// the value RTX Remix uses for animated water.
RWTexture2D<float> Disocclusion:register(u1);
// Stats byte 16: water pixels this frame (GpuFrame::WaterPass zeroes it first).
RWByteAddressBuffer Stats:register(u2);
static const uint FlagResponsivity=1,FlagDiscard=2,FlagSssMask=4;
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 bool inside=id.x<Width&&id.y<Height;
 bool water=false;float sss=0;
 // A rejected submission never loads texels that may not have been written.
 [branch]if(inside&&Accepted!=0){
  water=Water.Load(int3(id.xy,0))>0.5f;
  if((WaterFlags&FlagSssMask)!=0)sss=SssMask.Load(int3(id.xy,0));
 }
 if(inside){
  float responsive=(WaterFlags&FlagResponsivity)!=0&&water?clamp(WaterResponsivity,0.0f,1.0f):0;
  ResponsivityOut[id.xy]=sss!=0?clamp(sss,-1.0f,1.0f):responsive;
  Disocclusion[id.xy]=(WaterFlags&FlagDiscard)!=0&&water?10000.0f:0;
 }
 uint count=WaveActiveCountBits(water);
 if(count!=0&&WaveIsFirstLane()){uint old;Stats.InterlockedAdd(16,count,old);}
}
