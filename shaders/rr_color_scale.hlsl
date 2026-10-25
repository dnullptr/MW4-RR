// Development color scale and contrast tests: RR's color input, read from the color
// subrect at Offset, multiplied by Scale (a power of two) and, when Knee > 0, compressed
// by the contrast test's curve (src/rr_contrast_test.h), into an FP32 copy. When Scale
// brightens, RR also writes an FP32 output, so neither clips at FP16's 65504. The gate's
// Display shader expands RR's output and multiplies it by 1/Scale, so the picture keeps
// its brightness. The game's color is only read.
cbuffer Control:register(b0){uint Width,Height,OffsetX,OffsetY;float Scale,Knee;uint2 Reserved;}
Texture2D<float4> Color:register(t0);
RWTexture2D<float4> Scaled:register(u0);
static const float SeriesLimit=0.01; // rrcontrast::SeriesLimit
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float3 c=Color.Load(int3(id.xy+uint2(OffsetX,OffsetY),0)).rgb*Scale;
 float m=max(max(c.r,c.g),c.b);
 if(Knee>0&&m>0){float x=m/Knee;c*=x<SeriesLimit?1+x*(x/3-0.5):log(1+x)/x;}
 Scaled[id.xy]=float4(c,1);
}
