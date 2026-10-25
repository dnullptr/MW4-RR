#pragma once
// Shared by HLSL and C++ (no std:: calls; every comparison is false for NaN, so both agree).
namespace rrvrrt {
// Raised per-lobe ray target for cs_vrrt_resolve, in rays per pixel. Only a positive target
// below the cap is raised, to min(target * scale, cap). Never lowered; zero, negative, NaN,
// infinite and scale <= 1 pass through unchanged.
inline float RaisedTarget(float target,float scale,float cap){
 if(!(target>0&&target<cap&&scale>1))return target;
 const float raised=target*scale;
 return raised<cap?raised:cap;
}
}
