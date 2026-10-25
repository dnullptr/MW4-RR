// Guide test "Sky normals + non-zero sky depth": lift sky depth (0 with reverse-Z)
// to Floor in RR's private depth copy only. The game's depth is never written.
cbuffer Control:register(b0){uint Width,Height;float Floor;uint Reserved;}
RWTexture2D<float> Depth:register(u0);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 if(Depth[id.xy]==0)Depth[id.xy]=Floor;
}
