#pragma once
namespace rrlighting {
// Where the first spatial pass replacement takes each packed lighting lobe from.
// In game testing lobe 2 behaved as diffuse and lobe 1 as specular.
enum Source : unsigned {
    Raw,         // both lobes raw, boosted by inverse sampling probability
    NativeLobe1, // lobe 1 from native temporal output
    NativeLobe2, // lobe 2 from native temporal output
    FillLobe2,   // lobe 2: fresh unboosted sample where a ray landed, native elsewhere
    FillBoth,    // both lobes filled that way
    Count
};
inline constexpr const char *labels[Count] = {
    "Off (raw lighting)", "Lobe 1 from native temporal", "Lobe 2 from native temporal",
    "Lobe 2 fill gaps", "Both lobes fill gaps"};
inline constexpr unsigned Valid(unsigned source) {
    return source < Count ? source : Raw;
}
} // namespace rrlighting
