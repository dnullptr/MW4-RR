#pragma once
namespace rrlighting {
// Where the first spatial pass replacement takes the packed lighting lobes from.
// In game testing lobe 2 behaved as diffuse and lobe 1 as specular.
enum Source : unsigned {
    Raw,         // both lobes raw, boosted by inverse sampling probability
    // On pixels where the previous frame's SSS guide is nonzero, lobe 2 from native
    // temporal wherever it has history, even where sampled; else fresh unboosted, else
    // this frame's 5x5 mean. Raw lighting everywhere else, and everywhere while no guide
    // is available.
    SssHistory2,
    Count
};
// SSS-masked sources read the guide at t2, which only the private root binds.
inline constexpr bool SssMasked(unsigned source) {
    return source == SssHistory2;
}
} // namespace rrlighting
