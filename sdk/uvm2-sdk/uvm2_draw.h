/*
 * uvm2_draw.h — Vectrex analog beam control, recorded as a VIA command stream.
 *
 * This is the port of the multicart author's VectrexHaltCommandWriter: the
 * sequences a 6809 would write to the VIA to move, light and blank the beam,
 * emitted into a buffer instead of onto the bus.  uvm2_frame_end() hands the
 * whole buffer to uvm2_exec(), so the beam draws with no gaps and the CPU never
 * blocks on a clock edge while composing a frame.
 *
 * Coordinates are VPy/Vectrex units, roughly -127..127 with +Y up, and deltas
 * are what the SYS_MOVE / SYS_DRAW_DELTA syscalls carry.  Intensity is 0..127.
 */
#ifndef UVM2_DRAW_H
#define UVM2_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the VIA and prime the integrators.  Call once, after uvm2_bus_init. */
void uvm2_draw_init(void);

/* SYS_RESET0REF — beam back to centre, blanked (zero the integrators). */
void uvm2_draw_reset(void);
/* Drop the cached DAC/mux state — see the note in uvm2_draw.c. */
void uvm2_draw_invalidate(void);
/* Re-charge the zero-reference / Y / Z holds after an analog read. */
void uvm2_draw_prime_holds(void);

/* SYS_SET_INTENSITY — Z-axis DAC, 0..127. */
void uvm2_draw_intensity(int brightness);

/* Rotates the drawing 90 degrees (clockwise): for a horizontal arcade game on a
 * vertical screen. It applies to everything that goes through this layer, text
 * included. */
void uvm2_draw_rotate(int enable);
int  uvm2_draw_rotated(void);

/* SYS_MOVE / SYS_DRAW_DELTA — relative, blanked and lit respectively. */
void uvm2_draw_move(int dx, int dy);
void uvm2_draw_delta(int dx, int dy);

/* THE SAME ONES IN 1/16 OF A DEVICE UNIT. The integer grid is ~10 times coarser than the
 * reference cartridge's, and that deforms glyphs: 0.22 units of error per axis measured in
 * mhavoc, with 3.6% of the vectors entirely sub-unit. They need -DUVM2_SUBUNITS to have any
 * effect; without it they round and behave exactly like the integer ones. */
void uvm2_draw_delta_q4(int dx_q4, int dy_q4);
void uvm2_draw_move_abs_q4(int x_q4, int y_q4);
void uvm2_draw_move_q4(int dx_q4, int dy_q4);      /* relative jump, 1/16 (SYS_MOVE_Q4) */

/* The same straight line but with blanked sections, in ONE ramp. `gaps` are (start, end)
 * pairs as fractions 0..255 of the line. n = how many pairs. */
void uvm2_draw_delta_patterned(int dx, int dy, const unsigned char *gaps, int n);

/* Like the one above, but the VIA produces the gaps by itself: one byte of `pattern` = 8
 * dots. This is the BIOS's text, and it REQUIRES the SR dialect (BEAM_VIA_SR). See
 * uvm2_draw.c.
 *
 * Returns HOW MANY bytes it drew: the cell is the SR's 8 shifts and cannot be stretched, so
 * if the ramp rate does not reach, fewer characters fit. The caller carries on from there. */
int uvm2_draw_sweep_sr(int dx, int dy, const unsigned char *pattern, int n, int step);

/* Cap on the DURATION of one sweep, in T1 counts: it is set by the leakage of the Y
 * integrator, not by the number of characters. See uvm2_draw.c. */
extern volatile uint32_t uvm2_sweep_t1_max;

/* SYS_MOVE_ABS — absolute position, measured from centre. */
void uvm2_draw_move_abs(int x, int y);

/* Frame period in BUS CYCLES. 0 = free: it redraws as soon as the list is built, the way
 * the arcade machines did. 30000 = locked to 50 Hz. It starts at whatever UVM2_HZ says.
 * It is a variable and not a #define so it can be switched with the game running: the
 * question "is this shimmer a beat against the mains?" is either answered in seconds or it
 * is not answered at all. */
extern volatile uint32_t uvm2_pacer_cycles;

