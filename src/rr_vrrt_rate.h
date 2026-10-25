#pragma once
#ifdef __cplusplus
#include <climits>
#else
#define INT_MIN (-2147483647-1)
#endif
namespace rrvrrt {
#ifdef __cplusplus
using UInt=unsigned;
#else
typedef uint UInt;
#endif
struct Rate {bool supported;UInt denominator;UInt extra;};
inline Rate InverseRate(UInt packedRequest,int enabled,int promote,int clampRate,int threshold,int boundary,int ring){
 Rate result;result.supported=true;result.denominator=0;result.extra=0;
 packedRequest&=65535u;if(packedRequest==65535u)return result;
 int code=int(packedRequest&255u);
 if(enabled!=0){
  if(threshold==INT_MIN){result.supported=false;return result;}
  const int magnitude=threshold<0?-threshold:threshold;
  const int maximum=magnitude-1;
  const int inner=ring>boundary?1:0;
  if(!(code<maximum||threshold<1||clampRate==0))code=magnitude-2+inner;
  if(threshold<0&&code<=maximum&&promote!=0)code=maximum+inner;
 }
 const int inverse=code>=16?1:16-code;
 if(inverse<1||inverse>255){result.supported=false;return result;}
 result.denominator=UInt(inverse);result.extra=code>=16?UInt(code-16):0;return result;
}
}
