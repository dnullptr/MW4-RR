// Owned MRT sample parity, not a conventional averaged MSAA resolve.
cbuffer Control:register(b0){uint Width,Height,OutputW,OutputH;uint Serial,ResetHistory,Accepted,SssAccepted;uint Replacement,ColorX,ColorY,Flags;}
Texture2DMS<float,4> Producer:register(t0);
RWTexture2D<float> Guide:register(u0);
RWByteAddressBuffer Stats:register(u1);
RWByteAddressBuffer History:register(u2);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float value=0;
 // The source access must remain dominated by this branch in compiled DXIL.
 [branch]if(SssAccepted!=0){
  value=Producer.Load(int2(id.xy>>1),(id.x&1)+2*(id.y&1));
  if(!isfinite(value)||value==-65504.0f||abs(value)>=65504.0f){
   value=0;uint old;
   // Tolerant (Flags bit0): this pixel reads as no SSS and is counted. The
   // gate rejects the frame only when such samples are widespread.
   if((Flags&1)!=0)Stats.InterlockedAdd(12,1,old);
   else{Stats.InterlockedOr(0,1,old);History.InterlockedOr(0,1,old);}
  }
 }
 Guide[id.xy]=value;
}
