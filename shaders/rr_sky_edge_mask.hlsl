// RR responsivity mask on edges between geometry and sky: Strength (-1..0,
// negative = steadier) on every non-sky pixel within Radius of a sky pixel and
// every sky pixel within Radius of geometry, 0 elsewhere. Reads the guide
// conversion's Status (1 ordinary, 2 sky, 3 special; anything else is ignored).
cbuffer Control:register(b0){uint Width,Height,Radius;float Strength;}
Texture2D<uint> Status:register(t0);
RWTexture2D<float> Mask:register(u0);
bool Usable(uint s){return s>=1&&s<=3;}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 uint self=Status.Load(int3(id.xy,0));
 float value=0;
 if(Usable(self)){
  bool sky=self==2;
  int r=int(min(Radius,4u));
  bool edge=false;
  [loop]for(int y=-r;y<=r&&!edge;++y)
   [loop]for(int x=-r;x<=r&&!edge;++x){
    int2 q=int2(id.xy)+int2(x,y);
    if(any(q<0)||q.x>=int(Width)||q.y>=int(Height))continue;
    uint s=Status.Load(int3(q,0));
    edge=Usable(s)&&((s==2)!=sky);
   }
  if(edge)value=clamp(Strength,-1.0f,0.0f);
 }
 Mask[id.xy]=value;
}
