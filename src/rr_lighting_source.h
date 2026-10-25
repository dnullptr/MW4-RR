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
    CapGentle,   // raw, each sample capped at 8x its lobe's native temporal luminance
    CapStrong,   // raw, capped at 4x
    WFill,       // raw colour; 4th half unboosted where sampled, native elsewhere
    WNative,     // raw colour; 4th half from native temporal everywhere
    // The same selection only where the previous frame's SSS guide is nonzero;
    // raw lighting everywhere else, and everywhere while no guide is available.
    SssNative2,  // lobe 2 from native temporal on SSS pixels
    SssFill2,    // lobe 2 fill gaps on SSS pixels
    SssNative3,  // both lobes from native temporal on SSS pixels
    SssFill3,    // both lobes fill gaps on SSS pixels
    SssFill2Spatial, // lobe 2 fill gaps on SSS pixels; no native lobe-2 history -> this frame's 5x5 mean
    SssSpatial2,     // lobe 2 on SSS pixels: fresh unboosted, else this frame's 5x5 mean; never native
    SssFill2Settled, // as SssFill2Spatial, native only with lobe-2 history field >= 4
    Count
};
inline constexpr const char *labels[Count] = {
    "Off (raw lighting)", "Lobe 1 from native temporal", "Lobe 2 from native temporal",
    "Lobe 2 fill gaps", "Both lobes fill gaps", "Raw, cap bright samples (8x)",
    "Raw, cap bright samples (4x)", "Raw, AO fix A (fill gaps)", "Raw, AO fix B (native)",
    "SSS pixels: lobe 2 native temporal", "SSS pixels: lobe 2 fill gaps",
    "SSS pixels: both lobes native temporal", "SSS pixels: both lobes fill gaps",
    "SSS pixels: lobe 2 fill gaps, history + spatial (test)",
    "SSS pixels: lobe 2 spatial fill only (test)",
    "SSS pixels: lobe 2 fill gaps, settled + spatial (test)"};
inline constexpr unsigned Valid(unsigned source) {
    return source < Count ? source : Raw;
}
// SSS-masked sources read the guide at t2, which only the private root binds.
inline constexpr bool SssMasked(unsigned source) {
    return source >= SssNative2 && source < Count;
}
} // namespace rrlighting
