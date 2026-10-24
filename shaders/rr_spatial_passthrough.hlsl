// Preserve the native packed lighting representation. In particular, these
// uint words contain half-float pairs; treating them as ordinary float4 loses it.
Texture2D<uint4> lighting : register(t0);
Texture2D<uint> weights : register(t1);
RWTexture2D<uint4> outputLighting : register(u0);
RWTexture2D<uint> outputWeights : register(u1);
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width,height;
    outputLighting.GetDimensions(width,height);
    if(id.x>=width || id.y>=height) return;
    outputLighting[id.xy]=lighting.Load(int3(id.xy,0));
    outputWeights[id.xy]=weights.Load(int3(id.xy,0));
}
