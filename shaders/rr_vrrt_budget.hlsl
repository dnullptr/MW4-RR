#include "../src/rr_vrrt_target.h"
// Private copy of cs_vrrt_resolve's cb1 (38 registers) with the per-lobe ray targets in register
// 2 .xy raised by rrvrrt::RaisedTarget. Registers from Words on are written as 0 and never read,
// so a cb1 shorter than the block is never overrun. Scene is read only for register 2, to check
// that the render size is the one the caps were computed for. Dispatch (1,1,1).
cbuffer NativeSettings:register(b0){uint4 Settings[38];}
cbuffer NativeScene:register(b1){uint4 Scene[87];}
cbuffer Limits:register(b2){float Scale;float Cap0;float Cap1;uint Words;uint Width;uint Height;uint Pad0;uint Pad1;}
RWByteAddressBuffer Budget:register(u0);
[numthreads(64,1,1)]
void main(uint i:SV_DispatchThreadID){
 if(i>=38)return;
 uint4 v=0;
 [branch]if(i<Words){
  v=Settings[i];
  [branch]if(i==2&&all(asfloat(Scene[86].xy)==float2(Width,Height)))
   v.xy=asuint(float2(rrvrrt::RaisedTarget(asfloat(v.x),Scale,Cap0),rrvrrt::RaisedTarget(asfloat(v.y),Scale,Cap1)));
 }
 Budget.Store4(i*16,v);
}
