// Runs with the native cs_vrrt_resolve root signature, descriptor heaps and
// tables, immediately after that resolve and only while the ray budget test
// raised a lobe's target. b1 is the resolve's root CBV, which then points at a
// private copy of the game's first 608 bytes followed by these constants.
// A threshold t >= -16 gives each pixel at most 1 + max(raw-16, 0) rays, so a
// frame schedules at most W*H + x rays, x = Stats[lobe<<16] (this frame's
// deterministic rays). Below -16 (INT_MIN included) t becomes -16. When x
// exceeds the CPU's headroom (capacity - W*H - slack) t becomes 17: with the
// clamp on that allows at most one ray per pixel, with it off it leaves the
// game's own unraised behaviour. Positive thresholds never add rays.
RWByteAddressBuffer Budget:register(u0);
ByteAddressBuffer Stats:register(t0);
cbuffer Governor:register(b1){uint4 Game[38];uint Mask;uint Headroom0;uint Headroom1;uint Pad;}
static const int Negative=-16,Fallback=17;
[numthreads(2,1,1)]
void main(uint lobe:SV_GroupThreadID){
 if(((Mask>>lobe)&1u)==0)return;
 uint bytes;Budget.GetDimensions(bytes);
 if(bytes<32)return;
 int t=asint(Budget.Load(lobe*16));
 int g=t<Negative?Negative:t;
 uint size;Stats.GetDimensions(size);
 if(size<(lobe<<16)+4)g=Fallback;
 else if(Stats.Load(lobe<<16)>(lobe!=0?Headroom1:Headroom0))g=Fallback;
 if(g!=t)Budget.Store(lobe*16,asuint(g));
}