/* ── THE REFRESH RATE, IN HERTZ ──────────────────────────────────────────────────────
 *
 * AND WHY IT IS A GAME OPTION AND NOT A CONSTANT: mains ripple moves the beam, and it only
 * stays STATIONARY — that is, invisible — if the drawing repeats at the SAME frequency as
 * the wall socket. 50 Hz in Europe, 60 in America. A 60 Hz console running a drawing locked
 * to 50 sees a 10 Hz beat: a slow, very visible wobble. With the lock set to ITS frequency,
 * the ripple lands in the same phase every frame and goes from moving to being a fixed bias.
 *
 * `hz` = 0 leaves the frame free: it is presented as soon as the list is built, like the
 * arcade machine. It runs faster and shimmers with the mains; that is what a measurement
 * bench wants.
 *
 * LOCKING ONLY HELPS IF THE DRAWING FITS. A frame that overruns the period loses the lock
 * and lasts twice as long, and alternating 20 and 40 ms shimmers WORSE than not locking at
 * all. That is what `uvm2_refresh_fits()` is for: asking it after a typical frame says
 * whether the game can afford that frequency. `uvm2_stats.overrun` counts it too.
 *
 * This is the first entry in the GAME SETTINGS surface. Whatever comes after it (scale,
 * brightness, region) goes here and in the same shape: a function with a real name and real
 * units, not a global in cycles that each game interprets its own way. */
int      uvm2_draw_intensity_current(void);

/** Lift the pen without moving: call it BEFORE a `uvm2_draw_move_abs`, and it only has an
 *  effect if that jump measures zero. See the note on `s_penup_pending`. */
void     uvm2_draw_penup(void);

void     uvm2_set_refresh(unsigned hz);
unsigned uvm2_current_refresh(void);
/** 1 if the last frame fit inside the period (or if the refresh is free-running). */
int      uvm2_refresh_fits(void);

/* Frame boundary.  uvm2_frame_end() blanks, re-centres, replays the stream and
 * then pads the frame out to exactly 30000 bus cycles (50 Hz), locked to the
 * Vectrex clock rather than to any RP2350 timer. */
void uvm2_frame_begin(void);

/* One raw command into the list. Measurement benches only — see uvm2_draw.c. */
void uvm2_emit_raw(uint32_t reg, uint32_t data, uint32_t gap);
uint32_t uvm2_list_cycles(void);
/* Commands queued so far in the list being built. Measurement benches only. */
uint32_t uvm2_list_commands(void);
void uvm2_frame_end(void);

/* Frames completed since boot — the only clock a halted Vectrex gives us. */
uint32_t uvm2_frame_count(void);

/* Bus cycles the last frame actually consumed, padding included. Equals
 * UVM2_CYCLES_PER_FRAME for a frame that fit, and more for one that did not —
 * which is how audio keeps its tempo when drawing runs over. */
uint32_t uvm2_frame_bus_cycles(void);

/* ── Throughput levers (the point of the command-stream model) ──────────────
 * scale is the ramp duration in bus cycles for a full-range delta; it is the
 * dominant per-vector cost.  With scale = 128 a vector costs ~136 cycles, so a
 * 50 Hz frame fits ~220 of them. */
void uvm2_draw_set_scale(uint32_t cycles);

#ifdef __cplusplus
}
#endif

/* SAMPLING TIME OF EACH HOLD, in E cycles, PER CHANNEL.
 *
 * Three holds go through the mux — Y, Z and the zero reference — each with its own
 * capacitor, and a single pair of numbers does not fit all three. PiTrex has four separate
 * times (YSH_A/B, XSH_A/B) for exactly that reason.
 *
 * The minimum is what a tiny jump gets and the maximum is the cap for a full-scale one; in
 * between, the law is still logarithmic (tau*ln2 per bit). Defaults 4 and 15, the same as
 * always, so leaving them alone changes nothing.
 *
 * WHAT THE Y ONE IS FOR: with the `paths` trio on the console, a stroke that only asks for X
 * comes out SLANTED — Y moves where the list asks for vy = 0 exactly. If it is not sampled
 * for long enough, the held value stops half way. */
extern volatile int32_t uvm2_hold_y_min, uvm2_hold_y_max;
extern volatile int32_t uvm2_hold_z_min, uvm2_hold_z_max;

#endif /* UVM2_DRAW_H */
