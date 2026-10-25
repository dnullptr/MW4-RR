// Runs with the native cs_vrrt_resolve root signature, descriptor heaps and
// tables, immediately after that resolve and only while the ray budget test
// raised a lobe's target. b1 is the resolve's root CBV, which then points at a
// private copy of the game's first 608 bytes followed by these constants; b3 is
// the resolve's own (proven to hold 1392 bytes by ProveRayResolve).
// A threshold t >= -16 gives each pixel at most 1 + max(raw-16, 0) rays, so a
// frame schedules at most W*H + x rays, x = Stats[lobe<<16] (this frame's
// deterministic rays). Any t below -16 becomes -16; INT_MIN is clamped like
// every other value below it (A1). W*H is this frame's: the larger of the
// resolve's b3 register 86 and the armed extent (Area), since the extent can
// change after the CPU's proof (A2). When W*H + x exceeds Limit (capacity -
// slack), or the extent is not a valid one, t becomes 17: with the
// clamp on that allows at most one ray per pixel, with it off it leaves the
// game's own unraised behaviour. Positive thresholds never add rays.
// rrraybudget::GovernedThreshold (rr_ray_budget.h) is this rule on the CPU;
// -16, 17 and 16384 are its NegativeLimit, FallbackThreshold and MaxExtent.
// u0: the Budget the resolve just wrote; lobe l's threshold is its word at l*16.
RWByteAddressBuffer Budget:register(u0);
// t0: the resolve's ray statistics; lobe l's x (the sum of r over the pixels
// asking for r > 0 rays) is its word at l<<16.
ByteAddressBuffer Stats:register(t0);
cbuffer Governor:register(b1){uint4 Game[38];uint Mask;uint Limit0;uint Limit1;uint Area;}
cbuffer Scene:register(b3){uint4 Frame[87];}
static const int Negative=-16,Fallback=17;
// The bits of 16384.0f: a W or H whose bits are larger is negative, above
// 16384, infinite or NaN (compared as integers, whatever the float mode).
static const uint MaxExtentBits=0x46800000u;
[numthreads(2,1,1)]
void main(uint lobe:SV_GroupThreadID){
 if(((Mask>>lobe)&1u)==0)return;
 uint bytes;Budget.GetDimensions(bytes);
 if(bytes<32)return;
 int t=asint(Budget.Load(lobe*16));
 int g=t<Negative?Negative:t;
 uint wb=Frame[86].x,hb=Frame[86].y;
 uint size;Stats.GetDimensions(size);
 if(wb>MaxExtentBits||hb>MaxExtentBits||size<(lobe<<16)+4)g=Fallback;
 else{
  uint area=max(uint(ceil(asfloat(wb)))*uint(ceil(asfloat(hb))),Area);
  uint limit=lobe!=0?Limit1:Limit0;
  uint headroom=limit>area?limit-area:0u;
  if(area>limit||Stats.Load(lobe<<16)>headroom)g=Fallback;
 }
 if(g!=t)Budget.Store(lobe*16,asuint(g));
}
