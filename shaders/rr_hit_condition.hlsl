#include "../src/rr_hit_condition_math.h"
// Conditions RR's copy of the game's specular hit distance in place (src/rr_hit_condition.h).
// Runs before the native temporal denoiser on the guide recording, while its t5 holds this
// frame's raw lighting weights: a pixel whose lobe-1 bits show a fresh specular ray and whose
// raw distance is 0 is a miss; one without a ray is a hole. Distance holds the raw copy too,
// so only changed pixels are written.
cbuffer Control:register(b0){uint Width,Height,Mode,Reserved;}
Texture2D<float> Raw:register(t0); // the raw copy, for neighbour reads
#ifdef RR_FLOAT_WEIGHTS
Texture2D<float> Weights:register(t1);
#else
Texture2D<uint> Weights:register(t1);
#endif
Texture2D<uint> Status:register(t2); // guide conversion: 1 ordinary, 2 sky, 3 special
Texture2D<float4> NormalRoughness:register(t3);
Texture2D<uint4> PackedSurface:register(t4); // .x device depth, .w class bits
RWTexture2D<float> Distance:register(u0);
RWByteAddressBuffer Stats:register(u1);

groupshared uint counts[rrhitcond::StatCount];
void Count(uint i){InterlockedAdd(counts[i],1u);}
uint LoadWeights(int2 q){return asuint(Weights.Load(int3(q,0)));}
bool Inside(int2 q){return all(q>=0)&&q.x<int(Width)&&q.y<int(Height);}
// A neighbour sets the slope only if it is a surface of the centre's class.
bool SlopeSide(int2 q,uint w,out float depth){
 depth=0;
 if(!Inside(q))return false;
 uint4 s=PackedSurface.Load(int3(q,0));depth=asfloat(s.x);
 return s.w==w&&rrhitcond::SurfaceStatus(Status.Load(int3(q,0)))&&rrhitcond::SurfaceDepth(depth);
}
float Slope(int2 p,int2 axis,float centre,uint w){
 float before,after;
 bool hasBefore=SlopeSide(p-axis,w,before),hasAfter=SlopeSide(p+axis,w,after);
 return rrhitcond::Slope(centre,before,hasBefore,after,hasAfter);
}
// Accepted neighbours that have a hit (weighted sum of distances) or a fresh ray that missed.
void Gather(int2 p,float depth,uint w,out float hitWeight,out float hitSum,out float missWeight){
 hitWeight=0;hitSum=0;missWeight=0;
 float3 normal=NormalRoughness.Load(int3(p,0)).xyz;
 float sx=Slope(p,int2(1,0),depth,w),sy=Slope(p,int2(0,1),depth,w);
 [loop]for(int dy=-rrhitcond::Radius;dy<=rrhitcond::Radius;++dy)
  [loop]for(int dx=-rrhitcond::Radius;dx<=rrhitcond::Radius;++dx){
   int2 q=p+int2(dx,dy);
   if((dx==0&&dy==0)||!Inside(q))continue;
   float distance=Raw.Load(int3(q,0));
   bool hit=distance>0;
   if(!hit&&!rrhitcond::SpecularRay(LoadWeights(q)))continue; // another hole
   uint4 s=PackedSurface.Load(int3(q,0));
   float cosine=dot(normal,NormalRoughness.Load(int3(q,0)).xyz);
   if(!rrhitcond::SameSurface(w,s.w,Status.Load(int3(q,0)),depth,asfloat(s.x),depth+sx*dx+sy*dy,cosine))continue;
   float weight=rrhitcond::Weight(dx,dy);
   if(hit){hitWeight+=weight;hitSum+=weight*rrhitcond::Clamp(distance);}
   else missWeight+=weight;
  }
}
void Condition(int2 p){
 float raw=Raw.Load(int3(p,0));
 uint status=Status.Load(int3(p,0)),weights=LoadWeights(p);
 uint4 surface=PackedSurface.Load(int3(p,0));
 float depth=asfloat(surface.x);
 uint kind=rrhitcond::Classify(status,surface.w,depth,raw,rrhitcond::SpecularRay(weights));
 float hitWeight=0,hitSum=0,missWeight=0;
 if(kind==rrhitcond::Hole&&(Mode&rrhitcond::FillHoles)!=0)Gather(p,depth,surface.w,hitWeight,hitSum,missWeight);
 float result=rrhitcond::Resolve(kind,raw,Mode,hitWeight,hitSum,missWeight);
 if(asuint(result)!=asuint(raw))Distance[p]=result;
 if(kind==rrhitcond::Sky)Count(rrhitcond::CountSky);
 else if(kind==rrhitcond::Hit){
  Count(rrhitcond::CountHit);
  if(!rrhitcond::SpecularRay(weights))Count(rrhitcond::CountHitNoLobe1);
  if(!rrhitcond::DiffuseRay(weights))Count(rrhitcond::CountHitNoLobe2);
 }
 else if(kind==rrhitcond::Miss)Count(rrhitcond::CountMiss);
 else if(kind==rrhitcond::Hole){
  Count(rrhitcond::CountHole);
  if((Mode&rrhitcond::FillHoles)!=0&&rrhitcond::FilledAsMiss(hitWeight,missWeight))Count(rrhitcond::CountFilledMiss);
  else if((Mode&rrhitcond::FillHoles)!=0&&hitWeight>0)Count(rrhitcond::CountFilledHit);
 }
 else if(rrhitcond::SurfaceStatus(status)&&rrhitcond::SurfaceDepth(depth)&&(surface.w&rrhitcond::Alternate)!=0)Count(rrhitcond::CountViewmodel);
 else if(rrhitcond::SurfaceStatus(status)&&rrhitcond::SurfaceDepth(depth)&&(surface.w&rrhitcond::NoRtSpecular)!=0){
  Count(rrhitcond::CountNoRtSpecular);
  if(raw>0)Count(rrhitcond::CountNoRtSpecularHit);
 }
 else Count(rrhitcond::CountOther);
}
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID,uint index:SV_GroupIndex){
 if(index<rrhitcond::StatCount)counts[index]=0;
 GroupMemoryBarrierWithGroupSync();
 if(id.x<Width&&id.y<Height)Condition(int2(id.xy));
 GroupMemoryBarrierWithGroupSync();
 if(index<rrhitcond::StatCount&&counts[index]!=0)Stats.InterlockedAdd(rrhitcond::StatsOffset+index*4,counts[index]);
}
