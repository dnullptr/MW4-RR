// Development color scale test: RR's color input, read from the color subrect
// at Offset, multiplied by Scale (a power of two) into an FP32 copy, so nothing
// clips. The gate's Display shader multiplies RR's output by 1/Scale, so the
// picture keeps its brightness. The game's color is only read.
cbuffer Control:register(b0){uint Width,Height,OffsetX,OffsetY;float Scale;uint3 Reserved;}
Texture2D<float4> Color:register(t0);
RWTexture2D<float4> Scaled:register(u0);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 Scaled[id.xy]=float4(Color.Load(int3(id.xy+uint2(OffsetX,OffsetY),0)).rgb*Scale,1);
}
