// Guide test "Sky normals + non-zero sky depth": a copy of RR's depth, read from
// the depth subrect at Offset, with sky depth (0 with reverse-Z) lifted to
// Floor. RR reads the copy instead; the game's depth is never written.
cbuffer Control:register(b0){uint Width,Height,OffsetX,OffsetY;float Floor;uint3 Reserved;}
Texture2D<float> Depth:register(t0);
RWTexture2D<float> Floored:register(u0);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float depth=Depth.Load(int3(id.xy+uint2(OffsetX,OffsetY),0));
 Floored[id.xy]=depth==0?Floor:depth;
}
