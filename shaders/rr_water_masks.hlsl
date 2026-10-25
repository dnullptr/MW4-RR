// Development masks for running water (src/rr_water.h). Runs at SR time on
// the water classification the guide shader wrote at this frame's prepass.
cbuffer Control:register(b0){uint Width,Height,OutputW,OutputH;uint Serial,ResetHistory,Accepted,SssAccepted;uint Replacement,ColorX,ColorY,Flags;float Responsivity,OutputScale,WaterResponsivity;uint WaterFlags;}
// The SSS responsivity mask this frame's SSS resolve wrote; a null view
// unless WaterFlags bit2 says it is merged.
Texture2D<float> SssMask:register(t1);
// RR responsivity mask: SSS values where nonzero, else WaterResponsivity on water.
RWTexture2D<float> ResponsivityOut:register(u0);
// RR disocclusion mask. RR reads it in log scale; 10000 makes it drop history,
// the value RTX Remix uses for animated water.
RWTexture2D<float> Disocclusion:register(u1);
// In: the guide shader's water code/255 (RRWaterCode in rr_convert_guides.hlsl).
// Out: 1 on water, 0 elsewhere, which is also RR's animated texture mask.
RWTexture2D<float> Water:register(u2);
// Byte 16: water pixels this frame. Bytes 236-255 (rrwater::PrepassOffset):
// pixels per code bit, in rrwater::Prepass order. GpuFrame::WaterPass zeroes both
// first.
RWByteAddressBuffer Stats:register(u3);
static const uint FlagResponsivity=1,FlagDiscard=2,FlagSssMask=4,FlagOffset=236;
void Count(bool value,uint offset){
 uint count=WaveActiveCountBits(value);
 if(count!=0&&WaveIsFirstLane()){uint old;Stats.InterlockedAdd(offset,count,old);}
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 bool inside=id.x<Width&&id.y<Height;
 uint code=0;float sss=0;
 // A rejected submission never loads texels that may not have been written.
 [branch]if(inside&&Accepted!=0){
  code=(uint)round(saturate(Water[id.xy])*255.0f);
  if((WaterFlags&FlagSssMask)!=0)sss=SssMask.Load(int3(id.xy,0));
 }
 bool water=(code&1u)!=0;
 if(inside){
  float responsive=(WaterFlags&FlagResponsivity)!=0&&water?clamp(WaterResponsivity,0.0f,1.0f):0;
  ResponsivityOut[id.xy]=sss!=0?clamp(sss,-1.0f,1.0f):responsive;
  // Discard strength (rrwater::DiscardLevel, flag bits 8-15): 10000^(level/255).
  uint level=(WaterFlags>>8)&255u;
  float drop=level!=0?pow(10000.0f,level/255.0f):0;
  Disocclusion[id.xy]=(WaterFlags&FlagDiscard)!=0&&water?drop:0;
  Water[id.xy]=water?1.0f:0.0f;
 }
 Count(water,16);
 // rrwater::Prepass: special, water flag, override, NormalGloss bit 31, and a
 // special pixel the guide shader rejected (no converted bit 32).
 Count((code&2u)!=0,FlagOffset);
 Count((code&4u)!=0,FlagOffset+4);
 Count((code&8u)!=0,FlagOffset+8);
 Count((code&16u)!=0,FlagOffset+12);
 Count((code&2u)!=0&&(code&32u)==0,FlagOffset+16);
}
