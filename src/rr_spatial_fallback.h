#ifndef RR_SPATIAL_FALLBACK_H
#define RR_SPATIAL_FALLBACK_H
namespace rrspatial {
#ifdef __cplusplus
using UInt=unsigned;
#else
typedef uint UInt;
#endif
struct Axis { UInt first,second; float fraction; };
// Coordinates stay inside the active render subrect, excluding texture padding.
inline Axis Coordinate(UInt outputPixel,UInt inputExtent,UInt outputExtent) {
    float x=(float(outputPixel)+0.5f)*float(inputExtent)/float(outputExtent)-0.5f;
    x=x<0.0f?0.0f:x;
    x=x>float(inputExtent-1)?float(inputExtent-1):x;
    Axis a;a.first=UInt(x);a.second=a.first+1<inputExtent?a.first+1:a.first;
    a.fraction=x-float(a.first);return a;
}
inline bool UseFallback(bool replacement,bool rejected,bool blocked) {
    return replacement&&(rejected||blocked);
}
}

#endif
