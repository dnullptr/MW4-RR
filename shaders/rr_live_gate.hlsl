#include "../src/rr_spatial_fallback.h"
#include "../src/rr_gate_stats.h"
// Private full-frame presentation gate. Persistent state is initialized once
// outside replayable game recordings. A replayed reset must never clear it.
cbuffer Control:register(b0){uint RenderW,RenderH,OutputW,OutputH;uint Serial,ResetHistory,Accepted,Reserved;uint Replacement,ColorX,ColorY,Flags;float ResponsivityStrength,OutputScale;uint2 Unused;float ContrastKnee;uint3 Reserved2;}
Texture2D<uint> Status:register(t0);
Texture2D<float4> Reconstructed:register(t1);
Texture2D<float4> CurrentColor:register(t2);
RWTexture2D<float4> GameOutput:register(u0);
// Stats bytes: 0 bad frame, 4 copied frame, 8 isolated guide pixels, 12 sanitized SSS samples,
// 16 water pixels (rr_water_masks.hlsl), 20..63 hit distance statistics (rr_hit_condition.hlsl),
// 64..227 ray density (rr_vrrt_probability.hlsl), then rr_gate_stats.h: 228 non-finite color
// pixels RR receives, 232 non-finite pixels of the RR output Display shows (none on a frame
// it rejects or blocks), 236..255 water prepass flags (rr_water_masks.hlsl).
RWByteAddressBuffer Stats:register(u1);
RWByteAddressBuffer History:register(u2); // blocked history, latest serial
// Flags bit0: tolerate isolated unusable guide pixels. Each such pixel keeps
// its empty guides (as sky does) and is counted; the frame is rejected only
// when they are widespread, which is how a whole-frame layout failure shows.
static const uint TolerateIsolated=1;
uint IsolatedLimit(){return max(1u,(RenderW*RenderH)/64);}
bool Widespread(){return Stats.Load(8)>IsolatedLimit()||Stats.Load(12)>IsolatedLimit();}
// Exponent test: fxc may drop isnan/isfinite.
bool Finite3(float3 c){return all((asuint(c)&0x7f800000u)!=0x7f800000u);}
[numthreads(1,1,1)]void BeginFrame(uint3 id:SV_DispatchThreadID){
 uint last=History.Load(4);
 bool fresh=Serial!=0&&Serial>last;
 bool ordered=ResetHistory!=0||(last!=0&&Serial==last+1);
 uint bad=(!fresh||!ordered||Accepted==0)?1u:0u;
 uint blocked=History.Load(0);
 if(fresh){History.Store(4,Serial);if(ResetHistory!=0)blocked=0;}
 if(bad!=0)blocked=1;
 History.Store(0,blocked);
 // One unconditional write also avoids FXC dropping an initial zero store
 // when later branches conditionally overwrite the same raw buffer address.
 Stats.Store4(0,uint4(bad,0,0,0));
 // Not 16..227: the water, hit distance and density passes zero those. The density pass's
 // zero copy covers 228..255 too, from the slot's pre list, which runs before this.
 Stats.Store2(RR_STATS_NON_FINITE_COLOR,uint2(0,0));
}
[numthreads(8,8,1)]void Validate(uint3 id:SV_DispatchThreadID){
 if(id.x>=RenderW||id.y>=RenderH)return;
 uint old;
 // Counted only: the game's color (slot 10) at its subrect, as Display's fallback reads it.
 float3 color=CurrentColor.Load(int3(ColorX+id.x,ColorY+id.y,0)).rgb;
 if(!Finite3(color))Stats.InterlockedAdd(RR_STATS_NON_FINITE_COLOR,1,old);
 uint status=Status.Load(int3(id.xy,0));
 if(status>=1&&status<=3)return;
 if(status==4&&(Flags&TolerateIsolated)!=0){Stats.InterlockedAdd(8,1,old);return;}
 Stats.InterlockedOr(0,1,old);History.InterlockedOr(0,1,old);
}
[numthreads(8,8,1)]void Display(uint3 id:SV_DispatchThreadID){
 if(id.x>=OutputW||id.y>=OutputH)return;
 bool widespread=Widespread();
 bool rejected=Stats.Load(0)!=0||widespread,blocked=History.Load(0)!=0;
 if(widespread&&id.x==0&&id.y==0){uint old;History.InterlockedOr(0,1,old);}
 if(rejected||blocked){
  if(!rrspatial::UseFallback(Replacement!=0,rejected,blocked))return;
  rrspatial::Axis x=rrspatial::Coordinate(id.x,RenderW,OutputW);
  rrspatial::Axis y=rrspatial::Coordinate(id.y,RenderH,OutputH);
  float3 a=lerp(CurrentColor.Load(int3(ColorX+x.first,ColorY+y.first,0)).rgb,
                CurrentColor.Load(int3(ColorX+x.second,ColorY+y.first,0)).rgb,x.fraction);
  float3 b=lerp(CurrentColor.Load(int3(ColorX+x.first,ColorY+y.second,0)).rgb,
                CurrentColor.Load(int3(ColorX+x.second,ColorY+y.second,0)).rgb,x.fraction);
  GameOutput[id.xy]=float4(lerp(a,b,y.fraction),1);
  return;
 }
 float3 color=Reconstructed.Load(int3(id.xy,0)).rgb;
 if(!Finite3(color)){uint old;Stats.InterlockedAdd(RR_STATS_NON_FINITE_OUTPUT,1,old);}
 // Contrast test (rr_contrast_test.h ExpandFactor, SeriesLimit 0.01, MaxExponent 16): undo
 // the compression of RR's input, before the color scale test's factor. 0 = off.
 float t=max(max(color.r,color.g),color.b);
 if(ContrastKnee>0&&t>0){float e=min(t/ContrastKnee,16.0);color*=e<0.01?1+e*(0.5+e/6):(exp(e)-1)/e;}
 // Color scale test (rr_color_scale.h): undo the scale on RR's input. 0 = off.
 if(OutputScale>0)color*=OutputScale;
 if(ContrastKnee>0)color=min(color,65504.0);
 GameOutput[id.xy]=float4(color,1);
 if(id.x==0&&id.y==0)Stats.Store(4,1);
}
