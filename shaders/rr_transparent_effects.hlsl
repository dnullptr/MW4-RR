// Transparent effects responsivity mask (development test, src/rr_transparent_effects.h).
// Where a see-through layer added light (the color is brighter than the color
// before transparency), RR is told to lean on the current frame, so smoke, fog
// and light shafts that move without motion vectors of their own do not smear.
// The mask RR would otherwise get (water or SSS) is kept where it is larger.
cbuffer Control:register(b0){uint Width,Height,ColorX,ColorY,Flags;float Strength,Low,High;}
Texture2D<float4> Color:register(t0);
Texture2D<float3> Before:register(t1);
Texture2D<float> Existing:register(t2);
RWTexture2D<float> Output:register(u0);
static const uint ExistingFlag=1;
static const float Floor=1e-3;
float Luma(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float c=Luma(Color.Load(int3(id.xy+uint2(ColorX,ColorY),0)).rgb);
 float b=Luma(Before.Load(int3(id.xy,0)));
 float sum=abs(c)+abs(b);
 float added=sum>Floor?(c-b)/sum:0;
 float m=saturate(Strength)*saturate((added-Low)/(High-Low));
 float e=(Flags&ExistingFlag)!=0?Existing.Load(int3(id.xy,0)):0;
 // Only where a layer added light; elsewhere the existing mask (the SSS one is negative) stays.
 Output[id.xy]=m>0?max(m,e):e;
}
