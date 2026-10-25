// Runs with the native cs_vrrt_resolve root signature, descriptor heaps and
// tables, immediately after that resolve and only while the ray budget test
// raised a lobe's target. u0 is the resolve's own table-bound raw Budget view.
// A threshold in [-16, 17] keeps every budget-chosen code at or below 16, i.e.
// at most one ray per pixel and lobe (see src/rr_vrrt_rate.h).
#ifndef RR_GOVERNOR_LOBES
#define RR_GOVERNOR_LOBES 3
#endif
RWByteAddressBuffer Budget:register(u0);
static const int Positive=17,Negative=-16;
[numthreads(2,1,1)]
void main(uint lobe:SV_GroupThreadID){
 if(((uint(RR_GOVERNOR_LOBES)>>lobe)&1u)==0)return;
 uint bytes;Budget.GetDimensions(bytes);
 if(bytes<32)return;
 int t=asint(Budget.Load(lobe*16));
 if(t==int(0x80000000))return;
 int g=t>Positive?Positive:(t<Negative?Negative:t);
 if(g!=t)Budget.Store(lobe*16,asuint(g));
}
