#ifndef RR_GATE_STATS_H
#define RR_GATE_STATS_H
// Stats bytes of the gate's non-finite pixel counts (shaders/rr_live_gate.hlsl), read back by
// GpuFrame::ReadFinite: the color RR receives, then the RR output the gate displays. Plain
// macros, as the fxc-compiled gate includes this too.
#define RR_STATS_NON_FINITE_COLOR 228
#define RR_STATS_NON_FINITE_OUTPUT 232
#endif
