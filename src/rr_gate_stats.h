#ifndef RR_GATE_STATS_H
#define RR_GATE_STATS_H
// Stats bytes of the gate's non-finite pixel counts (shaders/rr_live_gate.hlsl), read back by
// GpuFrame::ReadFinite: the color RR receives, then the RR output the gate displays. Plain
// macros, as the fxc-compiled gate includes this too. They follow the ray density words
// (rr_ray_density.h, bytes 64..255).
#define RR_STATS_NON_FINITE_COLOR 256
#define RR_STATS_NON_FINITE_OUTPUT 260
#ifdef __cplusplus
namespace rrgatestats {
// Bytes of a slot's stats UAV and of its readback (rrlive::GpuFrame::StatsBytes). Every stats
// word lies below it; a new one goes after the last and must still fit.
inline constexpr unsigned StatsBytes = 512;
static_assert(RR_STATS_NON_FINITE_OUTPUT == RR_STATS_NON_FINITE_COLOR + 4 &&
                  RR_STATS_NON_FINITE_OUTPUT + 4 <= StatsBytes,
              "the non-finite counts fit the stats buffer");
} // namespace rrgatestats
#endif
#endif
