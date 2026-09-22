/*
 * uvm2_draw.c — Vectrex beam control as a recorded VIA command stream.
 *
 * Ported from the multicart's own VectrexHaltCommandWriter, which is the only
 * version of these sequences proven on real hardware.  Where that writer is a
 * C++ class building into a caller-supplied buffer, this is a single
 * module-level stream because our callers are syscalls, not a frame-composing
 * object.
 *
 * The beam is analog: Port A is a DAC feeding the X integrator directly, while
 * Y, Z (intensity) and the zero reference are sampled off that same DAC through
 * a mux + sample/hold on Port B.  Motion is therefore "set the DAC, release
 * /RAMP for N bus cycles" — the distance is the DAC value times the time, which
 * is why `scale` is the dominant per-vector cost and the main throughput lever.
 */

#include "uvm2_bus.h"
#include <limits.h>
#include "uvm2_draw.h"
#include "uvm2_config.h"
#include "uvm2_smp.h"          /* .vsmp samples injected into the list; see smp_inject */
#ifdef UVM2_PIO_STREAM
#include "uvm2_bus_stream.h"   /* vbus_list_*: a frame's list as a single DMA */
#endif
#ifdef UVM2_CMDS_IN_PSRAM
#include "uvm2_psram.h"
#endif

/* HOW MANY COMMANDS FIT IN ONE FRAME. A per-game knob, not a constant.
 *
 * The 8192 came from the multicart and from a count that no longer holds: "a vector is ~8
 * commands and a 50 Hz frame only has room for ~220 vectors". That was true WHILE THE PACER
 * WAS HOLDING IT BACK to 50 Hz. Unlimited (UVM2_HZ=0) the game draws everything it can fit,
 * and asteroids reaches 744 vectors: 8751 commands, of which 559 fell off the edge —
 * measured, stats.dropped.
 *
 * And a cap that is exceeded does NOT look like an error, it looks like an incomplete
 * drawing, which is the symptom of ten other things. Hence the counter's warning: if
 * dropped is not zero, nothing you see on screen is conclusive.
 *
 * It costs 3 bytes per command per buffer (two buffers in dual core), so raising it is not
 * free: 12288 is 36 KB per buffer, 72 KB for the pair. Raise it per game, with the counter
 * in front of you:
 *
 *     make uvm2 UVM2_CMD_CAPACITY=12288      # and check that stats.dropped stays at 0
 */
#ifndef UVM2_CMD_CAPACITY
#define UVM2_CMD_CAPACITY  8192u
#endif

/* One buffer single-core, two when core 1 replays: core 0 fills the buffer for
 * frame n while core 1 is still replaying frame n-1.  Single-core builds keep
 * exactly one, so nothing grows for a target that does not use it. */
#ifdef UVM2_DUAL_CORE
#  define UVM2_NBUF 2u
#else
#  define UVM2_NBUF 1u
#endif
/* THE LIST, IN SRAM OR IN THE EXTERNAL PSRAM.
 *
 * In SRAM the list competes with the game: dkong leaves 3788 bytes free out of the 496 KB,
 * so raising the 8192 cap does not fit (asking for 16384 overflows by 61748). The UVM2's
 * PSRAM is 8 MB and in an SRAM image nobody uses it, so there the cap stops being a memory
 * problem.
 *
 * WHY THE LATENCY IS AFFORDABLE. The list is REPLAYED at the Vectrex bus's pace: one
 * command is one bus cycle, 667 ns. A sequential read through the XIP cache is far below
 * that, so on core 1's side the latency hides completely. Where it can hurt is WRITING it —
 * core 0, flat out and unpaced — and that is not assumed: compare us_exec and the frame
 * period with the knob and without it.
 *
 * It needs no linker script: it is a pointer to a fixed address. The PSRAM is empty in an
 * SRAM image, and this way it does not touch the pico-sdk's memmap.
 */
#if defined(UVM2_CMDS_IN_PSRAM) && defined(UVM2_PSRAM_IMAGE)
#  error "UVM2_CMDS_IN_PSRAM with the image ALREADY in PSRAM: the list would overwrite the code itself"
#endif

#ifdef UVM2_CMDS_IN_PSRAM
/* THROUGH THE UNCACHED ALIAS (0x15000000), NOT THE NORMAL WINDOW (0x11000000).
 *
 * MEASURED, by writing 16 KB and checking it back through the alias: through the cached
 * window 2512 words of 4096 fail; through the alias, ZERO. The PSRAM is healthy — what
 * corrupts the data is going through the XIP cache on the way in, which fills the line by
 * reading from the chip what has not been written yet, mixes it, and hands back the mix.
 *
 * AND IT IS THE SAME TRAP THAT WAS ALREADY WRITTEN DOWN: verifying through the cache says
 * nothing about the chip. That is why the two-stage loader "worked" (12 KB payloads, which
 * fit in the 16 KB of cache), why asteroids from PSRAM "booted but shimmered", and why the
 * command list collapsed the drawing into a diagonal. One fault, not three.
 *
 * Here it is also the right thing by design: the list is written once and read once, so the
 * cache can contribute nothing — only get in the way. */
#  ifndef UVM2_PSRAM_CMDS_BASE
#    define UVM2_PSRAM_CMDS_BASE 0x15000000u
#  endif
typedef uint8_t uvm2_cmd_buf[UVM2_CMD_CAPACITY * 3u];
static uvm2_cmd_buf *const s_cmds = (uvm2_cmd_buf *)(uintptr_t)UVM2_PSRAM_CMDS_BASE;
#else
static uint8_t s_cmds[UVM2_NBUF][UVM2_CMD_CAPACITY * 3u];
#endif
static uint32_t s_count;
static uint32_t s_buf;                  /* buffer being filled; always 0 single-core */

/* OUTSIDE THE #ifdef UVM2_DUAL_CORE: it is used by UNCONDITIONAL code.
 *
 * These used to be inside, next to `s_len[2]`, and with that EVERY single-core target
 * stopped compiling. An `#ifdef` is not only an option: it is the boundary of what exists,
 * and putting a variable used outside it in there breaks builds nobody looks at daily. */
static volatile uint32_t s_cycles_pub[2];
/* The bounding box of the frame being built, in device units. */
static int32_t s_box_x0 = 32767, s_box_y0 = 32767, s_box_x1 = -32768, s_box_y1 = -32768;
static int32_t s_box_w_prev;
/* Commands in the list RIGHT AFTER opening the frame (the prologue). Anything past that
 * is real content. */
static uint32_t s_count_after_begin;
static uint32_t s_peak_i, s_peak_k, s_peak_n;

#ifdef UVM2_DUAL_CORE
/* The handshake, and the only shared state between the cores besides the
 * buffers themselves.  Both only ever count up, so a 32-bit load can never tear
 * and no lock is needed — ordering comes from the barriers around them. */
volatile uint32_t uvm2_frame_request;   /* frames core 0 has finished building */
volatile uint32_t uvm2_frame_done;      /* frames core 1 has finished replaying */
static volatile uint32_t s_len[2];
static uint32_t s_frame_no = 1;

const uint8_t *uvm2_frame_buffer(uint32_t frame) { return s_cmds[frame & 1u]; }
uint32_t        uvm2_frame_length(uint32_t frame) { return s_len[frame & 1u]; }
#endif
static uint32_t s_frames;

/* Shadow of the VIA state, so a command is only emitted when something really
 * changes.  Data bytes, not the shifted command fields. */
static uint8_t  s_porta = 0x00;
/* Every 8-bit value is a legal DAC setting, so `stale` needs its own flag rather
 * than a sentinel that could collide with a real one. */
static int      s_porta_stale = 1;
static uint8_t  s_portb = UVM2_PB_IDLE;
static uint8_t  s_pcr   = UVM2_PCR_IDLE;
static int s_beam_on = 0;  /* beam state (SR), for the SR dialect's keep-lit */
static int      s_y     = 0;        /* value currently held by the Y S/H  */
static int      s_z     = 0;        /* value currently held by the Z S/H  */
static int      s_pos_x = 0;        /* beam position since the last centre */
static int      s_pos_y = 0;

static uint32_t s_frame_cycles;     /* bus cycles the last frame really took */
/* Ramp cycles for a full-scale delta. 160 puts it at THE CARTRIDGE'S SIZE, checked on
 * screen with dkong on 2026-08-12; with 128 the scenery came out visibly small. And it is
 * CHEAPER than the 128 it replaced: 154 cycles per vector against 157. We had been drawing
 * small and paying more for it.
 *
 * The reason is not that making it bigger is free — distance is speed times time and the
 * DAC is already full: of 992 measured writes the median is 25, the p90 is 89 and 11
 * saturate at 127, so raising the gain would clip the long strokes — it is the fixup floor,
 * which doubles the delta and halves the ramp for as long as the scale stays above
 * UVM2_RAMP_FLOOR:
 *
 *     from 128:  128 -> 64 -> 32          it stops.  Final ramp 32
 *     from 160:  160 -> 80 -> 40 -> 20    one more.  Final ramp 20
 *
 * So this knob moves IN STEPS, not continuously. Measured:
 *     128 -> 157 c/vector    144 -> 147    160 -> 154
 * 144 is the cheapest and draws small; 160 gets the size right. If this ever needs real
 * tuning, the place is RAMP_FLOOR, not this number.
 *
 * IT IS PAID IN BRIGHTNESS: short vectors go from 32 ramp cycles to 20, i.e. less time lit.
 * No degradation was seen in dkong; it is what to look at if a game with fine detail comes
 * out dim. */
static uint32_t s_scale = 160;

/* Timings, in bus cycles.  Named because they are exactly the knobs to turn
 * when the picture is right but the frame is too expensive. */
/* Sample-and-hold settling, AS A FUNCTION OF THE JUMP.
 *
 * The capacitor takes as long as the voltage it has to travel, not as long as the stroke
 * that comes afterwards. And the fixup doubles the delta of short vectors, so a small step
 * ends up asking for a BIG jump — which is why the short ones suffered, the opposite of
 * what you would expect.
 *
 * MEASURED on the console 2026-08-12 with dkong: with 8 fixed the steps sag in Y and the
 * drawing shrinks; with 14 fixed they come out right but the refresh drops from 24.8 to
 * 13.3 fps (138 -> 167 cycles per vector), because it is paid on EVERY mux sample. With 10
 * they come out right "nearly always", which is the signature of a badly placed threshold:
 * some jumps make it and some do not.
 *
 * The physics: Ron(4052) x 10 nF = 1.8 us = 2.7 E cycles per tau. 8 cycles is 3 tau (5%
 * error), 14 is 5.2 tau (0.6%). A small jump can afford 5%; a full-scale one cannot.
 *
 * Linear in the jump between the two extremes. The real law is logarithmic, so this
 * OVERPAYS on medium jumps — and even so it is far cheaper than the fixed maximum. If it
 * needs tuning, this is the place, with the bracket on screen. */
#define UVM2_HOLD_MIN        4u     /* tiny jumps: they ask for no more        */
#define UVM2_HOLD_MAX        15u    /* full scale: 5.5 tau, 1 LSB of error     */
#define UVM2_HOLD_DELAY      UVM2_HOLD_MAX   /* when we do not know where we came from */
/* What value the BRIGHTNESS hold is primed with when the VIA is set up. The reference
 * cartridge uses 0x7F (full scale) in its frame preamble; the other two channels — zero
 * reference and Y — go to zero in both. A game changes it with -DUVM2_Z_PRIME=N if it
 * measures something else on ITS console. */
#ifndef UVM2_Z_PRIME
#define UVM2_Z_PRIME 0x7F
#endif
#define UVM2_BLANK_OFF_DELAY 3u     /* ramp starts this early, before lighting */
#define UVM2_BLANK_ON_DELAY  16u    /* beam stays lit after the ramp stops     */
#define UVM2_ZERO_BASE       45u    /* centring cost, plus scale/4             */

/* Commands lost in the frame being built, because the list filled up. Dumped into stats in
 * frame_end. Declared here and not below because emit() is what increments it. */
/* Signature of what emit() asks to be written, against what the chip hands back on
 * re-reading it. */
uint32_t uvm2_sig_emitted = 2166136261u, uvm2_sig_reread, uvm2_sig_bad, uvm2_sig_laps;
uint32_t uvm2_sig_copy, uvm2_sig_copy_bad, uvm2_sig_copy_laps;

static uint32_t s_dropped;

/* THE FRAME CLOSE HAS RESERVED ROOM. If the list fills up half way through the game, what
 * gets thrown away is the TAIL — and the tail is frame_end's close: blanking the beam, the
 * recalibration and the zero clamp. Those went through emit() too and were dropped just the
 * same, so a full frame left the beam LIT wherever it happened to be for the gap between
 * frames and the next one's preamble. On screen: blindingly bright strokes in the HUD
 * (which is drawn last and is the first thing lost) and a half-drawn HUD. Described on
 * 2026-08-31 on the 25m screen with barrels; that same day a frame of exactly 8192 commands
 * was read back, i.e. touching the ceiling.
 *
 * The game may only fill up to UVM2_CMD_RESERVE from the end; frame_end lifts the limit for
 * its close. A truncated frame still ends blanked and clamped. */
#define UVM2_CMD_RESERVE 64u
static uint32_t s_limit = UVM2_CMD_CAPACITY - UVM2_CMD_RESERVE;

/* WHAT THE LIST IS GOING TO COST, in E cycles and while it is being built.
 *
 * It is needed to close the frame where the reference closes it: its frame measures exactly
 * 30023 cycles and it spends the leftover on commands, not on silence. To know how much is
 * left over you have to know how much has been spent, and nobody was counting that: there
 * was only `bus_cycles`, which is measured AFTER replaying. */
static uint32_t s_cycles;

uint32_t uvm2_list_cycles(void) { return s_cycles; }

/* Commands queued so far in the list being built. Like `uvm2_emit_raw`, this is for
 * measurement benches only: reading it between two calls says how many commands that
 * call cost, which `uvm2_stats.commands` cannot because it is only written when the
 * frame closes. Nothing in a game should need it. */
uint32_t uvm2_list_commands(void) { return s_count; }

/* Those of the frame already PUBLISHED, which is the one core 1 replays. */
uint32_t uvm2_frame_cycles(uint32_t frame) { return s_cycles_pub[frame & 1u]; }

static inline void emit(uint32_t reg, uint32_t data, uint32_t delay)
{
    if (s_count < s_limit) {
        s_cycles += delay + 1u;   /* the write takes its E period, and the gap follows */
        const uint32_t w = UVM2_CMD(reg, data, delay);
        const uint32_t v = UVM2_CMD_PACK(w);
        uint8_t *d = &s_cmds[s_buf][s_count * 3u];
        d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16);
        s_count++;
#ifdef UVM2_CMDS_IN_PSRAM
        /* WHAT IS ASKED TO BE WRITTEN, signed here — before it goes out to the bus.
         *
         * The psramrw test writes through the uncached alias and gives ZERO failures in
         * 4096 words, but it does so with the machine IN SILENCE: no stream, no DMA and no
         * cartridge bus moving. The game does not. Comparing this signature against the one
         * from re-reading the list off the chip says whether the writes hold UP UNDER LOAD,
         * which is the only remaining difference between the test that passes and the game
         * that collapses. */
        uvm2_sig_emitted ^= d[0]; uvm2_sig_emitted *= 16777619u;
        uvm2_sig_emitted ^= d[1]; uvm2_sig_emitted *= 16777619u;
        uvm2_sig_emitted ^= d[2]; uvm2_sig_emitted *= 16777619u;
#endif
    }
    else                             s_dropped++;   /* NEVER silently: see stats.dropped */
}
/* ONE RAW COMMAND INTO THE LIST. Measurement benches only: a game draws through the
 * geometric API, not through here. It exists because measuring the EXECUTOR needs lists
 * with a known gap, and building those with draw_line means building the beam model on top
 * of them too. */
void uvm2_emit_raw(uint32_t reg, uint32_t data, uint32_t gap)
{
    emit(reg, data, gap);
}


/* ── Primitive register writes ────────────────────────────────────────────── */

extern volatile uint32_t BEAM_VIA_SR;   /* in vectrex-draw; see via_setup */

static void set_porta(uint8_t v, uint32_t delay)
{
    if (s_porta == v && !s_porta_stale) return;
    s_porta = v;
    s_porta_stale = 0;
    emit(UVM2_VIA_PORTA, v, delay);
}

/* Sample the current DAC value into one of the mux channels: disable the mux,
 * select the channel, enable it for `delay` cycles, then park Port B again. */
static void mux_sample(uint8_t channel, uint32_t delay)
{
    uint8_t keep = (uint8_t)(s_portb & 0xF8u);   /* preserve /RAMP + sound bits */
    emit(UVM2_VIA_PORTB, keep | channel | UVM2_PB_MUX_DISABLE, 0);
    emit(UVM2_VIA_PORTB, keep | channel | UVM2_PB_RAMP_OFF,    delay);
    emit(UVM2_VIA_PORTB, s_portb, 0);
}

/* Cycles the hold needs to travel `from` -> `to`.
 *
 * THE LAW IS LOGARITHMIC, not linear: an RC reaches 1 LSB at t = tau * ln(jump), with
 * tau = Ron(4052) x 10 nF = 1.8 us = 2.7 E cycles. Doubling the jump costs one more tau,
 * not twice the time.
 *
 * The first version interpolated linearly and was bad in BOTH directions: it overpaid on
 * small jumps — which are the majority — and came up SHORT on medium ones (a jump of 64
 * needs 11.3 cycles and it gave 9.5). It cost 151 cycles per vector against 138 for the
 * broken minimum and 167 for the fixed maximum.
 *
 * ln(d) = log2(d) * 0.693, and the integer log2 is the position of the highest bit, which
 * the CPU gives in one instruction. tau * ln2 = 1.87 cycles per bit. */
/* EACH CHANNEL ITS OWN TIME, AND THAT IS NOT SYMMETRY: THEY ARE DIFFERENT CAPACITORS.
 *
 * The 4 and the 15 came from a tau computed over C304 (10 nF) and applied to all three
 * channels equally. But THREE holds go through the mux — Y, Z and the zero reference — each
 * with its own capacitor, and PiTrex has four separate times (YSH_A/B, XSH_A/B) precisely
 * because one number does not fit them all. Another reference game arrives at the same
 * place from a different direction: five zero values, one per scale.
 *
 * And it is needed HERE, not in the abstract: with the `paths` trio on the console, a stroke
 * that only asks for X comes out SLANTED, i.e. Y moves where the list asks for vy = 0
 * exactly. If Y is not sampled for long enough, the held value stops half way and the error
 * depends on the jump — which is exactly what you see.
 *
 * LIVE knobs, separate per channel, with the long-standing values as defaults: moving the Y
 * one cannot change the brightness, and leaving them alone changes nothing already
 * measured. */
volatile int32_t uvm2_hold_y_min = (int32_t)UVM2_HOLD_MIN;
volatile int32_t uvm2_hold_y_max = (int32_t)UVM2_HOLD_MAX;
volatile int32_t uvm2_hold_z_min = (int32_t)UVM2_HOLD_MIN;
volatile int32_t uvm2_hold_z_max = (int32_t)UVM2_HOLD_MAX;

static uint32_t hold_between(int from, int to, int32_t lo, int32_t hi)
{
    if (lo < 1) lo = 1;
    if (hi < lo) hi = lo;
    uint32_t d = (uint32_t)(to > from ? to - from : from - to);
    if (d == 0) return (uint32_t)lo;
    uint32_t bits = 32u - (uint32_t)__builtin_clz(d);      /* ~log2(d) + 1 */
    int32_t  t    = (int32_t)((bits * 187u) / 100u);       /* tau * ln2    */
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    return (uint32_t)t;
}

/* the usual one, for places that are not a specific channel (the zero clamp) */
static uint32_t hold_for(int from, int to)
{
    return hold_between(from, to, (int32_t)UVM2_HOLD_MIN, (int32_t)UVM2_HOLD_MAX);
}

static void set_y(int y, uint32_t delay)
{
    /* Y IS NEVER CACHED. NEVER.
     *
     * "The S/H still holds it" was an assumption, and it is the kind of assumption this
     * project already pays dearly for: mux channel 0 is C304, a 10 nF capacitor (netlist,
     * see the logic-board notes), and a capacitor DISCHARGES. X does not have that problem
     * because it is the live DAC, with no hold — which is exactly why the measured drift
     * was 7 times larger in Y than in X.
     *
     * And this is not theory: in its frame 120 the reference cartridge opens channel 0 on
     * 100.0% of the ramps (647 of 647), whatever the value. We skipped it on 12 and sat at
     * 98.2%. It DOES cache Z (it recharges C306 only 4 times per frame), so the rule is not
     * "cache nothing": it is do not cache Y. */
    delay = hold_between(s_y, y, uvm2_hold_y_min, uvm2_hold_y_max);
    s_y = y;
    set_porta((uint8_t)y, 0);
    mux_sample(UVM2_MUX_Y, delay);
}

/* THE GAP AFTER SR=00 IS SET BY WHAT COMES NEXT, not by the blanking. Measured in the
 * reference frame: 15 cycles if ORA follows (the brightness batch) and 12 if the clamp's PCR
 * follows. The physical 8 — the SR's 8 shifts — fits in both. We used 12 for both. */
#define UVM2_SR_A_ORA  15u
#define UVM2_SR_A_PCR  12u
static void beam_off_and_wait_h(uint32_t gap);

static void set_z(int z, uint32_t delay)
{
    if (s_z == z) return;
    delay = hold_between(s_z, z, uvm2_hold_z_min, uvm2_hold_z_max);
    s_z = z;
    if (BEAM_VIA_SR) {
        /* THE REFERENCE CARTRIDGE'S BRIGHTNESS BATCH, verbatim from the capture
         * (x1229/frame-class: ORA=z+4 ORB=84+9 ORB=81): TWO ORB writes, without the
         * "disable first" step — the reference changes selection and enable in ONE write
         * and draws cleanly, so the extra step was over-caution on our side. The hold
         * window is fixed (9 cycles), and it is theirs. */
        /* BLANK BEFORE MOVING Z, WHICH IS THEIR ORDER.
         *
         * In the capture the SR=00 sits right against the brightness batch and BEFORE it:
         *     4:08 5:00 | a:00 | 1:78 0:84 0:81
         * and we had it afterwards:
         *     4:08 5:00 | 1:78 0:84 0:81 | a:00
         * i.e. we were changing the intensity WITH THE BEAM STILL LIT — and with the SR in
         * mode 110 the beam stays alive for 8 more cycles, so the stroke that has just
         * finished takes a final stretch at the NEXT one's brightness. See the note on the
         * SR and blanking for why SR=00 does not blank immediately. */
        beam_off_and_wait_h(UVM2_SR_A_ORA);   /* the brightness ORA comes next */
        set_porta((uint8_t)z, 3u);
        emit(UVM2_VIA_PORTB, 0x84u, 8u);            /* mux ON channel 2, a single step */
        emit(UVM2_VIA_PORTB, UVM2_PB_IDLE, 12u);    /* and park at 81, as the reference does */
        s_portb = UVM2_PB_IDLE;
        return;
    }
    set_porta((uint8_t)z, 0);
    mux_sample(UVM2_MUX_Z, delay);
}

/* X is the live DAC — no sample/hold, so it must be written last before a ramp. */
static void set_x(int x, uint32_t delay)
{
    set_porta((uint8_t)x, delay);
}

static int32_t s_drift_ax, s_drift_ay;   /* see the drift compensation, below */

/* The version WITH DEBT, for chained strokes, plus forgetting the debt on each jump.
 * They live in vectrex-draw so there are not three copies of the same rule. */
void vx_ramp_params_chain(int32_t dx, int32_t dy, int32_t *vx, int32_t *vy, uint32_t *t1);
void vx_chain_reset(void);
void vx_debt_reset(void);
/* The jump's half of the debt model: how much to absorb into the dark stretch, and the
 * ledger entry for each ramp actually emitted. See `move_one`, which is the only caller. */
void vx_debt_take(int32_t dx, int32_t dy, uint32_t q, int32_t *ax, int32_t *ay);
void vx_debt_record(int32_t dx, int32_t dy, uint32_t q, int32_t vx, int32_t vy, uint32_t t1);
/* THE JUMP travels blanked: there is nothing to slow down, so it runs at the DAC's full
 * scale like the rest. See `DAC_CAP` in ramp.rs. */
void vx_ramp_params_jump(int32_t dx, int32_t dy, int32_t *vx, int32_t *vy, uint32_t *t1);
void vx_ramp_params_chain_q4(int32_t dx, int32_t dy, int32_t *vx, int32_t *vy, uint32_t *t1);
void vx_ramp_params_jump_q4(int32_t dx, int32_t dy, int32_t *vx, int32_t *vy, uint32_t *t1);
void vx_ramp_params_chain_qn(int32_t dx, int32_t dy, uint32_t q,
                             int32_t *vx, int32_t *vy, uint32_t *t1);
void vx_ramp_params_jump_qn(int32_t dx, int32_t dy, uint32_t q,
                             int32_t *vx, int32_t *vy, uint32_t *t1);
void vx_ramp_params_with_t1(int32_t dx, int32_t dy, int32_t f, uint32_t t1,
                           int32_t *vx, int32_t *vy);

extern volatile uint32_t BEAM_VIA_SR;   /* in vectrex-draw; see via_setup */

/* Blank the beam through the SR if it is lit. With keep-lit (the SR dialect) the PCR blanks
 * NOTHING: EVERY path that moves the beam outside a stroke — re-zero, recalibration, frame
 * close — has to come through here BEFORE moving, or the traverse comes out drawn (the STAR
 * of rays into the centre seen on the console on 2026-09-03). */
/* HOW LONG THE BEAM REALLY TAKES TO BLANK AFTER BEING ASKED TO.
 *
 * With ACR = 0x98 the shift register runs in mode 110: it shifts 8 bits out at the Phi2 rate
 * and stops, and CB2 (~BLANK) keeps the LAST one. So writing SR = 0x00 does not blank: it
 * blanks EIGHT CYCLES LATER.
 *
 * The reference cartridge leaves 12-13 cycles between its SR=00 and the PCR=CC that bites
 * the zero clamp (measured: 10 of its 15 clamps). We left ZERO — `SR=00+0 PCR=cc`, in 6 of
 * 10 — so the clamp started dragging the beam to the centre WITH THE BEAM STILL LIT and drew
 * the path: a long line crossing the screen from the object to the centre, which is exactly
 * what the console showed in both photographs on 2026-09-04.
 *
 * The 8 is physics (the 8 shifts); the 12 is theirs, with margin. */
#define UVM2_SR_BLANK_CYCLES  12

/* LET THE LAST LIT RAMP FINISH BEFORE CLOSING THE BEAM.
 *
 * `moveto_seq` already does this (H_T1CH_BLANK = 29 against the 11 of "the micro-segment
 * continues"), but there it only covers the blanks IT emits. The ones that come out of this
 * file — set_z's, the zero clamp's — left the T1CH at 11, i.e. 18 cycles less than the
 * reference: 29 cases per frame in its frame 120, and each one is a cut ramp, i.e. a short
 * stroke.
 *
 * The 29 is THEIRS, measured: 21 cases with the SR right behind the T1CH, against 175 with a
 * gap of 16 where what follows is always ORA. The gap is not set by the blanking, it is set
 * by how long it takes to get there. See the same numbers in emit.rs. */
#define UVM2_H_T1CH_CONT   11u
#define UVM2_H_T1CH_BLANK  29u

static void extend_stroke_t1ch(void)
{
    if (s_count == 0u) return;
    uint8_t *p = &s_cmds[s_buf][(s_count - 1u) * 3u];
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (((v >> 8) & 0xFu) != (uint32_t)UVM2_VIA_T1CH) return;   /* not closing a ramp */
    uint32_t gap = (v >> 12) + (UVM2_H_T1CH_BLANK - UVM2_H_T1CH_CONT);
    if (gap > 4095u) gap = 4095u;
    s_cycles += gap - (v >> 12);
    v = (v & 0xFFFu) | (gap << 12);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
}

static void beam_off(void)
{
    if (s_beam_on) {
        extend_stroke_t1ch();
        emit(UVM2_VIA_SR, 0x00, 0);
        s_beam_on = 0;
    }
}

/* Like `beam_off`, but letting the SR finish shifting before the caller does anything that
 * MOVES the beam (the zero clamp, a jump). */
static void beam_off_and_wait_h(uint32_t gap)
{
    if (s_beam_on) {
        extend_stroke_t1ch();
        emit(UVM2_VIA_SR, 0x00, gap);
        s_beam_on = 0;
    }
}

static void beam_off_and_wait(void) { beam_off_and_wait_h(UVM2_SR_A_PCR); }

/* Ramps since the last re-zero. Declared up here because it is cleared by set_zero, which
 * comes before the knob. See uvm2_zero_every. */
static uint32_t s_ramps_from_zero;

static void set_zero(int active, uint32_t delay)
{
    /* The clamp drags the beam to the centre: if it arrives lit, it draws the traverse. And
     * ASKING for the blank is not enough: the SR takes 8 cycles to shift it out. See
     * beam_off_and_wait. */
    if (active) beam_off_and_wait();
    /* EVERY re-zero wipes the accumulated drift, because it puts the beam back at centre.
     * If the accumulator survives, we keep correcting an error that no longer exists — and
     * since re-zeros land in different places depending on the scene, that leftover CHANGES
     * between frames: it shimmers. A deterministic error cannot shimmer.
     *
     * Here and not in frame_begin: re-zeros happen many times inside one frame (one per
     * object, plus the ones the stroke cap inserts). */
    if (active) { s_drift_ax = 0; s_drift_ay = 0; s_ramps_from_zero = 0; }

    s_pcr = (uint8_t)((s_pcr & ~UVM2_PCR_ZERO_OFF) | (active ? 0u : UVM2_PCR_ZERO_OFF));
    emit(UVM2_VIA_PCR, s_pcr, delay);
}

/* `set_ramp` IS GONE. It toggled /RAMP by writing PB7 through PORTB, which is the
 * multicart's beam model; now T1 ends the ramp (ACR = 0x80) and those bits never reach the
 * pin. Leaving it would have been worse than deleting it: a function that compiles, can be
 * called, and does absolutely nothing. */
/* ── Public API ───────────────────────────────────────────────────────────── */

void uvm2_draw_set_scale(uint32_t cycles)
{
    if (cycles < 8u)  cycles = 8u;
    if (cycles > 255u) cycles = 255u;
    s_scale = cycles;
}


/* The VIA bring-up and the sample-and-hold priming, as one unit.
 *
 * This is the reference VectrexHaltCommandWriter's constructor, and it runs at the
 * top of EVERY frame — a fresh writer is built per frame, so all sixteen commands
 * go out again every 20 ms. We used to run it once, from uvm2_draw_init.
 *
 * Two reasons it belongs in the frame:
 *
 *  - the holds are capacitors. Primed once at boot, the zero reference, the Y
 *    hold and the Z hold droop, and the beam's idea of centre and scale drifts
 *    with them — which is the square that starts the right size and then shrinks
 *    and skews.
 *  - the PCR it writes is UVM2_PCR_IDLE, i.e. the zero clamp ASSERTED. Priming
 *    the holds against a clamped integrator gives a real reference; priming
 *    against a free-running one measures the drift instead. We released the
 *    clamp first and then primed, which is that mistake exactly.
 *
 * Sixteen commands is ~40 of the 30000 bus cycles in a frame: 0.13%. */
static void via_setup(void)
{
    s_porta = 0x00;
    s_portb = UVM2_PB_IDLE;
    s_pcr   = UVM2_PCR_IDLE;      /* zero clamp asserted, beam blanked */

    /* Port A all outputs (the DAC); Port B outputs except the comparator input
     * (bit 5) and PB6.  ACR: shift register under phase-2 control, T1 free-run
     * — the same values the Vectrex BIOS programs. */
    emit(UVM2_VIA_PORTA, s_porta, 0);
    emit(UVM2_VIA_DDRA,  0xFF,    0);
    emit(UVM2_VIA_PORTB, s_portb, 0);
    emit(UVM2_VIA_DDRB,  0x9F,    0);
    emit(UVM2_VIA_PCR,   s_pcr,   0);
    /* ACR IS PART OF THE BEAM MODEL, not a loose value:
     *   0x60 -> T1 free-running with PB7 DISABLED; /RAMP is toggled in software via PORTB.
     *   0x80 -> T1 one-shot WITH output on PB7: writing T1CH drops /RAMP and the count
     *           running out raises it. The 6522 ends the ramp, which is what the BIOS
     *           programs and what the shared layer assumes.
     * With 0x80 the PORTB bits that touch PB7 stop reaching the pin, so `set_ramp` does
     * nothing — which is why the shared path does not call it.
     *
     * ACR DEPENDS ON WHO LIGHTS THE BEAM. With our model (beam via PCR) T1 ends the ramp and
     * 0x80 is enough. With the BIOS's (beam via the shift register) you need 0x98, which
     * also puts the SR under phase-2 control — that is what makes writing $FF/$00 there turn
     * CB2 on and off. Both forms are tied to the SAME knob so they cannot end up half set.
     *
     * T2 PLAYS NO PART IN THE BLANKING, AND I ONCE WROTE IT HERE BY MISREADING THE MODE.
     *
     * I set T2 = 0x7530 copying the reference cartridge, believing that with ACR = 0x98 the
     * shift register ran FREE at the T2 rate. That is false: bits 4-2 of 0x98 are
     * **110 = shift out under Phi2 control**, which shifts 8 bits and STOPS; free-running at
     * the T2 rate is mode 100. In 110 CB2 keeps the last shifted bit, so 0x01 leaves the
     * beam lit and 0x00 blanked — it is a CLOSE, not a working cycle, and T2 has nothing to
     * do with it. Writing it broke nothing and fixed nothing: the dots at the micro-segment
     * joints were unchanged on the console.
     *
     * The reference does write it, but for another reason: T2 is ITS frame timer (its loop
     * waits on T2, which is why T2CH serves as a frame marker when analysing the capture).
     * We mark the frame another way, so we do not need it.
     *
     * HOW THE BEAM REALLY BLANKS, read off the netlist and not off memory: CB2 (`~BLANK`,
     * IC207 pin 19) is not a digital gate — it pulls the Z node through R315 (2.2k) against
     * diode D302, whose cathode is the brightness amplifier's output (IC303C). CB2 low sinks
     * Z and blanks; CB2 high leaves Z at whatever the sample-and-hold is holding. */
    emit(UVM2_VIA_ACR,   BEAM_VIA_SR ? 0x98 : 0x80,    BEAM_VIA_SR ? 23u : 0u);

    /* Prime each sample/hold channel from a DAC value of 0: zero reference,
     * then Y, then Z.  Without this the integrators start wherever the analog
     * section powered up. */
    if (BEAM_VIA_SR) {
        /* THEIR FRAME PROLOGUE, VERBATIM (frame 120, the 5 writes right behind the
         * ACR=0x98):
         *
         *     PCR=CC  clamp ON
         *     ORB=03  mux closed, selection at 1
         *     ORA=00  DAC to zero
         *     ORB=E2  channel 1 (zero reference) open: primed to ZERO
         *     ORB=E1  closed
         *
         * AND NOTHING ELSE: it primes neither Y nor Z here. We did fourteen writes — priming
         * Y, priming Z to 0x7F and a set_z to put it back — before the Z the game asks for,
         * i.e. THREE charges of C306 per frame where it does one. Y and the centre are left
         * to the zero block, which comes right after the Z and releases the clamp itself.
         * See uvm2_frame_begin. */
        /* The gaps are THEIRS, measured in the same capture: the prologue is not only the
         * list of writes, it is also the rhythm they come out at. We had them at zero. */
        emit(UVM2_VIA_PCR,   0xCC, 6);
        emit(UVM2_VIA_PORTB, 0x03, 0);
        emit(UVM2_VIA_PORTA, 0x00, 5);
        emit(UVM2_VIA_PORTB, 0xE2, 9);    /* channel 1's window */
        emit(UVM2_VIA_PORTB, 0xE1, 58);   /* and its settling before the Z */
        s_pcr = 0xCC; s_portb = 0xE1; s_porta = 0x00; s_porta_stale = 0;
        s_pos_x = 0; s_pos_y = 0;
        return;
    }
    set_porta(0x00, 0);
    mux_sample(UVM2_MUX_ZEROREF, UVM2_HOLD_DELAY);
    mux_sample(UVM2_MUX_Y,       UVM2_HOLD_DELAY);
    /* THE Z CHANNEL IS PRIMED AT FULL SCALE, AS THE REFERENCE DOES. Its frame preamble is
     * `ORA=7F, ORB=84 (channel 2), ORB=81` — it primes the brightness hold with 0x7F, not
     * with zero like the other two. We primed it to 0 along with the rest, so between the
     * priming and the game's first SET_INTENSITY the beam ran at MINIMUM brightness. The
     * reference starts at maximum, and coming down is cheap: a `set_z` changes it as soon as
     * the game asks for something else. */
    set_porta(UVM2_Z_PRIME, 0);
    mux_sample(UVM2_MUX_Z,       UVM2_HOLD_DELAY);

    s_y = 0; s_z = UVM2_Z_PRIME; s_pos_x = 0; s_pos_y = 0;
}


/* ── Per-board calibration: WHERE it would go, and why there is none right now ──
 *
 * The model's knobs are the SAME symbol our own cartridge's firmware uses, but their VALUE
 * does not have to be: they are not repository constants, they are MACHINE calibrations. If
 * this board needs its own values, they are written here at startup — they are AtomicU32
 * Relaxed, i.e. a volatile uint32_t from C — instead of forking the code.
 *
 * TODAY NONE IS NEEDED, and that is a result, not an oversight.
 *
 * ── THE SPEED CAP WAS WITHDRAWN ON 2026-09-09 ──────────────────────────────
 *
 * It was `VCAP` (and `VCAP_JUMP`, and the `VCAP_SLOW` rule), and it got as far as 24:
 * "6.7 cycles of wait per unit of length", measured on dkong's 25m screen on 2026-08-27
 * with clean plateaus (20 and 26 fine, 32 marginal, 40 poor -> 24) and 87 cycles per
 * operation against 109. The measurement was good and the knob was wrong: tuning a GLOBAL
 * render constant against ONE game is overfitting, and here it was paid for twice — at 24,
 * asterock came out at 90k frame cycles, 3x the budget.
 *
 * And the default was the WRONG default besides. Capping the speed below the DAC forces t1
 * to be stretched, and one long ramp does NOT travel what two short ones do: in Major
 * Havoc's score table, VCAP=42 doubled t1 to 16 on 109 of 424 lit strokes (rate 29-31)
 * where its own 427 all run with t1=8 at rate 49-51 — and those rows came out stacked on the
 * console. Their rule is simpler: t1 is the SMALLEST one that keeps the rate inside the
 * DAC's range. That is what `DAC_CAP` in ramp.rs does today, and it is not an adjustment but
 * the converter's full scale.
 *
 * It is written down here because the 25m measurement is still valid AS a measurement; what
 * was not valid was having a knob for it. */
extern volatile uint32_t MIN_T1, MIN_T1_START, DAC_ZERO, DRAW_SCALE, T1_TRANSPORT;
extern volatile uint32_t T1_JUMP;
/* THE MICRO-SEGMENT GAPS, as globals like the rest of the draw layer's knobs. They were
 * fields of vx_timings and it did NOT work: with both structs the same size and the right
 * value on the C side, Rust produced a gap saturated at 4095 per vector and the frame blew
 * out 30x. See the MT_ORA_Y block in emit.rs. */
extern volatile uint32_t MT_ORA_Y, MT_ORB_KEEP, MT_SR_ON, MT_ORA_X_ON;
extern volatile uint32_t CEILING_RULES, DEBT_ON, INTEGER_STROKE;

/* DAC_ZERO: put PORT A back to zero after each stroke, before blanking the beam. It is one
 * command and one E cycle per LIT stroke. A game sets it with -DUVM2_DAC_ZERO=0 after
 * measuring it on ITS console; without that it is 1 and changes nothing for anyone. */
#ifndef UVM2_DAC_ZERO
#define UVM2_DAC_ZERO 1
#endif

/* Between one list and the next, with the bus free: our own cartridge's BIOS defines it
 * (controllers, PSG). In the .um2 it is not needed: core 1 does it in its loop. */
__attribute__((weak)) void uvm2_frame_gap(void) { }

/* Forward-declared: uvm2_draw_init draws (the Z priming) before the cache defined further
 * down exists, and an unfilled structure is a null function pointer. */
static void vx_cart_refresh(void);

void uvm2_draw_init(void)
{
    /* THE CONSOLE'S CALIBRATION, BEFORE ANYTHING ELSE. The knobs it touches — scale, the
     * ramp's fixed term, the zero reference and the brightness — describe THIS tube, not
     * this game, so they are loaded here and all 44 ports inherit them without touching 44
     * files. If none is saved, nothing changes: `uvm2_config_load` returns 0 and the game
     * keeps whatever it was compiled with, which is the usual behaviour.
     *
     * The WIZARD is deliberately not opened from here: the game owns the frame loop, and
     * launching a user interface from an init function would be a hidden side effect. The
     * game does `if (!uvm2_config_load()) uvm2_config_wizard();`. */
    uvm2_have_calibration = uvm2_config_load();

    /* THE WAY OUT: PCR blanking is still alive for whoever needs it. Since 2026-09-04 the SR
     * dialect is the default (BEAM_VIA_SR starts at 1), so what is needed is a way to turn it
     * OFF, not on. */
#ifdef UVM2_BEAM_VIA_PCR
    BEAM_VIA_SR = 0u;
#endif
#ifdef UVM2_BEAM_VIA_SR
    /* THE BUILD KNOB HAS TO LAND ON THE LIVE SYMBOL. The build script had been defining
     * UVM2_BEAM_VIA_SR since 2026-09-03, but nobody consumed it: BEAM_VIA_SR stayed at its
     * default 0, via_setup emitted ACR=0x80 and the whole SR dialect was a no-op — CB2 stays
     * low through the PCR (0xCC/0xCE) and NEVER lights. In the emulator: a black screen with
     * a perfectly correct list. */
    BEAM_VIA_SR      = 1u;
#endif
    /* The reference cartridge draws no units shorter than 8 counts: its plotter sets t1 =
     * max(8, len*scale/127) and splits into micro-segments of 8 with a final REMAINDER
     * (T1CL=4/6/7 in the capture). With MIN_T1=8 and the cap at the DAC's full scale
     * (`DAC_CAP`, ramp.rs) we reproduce that model exactly.
     *
     * THE FLOOR IS THE DEFAULT, BECAUSE IT IS THE OTHER HALF OF THEIR RULE.
     *
     * It defaulted to 1 — "dkong's ones" — i.e. another global constant tuned against one
     * game, just like the speed cap withdrawn the same day. While the cap was 24 it did not
     * show: no stroke went below 8 because the speed was bounded. Once the cap is released,
     * the floor becomes the only thing preventing a 3-count ramp, and that does not exist in
     * their capture.
     *
     * MEASURED in snowbros, same scene, changing only the cap:
     *     cap 24, floor 1     251 strokes, NONE below t1 = 8
     *     cap 127, floor 1    251 strokes, 68 (27%) with t1 of 3, 4, 6 and 7 -> broken
     * And on the console: "snowbros less broken, but broken too. the framerate is good" —
     * which is exactly the signature: shorter ramps (cheaper frame) and wrong geometry.
     *
     * A game lowers it with -DUVM2_MIN_T1=N if it has a measurement of ITS OWN to justify
     * it. */
#ifndef UVM2_MIN_T1
#define UVM2_MIN_T1 8
#endif
    MIN_T1          = UVM2_MIN_T1;
    MIN_T1_START    = UVM2_MIN_T1;
    /* FIXED-TIME JUMP (the reference dialect: t1 given, rate variable). With this the DAC
     * cap stops deciding the duration — they are two alternative models of the same jump
     * (fixed rate / fixed time), not two settings that add up. Measured in Major Havoc:
     * t1=31 on 99% of its jumps. See the T1_JUMP block in ramp.rs. */
#ifdef UVM2_T1_JUMP
    T1_JUMP        = UVM2_T1_JUMP;
#endif
#ifdef UVM2_MT_ORA_Y
    MT_ORA_Y        = UVM2_MT_ORA_Y;
#endif
#ifdef UVM2_MT_ORB_KEEP
    MT_ORB_KEEP     = UVM2_MT_ORB_KEEP;
#endif
#ifdef UVM2_MT_SR_ON
    MT_SR_ON        = UVM2_MT_SR_ON;
#endif
#ifdef UVM2_MT_ORA_X_ON
    MT_ORA_X_ON     = UVM2_MT_ORA_X_ON;
#endif
    /* WHO RULES ON SHALLOW DIAGONALS: the minor axis's ceiling (1, the long-standing one) or
     * the speed cap (0). See the `ramp_params_q` note in ramp.rs. Compare on the console
     * before moving the default. */
#ifdef UVM2_CEILING_YIELDS
    CEILING_RULES     = 0u;
#endif
    /* THE DEBT TURNS ITSELF OFF WHEN THE INPUT IS PRECISE.
     *
     * It exists to compensate the rounding of the INPUT: with geometry in 1/16 the residue
     * accumulates and has to be collected. At 1/64 or finer the input is already almost
     * exact and the correction stops removing error and starts ADDING it.
     *
     * MEASURED against their frame 120 of Major Havoc, same input geometry, comparing the
     * sequence of lit strokes IN ORDER:
     *
     *     with debt   418 of 427 identical to theirs   (97.9%)
     *     without     426 of 427                        (99.8%)
     *
     * The 8 that diverge are all +-1 on a rate, and the correction puts every one of them
     * there. The threshold is the same 6 at which the input stops losing rates of theirs:
     * see the UVM2_Q_BITS note. -DUVM2_DEBT_OFF / -DUVM2_DEBT_ON force either one. */
#if UVM2_Q_BITS >= 6
    DEBT_ON        = 0u;
#endif
#ifdef UVM2_DEBT_ON
    DEBT_ON        = 1u;
#endif
#ifdef UVM2_DEBT_OFF
    DEBT_ON        = 0u;
#endif
    /* ONE STROKE = ONE RAMP, like the reference, and it is the default: confirmed on the
     * console on 2026-09-04 (with micro-segments, Major Havoc was dotted; with the whole
     * stroke it comes out clean). See the INTEGER_STROKE note in emit.rs. */
#ifdef UVM2_MICROSEGMENTS
    INTEGER_STROKE    = 0u;
#endif
    DAC_ZERO        = UVM2_DAC_ZERO;
    /* THE SCALE, if the game sets it (-DUVM2_DRAW_SCALE / -DUVM2_T1_TRANSPORT). Without
     * that it keeps the long-standing one. It exists because the good scale was found from
     * the debug panel and EVAPORATED on the first reboot: a live knob is not a decision until
     * it is compiled in. */
#ifdef UVM2_DRAW_SCALE
    DRAW_SCALE      = UVM2_DRAW_SCALE;
#endif
#ifdef UVM2_T1_TRANSPORT
    T1_TRANSPORT    = UVM2_T1_TRANSPORT;
#endif

    s_count = 0;
    via_setup();
    /* PRIMING Z, ONCE AND AT STARTUP. It used to live in via_setup, i.e. once PER FRAME,
     * and now the frame prologue is theirs and does not prime Z. It is still needed here in
     * case a game draws before its first SET_INTENSITY: without this the beam would run at
     * whatever C306 happened to hold at power-up. Full scale, like the reference. */
    vx_cart_refresh();         /* this draws before the first frame_begin */
    if (BEAM_VIA_SR) set_z(UVM2_Z_PRIME, UVM2_HOLD_DELAY);   /* s_z_last starts there too */

    uvm2_stats.bus_cycles = uvm2_exec(s_cmds[s_buf], s_count);
    uvm2_stats.commands   = s_count;
    s_count = 0;
}

/* Forget what we believe the hardware holds.
 *
 * set_porta/set_y/set_z skip a write when their cached value already matches —
 * a real saving, since a vector that reuses the same Y costs four commands less.
 * The cache is only valid while NOTHING ELSE drives the DAC or the mux. The
 * joystick conversion does both: one CD4052 serves the pots and the beam on
 * shared select lines, so a SAR sweep on an axis lands in the Y sample-and-hold
 * as a side effect. The software then believes Y is still correct, skips the
 * write, and the frame's first vector is drawn at whatever Y the joystick left —
 * a stray bright segment that FOLLOWS THE STICK. (Seen on hardware 2026-08-04;
 * the cart does not show it because its read_axes re-zeros the beam and its SDK
 * resets its own tracking every frame.)
 *
 * Call this after anything that touches Port A/B outside the draw stream. */
void uvm2_draw_invalidate(void)
{
    s_porta_stale = 1;
    s_y = INT_MIN;
    s_z = INT_MIN;
}

/* Re-prime the beam's sample-and-holds from a DAC value of 0.
 *
 * The analog joystick read does not merely disturb the mux — it WRITES THROUGH
 * it. `read_axis_analog` drives Port B with PB0 low, which ENABLES the CD4052,
 * and the two halves share select lines, so while the SAR sweeps the DAC to find
 * an axis it is simultaneously charging a beam sample-and-hold:
 *
 *     mux channel 0  (joystick X)  ->  Y sample-and-hold
 *     mux channel 1  (joystick Y)  ->  the ZERO REFERENCE capacitor
 *
 * Y is rewritten by the draw path every frame once its cache is invalidated. The zero
 * reference it never rewrites — uvm2_draw_init primed it ONCE at start-up — so
 * after the first joystick read the beam's idea of centre is whatever the SAR
 * left behind. That is the bright streak through the middle of the screen which
 * tracks the stick, in every scene (hardware, 2026-08-04).
 *
 * Cheap: three mux samples, ~9 commands and ~24 bus cycles of a 30000 budget. */
void uvm2_draw_prime_holds(void)
{
    set_porta(0x00, 0);
    mux_sample(UVM2_MUX_ZEROREF, UVM2_HOLD_DELAY);
    mux_sample(UVM2_MUX_Y,       UVM2_HOLD_DELAY);
    set_porta(UVM2_Z_PRIME, 0);          /* full scale, as via_setup does */
    mux_sample(UVM2_MUX_Z,       UVM2_HOLD_DELAY);
    s_y = 0;
    s_z = UVM2_Z_PRIME;
}

/* Defined below with the other knobs; used up here. */
extern volatile int32_t uvm2_zero_settle_e;
extern volatile int32_t uvm2_zero_offset;
extern volatile int32_t uvm2_min_gap;

void uvm2_draw_reset(void)
{
    /* BLANK BEFORE CLAMPING THE ZERO. With keep-lit (the SR dialect) the beam arrives at the
     * re-zero still LIT; the blanking lives in beam_off() and set_zero(1) calls it too, but
     * here we also have to blank BEFORE re-priming the reference (mux). */
    beam_off_and_wait();   /* the SR needs its 8 cycles BEFORE the clamp moves the beam */

    if (BEAM_VIA_SR) {
        /* THE REFERENCE CARTRIDGE'S ZERO BLOCK, VERBATIM (x10,517 in the capture,
         * 43 cycles):
         *
         *     PCR=CC +7  clamp ON
         *     ORB=81 +1  mux parked
         *     ORA=00 +6  DAC to zero
         *     ORB=C0 +11 channel 0 (Y) open: Y is re-primed INSIDE the clamp
         *     ORB=82 +1  channel 1 (zero reference) open
         *     ORA=of +7  the OFFSET calibrated per console (theirs: 0x07)
         *     ORA=FF +4  the final $FF touch (its meaning is still OPEN; it is reproduced
         *                as-is — the rule is to copy, not to invent)
         *     ORB=83 +6  mux closed
         *     PCR=CE +9  clamp released
         *
         * Priming AGAINST the clamp is what this file already argued for in via_setup; the
         * reference does it on EVERY re-zero and in 43 cycles, not in our ~86. The offset is
         * a MACHINE calibration: 0 keeps our current calibration; sweep it on the console. */
        if (s_pos_x == 0 && s_pos_y == 0 && (s_pcr & UVM2_PCR_ZERO_OFF) != 0)
            return;                     /* already centred and released: do not repeat */
        s_drift_ax = 0; s_drift_ay = 0;   /* what set_zero(1) used to do */
        s_ramps_from_zero = 0;
        /* AND THE DEBT, WHICH HERE IS THROWN AWAY. The re-zero puts the beam back at centre
         * in hardware: what we believed and what is there coincide again, so there is
         * nothing left to owe. A JUMP does not throw it away (see vx_chain_reset): there the
         * beam is still where it was, just somewhere other than where we think. */
        vx_debt_reset();
        emit(UVM2_VIA_PCR,   0xCC, 6);
        emit(UVM2_VIA_PORTB, 0x81, 0);
        emit(UVM2_VIA_PORTA, 0x00, 5);
        emit(UVM2_VIA_PORTB, 0xC0, 10);
        emit(UVM2_VIA_PORTB, 0x82, 0);
        emit(UVM2_VIA_PORTA, (uint8_t)uvm2_zero_offset, 6);
        /* THE `ORA=0xFF` GOES BEFORE CLOSING THE MUX, AS THEIRS DOES. AND I GOT THIS WRONG.
         *
         * I had it inverted on purpose, reasoning that with the reference channel OPEN that
         * 0xFF charges the ZERO REFERENCE capacitor to 127 instead of to the offset — and
         * that reference is the beam's ORIGIN (dx = xsh - rsh, dy = rsh - ysh), so it would
         * displace every vector. I blamed it for some "open vectors and shimmer" that
         * appeared around then.
         *
         * THE CAPTURE PLAYER REFUTES IT: it replays THEIR stream byte for byte on our board,
         * in this very order, and the drawing comes out clean with no open vectors. If it
         * works in theirs, the mechanism I feared does not happen, or something else
         * compensates for it — and what I was fixing was somewhere else.
         *
         * WHAT that 0xFF is for is still unknown. But not knowing what it is for is no
         * reason to do it differently: the reference says this is how it goes. */
        emit(UVM2_VIA_PORTA, 0xFF, 3);
        emit(UVM2_VIA_PORTB, 0x83, 5);
        emit(UVM2_VIA_PCR,   0xCE, 8);
        /* The caches, with the LAST thing actually written: the order is now ORA=FF and
         * then ORB=83, so Port B ends at 0x83 and Port A at 0xFF just the same, but the one
         * that closes the sequence is the ORB. */
        s_pcr = 0xCE; s_portb = 0x83;
        s_porta = 0xFF; s_porta_stale = 0;
        s_y = 0;                        /* channel 0 was left primed at zero */
        s_pos_x = 0; s_pos_y = 0;
        return;
    }

    /* RE-PRIME THE ZERO REFERENCE, THE WAY THE BIOS DOES.
     *
     * Reset0Ref ($F354) falls into Reset_Pen ($F35B), which on EVERY re-zero puts the DAC to
     * zero and gives the reference channel its mux cycle:
     *
     *     CLR <VIA_port_a      ; DAC to zero
     *     STA <VIA_port_b      ; mux=1, disabled
     *     STB <VIA_port_b      ; mux=1, enabled
     *     STB <VIA_port_b      ; again
     *     LDB #$01 / STB       ; disable
     *
     * We did it ONCE PER FRAME, in via_setup. And that capacitor leaks: if the reference has
     * drifted, the "zero" the beam returns to is not the same at the start of the frame as
     * at the end, and the error depends on how many objects were drawn before it — i.e. it
     * CHANGES when a barrel appears. That is shimmer, and it matches what the console
     * showed.
     *
     * It costs three writes and their settling per re-zero. With object boundaries there are
     * few re-zeros, so it comes out cheap. UVM2_NO_REPRIME_ON_RESET disables it for
     * comparison. */
#ifndef UVM2_NO_REPRIME_ON_RESET
    set_porta(0x00, 0);
    mux_sample(UVM2_MUX_ZEROREF, hold_for(0, 0));
#endif

    /* Already centred and already released? Nothing to do — re-zeroing is the
     * single most expensive thing a frame can repeat needlessly. */
    if (s_pos_x == 0 && s_pos_y == 0 && (s_pcr & UVM2_PCR_ZERO_OFF) != 0)
        return;

    set_zero(1, uvm2_zero_settle_e >= 0 ? (uint32_t)uvm2_zero_settle_e
                                       : UVM2_ZERO_BASE + s_scale / 4u);
    set_zero(0, 0);
    s_pos_x = 0;
    s_pos_y = 0;
}

/* The last intensity asked for, which SURVIVES the frame. via_setup() primes Z to 0 every
 * frame, so without this the gap between releasing the clamp and the game's first
 * SET_INTENSITY is travelled with an unknown Z. The reference has no such gap: it sets Z
 * and THEN releases the clamp (SetZ(0x5F); SetZero(false);). */
static int s_z_last = UVM2_Z_PRIME;   /* otherwise frame_begin undoes the Z priming */

/** The brightness last asked for. `uvm2_config_current` needs it, since it has to be able
 *  to READ the knobs and not only write them. */
int uvm2_draw_intensity_current(void) { return s_z_last; }

void uvm2_draw_intensity(int brightness)
{
    if (brightness < 0)   brightness = 0;
    if (brightness > 127) brightness = 127;
    s_z_last = brightness;
    set_z(brightness, UVM2_HOLD_DELAY);
}

/* While both deltas are small, trade DAC range for ramp time: doubling the
 * delta and halving the scale draws the same length in half the cycles.  This
 * is the reference writer's Fixup(), left in but disabled. */
/* Doubling dx,dy while halving the ramp draws the SAME vector in half the time:
 * the distance is the DAC value times the ramp duration, so the product is what
 * matters and only the time costs us anything.  Two limits stop the halving.
 *
 *  - The DAC.  Port A is a signed 8-bit converter, so a value can be doubled only
 *    while it is below half of full scale.  That bound is derived from the part,
 *    not chosen: UVM2_DAC_HALF.
 *
 *  - How short a ramp the analog section still integrates linearly.  That number
 *    is NOT known.  It belongs to the integrator's time constant and to the
 *    settling of the sample-and-holds feeding it, and nothing we have measured
 *    pins it down — so it is a build-time knob meant to be SWEPT on hardware with
 *    the geometry in view, not a constant to be tuned by eye.  32 is merely where
 *    it has sat since the fixup was written; it has never been measured, and it
 *    is currently the single biggest term in the frame (the ramp is ~90% of the
 *    time once beam travel is accounted for).
 *
 * Expect a plateau rather than a cliff when sweeping it: take the middle of the
 * range where the picture is still square, not the last value that survives. */
#define UVM2_DAC_FULL_SCALE 128
#define UVM2_DAC_HALF       (UVM2_DAC_FULL_SCALE / 2)

#ifndef UVM2_RAMP_FLOOR
#define UVM2_RAMP_FLOOR 32u
#endif

/* `fixup` IS GONE, and not as a matter of opinion: NOBODY CALLED IT. It doubled the delta
 * and halved the ramp while the DAC had headroom, and its measured justification — "128 of
 * the ~169 cycles per vector were the FIXED ramp" — describes a ramp model this SDK no
 * longer has: since 2026-08-27 the duration comes from the length
 * (t1 = len*DRAW_SCALE/DAC_CAP), so a short stroke no longer pays for a full-range ramp and
 * there is nothing to fix. It sat there defined, without a single call, and
 * `uvm2_draw_set_fixup(1)` in uvm2_svc.c set a flag nobody read — a line that says
 * something is on when it does not exist, which is worse than not having it (the same
 * argument that deleted `set_ramp` a few lines above). The reference does nothing similar
 * either: its plotter splits the length into micro-segments of 8, not into scale. */

/* ── Drift compensation ────────────────────────────────────────────────────
 *
 * MEASURED ON THE CONSOLE on 2026-08-12 with a bench that draws a rectangular grid of jumps
 * and lets the correction be adjusted from the controller until it comes out straight.
 * Uncompensated, the grid comes out as a diagonal cascade: the beam does NOT end where it
 * is told, and the error accumulates jump after jump.
 *
 *     X  -16/256  = -0.06 units per jump
 *     Y -112/256  = -0.44 units per jump
 *
 * SEVEN TIMES MORE IN Y, and that has a physical explanation: X goes straight to the DAC,
 * Y goes through the mux and the sample-and-hold. The capacitor is what adds it. That is why
 * EVERY symptom that day was vertical — sagging steps in Y, Kong split in half, the scenery
 * shrunk.
 *
 * The sign says the beam OVERSHOOTS, not that it falls short: the deflection-delay
 * hypothesis said the opposite. It fits with the integrator carrying on for an instant
 * after the ramp is frozen.
 *
 * THE ACCUMULATOR IS WHAT MAKES IT WORK. The error is a FRACTION of a unit, and adding a
 * whole unit per jump goes from "short" to "overshoot" with no middle point (verified: with
 * 1 unit the columns went off the other way). It is carried in 1/256 and only transferred to
 * the delta when a whole unit is complete; the remainder is kept. It is Bresenham: the mean
 * correction is exact even though each step is an integer.
 *
 * It is zeroed in frame_begin: the frame's re-zero wipes the real drift, so carrying the
 * accumulator over would be correcting an error that no longer exists.
 *
 * COST: ZERO bus cycles. It is arithmetic on a delta that was going to be written anyway.
 * To disable it, UVM2_DRIFT_X/Y = 0.
 *
 * ADJUSTABLE HOT, not defines: the bench moves them from the controller and over SWD
 * without recompiling.
 *
 *   uvm2_drift_mode 0 -> the correction follows the SIGN of the jump
 *                   1 -> FIXED direction, always to the same side
 * Those two hypotheses are indistinguishable if every jump goes the same way, which is the
 * flaw the first bench had. With text they separate.
 *
 * ZERO = DISABLED, and it stays that way until it is properly measured. What IS measured is
 * that the error EXISTS and is systematic; the MODEL is not closed: the first bench could
 * not tell whether the correction should follow the sign of the jump or always go to the
 * same side, because all its jumps went the same way. Applying it with the wrong model makes
 * dkong worse (tried: platforms shifted upwards and shimmer when the barrels appear). */
volatile int32_t uvm2_drift_x    = 0;
volatile int32_t uvm2_drift_y    = 0;
volatile int32_t uvm2_drift_mode = 0;


void uvm2_draw_drift_reset(void) { s_drift_ax = s_drift_ay = 0; }

static int drift_fix(int d, int32_t *acc, int32_t cte)
{
    if (cte == 0) return 0;
    if (uvm2_drift_mode == 0 && d == 0) return 0;   /* signed: no jump, no error */
    *acc += cte;
    int32_t integer = *acc / 256;
    *acc -= integer * 256;
    if (uvm2_drift_mode) return (int)integer;        /* fixed direction */
    return (d > 0) ? (int)integer : -(int)integer;    /* with the jump's sign */
}


/* ── Sink into the SHARED draw layer ───────────────────────────────────────
 *
 * The beam model no longer lives here: it lives in `vectrex-draw`, the same crate our own
 * cartridge's firmware links. What is left on this side is stacking up the commands the
 * model emits — which is the only thing that really differs between the two boards.
 *
 * There is no second path: the layer lives in `../vectrex-draw`, in this same tree, and
 * CMake builds it. There is ONE beam model. */

struct vx_sink {
    void *ctx;
    void (*emit)(void *, uint32_t, uint32_t, uint32_t);
    void (*wait_ramp)(void *, uint32_t, int32_t);
    void (*beam_blanked)(void *);
    void (*y_held)(void *, int32_t);
    /* THE ORDER AND THE COUNT HAVE TO MATCH `CSink` in vectrex-draw/src/emit.rs.
     * These three used to be declared separately, in a `vx_sink_extra` NOBODY CONSUMED: the
     * emitter asks for them, but Rust's `CSink` did not have them, so they always returned
     * the trait's default (false) and the UVM2 paid for the Y sampling and the blank/lit
     * pair on EVERY vector. Our own cartridge's firmware does implement them — it uses the
     * crate as an rlib — which is why only one of the two boards had the emitter
     * optimisations they share. */
    int  (*y_can_skip)(void *, int32_t);
    int  (*beam_is_lit)(void *);
    void (*beam_lit)(void *);
    int  (*x_can_skip)(void *, int32_t);
    /* AT THE END, like the other optional ones: whoever does not fill it in keeps the old behaviour. */
    void (*extend_last)(void *, uint32_t);
};
struct vx_timings { uint32_t e6809_q8, y_mux_q8, moveto_settle_q8, beam_on_q8;
                    int32_t blank_settle_q8; uint32_t keep_lit; uint32_t x_settle_q8;
                    /* The micro-segment gaps, in E cycles. 0 = the long-standing value
                     * (the cadence measured from the reference's asterock). They are
                     * parameterised because the reference cartridge does NOT have a single
                     * cadence: it builds a different plotter for each title. See the block
                     * in emit.rs. */
                    uint32_t mt_ora_y, mt_orb_keep, mt_sr_on, mt_ora_x_on; };
extern volatile uint32_t UNITS_CONTINUE;   /* in emit.rs */
void vx_moveto_seq(struct vx_sink *, int32_t vx, int32_t vy, uint32_t t1,
                   const struct vx_timings *);
void vx_draw_line_patterned_seq(struct vx_sink *, int32_t vx, int32_t vy, uint32_t t1,
                                const struct vx_timings *, const uint16_t *gaps, uint32_t n);
void vx_draw_line_seq(struct vx_sink *, int32_t vx, int32_t vy, uint32_t t1,
                      const struct vx_timings *);
void vx_ramp_params(int32_t dx, int32_t dy, int32_t *vx, int32_t *vy, uint32_t *t1);

/* The delay arrives in Q8 of an E cycle; the command field is an integer, so resolution has
 * to be dropped. IT IS TRUNCATED, not rounded.
 *
 * Rounding to nearest looks correct — truncating biases the gaps downwards — and that is why
 * I wrote it that way. But OUR CARTRIDGE TRUNCATES: `e6809_raw` does `cycles / 256` and
 * `stream_park(n - 1)`, so with E6809_SCALE_Q8 = 64 a gap of `e(2)` is 128 Q8 and there it
 * waits ZERO. Rounding, here it waited ONE. That is ~6 gaps per vector: with 300 vectors,
 * about 1,800 extra E cycles per frame — 6% of the 30,000 budget, on a board that was
 * already not making it.
 *
 * Between "correct in the abstract" and "what the other board does", the second wins: two
 * implementations of the same model that round differently are two models.
 *
 * EXTENDING THE GAP OF THE LAST COMMAND ALREADY EMITTED.
 *
 * WHY IT IS NEEDED. MEASURED in their frame 120 of Major Havoc: the gap the reference leaves
 * after `T1CH` is 11 E cycles when the micro-segment CONTINUES, and 16 when the next step
 * blanks the beam — the separation is perfect, 175 of 175. It is physical: the last ramp of
 * a lit stroke has to FINISH before the beam is closed; cutting it at 11 makes the stroke
 * short, which is the signature of the dots.
 *
 * And it goes here, not in `draw_line_seq`, because the one that blanks is the NEXT call
 * (`moveto_seq` or the re-zero): the stroke's emitter cannot know when it emits. */
static void vxs_extend_last(void *ctx, uint32_t extra_q8)
{
    (void)ctx;
    if (s_count == 0u) return;
    uint32_t d = extra_q8 / 256u;
    if (d == 0u) return;
    /* The 24 bits already packed: data in 0-7, register in 8-11, gap in the top 12.
     * ONLY the gap is touched; data and register are copied as they are. */
    uint8_t *p = &s_cmds[s_buf][(s_count - 1u) * 3u];
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    uint32_t gap = (v >> 12) + d;
    if (gap > 4095u) gap = 4095u;
    s_cycles += gap - (v >> 12);
    v = (v & 0xFFFu) | (gap << 12);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
}

/* ONE DAC SAMPLE, PUT INTO THE LIST. See uvm2_smp.h for the model; what justifies
 * the PLACE is this:
 *
 * It is called right before a `T1CL`, and there the previous ramp's timer has
 * ALREADY expired (its gap is 11 cycles for a count of 8), so PB7 is high and the
 * integrators are frozen. Writing Port A does not move the beam — but Port A is
 * the X rate the coming ramp will use, so the last write RESTORES `s_porta`.
 * Without that, the next stroke is drawn at the speed of an audio sample.
 *
 * It is four commands (data, BDIR up, BDIR down, restore X) and it touches
 * neither the PCR nor the ACR nor the shift register: nothing the beam model
 * considers its own, beyond the Port A it hands back as it found it. The visible
 * price is that this point of the stroke sits 4 cycles longer with the beam
 * parked, on top of the ~18 it already sits in every micro-segment — 22% more
 * brightness on one micro-segment in six. If it shows on the console, the knob is
 * UVM2_SMP_HZ. */
static void smp_inject(void)
{
    uint8_t v, base, freeze;

    if (!uvm2_smp_active()) return;           /* safety net: the caller already tests it */
    if (!uvm2_smp_due(s_cycles, &v)) return;

    base = (uint8_t)(s_portb & (uint8_t)~0x18u);   /* the rest of Port B, without BC1/BDIR */

    /* IF THE RAMP IS OPEN, FREEZE IT FIRST — and this is what lets a sample go out
     * during the inter-frame rhythm as well as inside a stroke.
     *
     * Before a T1CL the timer has expired on its own and PB7 is already high: nothing
     * to do. But `frame_filler` runs with ACR=0x18, which takes PB7 away from T1 and
     * leaves the ramp OPEN for its whole zero-reference alternation, using Port A as
     * the discharge current. Writing a sample there without freezing would inject a
     * wrong drive into the integrator — which is why injection used to skip that block
     * entirely. Measured on hardware once the frame was paced at 40 Hz, skipping it
     * left a gap of 7689 bus cycles, 5.1 ms of every 26 with no audio at all: a fifth
     * of the frame silent, which is a buzz, not grain.
     *
     * Freezing costs two commands and pauses the discharge for the six cycles the PSG
     * write takes. It introduces no error — a frozen integrator holds — it only spends
     * about 6% of that block's integration time, which `frame_filler` absorbs by
     * emitting fewer alternation units. */
    freeze = (uint8_t)((s_portb & UVM2_PB_RAMP_OFF) == 0u);
    if (freeze) emit(UVM2_VIA_PORTB, (uint8_t)(base | UVM2_PB_RAMP_OFF), 0);

    /* One full PSG write: address latch, then the data byte. */
#define SMP_PSG(reg, val)                                    \
    do {                                                     \
        emit(UVM2_VIA_PORTA, (reg),            0);           \
        emit(UVM2_VIA_PORTB, base | 0x18u,     0);           \
        emit(UVM2_VIA_PORTB, base,             0);           \
        emit(UVM2_VIA_PORTA, (val),            0);           \
        emit(UVM2_VIA_PORTB, base | 0x10u,     0);           \
        emit(UVM2_VIA_PORTB, base,             0);           \
    } while (0)

    if (uvm2_smp_needs_latch()) {
        /* THE MIXER FIRST, ONCE PER LIST: with its tone bit enabled the channel gates a
         * square wave and the volume only scales it — a tone, not the sample. */
        SMP_PSG(7u, uvm2_smp_mixer());
        /* And leave the volume register addressed, so the rest are three commands. */
        emit(UVM2_VIA_PORTA, UVM2_SMP_REG,     0);
        emit(UVM2_VIA_PORTB, base | 0x18u,     0);
        emit(UVM2_VIA_PORTB, base,             0);
    }
#undef SMP_PSG

    emit(UVM2_VIA_PORTA, v,                    0);
    emit(UVM2_VIA_PORTB, base | 0x10u,         0);   /* BDIR = write the data */
    emit(UVM2_VIA_PORTB, base,                 0);   /* the PSG latches on this fall */
    if (s_portb != base && !freeze) emit(UVM2_VIA_PORTB, s_portb, 0);
    if (freeze) emit(UVM2_VIA_PORTB, s_portb, 0);  /* ramp open again, as it was */
    emit(UVM2_VIA_PORTA, s_porta,              0);   /* the X rate the ramp needs */
}

static void vxs_emit(void *ctx, uint32_t reg, uint32_t data, uint32_t delay_q8)
{
    (void)ctx;
    /* THE AUDIO GAP, BEFORE STARTING THE RAMP. It goes here and not in `emit` on
     * purpose: `emit` is also called by via_setup's prologue, by the recalibration
     * and by the inter-frame rhythm, and in those three the beam is NOT parked
     * (the rhythm opens the ramp through PORTB and uses Port A as the
     * zero-reference discharge alternation). This path is the drawing one, which
     * is the only one with the guarantee. */
    /* `uvm2_smp_active()` is a `static inline` over an integer (uvm2_smp.h): this is a
     * test, not a call. Without it, every T1CL entered `smp_inject` just so its first line
     * could ask the same thing from another translation unit — two calls per ramp in a game
     * with no audio. Exactly the same semantics. */
    if (reg == UVM2_VIA_T1CL && uvm2_smp_active()) smp_inject();
    /* set_porta owns the Port A cache, and the model writes PORTA on its own account.
     * Without this the cache would believe a value that is no longer in the DAC and would
     * skip the next write — a fault that only shows up now and then, which is the worst
     * kind. */
    if (reg == UVM2_VIA_PORTA) { s_porta = (uint8_t)data; s_porta_stale = 0; }
    /* CLAMP BEFORE PACKING. The command puts the delay in `delay << 20`, so a value wider
     * than 12 bits spills into the register and data fields: corrupt commands and a black
     * screen, without a single warning. It was uncovered by a negative beam_on that wrapped
     * around in a uint32_t. It is the same family as the 8192 cap that dropped commands
     * silently — a silent limit is not a limit. */
    uint32_t d = delay_q8 / 256u;
    /* IF THE MODEL ASKS FOR A GAP, LET THERE BE AT LEAST ONE CYCLE.
     *
     * The truncation is deliberate — our own cartridge does it, and two implementations that
     * round differently are two models — but HERE e6809_q8 is 64, not 256. In that unit the
     * grid is in quarters: e(1), e(2) and e(3) are worth 0.25, 0.50 and 0.75 cycles and
     * VANISH ENTIRELY. On our own cartridge, with 256, e(1) is worth a whole cycle. So the
     * two boards are NOT equalised: the UVM2 loses gaps the other one has.
     *
     * Measured consequence in the real list: 593 consecutive writes to ORA/ORB with a gap of
     * zero, 9% of the commands, and 438 of them ORB->ORA — inhibit the mux and write the X
     * DAC in the next E period. Malban warns about exactly that: "it can sometimes be
     * problematic to have ORB / ORA be set too fast without a delay", and that it depends on
     * the console.
     *
     * uvm2_min_gap = 1 sets the floor; 0 keeps the long-standing truncation. */
    if (uvm2_min_gap && delay_q8 && d == 0u) d = 1u;
    if (d > 4095u) d = 4095u;
    emit(reg, data, d);
}

/* NOTHING IS POLLED HERE. On our own cartridge this asks the VIA's T1 flag, like the BIOS;
 * here the list is replayed by an executor that does not read, and reading the VIA in the
 * middle of a list while we drive the data bus is what caused the ghost vectors. The ramp is
 * counted and that is it — and that difference is WRITTEN into the trait, not hidden.
 *
 * ── AND HERE IS THE BUG THAT COST THE AFTERNOON OF 2026-08-18 ───────────────
 *
 * The delay CANNOT hang off the previous command when that command is T1CH.
 *
 * This board's executor holds R/W LOW through all the delay cycles, with the address and
 * data still driven. Its own comment says why that is allowed:
 *
 *     "every register we write (PORTA, PORTB, PCR, ACR, DDRx) takes the same value
 *      IDEMPOTENTLY, which is why the reference executor holds R/W low for the whole
 *      command"
 *
 * That list is THEIRS. Their model never writes T1: it toggles /RAMP through PORTB. Ours
 * writes T1CH — and writing T1C-H **restarts the timer**. It is not idempotent.
 *
 * Hanging the delay off that write makes the VIA re-trigger T1 on EVERY E cycle: PB7 stays
 * low, the integrator does not stop where it should, and the last re-trigger starts a whole
 * count that runs on past the delay. Hence the strokes that overshoot, the drawing coming
 * out bigger than it should, and vertices that do not meet — with the arithmetic coming out
 * perfect, which is what threw six hypotheses in a row off the scent.
 *
 * SOLUTION: the delay is carried by a HARMLESS command behind it. T1LL (the low-byte latch)
 * is rewritten with the same value: writing it touches neither the count in progress nor
 * reloads anything in one-shot mode, so repeating it 95 times does absolutely nothing. It
 * costs one command per vector. */
static void vxs_wait_ramp(void *ctx, uint32_t t1, int32_t extra_q8)
{
    (void)ctx;
    /* SIGNED: negative = blank BEFORE the ramp finishes. It is genuinely needed — measured
     * on the console, raising the delay WIDENS the gap at the vertices, i.e. the knee is
     * below zero. */
    int32_t d = (int32_t)t1 + extra_q8 / 256;
    if (d < 0) d = 0;
#if defined(UVM2_PIO_STREAM) && !defined(UVM2_CMDS_IN_PSRAM)
    /* NO CARRIER: the delay is FOLDED into the previous command. The rewritten T1LL was the
     * delay's carrier for the SIO executor, which holds the write through the wait and
     * cannot hang the delay off T1CH (see the note above). The PIO stream parks with the
     * PARK pattern between commands — it rewrites nothing — so there the delay can live in
     * the field of the command already in the list. The reference cartridge never writes
     * T1LL at all (226 vs 0 per frame was the big remainder in the comparison); this leaves
     * it at 0.
     * SRAM only: patching the list after the fact breaks the PSRAM signature, and the SIO
     * path keeps the carrier because there it really is necessary. */
    if (s_count > 0u && d > 0) {
        uint8_t *p = &s_cmds[s_buf][(s_count - 1u) * 3u];
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        uint32_t cur  = v >> 12;
        uint32_t room = UVM2_CMD_MAX_DELAY - cur;
        uint32_t take = (uint32_t)d < room ? (uint32_t)d : room;
        v = (v & 0x0FFFu) | ((cur + take) << 12);
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
        s_cycles += take;          /* this path does not go through emit: counted by hand */
        d -= (int32_t)take;
    }
    while (d > 0) {                /* very rare remainder (>4095): chained carriers */
        uint32_t chunk = d > (int32_t)UVM2_CMD_MAX_DELAY ? UVM2_CMD_MAX_DELAY : (uint32_t)d;
        emit(UVM2_VIA_T1LL, t1 & 0xFFu, chunk);
        d -= (int32_t)chunk;
    }
#else
    if (d > 4095) d = 4095;        /* the field is 12 bits */
    emit(UVM2_VIA_T1LL, t1 & 0xFFu, (uint32_t)d);
#endif
}

static void vxs_y_held(void *ctx, int32_t vy) { (void)ctx; s_y = (int)vy; }

/* Does the Y S&H already hold this value? The data was already tracked — `set_y` does the
 * same `if (s_y == y) return;` — but the emitter could not consult it and re-emitted the
 * three sampling writes and their charge window. MEASURED in dkong: Y changes on 91% of the
 * vectors, so this fires on the remaining 9%; it is not the big lever, but it is exact and
 * costs nothing. */
static int vxs_y_can_skip(void *ctx, int32_t vy)
{
    (void)ctx;
#ifdef UVM2_NO_Y_SHORTCUT
    /* DIAGNOSTIC KNOB: never skip the Y sampling. It exists because this shortcut is the
     * ONLY asymmetry between the two axes (X reads the live DAC, which invalidates itself;
     * Y believes it remembers what a capacitor is holding), and the measured symptom is
     * asymmetric too: in one frame our list integrates X 168 / Y 173 and the beam draws
     * X 207 / Y 45. Turning it on and re-measuring separates "the S&H does not hold what we
     * think" from any other cause, with a single variable. */
    (void)vy; return 0;
#else
#ifdef VPY_MEASURE_ROUNDING
    { extern volatile int probe_a, probe_b, probe_c;
      probe_a++; if (s_y == (int)vy) probe_b++; probe_c = s_y; }
#endif
    return s_y == (int)vy;
#endif
}

/* THE BEAM, FOR `keep_lit`. Without these two the emitter ALWAYS believes the beam is
 * blanked: with `uvm2_keep_lit = 1` it would take the "light it" branch on every vector and
 * never blank, so the beam would stay lit while the next vector's DACs are being set up and
 * would smear the screen. That is why they go with the knob, not before it. */
static int  vxs_beam_is_lit(void *ctx)  { (void)ctx; return s_beam_on; }
static void vxs_beam_lit(void *ctx)     { (void)ctx; s_beam_on = 1; }
static void vxs_beam_blanked(void *ctx) { (void)ctx; s_beam_on = 0; }

/* X has no sample-and-hold: the DAC holds the LAST thing written to PORT_A, and `s_porta`
 * already tracks that (`vxs_emit` updates it on every write, including the Y sampling ones
 * and the CLR to zero). So no new cache is needed: asking it is exactly the right question,
 * and it invalidates itself. */
static int vxs_x_can_skip(void *ctx, int32_t vx)
{
    (void)ctx;
    return !s_porta_stale && s_porta == (uint8_t)(int8_t)vx;
}

static struct vx_sink vx_cart_sink(void)
{
    struct vx_sink s = { 0 };
    s.emit = vxs_emit;
    s.wait_ramp = vxs_wait_ramp;
    s.y_held = vxs_y_held;
    s.y_can_skip = vxs_y_can_skip;
    s.beam_is_lit = vxs_beam_is_lit;
    s.beam_lit = vxs_beam_lit;
    s.beam_blanked = vxs_beam_blanked;
    s.x_can_skip = vxs_x_can_skip;
    s.extend_last = vxs_extend_last;
    return s;
}

/* The gaps, with the SAME values as our own cartridge.
 *
 * They used to be set to the reference's (e6809 with no discount, 3 and 16 E cycles) on the
 * reasoning that importing ours would be tuning blind. The result was worse than either
 * option: OUR speed model with THEIR timings, a combination nobody had tested. If it is to
 * be unified, it is unified whole — and if something later has to be separated, it is
 * separated with a measurement in front of it.
 *
 * Equivalences, from the firmware:
 *   E6809_SCALE_Q8    = 64        -> as-is, it is already Q8
 *   Y_MUX_ECYC        = 14 E
 *   BEAM_ON_DELAY_CYC = 200 CPU   -> 2 E cycles   (CPU_PER_ECYCLE = 100)
 *   BLANK_SETTLE_CYC  = 1100 CPU  -> 11 E cycles
 *   BEAM_KEEP_LIT     = 0
 *
 * THEY ARE STILL FROM ANOTHER CONSOLE. One console is not evidence: they were measured on
 * ours, against our bus path. They are the starting point of the sweep, not the end — but at
 * least it is now ONE coherent configuration.
 *
 * ADJUSTABLE HOT, not defines. On this board they cannot be touched over SWD — one write
 * stops the core and it does not come back, and resetting drops you into the menu — so the
 * way in is the CONTROLLER, the same way the drift bench did it. Volatile and not static:
 * they have to survive the linker and be visible.
 *
 * MEASURED ON THIS CONSOLE on 2026-08-18 with the beam-tuning bench, adjusting from the
 * controller and watching the pentagon's vertices. With 16 and 2 the corners opened up; with
 * 12 and 0 they close. A small spur is left at the top vertex.
 *
 * And they are NOT our own cartridge's (11 and 2), which is the point: this is MACHINE
 * calibration. The 11/2 came from a bisection on OUR console, with a different bus path and
 * a different deflection amplifier. Here, what you see here rules.
 *
 * `beam_on = 0` deserves a note: light the beam AT THE SAME TIME as the ramp starts. On our
 * console that leaves a bright dot at the departing vertex, which is why there is a 2; here
 * the transport already inserts its own E period between the two writes, so the explicit
 * delay is redundant. The same physical quantity, paid for by someone else. */
volatile int32_t uvm2_beam_on_e = 0;        /* E cycles between starting and lighting */
/* RUN-TIME KNOBS, not constants. On this board nothing can be read over SWD — the core
 * drives the bus with its E phase locked to the clock and stopping it is a phase violation —
 * so sweeping a constant cost one flash per value. These are adjusted from the service menu
 * with the controller, with the fps alongside.
 *
 * y_mux: the Y mux window. It was 2 and somebody raised it to 14 (see emit.rs). That is 12 E
 * cycles on EVERY operation that changes Y: ~5400 per frame, 3.6 ms.
 * keep_lit: keep the beam lit between chained segments, which saves the pair of BLANK writes
 * and their settling. It has never been tried here.
 *
 * 4, MEASURED ON THIS CONSOLE on 2026-08-27 and not inherited. Plateau: 11 (which is what
 * came from our own cartridge), 8, 6, 4 and 2 all draw correctly WHILE PLAYING — with big Y
 * jumps, which is when the sample-and-hold has the worst of it — and 0 BREAKS. Centre of the
 * plateau: 4. It is worth 5 of the 93 cycles per operation. It used to be the 6809's gap of
 * 12 minus the write's own cycle. */
/* THE ZERO CLAMP, which until today was a worst-case constant.
 *
 * `UVM2_ZERO_BASE + s_scale/4` gives 85 E cycles and they are paid IN FULL on every re-zero,
 * wherever the beam happens to be. MEASURED on 2026-08-27 by reading the command list the
 * cartridge was replaying over SWD: 73 re-zeros per frame at 85 = 6205 cycles, 9.5% of the
 * frame and the second largest cost after the ramp.
 *
 * And the 45 comes from nowhere: its comment said "centring cost" and nothing else. The BIOS
 * does not help, because it DOES NOT WAIT — `Reset0Ref` ($F354) drives /ZERO low, does
 * Reset_Pen's mux cycle and RTS. On the Vectrex the zero stays asserted while the 6809 goes
 * off to do something else, so there the duration is set by the program, not by the routine,
 * and there is no number to copy.
 *
 * Since there is nothing to derive it from, it is swept on the console with the drawing in
 * front of you, the same way y_mux was. Every cycle taken off it is worth 73 frame cycles.
 * With 0 it has to BREAK: that is the test that the knob reaches the drawing.
 *
 * ONLY THE PER-OBJECT RE-ZERO. The end-of-frame clamp (uvm2_frame_end) keeps the constant:
 * it happens once, it does not weigh anything, and shortening it only risks burning a dot.
 *
 * A game can fix it in its Makefile (-DUVM2_ZERO_SETTLE_E=N) after measuring it on its own
 * console; without that it is -1 and changes nothing for anyone. */
#ifndef UVM2_ZERO_SETTLE_E
#define UVM2_ZERO_SETTLE_E (-1)
#endif
volatile int32_t uvm2_min_gap = 0;   /* see the note in vxs_emit */
volatile int32_t uvm2_zero_settle_e = UVM2_ZERO_SETTLE_E;   /* <0 = ZERO_BASE + scale/4 */
/* THE ZERO REFERENCE. The zero block loads this value into the reference channel, and that
 * channel is the beam's ORIGIN (dx = xsh - rsh, dy = rsh - ysh): if it is not the one the
 * machine expects, the WHOLE frame comes out displaced.
 *
 * 7, MEASURED IN THEIR ZERO BLOCK. Comparing ours with theirs in the same Major Havoc frame,
 * the nine writes match register for register and gap for gap except this one: they write
 * ORA=07 where we put ORA=00. It fits what was already written down — other reference
 * software also writes a NON-ZERO value there, and the BIOS puts 0.
 *
 * It can be overridden per game with -DUVM2_ZERO_OFFSET if some console asks for another:
 * this is machine calibration, not a universal constant. */
/* HOW MANY RAMPS BETWEEN ZERO CLAMPS.
 *
 * MEASURED in their frame 120 of Major Havoc: the reference puts 12 zero blocks in the BODY
 * of the frame, separated by 72, 66, 55, 73, 67, 57, 52, 55, 52, 61 and 59 ramps — median
 * 59, and far tighter by ramp count (12% spread) than by time (20%).
 *
 * WE CLAMPED ONCE PER FRAME, and the position error accumulated across all 616 ramps with
 * nothing wiping it: on the console, each row of the high-score list came out further right
 * and further down than the one above, in a staircase. The list is correct — integrated as a
 * beam it ends 1.4 units from what was asked — so what drifted was the INTEGRATOR, and the
 * only thing that works against that is going back to zero.
 *
 * THIS EXISTED, BUT IN THE LAYER ABOVE: `VPY_MAX_CONSECUTIVE_DRAWS` lives in sdk_rp2350.c,
 * i.e. only the SBT ports have it. A bench (or a game) calling the SDK directly NEVER
 * re-centred. Bounding the drift is not an optimisation of an optional layer: it goes here,
 * and it acts as a FLOOR — any re-zero, wherever it comes from, resets the counter, so if the
 * layer above already clamps more often, this one never fires.
 *
 * 0 disables it. */
#ifndef UVM2_ZERO_EVERY
#define UVM2_ZERO_EVERY 0   /* the COUNT net: off; distance rules (UVM2_ZERO_JUMP) */
#endif
volatile int32_t uvm2_zero_every = UVM2_ZERO_EVERY;

/* RE-CENTRE WHEN THE JUMP IS LONG — THEIR CRITERION, MEASURED.
 *
 * Pairing every blanked transport in their captures with whether it carries a zero block in
 * front, over 2020 transports across 8 frames:
 *
 *     the 66 it re-centres:  min 20.1  median 42.0  max 82.0 units
 *     the 1954 it does not:  p90 3.5   p99 20.2   max 37.1
 *
 * So: **it re-centres when the beam is about to travel far**, not every N ramps. With the
 * threshold at 20 it gets 66 of 66 right and only 22 of 1954 (1.1%) would be spurious. And it
 * makes physical sense: a long jump is where the accumulated error weighs most and where the
 * re-zero is free, because the beam is going to cross the screen anyway.
 *
 * WE re-centred by RAMP COUNT, and that lets long jumps through: measured in mhavoc,
 * transports of up to 102 units with no re-centring. On the console it looked like objects
 * shifting from one frame to the next — "heartbeats".
 *
 * 0 disables it; `uvm2_zero_every` remains as a safety net in case a scene has no long
 * jumps. */
#ifndef UVM2_ZERO_JUMP
#define UVM2_ZERO_JUMP 24   /* the floor both captures share; see move_abs_internal */
#endif
volatile int32_t uvm2_zero_jump = UVM2_ZERO_JUMP;

/* Emit the pen-up when the requested jump measures ZERO. See move_abs_internal. */
#ifndef UVM2_PENUP_ZERO
/* 0 BY DEFAULT, AND NOT OUT OF CAUTION: with the bench's geometry there is no way to know
 * when it applies. Their 12 pen-ups carry SR=00/SR=01 around them, but the reference geometry
 * file only stores the LIT segments — where they lifted the pen is not in the file. Turned on,
 * it fires on all 240 chained strokes (against their 12) and sinks the transport similarity
 * from 91.6% to 41.7%. It stays for when the input carries that information. */
#define UVM2_PENUP_ZERO 0
#endif
volatile int32_t uvm2_penup_zero = UVM2_PENUP_ZERO;

/* The staircase of jump durations and its rate cap. See move_one. 0 disables it. */
#ifndef UVM2_STAIR_JUMP
#define UVM2_STAIR_JUMP 1
#endif
volatile int32_t uvm2_stair_jump = UVM2_STAIR_JUMP;
volatile int32_t uvm2_stair_cap  = 120;

/* THE GAME ANNOUNCES THAT IT IS ABOUT TO LIFT THE PEN. It is set before a `move_abs`, and
 * only consumed if that jump turns out to measure ZERO — if it moves, the jump itself blanks.
 * It is needed because "I blanked and lit again" and "I stayed lit" give the SAME geometry:
 * without this hint, two adjacent strokes join with a corner the reference does not draw. */
static int s_penup_pending;
void uvm2_draw_penup(void) { s_penup_pending = 1; }

/* How many "start steps" a jump has to measure to deserve the soft start. Their measured
 * threshold is at rate >= 100, i.e. about 20 times the step. 0 disables it. */
#ifndef UVM2_SOFT_START
#define UVM2_SOFT_START 124
#endif
/* The RATE from which the jump carries a tiny unit in front. 124 is their measured minimum;
 * below 124 they NEVER prime (0 of 179). 0 disables it.
 *
 * IT IS SEEDED FROM THE MACRO. It used to be the literal 124 and `UVM2_SOFT_START` was used
 * nowhere: it was defined above and that was it. So `-DUVM2_SOFT_START=N` compiled without
 * a warning and the threshold stayed 124 whatever happened — a whole sweep measuring the
 * same value. It is the same fault that already cost VK_RUNG_STEP and the draw scale. */
volatile int32_t uvm2_soft_start = UVM2_SOFT_START;

/* THE REFERENCE CARTRIDGE'S FRAME CLOSE: neither recalibration to the rails nor silence at
 * the end.
 *
 * It travels with the SR dialect because it is their method, not a loose knob. At 0 you get
 * back the BIOS close we used to have — two sweeps to the rails per frame and
 * `uvm2_bus_delay` for the rest — which is what to go back to if frame-to-frame drift ever
 * appears. */
#ifndef UVM2_FRAME_CLOSE
#define UVM2_FRAME_CLOSE 1
#endif
volatile int32_t uvm2_frame_close = UVM2_FRAME_CLOSE;
#define FRAME_CLOSE (BEAM_VIA_SR && uvm2_frame_close)

/* THE FILLER IS SEPARATE FROM THE CLOSE, because the two halves do not cost the same.
 *
 * Measured in the emulator with mhavoc, 199 frames: removing the recalibration is FREE (117
 * frames drawn against 114 with it), but adding the filler drops it to 86 — 26%. The bus
 * takes the same time either way (both close the frame in 30000 cycles; what changes is that
 * one spends them on 220 commands and the other in silence), so the cost is not in the bus
 * but in building and replaying those commands.
 *
 * And the emulator OVERCHARGED: it replays core 1's loop instruction by instruction, whereas
 * on the board it is 220 more commands in the SAME list and the SAME DMA.
 *
 * VERIFIED ON THE CONSOLE on 2026-09-05 with both binaries (same code, only this knob
 * differing): they run EQUALLY smoothly, so the emulator's 26% does not exist on the board.
 * And the filler also SHOWS: without it "the lines of text bunch up a little". Silence does
 * not refresh a capacitor, and the ~3500 cycles of gap between frames are enough for the
 * drawing to compress.
 *
 * It stays on. The knob stays in case some port needs those commands for something else, not
 * because there is any doubt. */
#ifndef UVM2_FRAME_FILLER
#define UVM2_FRAME_FILLER 1
#endif
volatile int32_t uvm2_frame_filler = UVM2_FRAME_FILLER;
#define FRAME_FILLER (BEAM_VIA_SR && uvm2_frame_filler)

#ifndef UVM2_ZERO_OFFSET
/* THE VALUE PRIMED INTO THE ZERO REFERENCE. 0x23 (35) is the factory value another reference
 * title uses for its text (`calibrationValue16`); for long strokes it uses 0x56 (86). It used
 * to be 7, i.e. practically the BIOS's 0, which was already written down as wrong. It is
 * calibrated per console — that is what the wizard is for. */
#define UVM2_ZERO_OFFSET 0x23
#endif
volatile int32_t uvm2_zero_offset = UVM2_ZERO_OFFSET;
volatile int32_t uvm2_y_mux_e = 4;
volatile int32_t uvm2_keep_lit  = 0;
/* A game can fix it (-DUVM2_BLANK_SETTLE_E=N) after sweeping it on its own console. */
#ifndef UVM2_BLANK_SETTLE_E
#define UVM2_BLANK_SETTLE_E 12
#endif
volatile int32_t uvm2_blank_settle_e = UVM2_BLANK_SETTLE_E;  /* E cycles lit after stopping */
/* X DAC SETTLING, before starting the ramp. See x_settle_q8 in emit.rs: Y arrives sampled
 * and held after `y_mux` cycles of window, and X goes straight to the DAC with the ramp
 * starting three commands later. 0 = as before. */
volatile int32_t uvm2_x_settle_e = 0;

/* THE JUMP'S SETTLING. It used to be `k.moveto_settle_q8 = 0u` hard-coded, the ONLY one of
 * vx_timings' six terms that did not come from a knob — and with no measurement behind it.
 *
 * What it does: a blanked jump ends its ramp and the amplifier is still arriving. With 0
 * nothing is compensated, so the beam lands SHORT and everything drawn afterwards starts from
 * the wrong place. In a drawing whose outline is a continuous polyline (no jumps) and whose
 * interior detail is loose strokes (each with a jump in front), the symptom is exactly what
 * the console shows: a perfect outline and a displaced interior.
 *
 * It is left at 0 — the value that was there — so that introducing it changes nothing: what
 * changes is that it CAN NOW BE MEASURED, with the bench and the panel, instead of argued
 * about. */
volatile int32_t uvm2_moveto_settle_e = 0;
/* Frame period in BUS CYCLES, 0 = free. It starts at whatever UVM2_HZ says so nobody's
 * behaviour changes; the panel moves it hot. 30000 = 50 Hz.
 *
 * `used`: without it --gc-sections collects it and the panel cannot find it. It happened to
 * this one and not to the other knobs because those are referenced by vx_cart_timings; only
 * uvm2_frame_end looks at this one, and the linker decided it was surplus. A knob that is not
 * in the ELF cannot be touched hot, which is the whole point.
 *
 * How long the last frame took to draw, WITHOUT the lock's padding. Deliberately separate
 * from `s_frame_cycles`: that one equals the period when the frame fit, so it is no use for
 * finding out whether it fits. */
static uint32_t s_draw_cycles;

/* THE REFRESH IN HERTZ. See the note in uvm2_draw.h: it is a GAME option because it has to
 * match the frequency of the WALL SOCKET, not any preference of ours. The division is done
 * here once and not in every game. */
void uvm2_set_refresh(unsigned hz)
{
    uvm2_pacer_cycles = hz ? (UVM2_BUS_HZ / hz) : 0u;
}

unsigned uvm2_current_refresh(void)
{
    return uvm2_pacer_cycles ? (UVM2_BUS_HZ / uvm2_pacer_cycles) : 0u;
}

int uvm2_refresh_fits(void)
{
    return uvm2_pacer_cycles == 0u || s_draw_cycles <= uvm2_pacer_cycles;
}

__attribute__((used)) volatile uint32_t uvm2_pacer_cycles =
#if UVM2_HZ == 0
    0u;
#else
    UVM2_CYCLES_PER_FRAME;
#endif

static struct vx_timings vx_cart_timings(void)
{
    /* ZEROED ON ENTRY. It used to be filled field by field, and a NEW field that got
     * forgotten comes out with stack garbage — and a garbage gap saturates the command's
     * 12-bit field and blows the frame up (seen: 788,661 cycles with an ORA+4096). */
    struct vx_timings k = (struct vx_timings){0};
    k.e6809_q8 = 64u;
    k.y_mux_q8 = (uint32_t)(uvm2_y_mux_e > 0 ? uvm2_y_mux_e : 0) * 256u;
    k.moveto_settle_q8 = (uint32_t)(uvm2_moveto_settle_e > 0 ? uvm2_moveto_settle_e : 0) * 256u;
    /* Unsigned: a negative beam_on means nothing (you cannot light the beam before the ramp
     * starts), and letting it through wrapped around to ~4 billion. */
    k.beam_on_q8 = uvm2_beam_on_e > 0 ? (uint32_t)uvm2_beam_on_e * 256u : 0u;
    /* 16, NOT our own cartridge's 11. HERE there IS per-board calibration, and this time
     * with a measurement behind it: with 11 the pentagon's corners come out slightly open —
     * the beam blanks BEFORE it finishes arriving. 16 is the value the reference measured for
     * THIS board (c_BlankOnDelay), and our 11 came from a bisection on OUR console, with a
     * different bus path and a different amplifier.
     *
     * It is the term that turns bright dots at the vertices and open corners into the two
     * ends of one knob. Raise the big one first and one step at a time; `beam_on` stays at 2
     * (theirs is 3) until it is needed. */
    k.blank_settle_q8 = uvm2_blank_settle_e * 256;
    k.keep_lit = (uint32_t)(uvm2_keep_lit ? 1 : 0);
    k.x_settle_q8 = (uint32_t)(uvm2_x_settle_e > 0 ? uvm2_x_settle_e : 0) * 256u;
    return k;
}

/* ── BOTH OF THEM, ONCE PER FRAME AND NOT TWICE PER VECTOR ──────────────────────────
 *
 * `vx_cart_sink()` fills nine function pointers over a zeroed structure, and
 * `vx_cart_timings()` seven fields reading six knobs. Both used to be built ON THE STACK in
 * every `move_one`, every `delta_one` and every `move_abs_internal` — about 2,262 times per
 * frame in esb (1131 vectors), to produce exactly the same pair of structures.
 *
 * WITHIN A FRAME THEY ARE CONSTANT: the sink is a set of fixed function pointers, and the
 * timing knobs are moved by `uvm2_config_load()` or the calibration wizard, which run OUTSIDE
 * the drawing. Refreshing them in `uvm2_frame_begin` is therefore exact, and the only thing
 * that changes is that a knob moved mid-frame takes effect in the next one — which is a 20 ms
 * frame on a control being turned by hand.
 *
 * Also in `uvm2_draw_init`, because that draws (the Z priming) before the first frame_begin,
 * and an unfilled structure is a null function pointer. */
static struct vx_sink    s_sink_cache;
static struct vx_timings s_k_cache;
static void vx_cart_refresh(void)
{
    s_sink_cache = vx_cart_sink();
    s_k_cache    = vx_cart_timings();
}

/* SPLITTING WHAT DOES NOT FIT IN ONE RAMP — and this was missing entirely.
 *
 * A ramp expresses at most +-127 (vx_ramp_params clamps to (-128,127)), but
 * uvm2_draw_move/delta passed the RAW delta and also updated s_pos with the WHOLE value.
 * Result: every move or stroke longer than 127 units was clipped SILENTLY and the model
 * believed the beam was somewhere it was not, for ever after.
 *
 * MEASURED on the host on 2026-08-24 with the grid bench: a grid written to occupy
 * [-100..100] generated a command stream that took the beam to [-100..444] — four times off
 * screen. On the console that looks like "it draws half of it, jammed against the edge",
 * which is what it was.
 *
 * The RP2350 SDK DID do it ("splits >127 i8 chunks" in beam_draw_to). This one did not:
 * another divergence between the two cartridges that no number gave away.
 *
 * THE SPLIT IS EXACT. The pieces add up to the original delta down to the last bit: the ideal
 * position is accumulated and each piece is the difference against what has already been
 * emitted, so the division error does not accumulate — which is exactly what we were chasing
 * in the game. */
/* THE DRAWER'S INTERNAL UNIT. With -DUVM2_SUBUNITS positions and deltas travel in 1/16 of a
 * device unit along the WHOLE path; without it, in integers, exactly as before (and
 * `ramp_params_q` with exact integer values gives bit-for-bit the same as the old path —
 * there is a test that checks it).
 *
 * WHY. `VS_RND` in sdk_rp2350.c divides the game's coordinates by 127 and ROUNDS TO AN
 * INTEGER before anyone sees them. MEASURED in mhavoc over 81,552 vectors: 0.22 units of
 * error per axis, 3.6% of vectors entirely sub-unit and 0.26% disappearing because both
 * endpoints land on the same point. The reference cartridge's grid is ~1/20 of a unit (the
 * granularity of the rate at t1=8): we were ten times coarser.
 *
 * The chain's debt did NOT cover this: it corrects the RAMP's residue, and it received i8,
 * i.e. already rounded. They are two different losses.
 *
 * HOW MANY FRACTION BITS the input carries. 4 (1/16) is what the ports use; the comparison
 * bench asks for 8 because at 1/16 10.5% of their rates cannot be reproduced — their vector
 * with rate 32 at t1 = 8 measures exactly 1.6 units and 1/16 can only say 1.5625 or 1.625.
 * At 1/64 or finer all 210 of the frame come out EXACT.
 *
 * ASKING FOR THE PRECISION IS ALREADY ASKING FOR THE MODE. Until 2026-09-09 the `#else`
 * below REDEFINED to 0 a UVM2_Q_BITS that came from the command line, and it did so
 * silently: a bench was compiled with -DUVM2_Q_BITS=8 and without UVM2_SUBUNITS, so main.c
 * read 8 (its contract `#error` passed), the drawing path read 0, and the table in 1/256 was
 * drawn as if those were whole units. The bench fell from 94.2% to 5.3% of commands identical
 * to theirs and everything measured with it was noise. The compiler DID say so — two
 * macro-redefined warnings in the log — and nobody was looking at them. */
#if defined(UVM2_Q_BITS) && (UVM2_Q_BITS > 0) && !defined(UVM2_SUBUNITS)
#define UVM2_SUBUNITS 1
#endif
#ifdef UVM2_SUBUNITS
#ifndef UVM2_Q_BITS
#define UVM2_Q_BITS 4
#endif
#define UVM2_Q (1 << UVM2_Q_BITS)
#else
#ifndef UVM2_Q_BITS
#define UVM2_Q_BITS 0
#endif
#define UVM2_Q 1
#endif

/* HOW FAR A SINGLE RAMP TRAVELS, DERIVED AND NOT CHOSEN.
 *
 * A ramp advances `v * t1 / DRAW_SCALE`, and both of its factors have a cap:
 *   |v| <= 127   the rate is a signed byte in the position DAC
 *    t1 <= 255   the reference writes T1CH = 0 in every one of its frames, i.e. t1 in a
 *                byte; measured, it uses from 8 up to 252 and DOES NOT SPLIT A SINGLE STROKE
 *
 * So the maximum is 127 * 255 / DRAW_SCALE — with the stock scale (160), 202 units. This used
 * to be a plain 127, which comes from nowhere: it split strokes that fit in one pass.
 * MEASURED in dkong, the 25m frame frozen: 16 strokes of 276 came out split, 179 units median
 * and 193 the longest — ALL of them fit in one ramp with the real cap, and none of them
 * with 127.
 *
 * It is computed at run time because DRAW_SCALE is a live variable. */
static inline int uvm2_max_step(void)
{
    /* t1'S CEILING IS DRAW_SCALE, NOT 255 — AND THAT MAKES THE SCALE CANCEL OUT.
     *
     * This used to say (127 * 255) / DRAW_SCALE, giving 202 at scale 160 and 255 at 127. But
     * `ramp_params_q` bounds the time with `.clamp(min_t1, s)`, where `s` IS DRAW_SCALE (and
     * the model's documentation says so: "T1 clamped to [MIN_T1, 0x7F]"). With both factors at
     * their cap:
     *
     *     v * t1 / DRAW_SCALE  =  127 * DRAW_SCALE / DRAW_SCALE  =  127
     *
     * So one ramp reaches 127 device units, WHATEVER the scale. The 255 was the 6522's 16-bit
     * counter cap, not the one the model uses, and that is why the splitter did not split
     * strokes of 128 to 255: they came out clipped and the platforms were left OPEN at one
     * end (seen on the console, 2026-09-11).
     *
     * This replaces an `if (p > 127) p = 127;` placed under `#if UVM2_Q_BITS == 0`. It covered
     * the symptom only on the integer path, so when dkong moved to subunits the clipping came
     * back intact. A derived cap does not need to know which path you came in by. */
    /* -- AND THE CEILING IS NOT ONLY DRAW_SCALE: T1_TRANSPORT IS SMALLER ---------
     *
     * This said `t1_max = DRAW_SCALE`, giving 127 units of reach. But `ramp_params_q` does
     * not clamp t1 to [MIN_T1, s] and leave it there: it also applies `.min(T1_TRANSPORT)`,
     * which is 110 in dkong. With the DAC's limit at 127:
     *
     *     real reach = 127 * min(DRAW_SCALE, T1_TRANSPORT) / DRAW_SCALE = 110
     *
     * So every stroke -- or JUMP, which `uvm2_draw_move` splits with this same limit -- of
     * between 110 and 127 units came out as ONE ramp that cannot get there: t1 pins at 110,
     * the rate it needs exceeds 127, the DAC clips it and the beam lands SHORT.
     *
     * MEASURED on the title frame: 33 of 331 jumps with t1 pinned at 110 and 29 with |v| at
     * 128 (the i8 limit). Each one places its figure in the wrong spot, and since the error
     * depends on the jump's distance, every letter fell at a different Y -- the ragged Y of
     * the text on hardware. dkong's lit strokes never reach 110 (the longest is 49), so the
     * jumps paid for this almost alone.
     *
     * Same fault that left the platforms open -- a split limit that was not the real limit
     * -- and it was fixed halfway: the 255 became DRAW_SCALE and nobody looked at the other
     * factor of the same `.min()`. */
    const int t1_ceiling = (int)T1_TRANSPORT;
    const int t1_max = (t1_ceiling > 0 && t1_ceiling < (int)DRAW_SCALE) ? t1_ceiling : (int)DRAW_SCALE;
    int p = (127 * t1_max) / (int)DRAW_SCALE;
    return (p > 0 ? p : 1) * UVM2_Q;
}
#define UVM2_MAX_STEP uvm2_max_step()



static void move_abs_internal(int x, int y);

/* STOPWATCHES ON A VECTOR'S PATH THROUGH THE BOARD (BIOS only; read over SWD without
 * stopping): [0]/[1] the JUMP's ramp and sequence, [2]/[3] the STROKE's ramp and sequence, in
 * accumulated us (TIMER0 RAWL) and their count. To find out WHERE the 9-11 us per call the
 * table measures actually go, which the emulator's profile cannot see (it counts
 * instructions, not waits). */
#ifdef UVM2_BIOS
volatile uint32_t uvm2_sdk_us[4], uvm2_sdk_n[4];
#define SDK_T0() (*(volatile uint32_t *)0x400B0028u)
#define SDK_ACUM(i, t0) do { uvm2_sdk_us[i] += SDK_T0() - (t0); uvm2_sdk_n[i]++; } while (0)
#else
#define SDK_T0() 0u
#define SDK_ACUM(i, t0) ((void)(t0))
#endif
static void split(int dx, int dy, void (*emit_piece)(int, int))
{
    int m = (dx < 0 ? -dx : dx);
    int my = (dy < 0 ? -dy : dy);
    if (my > m) m = my;
    if (m <= UVM2_MAX_STEP){ emit_piece(dx, dy); return; }

    int n = (m + UVM2_MAX_STEP - 1) / UVM2_MAX_STEP;
    int hx = 0, hy = 0;                       /* what has already been emitted */
    for (int i = 1; i <= n; i++){
        int ox = (dx * i) / n;                     /* ideal position after i pieces */
        int oy = (dy * i) / n;
        emit_piece(ox - hx, oy - hy);
        hx = ox; hy = oy;
    }
}

/* HOW FAR A START RAMP TRAVELS, in internal units: rate 1 for 8 T1 counts. It is not a chosen
 * number — it comes from the same constants as the rest of the drawing. */
static int start_step(void)
{
    int q = (8 * (1 << UVM2_Q_BITS)) / (int)DRAW_SCALE;
    return q > 0 ? q : 1;
}

static void move_one(int dx, int dy)
{
    /* THE JUMP ABSORBS THE DEBT, which is the half of the model that was never wired up.
     *
     * The debt is "where the beam really is minus where the drawing believes it is",
     * accumulated over every ramp that rounds. `vx_chain_reset` stopped throwing it away
     * some time ago and `ramp_params_jump_with_debt` was written to absorb it here — but
     * nothing ever called that function. Its only entry point, `vx_ramp_params_q4`, has no
     * callers anywhere in the tree, so the jump went through `vx_ramp_params_jump_qn` ->
     * `ramp_params_q` bare: it neither absorbed the debt nor recorded its own.
     *
     * What that left is not "no correction" but correction IN THE WRONG PLACE. The stroke
     * path does chain (`ramp_params_chain_qn` asks for delta + debt), so the whole
     * accumulated debt landed on the first LIT stroke after every jump, stretching it.
     * That is the deformation: strokes landing short and corners not closing.
     *
     * MEASURED with `jump_absorbs_or_not` in ramp.rs, replaying real geometry through
     * the real model, jump-without-debt versus jump-with-debt:
     *
     *     reference frame 120, 427 segments  median X  -0.619 -> -0.038   worst -0.895 -> -0.187
     *     Star Wars logo, 149 segs 48 jumps  median X  -0.031 -> +0.000   worst -2.306 -> +1.669
     *
     * The two notes that used to live here and in ramp.rs saying this was tried and came
     * out worse (+705 in X; 86.8 -> 99.8 units over an asterock frame) are both from BEFORE
     * UVM2_SUBUNITS: one was fixed by the "an axis not asked does not move" guard, the
     * other measured in whole units, and its own text says it is not proof either way.
     *
     * THE LEDGER IS KEPT HERE AND NOT INSIDE THE PARAMS FUNCTION, because this is the only
     * place that knows what actually reaches the bus: the duration ladder below replaces t1
     * and recomputes the rates after the ramp is chosen, and the soft start splits the jump
     * into a priming unit plus a long ramp. `vx_debt_take` says how much to absorb,
     * `vx_debt_record` is called once per ramp emitted with that ramp's slice of the
     * request, and the slices add up to the whole jump. */
    vx_chain_reset();
    const int dx_asked_ = dx, dy_asked_ = dy;
    {
        int32_t ax_ = 0, ay_ = 0;
        vx_debt_take(dx, dy, UVM2_Q_BITS, &ax_, &ay_);
        dx += ax_;
        dy += ay_;
        /* THE BELIEF ABOUT POSITION DOES NOT MOVE WITH THE CORRECTION. The correction aims
         * the real beam; `s_pos` is where the drawing thinks the beam is, and the target it
         * was asked for has not changed. The ramps below advance `s_pos` by the corrected
         * delta, so give it back here — otherwise every jump shifts the model's own idea of
         * the origin by the debt and the next `move_abs` asks for the wrong delta. */
        s_pos_x -= ax_;
        s_pos_y -= ay_;
    }

    /* DRIFT COMPENSATION, BACK AGAIN. It was here — in the jump, which is where it was
     * measured — and it went DEAD in the commit that introduced vectrex-draw: the new path's
     * `return` moved above it and these two lines were left underneath with nobody calling
     * them. `drift_fix` kept compiling, defined and unused across the whole tree.
     *
     * It was not a decision anyone took: it is a regression, and with it a console
     * measurement from 2026-08-12 was lost (X -16/256, Y -112/256, with the drift bench).
     *
     * THE SYMPTOM, seen on 2026-09-14: dkong's menu DRIFTS TO THE RIGHT as it goes down. All
     * four entries are asked for at the SAME x — VK_MENU_X — and each one comes out further
     * right than the one before, in the order they are drawn. It is exactly the diagonal
     * cascade the note below describes.
     *
     * Both values still default to 0, so reconnecting it changes nothing until they are
     * calibrated: what is fixed today is that the knob actually EXISTS.
     *
     * IT DID NOT FIT IN DKONG, AND THAT TURNED OUT TO BE ANOTHER LOST KNOB. That image runs
     * entirely from SRAM and was at 100%; looking for the bytes turned up the fact that the
     * romset buffer — 23 KB occupied FOR THE WHOLE GAME by something only used at startup —
     * could have gone to PSRAM long ago and nobody was defining the symbol. Restored in
     * uvm2.mk, dkong has 18 KB free and this goes in without argument. */
    dx += drift_fix(dx, &s_drift_ax, uvm2_drift_x);
    dy += drift_fix(dy, &s_drift_ay, uvm2_drift_y);

    /* SOFT START FOR FAST JUMPS.
     *
     * MEASURED in their frame 120 of Major Havoc: 20 of their 26 jumps at rate >= 100 (77%)
     * are preceded by a TINY unit — t1 = 8 in all 20, rate |v| <= 4 (almost always +-1) and
     * in the SAME DIRECTION as the jump (X: 20 of 20, Y: 17 of 20). And 10 of their 12
     * departures from a re-zero start that way. We NEVER did it, not once.
     *
     * Physically it is what it looks like: after the zero clamp the integrators are at rest,
     * and a ramp that starts from rest at rate 124 travels short. The preceding unit gets
     * them moving. The symptom on the console was the high-score table with its rows BUNCHED
     * towards the centre — every object landing short — with the list geometrically CORRECT:
     * position error per stroke, median 0.01 in X and 0.06 in Y, worst case 0.42.
     *
     * Its travel is DEDUCTED from the jump, so the final position does not change. */
    int forced = 0;
    int res_tot_x_ = 0, res_tot_y_ = 0;   /* what the priming unit took of the request */
    int32_t px_ = 0, py_ = 0; uint32_t pt1_ = 0;   /* the long ramp, to emit it as computed */
    if (uvm2_soft_start > 0) {
        /* THEIR CRITERION IS THE JUMP'S RATE, NOT ITS DISTANCE. Measured over their 198
         * transports that move anything: the 19 that carry a tiny unit in front have rate
         * 124..126, and of the 179 that do not, **none** reaches 124. Perfect separation, no
         * overlap.
         *
         * This used to fire on distance and did so on 41 of 43 jumps, against their 19 of 198
         * — which is why it came out worse on the console. The error was the threshold, not
         * the idea. */
#if UVM2_Q_BITS > 0
        vx_ramp_params_jump_qn(dx, dy, UVM2_Q_BITS, &px_, &py_, &pt1_);
#else
        vx_ramp_params_jump(dx, dy, &px_, &py_, &pt1_);
#endif
        const int rate = (px_ < 0 ? -px_ : px_) > (py_ < 0 ? -py_ : py_)
                       ? (px_ < 0 ? -px_ : px_) : (py_ < 0 ? -py_ : py_);
        if (rate >= uvm2_soft_start) {
            /* THE TINY UNIT CARRIES THE RESIDUE, not a fixed step.
             *
             * Verified in their arithmetic: in their transport #45 the jump asks for -20.25
             * units in X; their main ramp (-124, t1=26) travels -20.15; the residue is -0.10,
             * which at t1 = 8 is -2 — and -2 is exactly what they emit. In Y, the same. So
             * they choose the long ramp first and put whatever is left over in front of it.
             *
             * With a fixed +-1 only 5 of 22 match; with the residue, the tiny unit comes out
             * at THEIR value. */
            const int t1p = 8;
            /* THE MAIN JUMP TRUNCATES, IT DOES NOT ROUND, when it carries a priming unit.
             *
             * `vx_ramp_params_jump` rounds to nearest, so the long ramp can OVERSHOOT and
             * leave a residue of the opposite sign — and then the tiny unit pushes backwards.
             * The reference falls short and the residue goes in the same direction as the
             * jump.
             *
             * MEASURED in their #45: it asks for -20.25 units; at t1 = 26 the exact rate is
             * -124.6. Rounding gives -125 (ours), truncating -124 (theirs), and with -124 the
             * residue is -0.10, which at t1 = 8 is the -2 they emit. */
            {
                const long rec = ((long)px_ * (long)pt1_ * (1L << UVM2_Q_BITS)) / (long)DRAW_SCALE;
                if ((dx > 0 && rec > dx) || (dx < 0 && rec < dx)) px_ += (px_ > 0 ? -1 : 1);
                const long rey = ((long)py_ * (long)pt1_ * (1L << UVM2_Q_BITS)) / (long)DRAW_SCALE;
                if ((dy > 0 && rey > dy) || (dy < 0 && rey < dy)) py_ += (py_ > 0 ? -1 : 1);
            }
            /* What the main ramp travels, in internal units (the same rounding as the
             * model's own travel helper: v * t1 / DRAW_SCALE). */
            const int rec_x = (int)(((long)px_ * (long)pt1_ * (1L << UVM2_Q_BITS)) / (long)DRAW_SCALE);
            const int rec_y = (int)(((long)py_ * (long)pt1_ * (1L << UVM2_Q_BITS)) / (long)DRAW_SCALE);
            const int res_x = dx - rec_x, res_y = dy - rec_y;
            if (res_x || res_y) {
                int32_t ax, ay; uint32_t at1;
#if UVM2_Q_BITS > 0
                vx_ramp_params_jump_qn(res_x, res_y, UVM2_Q_BITS, &ax, &ay, &at1);
#else
                vx_ramp_params_jump(res_x, res_y, &ax, &ay, &at1);
#endif
                if (at1 > (uint32_t)t1p) at1 = (uint32_t)t1p;   /* theirs is ALWAYS t1 = 8 */
                struct vx_sink s0 = s_sink_cache;      /* local copy: the call wants non-const */
                struct vx_timings k0 = s_k_cache;
                /* THE LONG RAMP COMES AFTER THIS ONE, so the beam is blanked BEFORE this
                 * unit and not inside its mux window. See UNITS_CONTINUE. */
                UNITS_CONTINUE = 1;
                vx_moveto_seq(&s0, ax, ay, at1, &k0);
                UNITS_CONTINUE = 0;
                s_pos_x += res_x; s_pos_y += res_y;
                dx -= res_x; dy -= res_y;
                /* This ramp's slice of the ledger. The long ramp below records the rest,
                 * and the two slices add up to the ORIGINAL request — not the corrected
                 * one, or the absorbed debt would be asked for twice and never clear. */
                res_tot_x_ = res_x; res_tot_y_ = res_y;
                vx_debt_record(res_x, res_y, UVM2_Q_BITS, ax, ay, at1);
                s_ramps_from_zero++;
                uvm2_stats.moves++;
                uvm2_stats.ramp_cycles += at1;
                vx_chain_reset();
                /* AND THE LONG RAMP IS EMITTED AS IT WAS COMPUTED, not recomputed over the
                 * already-reduced delta: taking the residue off it shortens it by one t1 and
                 * the rate runs away to 127-128. Measured in their #45 — they emit
                 * (-124, t1=26) and we came out with (-128, t1=25) — and in their #393. The
                 * reference chooses the ramp FIRST and puts what is left over in front; not
                 * the other way round. */
                forced = 1;
            }
        }
    }

    {
        int32_t vx, vy; uint32_t t1;
        s_pos_x += dx;
        s_pos_y += dy;
        /* THE JUMP'S OWN SIGN BIAS IS GONE (2026-09-12), which is what made absorbing the
         * debt here safe. The absorption itself is at the top of this function.
         *
         * Giving jumps their OWN debt was tried and measured worse: over the 255 real jumps
         * of an asterock frame the accumulated error rose from 86.8 to 99.8 units in X and
         * from 91.6 to 100.6 in Y. The reason recorded here was that the jump's error is not
         * a fractional residue another ramp can absorb but a SIGN BIAS of the ramp model --
         * "negative delta X falls 1.3 units short, positive delta Y overshoots by 3.9, the
         * other two directions are exact" -- so asking for "delta + debt" only moves it to
         * another delta with another bias.
         *
         * THAT MEASUREMENT WAS TAKEN IN WHOLE UNITS, BEFORE UVM2_SUBUNITS. Re-measured in
         * Q4 with dkong's configuration (test `jump_bias` in ramp.rs, which ships so
         * this can be re-run):
         *
         *     dx>0 dy=0   X -0.009 (worst -0.12)     dx<0 dy=0   X +0.009 (worst +0.12)
         *     dx=0 dy>0   Y -0.009                   dx=0 dy<0   Y +0.009
         *     ...and the four mixed quadrants the same magnitude
         *
         * No privileged direction is left: all eight are 0.009 units and PERFECTLY
         * ANTISYMMETRIC, which is truncation towards zero and cancels itself along a route
         * with mixed signs. 1.3-3.9 units per jump became 0.009 -- ~150x -- and sub-unit
         * precision did it, not a debt chain.
         *
         * WHAT THIS UNLOCKS: the sign bias is what made reordering unsafe (change the order,
         * change the mix of signs, change the frame's accumulated error, and THAT is what
         * flickers -- see VPY_NO_REORDER in dkong's Makefile). That objection no longer
         * holds on this arithmetic and the reordering is worth retrying.
         *
         * It is still not proof that adding debt would now help: this measures the MODEL's
         * error, and the physical error of the beam (integrator and amplifier) is a separate
         * thing that only the console can measure. */
        /* WITH THE JUMPS' SPEED CAP, not the strokes'. The beam is blanked: slowing it down
         * gives no brightness, it only spends frame. Measured in the reference capture: it
         * jumps at 1.8x the speed it draws at (median rate 64 against 35), and a jump cost us
         * ~267 cycles against their ~32. */
if (forced) { vx = px_; vy = py_; t1 = pt1_; } else {
#if UVM2_Q_BITS > 0
        { uint32_t t0_ = SDK_T0(); vx_ramp_params_jump_qn(dx, dy, UVM2_Q_BITS, &vx, &vy, &t1); SDK_ACUM(0, t0_); }
#else
        vx_ramp_params_jump(dx, dy, &vx, &vy, &t1);
#endif
        /* THE STAIRCASE OF JUMP DURATIONS.
         *
         * The reference does not COMPUTE a jump's t1: it PICKS it from {8, 18, 31}, the first
         * step whose rate does not exceed 120. Measured over their 1041 jumps that follow a
         * stroke, that rule explains 1008 (97%), and in their frame 120 it explains ALL of
         * them — including the three that had resisted, where they use 18 and we came out
         * with 9, 13 and 15.
         *
         * It only acts if some step FITS: on long jumps the earlier calculation rules. */
        if (uvm2_stair_jump) {
            static const uint32_t STEPS[3] = { 8u, 18u, 31u };
            /* THE DISTANCE COMES FROM THE DELTA, NOT FROM THE RAMP.
             *
             * Taking it from (rate x t1) looks equivalent and is not: if that ramp SATURATED
             * at +-127 it does not cover the whole delta and the distance comes out short —
             * so the staircase picked a step that does not fit and the rates saturated again.
             * The symptom was (127,127) on a pure-X jump, with vy = 127 where dy = 0. */
            const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            const long m_q = adx > ady ? adx : ady;          /* in internal units */
            for (unsigned e = 0; e < 3u; e++) {
                /* it fits if  m_q * scale / (Q * step)  <=  cap */
                if (m_q * (long)DRAW_SCALE
                    <= (long)uvm2_stair_cap * (long)UVM2_Q * (long)STEPS[e]) {
                    if (STEPS[e] != t1) {
                        t1 = STEPS[e];
                        vx_ramp_params_with_t1(dx, dy, UVM2_Q, t1, &vx, &vy);
                    }
                    break;
                }
            }
        }
        }
        struct vx_sink sink = s_sink_cache;
        struct vx_timings k = s_k_cache;
        { uint32_t t0_ = SDK_T0(); vx_moveto_seq(&sink, vx, vy, t1, &k); SDK_ACUM(1, t0_); }
        /* The rest of the ledger: whatever the priming unit did not take of the ORIGINAL
         * request, against what this ramp — with whatever t1 the ladder settled on —
         * actually travels. */
        vx_debt_record(dx_asked_ - res_tot_x_, dy_asked_ - res_tot_y_,
                       UVM2_Q_BITS, vx, vy, t1);
        s_ramps_from_zero++;
        uvm2_stats.moves++;
        uvm2_stats.ramp_cycles += t1;
    }
}

/* THE API TAKES GAME UNITS, like uvm2_draw_delta and uvm2_draw_move_abs: it multiplies by
 * UVM2_Q internally. This was the only one of the three that did not, and with UVM2_SUBUNITS
 * (Q4) jumps came out 16 times short for anyone calling it in game units: uvm2_text.c (which
 * mixes move and delta with the same coordinates) and SYS_MOVE in uvm2_svc.c. It showed up in
 * our own cartridge's BIOS: dkong's frame box came out 2,256 units wide where the .um2 gave
 * 20,723. move_abs_internal, which works in INTERNAL units, uses the helper below. */
static void move_rel_internal(int dx, int dy){ split(dx, dy, move_one); }
/* 90-DEGREE ROTATION.
 *
 * The Vectrex's screen is VERTICAL and quite a few arcade machines are horizontal: Major
 * Havoc declares 580x500 with no rotation (MAME, mhavoc.cpp:1005), i.e. barely wider than
 * tall, but others genuinely are. Without rotating, the game's wide axis is the tube's narrow
 * one and it sets the scale: the drawing ends up small with unused bands above and below.
 *
 * It is rotated HERE and not in each port for the usual reason: a transformation in the
 * shared layer applies to every game, text included, and there are not two places where it
 * can diverge. It is an axis permutation with a sign, so it costs no precision and does not
 * touch the beam model — what reaches the ramp is still an integer (dx,dy).
 *
 * The direction is clockwise as you look at the screen: what goes right on the arcade machine
 * goes down here. */
static int s_rotate;

void uvm2_draw_rotate(int enable) { s_rotate = enable ? 1 : 0; }
int  uvm2_draw_rotated(void)  { return s_rotate; }

#define ROTATE(X, Y) do { if (s_rotate) { const int rotate_t_ = (X); (X) = (Y); (Y) = -rotate_t_; } } while (0)

void uvm2_draw_move(int dx, int dy){ ROTATE(dx, dy); split(dx * UVM2_Q, dy * UVM2_Q, move_one); }
/* THE RELATIVE JUMP IN 1/16 OF A UNIT, for VPy (SYS_MOVE_Q4): the compiler carries the
 * position in subunits and asks for the jump in the same ones; without UVM2_SUBUNITS it
 * rounds. */
void uvm2_draw_move_q4(int dx_q4, int dy_q4)
{
    ROTATE(dx_q4, dy_q4);
#if UVM2_Q_BITS > 0
    split(dx_q4, dy_q4, move_one);
#else
    split((dx_q4 + (dx_q4 < 0 ? -8 : 8)) / 16, (dy_q4 + (dy_q4 < 0 ? -8 : 8)) / 16, move_one);
#endif
}


/* THE DIFFUSION LIVES IN THE SHARED CRATE. It was here for a few hours, and the emulator had
 * another copy, and our own cartridge's firmware did not have it at all — three different
 * decisions about the same rule, which is the form of divergence we had been paying for all
 * day. It is now `vx_ramp_params_chain` in vectrex-draw, and all three use it. */
static void delta_one(int dx, int dy)
{
    int32_t vx, vy; uint32_t t1;
#ifdef VPY_MEASURE_ROUNDING
    /* PROBE: the delta that really reaches the ramp, and what it gives back. On the C side
     * because --gc-sections collects a Rust static even when it is #[no_mangle]. */
    { extern volatile unsigned vpy_red_n, vpy_red_err_c, vpy_red_subunit, vpy_mains_zero;
       }
#endif
    s_pos_x += dx;
    s_pos_y += dy;
    /* Feed the frame's bounding box with the position already in device units. */

#if UVM2_Q_BITS > 0
    { uint32_t t0_ = SDK_T0(); vx_ramp_params_chain_qn(dx, dy, UVM2_Q_BITS, &vx, &vy, &t1); SDK_ACUM(2, t0_); }
#else
    vx_ramp_params_chain(dx, dy, &vx, &vy, &t1);
#endif

    struct vx_sink sink = s_sink_cache;
    struct vx_timings k = s_k_cache;
    { uint32_t t0_ = SDK_T0(); vx_draw_line_seq(&sink, vx, vy, t1, &k); SDK_ACUM(3, t0_); }
    s_ramps_from_zero++;
    uvm2_stats.vectors++;
    uvm2_stats.ramp_cycles += t1;
}

void uvm2_draw_delta(int dx, int dy){ ROTATE(dx, dy); split(dx * UVM2_Q, dy * UVM2_Q, delta_one); }

/* THE SAME ONE IN 1/16 OF A UNIT. This is the one that lets you ask for what integers
 * cannot — a stroke of 2.5 units, or one of half a unit, which in integers DISAPPEARS.
 * Without -DUVM2_SUBUNITS it rounds to an integer and behaves like the one above, so a game
 * can always call it. */
void uvm2_draw_delta_q4(int dx_q4, int dy_q4)
{
    ROTATE(dx_q4, dy_q4);
#if UVM2_Q_BITS > 0   /* the API's unit is the INTERNAL one; see UVM2_Q_BITS */
    split(dx_q4, dy_q4, delta_one);
#else
    split((dx_q4 + (dx_q4 < 0 ? -8 : 8)) / 16, (dy_q4 + (dy_q4 < 0 ? -8 : 8)) / 16, delta_one);
#endif
}

/* A STRAIGHT LINE WITH GAPS, IN A SINGLE RAMP.
 *
 * `gaps` are (start, end) pairs as FRACTIONS 0..255 of the vector, not T1 counts: the caller
 * knows where a masked stretch begins and ends along the line, and has no business knowing
 * anything about T1. The conversion belongs here.
 *
 * Splitting the line into pieces costs one ramp per piece — two DACs, the T1 count, open and
 * close; this programs it ONCE and only toggles BLANK along the way.
 */
void uvm2_draw_delta_patterned(int dx, int dy, const unsigned char *gaps, int n)
{
    ROTATE(dx, dy);
    int32_t vx, vy; uint32_t t1;
    s_pos_x += dx;
    s_pos_y += dy;
    /* Feed the frame's bounding box with the position already in device units. */
    {
        const int32_t ux = s_pos_x / (int32_t)UVM2_Q, uy = s_pos_y / (int32_t)UVM2_Q;
        if (ux < s_box_x0) s_box_x0 = ux;
        if (ux > s_box_x1) s_box_x1 = ux;
        if (uy < s_box_y0) s_box_y0 = uy;
        if (uy > s_box_y1) s_box_y1 = uy;
    }
    /* Chained like any other stroke: carrying gaps does not change the fact that it is a
     * ramp that rounds. It was left on the flat variant because dkong does not come through
     * here and nobody looked. */
#if UVM2_Q_BITS > 0   /* the API's unit is the INTERNAL one; see UVM2_Q_BITS */
    vx_ramp_params_chain_q4(dx, dy, &vx, &vy, &t1);
#else
    vx_ramp_params_chain(dx, dy, &vx, &vy, &t1);
#endif
    struct vx_sink sink = s_sink_cache;
    struct vx_timings k = s_k_cache;

    /* fractions -> T1 counts. A gap that rounds down to zero is not emitted: it would cost
     * two bus writes and blank nothing. */
    enum { MAX = 16 };
    uint16_t counts[MAX * 2];
    int m = 0;
    for (int i = 0; i < n && m < MAX; i++){
        uint32_t a = ((uint32_t)gaps[i*2]     * t1) >> 8;
        uint32_t b = ((uint32_t)gaps[i*2 + 1] * t1) >> 8;
        if (b > t1) b = t1;
        if (b <= a) continue;
        counts[m*2] = (uint16_t)a; counts[m*2 + 1] = (uint16_t)b; m++;
    }
    if (m == 0) vx_draw_line_seq(&sink, vx, vy, t1, &k);
    else        vx_draw_line_patterned_seq(&sink, vx, vy, t1, &k, counts, (uint32_t)m);
    uvm2_stats.vectors++;
    uvm2_stats.ramp_cycles += t1;
}

void vx_draw_line_sr_seq(struct vx_sink *, int32_t vx, int32_t vy, uint32_t t1,
                         const struct vx_timings *, const unsigned char *pattern,
                         uint32_t n, uint32_t step);

/* A STRAIGHT LINE WHOSE GAPS COME OUT OF THE SHIFT REGISTER: the BIOS's text.
 *
 * `uvm2_draw_delta_patterned` already programs ONE ramp and toggles BLANK along the way, but
 * it toggles it with the PCR: two commands per gap. With ACR = 0x98 the VIA shifts EIGHT bits
 * out by itself at the Phi2 rate, so one write to the SR paints eight dots — which is how the
 * Vectrex draws its own text, and the reason it fits so much of it.
 *
 * `pattern` is the bytes, one per 8 dots, bit 7 first. IT REQUIRES THE SR DIALECT: with
 * BEAM_VIA_SR at 0 the ACR comes out 0x80, the register does not shift and this paints
 * nothing. The caller chooses `dx` (and with it the ramp speed) so that 8 Phi2 cycles are
 * worth the character width it wants; `step` is the T1 counts between writes and is 8 unless
 * separation between groups is wanted. See draw_line_sr_seq in emit.rs. */
int32_t vx_ramp_dist(int32_t v, uint32_t t1, int32_t f);
int32_t vx_ramp_vel_no_lag(int32_t p, uint32_t t1, int32_t f);

/* HOW MANY T1 COUNTS A SWEEP MAY LAST. It is a cap on TIME, not on characters: it is set by
 * the Y integrator's leakage (see below). 64 counts is ~43 us, about the same as an ordinary
 * game vector. A variable and not a #define so it can be swept on the console.
 *
 * 65535 MEASURED, and the first reading said the opposite because it compared two builds that
 * differed in TWO things. Over the high-score screen, with the re-zeros held equal:
 *
 *     cap 255 : 270 sweeps, 1180 bytes, 51299 cycles
 *     no cap  : 258 sweeps, 1168 bytes, 50375 cycles   -> 924 fewer
 *
 * The 20 extra re-zeros blamed on it came from another flag that was in the same build.
 * Merging the pieces does save the ramps and the sacrificed bytes it promises.
 *
 * It is still the SLANT knob (the Y integrator's leakage: a sweep of 432 counts bends more
 * than one of 252). Lowering it splits the sweeps again. */
volatile uint32_t uvm2_sweep_t1_max = 65535;

int uvm2_draw_sweep_sr(int dx, int dy, const unsigned char *pattern, int n, int step)
{
    /* 65535 AND NOT 255: T1 IS 16-BIT AND THE CLIPPING WAS OURS.
     *
     * It sat at 255 from the moment the slant was measured (the Y integrator's leakage, see
     * below), and that split every line longer than 255/step characters -- 13 at step 18.
     * But the real cap is set by `draw_line_sr_seq`, which takes t1 as a u16, and the Vectrex
     * BIOS proves the hardware does not ask for it: its `Print_Str` ($F495) sweeps the whole
     * string without using T1 at all, opening /RAMP through PB7 and closing it when the loop
     * of register writes ends.
     *
     * `uvm2_sweep_t1_max` is still the slant knob: lowering it splits long sweeps again. What
     * is removed is the fixed ceiling, not the control. */
    uint32_t t1_max = uvm2_sweep_t1_max > 65535u ? 65535u : uvm2_sweep_t1_max;
    ROTATE(dx, dy);
    int32_t vx, vy; uint32_t t1;
    if (n < 1 || step < 1) return 0;

    /* THE CELL IS THE SHIFTS, AND IT CANNOT BE STRETCHED.
     *
     * There used to be a loop here that lengthened `t1` when the speed saturated, so the ramp
     * would reach the requested end. That is a blunder: the SR's 8 shifts last 8 Phi2 cycles
     * whatever happens, so lengthening the ramp widens the CELL but not the INK — each
     * character ends up as a tiny smudge in an enormous cell. Measured in the emulator over a
     * row of text: lit stretches of 77 units in cells of 987, i.e. 1/12 of what it should be.
     *
     * What CAN be moved is HOW MANY characters fit. The ramp lasts n*step counts and covers
     * whatever it covers at that speed; if the speed goes out of range, fewer characters fit
     * — not a longer ramp. How many were drawn is returned and the caller carries on from
     * there.
     *
     * A SWEEP CANNOT LAST LONG: THE Y INTEGRATOR LEAKS TOWARDS ZERO.
     *
     * Measured on the console (2026-09-21): a line of text comes out STRAIGHT while its Y is
     * zero and tilts more and more the further away it goes. That is dY/dt = -Y/tau: the
     * leakage always pulls towards the centre and is proportional to the height. It is not a
     * count error — it is time. A game vector lasts 20-40 counts (~25 us) and it does not
     * show; a whole-line sweep reaches 255 (~170 us) and it does.
     *
     * So the duration is bounded and as many characters fit as fit. The caller already
     * receives how many were drawn and carries on from there, which is exactly what that
     * return value is for. This is the knob to move if the slant comes back. */
    if (t1_max < 1) t1_max = 1;
    int fits = n;
    for (;;) {
        t1 = (uint32_t)fits * (uint32_t)step;
        if (t1 > t1_max) { fits = t1_max / step; if (fits < 1) fits = 1; continue; }
        /* THE SPEED IS COMPUTED BY `vx_ramp_vel_no_lag` AND NOT BY `..._with_t1`, and the
         * difference was read off the cartridge's own command list: for 14 cells of 122
         * subunits in 140 counts, the latter returns 88 instead of 122 because its model
         * counts the ramp's start-up delay (~54 counts). For a stroke that is fine — only
         * where it ends matters — but the sweep's pattern RUNS OUT at `t1`, so the letters
         * came out at 72% of their width, cramped. Whatever the ramp keeps travelling
         * afterwards is dark tail. */
        vx = vx_ramp_vel_no_lag(dx / n * fits, t1, (int32_t)UVM2_Q);
        vy = vx_ramp_vel_no_lag(dy / n * fits, t1, (int32_t)UVM2_Q);
        if (fits <= 1) break;
        /* 126 AND NOT 120: `vx_ramp_params_with_t1` clips at +-127, and a margin of 7 threw
         * away characters that fit. The big cell in aae_esb gives vx = 122 exactly — with the
         * guard at 120 the sweep split and the line came out incomplete ("REBE FORC ROST" on
         * the bench). One unit of margin is left for the rounding. */
        if (vx <= 126 && vx >= -126 && vy <= 126 && vy >= -126) break;
        fits -= fits / 4 + 1;      /* the speed does not reach: fewer characters per sweep */
    }

    /* The position recorded is the one the ramp REALLY travels: with `t1` fixed, the speed
     * comes out rounded and the distance is not the one asked for. See vx_ramp_dist. */
    dx = vx_ramp_dist(vx, t1, (int32_t)UVM2_Q);
    dy = vx_ramp_dist(vy, t1, (int32_t)UVM2_Q);
    s_pos_x += dx;
    s_pos_y += dy;
    {   /* the frame's bounding box, in device units */
        const int32_t ux = s_pos_x / (int32_t)UVM2_Q, uy = s_pos_y / (int32_t)UVM2_Q;
        if (ux < s_box_x0) s_box_x0 = ux;
        if (ux > s_box_x1) s_box_x1 = ux;
        if (uy < s_box_y0) s_box_y0 = uy;
        if (uy > s_box_y1) s_box_y1 = uy;
    }

    /* Y, WITH ITS REAL WINDOW. `set_y` scales the sampling time with how far it has to go
     * (hold_between): mux channel 0 is a 10 nF capacitor and the emitter's fixed window — 4
     * cycles — does not discharge it after a large jump. What is left is residual velocity in
     * Y, i.e. a slanted line, and all the more so the higher the text sits. Set here,
     * `y_can_skip` stops the emitter repeating it. */
    set_y((int)vy, 0);

    /* THE SWEEP'S LAST BYTE IS LOST, so one is sacrificed on purpose.
     *
     * The pattern does not start at the same time as the ramp — between the T1CH and the
     * first write there is the lighting gap — so everything runs a little late and the last
     * byte starts shifting just as the ramp ends: its dots are painted with the beam already
     * stopped. On the bench the alphabet was missing F, L and R, which are the sixth and the
     * twelfth with the sweep cutting every six.
     *
     * Lengthening the ramp looked like the cure and it is NOT: it moves the cells and the
     * edge letters came out overlapping, tried with a tail of 8 counts and with a tail of a
     * whole step. What DOES work is putting nothing worth losing there: the last byte is a
     * ZERO and the caller is told one fewer was drawn, so the real last letter always has a
     * whole cell of ramp behind it. It costs one character per sweep. */
    unsigned char tail[64];   /* vx_draw_line_sr_seq's cap (MAX in emit.rs) */
    uint32_t n_emit = (uint32_t)fits;
    int n_reported = fits;
    if (fits > 1 && fits < (int)sizeof tail) {
        for (int i = 0; i < fits; i++) tail[i] = pattern[i];
        tail[fits - 1] = 0x00;              /* the last cell, blacked out */
        pattern = tail;
        n_reported = fits - 1;
    }

    struct vx_sink sink = s_sink_cache;
    struct vx_timings k = s_k_cache;
    vx_draw_line_sr_seq(&sink, vx, vy, t1, &k, pattern, n_emit, (uint32_t)step);
    uvm2_stats.vectors++;
    uvm2_stats.ramp_cycles += t1;
    return n_reported;
}

/* ── THERE WAS A "RATE LAYER" HERE (uvm2_draw_rate / uvm2_draw_raw), AND IT WAS A MISTAKE ──
 *
 * I wrote it so a test bench could replay the reference stream without rounding positions:
 * you handed it (vy, vx, t1, sr, gap) and it emitted the micro-segment. It was cut as soon as
 * it was seen, and rightly: **that turns the SDK into a pipe**. If the bench hands it the
 * rates, the times and the gaps ready-made, what gets verified is that the pipe carries what
 * is put into it — not that OUR model generates the right calls. To replay someone else's
 * stream as-is there is already a capture player, which uses `uvm2_exec` and does not pretend
 * to be anything else.
 *
 * What is compared has to come in through a game's front door: `uvm2_draw_intensity`,
 * `_move_abs`, `_delta`. That the SDK chooses the rate, the time, the splitting and the
 * re-zeros is PRECISELY what is under test.
 *
 * The technical reason I wrote it is still true and is recorded here: the positions the beam
 * reaches are fractional (v*t1/DRAW_SCALE: 127*8/160 = 6.35) and the API takes integers, so a
 * jump's distance can differ by less than a unit and move its t1 by 1-2 counts. That is a
 * limit of giving coordinates, and a game gives coordinates: it is the correct behaviour, not
 * a defect to be papered over. */

/* BOTH PUBLIC ENTRY POINTS CONVERT TO THE INTERNAL UNIT; THE BODY LIVES BELOW.
 *
 * I got this wrong the first time by chaining them — the integer one called the 1/16 one
 * after multiplying by UVM2_Q — and without the define that multiplied by 1 and divided by
 * 16: the drawing shrank and the jumps went from 180 to 406 per frame. The conversion has to
 * be in EVERY entry point, not chained. */
void uvm2_draw_move_abs(int x, int y) { ROTATE(x, y); move_abs_internal(x * UVM2_Q, y * UVM2_Q); }

void uvm2_draw_move_abs_q4(int x_q4, int y_q4)
{
    ROTATE(x_q4, y_q4);
#if UVM2_Q_BITS > 0   /* the API's unit is the INTERNAL one; see UVM2_Q_BITS */
    move_abs_internal(x_q4, y_q4);
#else
    move_abs_internal((x_q4 + (x_q4 < 0 ? -8 : 8)) / 16,
                     (y_q4 + (y_q4 < 0 ? -8 : 8)) / 16);
#endif
}

/* The body, in the internal unit: `s_pos_*` is in that same unit, so the subtraction is
 * direct and there is no hidden conversion in here. */
static void move_abs_internal(int x, int y)
{
    /* THE DRIFT FLOOR. It goes in the jump and not in the stroke on purpose: clamping the
     * zero drags the beam to the centre, so it can only be done where the drawing was going
     * to jump anyway. `uvm2_draw_reset` leaves `s_pos` at (0,0), so the jump below recomputes
     * itself from the origin. See uvm2_zero_every. */
    {
        /* The transport's distance, in device units. Measured BEFORE jumping, because the
         * re-zero leaves `s_pos` at the origin and afterwards there is no way to know. */
        const int dx = x - s_pos_x, dy = y - s_pos_y;
        const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        const int m = (ax > ay ? ax : ay) >> UVM2_Q_BITS;
        /* THE DIRECTION, NOT ONLY THE DISTANCE. (This block turned out to be wrong; see the
         * re-measurement immediately below, which replaced it.) */
        /* NEITHER THE SIGN NOR THE 20: BOTH HALVES OF THIS RULE WERE MEASURED AND FALSE.
         *
         * The block above was written off frame 120 of HIS MAJOR HAVOC and claimed two
         * things: re-centre from 20 units up, and only when the jump goes LEFT (the
         * "carriage return"). Re-measured transport by transport over BOTH of his bus
         * captures -- 7 frames of each, cut on his own $3FFE frame marker:
         *
         *                       never pinches below   pinches going right / left
         *     his Vector Kong         28.9 u                  126 / 63
         *     his Major Havoc         23.7 u                   35 / 42
         *
         * THE SIGN DOES NOT EXIST. In Vector Kong TWO OUT OF THREE of his re-zeros go
         * RIGHT -- exactly what this condition threw away. In Major Havoc it is 35/42, an
         * even split. It is not a direction rule; it looked like one in his mhavoc because
         * that scene is rows of text, so every long jump in it happened to go the same way.
         *
         * AND 20 IS BELOW THE FLOOR OF BOTH. He never re-centres a transport shorter than
         * 23.7 units. We did, and that is where it hurts: the jumps between the letters of
         * dkong's logo measure 20 and 22 units, so we put two pinches INSIDE one figure.
         * Each drags the beam to the centre and forces a flight back, and whatever is drawn
         * after it lands displaced relative to what came before -- the overlapping letters
         * Daniel sees on the console.
         *
         * 24 IS THE FLOOR THE TWO CAPTURES SHARE, and no more than that: above it they
         * disagree (Vector Kong has 28 transports between 24 and 29 that he does NOT pinch,
         * and Major Havoc has none in that band). So distance alone does NOT close the
         * rule, and the second criterion is not in these captures. The floor is, and the
         * floor is all that goes here.
         *
         * To re-measure: the clamp-analysis script over either capture's bus.csv. */
        const int far = uvm2_zero_jump > 0 && m >= uvm2_zero_jump;
        const int many = uvm2_zero_every > 0 && s_ramps_from_zero >= (uint32_t)uvm2_zero_every;
        if (far || many) uvm2_draw_reset();
    }
    int dx = x - s_pos_x;
    int dy = y - s_pos_y;
    if (dx != 0 || dy != 0) s_penup_pending = 0;   /* the jump blanks on its own account */
    if (dx == 0 && dy == 0) {
        /* LIFT THE PEN EVEN WHEN THERE IS NOTHING TO MOVE.
         *
         * The game asks for a jump of ZERO distance: the next stroke starts where the
         * previous one ended. This used to return without emitting anything, and with
         * keep-lit that leaves the beam LIT between the two — i.e. we draw the corner that
         * joins them. The game asked for a jump: it wanted them separated.
         *
         * MEASURED in their frame 120: they emit 12 units of `(0, 0, t1=8)` — an 8-cycle ramp
         * that moves nothing, with the beam blanked — and ALL TWELVE carry `SR=00` and
         * `SR=01` around them. The 228 genuinely chained strokes have no SR write at all. So
         * that unit IS the zero-distance pen-up, and we emitted zero of the 12. */
        const int announced = s_penup_pending;
        s_penup_pending = 0;
        if ((uvm2_penup_zero || announced) && s_beam_on) {
            int32_t vx, vy; uint32_t t1;
#if UVM2_Q_BITS > 0
            vx_ramp_params_jump_qn(0, 0, UVM2_Q_BITS, &vx, &vy, &t1);
#else
            vx_ramp_params_jump(0, 0, &vx, &vy, &t1);
#endif
            struct vx_sink sink = s_sink_cache;
            struct vx_timings k = s_k_cache;
            vx_moveto_seq(&sink, 0, 0, t1, &k);   /* zero rates: it does not move, only separates */
            s_ramps_from_zero++;
            uvm2_stats.moves++;
            vx_chain_reset();
        }
        return;
    }
    move_rel_internal(dx, dy);
}

/* ── Frame ────────────────────────────────────────────────────────────────── */

/* ---- Recalibrate ($F2E6) — THE ROUTINE THIS SDK DID NOT HAVE EITHER ---------
 *
 * The original BIOS calls it EVERY FRAME from `Wait_Recal` ($F192), and it is not a passive
 * zero:
 *
 *     Recalibrate:  LDX #Recal_Points   ; $7F7F,$8080
 *                   BSR Moveto_ix_FF    ; scale $FF, move to (+127,+127)
 *                   JSR Reset0Int       ; integrators to zero
 *                   BSR Moveto_ix       ; move to (-128,-128), SAME scale $FF
 *                   BRA Reset0Ref
 *
 * It sweeps the integrators to BOTH rails and nulls them in between. This SDK only did the
 * equivalent of `Reset0Ref` (`uvm2_draw_reset`), which zeroes the REFERENCE but drains
 * nothing: any residual charge stays where it is.
 *
 * WHY IT IS TRANSCRIBED HERE. On our own cartridge this very thing, on 2026-08-17, was the
 * cause of EVERYTHING accumulating: the drawing collapsed towards Y = 0 and lost intensity,
 * and the 33 VPy tests were useless. With the console switched off for an hour the first boot
 * came out clean and the following ones did not — an hour without mains power was the only
 * drain we had. With `Recalibrate` in place, solved.
 *
 * This SDK had not one reference to $7F7F or to Recalibrate, so it carries the same hole. And
 * it fits the UVM2 flicker that is still unexplained.
 *
 * MAXIMUM SCALE ON PURPOSE. The BIOS asks for $FF and using the current scale will not do: a
 * shorter ramp does not take the integrator to the rail, which is the whole point of the
 * exercise. That is why `s_scale` is saved, forced to 255 and restored.
 *
 * It costs two maximum-scale moves per frame. If it ever needs measuring,
 * -DUVM2_NO_RECALIBRATE removes it.
 */
#ifndef UVM2_NO_RECALIBRATE
/* A move at a FIXED scale, outside the beam model: both need the same thing here — sweep the
 * integrator to the rail — and neither should choose the duration. With T1 active the 6522
 * ends the ramp; without it, PB7 by hand like the rest of that path. */
static void recal_move(int dx, int dy)
{
    set_y(dy, UVM2_HOLD_DELAY);
    set_x(dx, UVM2_HOLD_DELAY);
    /* T1 ends the ramp, not PORTB: with ACR = 0x80 the PORTB bits that touch PB7 no longer
     * reach the pin, so a `set_ramp` here would be a silent no-op and the sweep to the rails
     * — which is THE WHOLE point of Recalibrate — would not happen. */
    emit(UVM2_VIA_T1CL, 255u, 0);
    emit(UVM2_VIA_T1CH, 0u, 255u + UVM2_BLANK_ON_DELAY);
}

static void uvm2_recalibrate(void)
{
    /* INVALIDATE THE CACHES BEFORE SWEEPING, and this is not caution: it is a measured bug.
     *
     * `set_y` and `set_porta` skip the write if they believe the DAC already holds that
     * value. With the shared model what gets written to the DAC is a VELOCITY, and that
     * saturates at +-127 constantly — every vector with |delta| >= 32 gives vx = 127 — so the
     * cache very often believes it is already 127 and SKIPS the move to the rail.
     *
     * And the move to the rail is THE WHOLE point of Recalibrate. Without it the zero
     * reference is not restored and the error accumulates frame after frame: the drawing
     * drifts out of place, which is exactly the symptom we spent an afternoon looking at.
     *
     * The cache belongs to the board; the saturation belongs to the model. Neither is wrong
     * on its own — together they eat the recalibration. */
    uvm2_draw_invalidate();
    uvm2_stats.recals++;

    /* The sweep to the rails is the longest move in the frame: lit, it is the brightest
     * diagonal on the screen. Blank here and do not trust that the frame arrived blanked. */
    beam_off();

    const uint32_t scale = s_scale;
    /* $FF EXPLICITLY, and that is why it CANNOT go through `uvm2_draw_move`: with the T1
     * model active that path would compute its own t1 from the length, and a shorter ramp
     * does not take the integrator to the rail — which is the whole point of the exercise.
     * The same reason the firmware has `recal_moveto` separate from `moveto`. */
    s_scale = 255u;                       /* Moveto_ix_FF: LDB #$FF / STB t1_cnt_lo */

    /* The two points are ABSOLUTE in the BIOS ($7F7F and $8080), and here the moves are
     * relative: go to the rail and cross to the opposite one, which is the same journey — the
     * full outbound and the full return. */
    recal_move(127, 127);                 /* -> (+127, +127) */

    /* Reset0Int ($F36C): LDD #$00CC / STB cntl / STA shift. Here it is the zero clamp, which
     * is what that 0xCC turns on. */
    set_zero(1, UVM2_HOLD_DELAY);
    set_zero(0, UVM2_HOLD_DELAY);

    recal_move(-128, -128);               /* -> (-128, -128), same scale */

    s_scale = scale;
    uvm2_draw_reset();                    /* Reset0Ref */
}
#endif

/* ARTIFICIAL DELAY BETWEEN FRAMES, to separate PSRAM from TIME.
 *
 * With the list in PSRAM the drawing collapses to x=y, and everything else has already been
 * ruled out: the data is byte-for-byte identical (signed at both ends), the time per vector
 * is the same, the phase is correct, both cores are innocent and during the drawing there is
 * not one access to the chip. The ONLY thing that changes is that writing the list there
 * costs ~6.9 ms per frame — measured: period 20097 us against 13200 of bus.
 *
 * So that gap is reproduced WITHOUT PSRAM. If it collapses just the same, the PSRAM is
 * innocent and the cause is the gap between frames; if it draws, the gap is innocent and we
 * go back to the chip. One image, one reading, and a whole branch closed.
 *
 *     make uvm2 UVM2_DELAY_US=6900
 */
#ifdef UVM2_DELAY_US
static void artificial_delay(void)
{
    const uint32_t t0 = *(volatile uint32_t *)0x400B000Cu;   /* TIMER0 TIMELR */
    while ((*(volatile uint32_t *)0x400B000Cu - t0) < (uint32_t)UVM2_DELAY_US) { }
}
#endif

#ifdef UVM2_INPUT_COUNT
/* DIAGNOSTIC: how many drawing calls go INTO the SDK, against the ramps that come OUT. It is
 * the only way to know whether a stroke is split BEFORE or AFTER uvm2_draw. It is published in
 * `recals`, which reads 0 in these games, so the report format does not have to change. */
extern unsigned uvm2_input_count;
#endif

void uvm2_frame_begin(void)
{
#ifdef UVM2_INPUT_COUNT
    uvm2_stats.recals = uvm2_input_count;   /* the one from the frame just finished */
    uvm2_input_count = 0;
#endif
    vx_cart_refresh();       /* the sink and the timings, once for the whole frame */
    s_count = 0;
    s_cycles = 0;
    s_limit = UVM2_CMD_CAPACITY - UVM2_CMD_RESERVE;
    s_dropped              = 0;
    uvm2_stats.vectors     = 0;
    uvm2_stats.moves       = 0;
    uvm2_stats.ramp_cycles = 0;

    /* Re-programme the VIA and re-prime the holds, with the zero clamp still
     * asserted — see via_setup().  UVM2_NO_FRAME_SETUP goes back to priming
     * once at boot, for A/B testing. */
#ifndef UVM2_NO_FRAME_SETUP
    via_setup();
#endif

    /* Restore the intensity BEFORE releasing the clamp, like the reference writer:
     *
     *     commandWriter.SetZ(0x5F);
     *     commandWriter.SetZero(false);
     *
     * via_setup() has just primed Z to 0, and the game will not set its own until its first
     * SET_INTENSITY. Between those two things there are commands running with the clamp
     * already released and Z at a value nobody chose.
     *
     * THE RESTORING Z IS REDUNDANT ON THE REFERENCE PATH. It was there so the beam would not
     * run with an undefined Z between releasing the clamp and the game's first
     * SET_INTENSITY; now the clamp is released by the zero block, which comes AFTER that Z,
     * so the gap it covered no longer exists. Their frame has none: writing it charged C306
     * twice per frame — ours and the game's — for the same value. */
    if (!BEAM_VIA_SR) set_z(s_z_last, UVM2_HOLD_DELAY);

    /* Only now release the clamp that has held the beam at centre since the
     * last frame ended.  The clamp covers the whole inter-frame gap and the
     * priming above, so no drift reaches the screen and the holds are charged
     * against a beam that is actually at zero. */
#ifndef UVM2_HOLD_ZERO
    /* THE ZERO BLOCK RELEASES THE CLAMP, AS THEIRS DOES. In their frame the sequence is
     * prologue -> the game's Z -> zero block (which ends in PCR=CE), and there is no loose
     * PCR=CE before it. Releasing it here also had an effect nobody wanted: it left `s_pcr`
     * with the release bit set, and with that the frame's first `uvm2_draw_reset` believed it
     * was "already centred and released" and SKIPPED the whole block — i.e. the frame started
     * with no re-zero, trusting that the centre was still where the previous frame left it.
     * With the clamp asserted, that same `if` lets the block through. */
    if (!BEAM_VIA_SR) set_zero(0, 0);
#endif
    /* What the prologue left in the list: the bar frame_end measures against when deciding
     * whether there is anything to publish. */
    s_count_after_begin = s_count;
}

/* PER BUFFER, not a single one: with double buffering core 0 signs the frame it has just
 * built and core 1 replays the PREVIOUS one. Comparing the two without indexing is comparing
 * different frames, which never match and say nothing. */
uint32_t uvm2_sig_written[2], uvm2_sig_n[2];

/* THE FRAME PERIOD IS MEASURED ON BOTH PATHS.
 *
 * It used to be at the end of uvm2_frame_end, and the DUAL-CORE path returns much earlier: so
 * in every dual-core build — i.e. in the one that actually runs — us_frame_last,
 * us_frame_min/max and slow_frames stayed at zero FOREVER. And a counter at zero reads
 * exactly like "not needed". It was pulled out here so both can call it. */
static void measure_period(void)
{
/* THE FRAME'S REAL PERIOD, in wall-clock microseconds.
 *
 * It is taken at the END, from one frame_end to the next, so it includes the one thing nobody
 * else measures: the time the game spends emulating between frames. See the us_frame_*
 * comment in uvm2_bus.h.
 *
 * TIMER0's TIMELR raw (0x400B000C): 32 bits of microseconds, more than enough for a
 * difference between frames, and without dragging pico/time.h into a file that lives in SRAM.
 *
 * The first 60 frames DO NOT count: at startup there is the ROM load, attract screens and the
 * loader itself, and that rubbish stays in the minimum and the maximum for ever. It already
 * happened once, with the statistics seeded from boot. */
{
    static uint32_t s_us_prev;
    static uint32_t s_warmup = 60;
#ifdef UVM2_HOST
    const uint32_t now = 0;   /* no TIMER0 in a host harness — see tools/ */
#else
    const uint32_t now = *(volatile uint32_t *)0x400B000Cu;
#endif

    if (s_warmup) {
        s_warmup--;
        uvm2_stats.us_frame_min = 0xFFFFFFFFu;
    } else {
        const uint32_t d = now - s_us_prev;
        uvm2_stats.us_frame_last = d;
        uvm2_stats.measured_frames++;
        if (d < uvm2_stats.us_frame_min) uvm2_stats.us_frame_min = d;
        if (d > uvm2_stats.us_frame_max) {
            uvm2_stats.us_frame_max     = d;
            uvm2_stats.vectors_at_max  = uvm2_stats.vectors_last;
            /* THE PHOTOGRAPH OF THE WORST FRAME: whose stutter it was, not only that it happened. */
            uvm2_stats.at_max_us_exec   = uvm2_stats.us_exec;
            uvm2_stats.at_max_us_input  = uvm2_stats.us_input;
            uvm2_stats.at_max_us_rest   = uvm2_stats.us_rest;
            uvm2_stats.at_max_us_wait   = uvm2_stats.us_wait;
            uvm2_stats.at_max_dropped   = uvm2_stats.dropped;
            uvm2_stats.at_max_commands  = uvm2_stats.commands;
        }
        /* THE SHAPE OF THE DISTRIBUTION, in millisecond bands. */
        {
            static const uint32_t BAND[7] = { 20500u, 21000u, 22000u, 24000u,
                                              28000u, 36000u, 52000u };
            unsigned b = 7u;
            for (unsigned i = 0; i < 7u; i++) if (d < BAND[i]) { b = i; break; }
            uvm2_stats.hist_frame[b]++;
        }
        /* AND THE RHYTHM: how many frames pass between two slow ones. A 1 Hz beat gives ~50. */
        {
            static uint32_t since_slow;
            since_slow++;
            if (d > 20500u) {
                uvm2_stats.slow_gap_last = since_slow;
                if (uvm2_stats.slow_gap_max == 0u ||
                    since_slow > uvm2_stats.slow_gap_max)
                    uvm2_stats.slow_gap_max = since_slow;
                if (uvm2_stats.slow_gap_min == 0u ||
                    since_slow < uvm2_stats.slow_gap_min)
                    uvm2_stats.slow_gap_min = since_slow;
                since_slow = 0u;
            }
        }
        if (d > 20500u) uvm2_stats.slow_frames++;
    }
    s_us_prev = now;
}
}

/* THEIR FRAME FILLER, VERBATIM.
 *
 * The reference's frame measures exactly 30023 cycles, and whatever is left after drawing it
 * burns with commands: ORA at +-64 alternating with the ramp OPEN (ORB=0x00 -> PB0=0, mux on
 * channel 0, PB7=0 ramp active) and ACR=0x18, which takes PB7 out of T1's hands. Measured
 * over the capture's 1061 frames: 246 alternations when it draws 1621 writes and 70 when it
 * draws 3348 — more drawing, less filler, and the total always 30023.
 *
 * Since it alternates in equal stretches the net displacement is zero, and the beam is
 * blanked (the SR was left at 0 and with ACR = 0x18 the mode bits are still 110, so CB2 keeps
 * the last bit). In other words: it parks the beam and keeps the integrators and the
 * capacitors alive through the gap between frames, where we sat in silence with
 * `uvm2_bus_delay`. Silence does not refresh a capacitor.
 *
 * The cycles are theirs: header 3/6/3/6/0/212 and each alternation 6/0/41. */
static void frame_filler(void)
{
    const uint32_t target = uvm2_pacer_cycles;
    if (target == 0u) return;                   /* no fixed pace, no leftover */
    const uint32_t HDR = 3u+1u + 6u+1u + 3u+1u + 6u+1u + 0u+1u + 212u+1u;   /* 236 */
    const uint32_t ALT = 6u+1u + 0u+1u + 41u+1u;                            /*  50 */
    if (s_cycles + HDR + ALT > target) return;  /* not even one fits: leave the silence */

    emit(UVM2_VIA_PORTA, 0x40, 3);
    emit(UVM2_VIA_PORTB, 0x00, 6);    /* channel 0, ramp open */
    emit(UVM2_VIA_PCR,   0xCE, 3);
    emit(UVM2_VIA_ACR,   0x18, 6);    /* PB7 out of T1's hands: PORTB drives the ramp */
    emit(UVM2_VIA_T1CL,  0xBF, 0);
    emit(UVM2_VIA_T1CH,  0x00, 212);
    /* THE BOUND IS RE-CHECKED EACH TURN, not computed once up front. It used to be
     * `n = (target - s_cycles) / ALT` with a plain `for`, which was exact while this
     * loop was the only thing emitting — and stopped being exact the moment samples
     * started going in here too, since each one adds cycles the count did not know
     * about. Asking `does another unit still fit` cannot drift. */
    unsigned n = 0;
    while (s_cycles + ALT <= target) {
        unsigned i = n++;
        smp_inject();      /* the inter-frame gap carries audio too: see smp_inject */
        emit(UVM2_VIA_PORTA, (i & 1u) ? 0x40 : 0xC0, 6);
        emit(UVM2_VIA_T1CL,  0x1F, 0);
        emit(UVM2_VIA_T1CH,  0x00, 41);
    }
    /* AND THE REMAINDER, WHICH OTHERWISE FALLS OUTSIDE. The alternations go in steps of 50
     * cycles, so fewer than 50 are always left at the end: they are closed with one more unit
     * whose gap is exactly what is missing. Without this the frame stopped at 29985 of
     * 30000 — not much, but it is the same class of mismatch we had been chasing all night. */
    uint32_t remainder = target > s_cycles ? target - s_cycles : 0u;
    if (remainder >= 3u + 1u + 0u + 1u + 1u + 1u) {
        const uint32_t gap = remainder - (6u + 1u + 0u + 1u + 1u);
        uint32_t t1 = gap > 10u ? gap - 10u : 1u;   /* their gap is t1 + 10 */
        if (t1 > 255u) t1 = 255u;
        emit(UVM2_VIA_PORTA, (n & 1u) ? 0x40 : 0xC0, 6);
        emit(UVM2_VIA_T1CL,  (uint8_t)t1, 0);
        emit(UVM2_VIA_T1CH,  0x00, gap);
        n++;
    }
    /* The caches, with the last thing that really went out. via_setup restores the ACR and
     * the PCR in the next frame. */
    s_porta = (uint8_t)((n & 1u) ? 0x40 : 0xC0); s_porta_stale = 0;
    s_portb = 0x00; s_pcr = 0xCE;
    uvm2_draw_invalidate();
}

void uvm2_frame_end(void)
{
    uint32_t cycles = 0;

    /* DO NOT PUBLISH AN EMPTY FRAME.
     *
     * A game can close its picture before the loop gets here — the Major Havoc port publishes
     * on every avg_mgo, which is where a picture really ends — and then this call finds the
     * list freshly opened, with the prologue and nothing else. Publishing it is a BLACK FRAME
     * interleaved, i.e. flicker of the worst kind.
     *
     * If nobody has put ANYTHING into the list since `uvm2_frame_begin`, there is nothing to
     * show: return without touching the beam, which the previous publish already clamped.
     *
     * IT LOOKS AT THE LIST, NOT AT THE GEOMETRY COUNTERS. The first version asked about
     * `vectors`/`moves`, which is a SYMPTOM and not the thing: a bench that emits raw commands
     * touches neither, so all its frames looked empty and none was published — core 1 was left
     * with nothing to draw and `uvm2_frame_request` stayed at zero. */
    if (s_count <= s_count_after_begin) return;

    /* BLANK EXPLICITLY, do not assume it. This used to say "blanked already (every lit
     * segment restores the PCR)", which is an ASSUMPTION: it only holds if the frame ended on
     * a lit segment. A frame with no drawing, or one that ends on a move, or on text, leaves
     * here with the beam however it was. The reference does not assume: it emits
     * SetBlank(true) when closing every frame. It costs one command. */
    s_pcr = (uint8_t)(s_pcr & ~UVM2_PCR_BLANK_OFF);
    s_limit = UVM2_CMD_CAPACITY;   /* the close ALWAYS fits, see UVM2_CMD_RESERVE */
    /* THE BLANK GOES BEFORE THE PCR, AS THEIRS DOES. Their frame close is
     * `T1CH+29 SR=00+14`, and we put the PCR in the middle: with the PCR in front, the last
     * command emitted was no longer the stroke's T1CH and `extend_stroke_t1ch` could not close
     * it, so the last stroke of EVERY frame came out 18 cycles short. */
    beam_off_and_wait_h(14u);
    emit(UVM2_VIA_PCR, s_pcr, 0);

    /* RECALIBRATE, with the beam already blanked and before clamping. It is where the BIOS
     * has it: `Recalibrate` is the last thing `Wait_Recal` does, i.e. frame-CLOSE work. See
     * the uvm2_recalibrate block.
     *
     * THE REFERENCE DOES NOT DO IT. Its whole frame close is `T1CH+29 SR=00+14` — one write —
     * and it never sweeps to the rails. It can afford that because its zero block runs 12
     * times PER FRAME re-priming C305 with the calibrated offset, which is the reference the
     * sweep was there to restore; and we now emit that same block, with its same cycles.
     *
     * It is turned off along with the rest of their close (UVM2_FRAME_CLOSE=0 brings both
     * back), and the symptom to watch for if we ever have to return is the usual one: the
     * drawing drifting out of place frame after frame. */
#ifndef UVM2_NO_RECALIBRATE
    if (!FRAME_CLOSE) uvm2_recalibrate();
#endif

    /* And NOW clamp the beam at the centre: a stopped integrator drifts, and a drifting beam
     * is a bright dot burnt into the middle of the screen. */
    set_zero(1, UVM2_ZERO_BASE + s_scale / 4u);
    s_pos_x = 0;
    s_pos_y = 0;

    /* AND THE FRAME'S LEFTOVER IS SPENT AS THEIRS IS: with commands, not in silence. */
    if (FRAME_FILLER) frame_filler();

    /* THE AUDIO CLOCK CLOSES WITH THE LIST, and here — before the dual-core fork —
     * because it is the only point both paths go through with `s_cycles` final. It
     * tells the sequencer how long the whole frame measured, so whatever did not
     * fit at the end of this list is charged at the start of the next and the
     * sample does not slow down at every boundary.
     *
     * IT GOES AFTER COPYING THE COUNT INTO THE STATS: `uvm2_smp_frame` zeroes it
     * for the coming frame, and a counter read after it was reset is a zero that
     * looks like a diagnosis. */
    uvm2_stats.samples = uvm2_smp_injected;
    uvm2_smp_frame(s_cycles);

#ifdef UVM2_DUAL_CORE
    /* Hand the finished list to core 1 and go straight back to the game.  The
     * replay, the input, the audio and the 50 Hz pacing all happen over there
     * now (uvm2_core1.c), so the game's next frame of logic overlaps the beam
     * still drawing this one. */
    /* A CHECKSUM AT BOTH ENDS, so nothing has to be guessed.
     *
     * With the list in PSRAM the drawing collapses into an x=y diagonal, and the list is
     * correct in content and in timings (7.6 bus cycles per command, the same ratio as the
     * version that draws correctly). Two opposite causes remain: that what core 1 READS is not
     * what core 0 WROTE — coherence — or that the data is fine and the problem is when it
     * arrives. Here what was written is signed; in uvm2_core1.c what is read is signed just
     * before replaying it. If the signatures match, coherence is ruled out once and for
     * all. */
    {
        /* THROUGH THE UNCACHED ALIAS. Signing through the normal window reads the XIP cache,
         * which has just been written: the signature ALWAYS comes out right and says nothing
         * about the chip. That is 16 KB of cache against a list of 64-128 KB, so most of what
         * is replayed afterwards comes from the PSRAM and can be something else. That
         * confusion already invalidated a whole test here, and the loader's before it.
         * (No translation needed: the list already lives in the uncached alias.) */
        const volatile uint8_t *list = (const volatile uint8_t *)&s_cmds[s_buf][0];
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < s_count * 3u; i++) { h ^= list[i]; h *= 16777619u; }
        uvm2_sig_written[s_buf & 1u] = h;
        uvm2_sig_n[s_buf & 1u]       = s_count;
#ifdef UVM2_CMDS_IN_PSRAM
        /* THE SAME SUM, BOTH SIDES. `h` is what the CHIP hands back on re-reading the list
         * through the uncached alias; uvm2_sig_emitted is what emit() asked to be written. If
         * they do not match, the writes do not hold up with the stream, the DMA and the bus
         * running — which is the only difference between the psramrw test (ZERO failures,
         * machine silent) and the game (which collapses). */
        uvm2_sig_reread = h;
        uvm2_sig_laps++;
        if (h != uvm2_sig_emitted) uvm2_sig_bad++;
#endif
    }

    /* THE FRAME'S BOUNDING BOX, AND ITS JUMP RELATIVE TO THE PREVIOUS ONE.
     *
     * What is seen on the console during the flicker is "the logo gets bigger for an
     * instant". If a frame is drawn at a different SCALE its box jumps, and no counting metric
     * sees that — not the commands, not the strokes, not the cycles.
     *
     * It is measured HERE, over the COMPLETE frame just built. It cannot be done in the
     * emulator: its 20 ms slot cuts our 28 ms frames and produces x3 jumps that are its own,
     * not the game's. */
    {
        const int32_t w = s_box_x1 - s_box_x0, h = s_box_y1 - s_box_y0;
        uvm2_stats.box_w = (uint32_t)(w > 0 ? w : 0);
        uvm2_stats.box_h = (uint32_t)(h > 0 ? h : 0);
        if (s_box_w_prev > 0 && w > 0) {
            /* ratio in hundredths against the previous frame; 100 = same width */
            uint32_t r = (uint32_t)(((int64_t)w * 100) / s_box_w_prev);
            uvm2_stats.box_ratio_last = r;
            if (r > uvm2_stats.box_ratio_max) uvm2_stats.box_ratio_max = r;
            if (uvm2_stats.box_ratio_min == 0u || r < uvm2_stats.box_ratio_min)
                uvm2_stats.box_ratio_min = r;
            if (r > 115u || r < 87u) {
                uvm2_stats.jump_box++;
                /* THE SURROUNDING RATIOS, to see the SHAPE of the jump: a fault goes up and
                 * COMES BACK on the next frame; an animation goes up and STAYS. Without this a
                 * counter cannot tell a defect from a legitimate zoom. The four ratios from
                 * the jump onwards are kept, for the last 6 jumps. */
                s_peak_n = 4u;
                s_peak_i = (s_peak_i + 1u) % 6u;
                uvm2_stats.box_peak[s_peak_i][0] = r;
                uvm2_stats.box_peak[s_peak_i][1] = 0;
                uvm2_stats.box_peak[s_peak_i][2] = 0;
                uvm2_stats.box_peak[s_peak_i][3] = 0;
                s_peak_k = 1u;
            } else if (s_peak_n && s_peak_k < 4u) {
                uvm2_stats.box_peak[s_peak_i][s_peak_k++] = r;
                if (s_peak_k >= 4u) s_peak_n = 0u;
            }
        }
        s_box_w_prev = w;
        s_box_x0 = s_box_y0 = 32767; s_box_x1 = s_box_y1 = -32768;
    }
    /* The avg_mgo firing distribution of the frame being closed. >1 = duplicated geometry. */
    {
        uint32_t m = uvm2_stats.mgo_per_frame;
        uvm2_stats.mgo_hist[m < 3u ? m : 3u]++;
        uvm2_stats.mgo_per_frame = 0;
    }
    s_len[s_buf]        = s_count;
    s_cycles_pub[s_buf] = s_cycles;
    uvm2_stats.commands = s_count;
    uvm2_stats.dropped  = s_dropped;

    uvm2_stats.vectors_last     = uvm2_stats.vectors;
    uvm2_stats.moves_last       = uvm2_stats.moves;
    uvm2_stats.ramp_cycles_last = uvm2_stats.ramp_cycles;

    s_count = 0;
#ifdef UVM2_CMDS_IN_PSRAM
    uvm2_sig_emitted = 2166136261u;
#endif
    s_frames++;
#ifdef UVM2_CMDS_IN_PSRAM
    uvm2_sig_emitted = 2166136261u;
#endif

    __asm volatile ("dmb" ::: "memory");   /* the buffer and its length, then the flag */
    uvm2_frame_request = s_frame_no;

    /* Next we fill the OTHER buffer, which core 1 last replayed for frame n-1.
     * Block until it has finished with it — this is the only place core 0 ever
     * waits, and it waits at most one frame. */
    /* WHO RULES, THE BEAM OR THE LOGIC? This is core 0's only wait, and its duration says so
     * unambiguously: if it waits a lot, core 1 is tight and the beam rules; if it waits for
     * nothing, core 1 is idle and the game's logic rules. Spins are counted, not time: only a
     * comparison is needed. */
    {
        uint32_t rotations = 0;
        while ((int32_t)(uvm2_frame_done - (s_frame_no - 1u)) < 0) { rotations++; }
        uvm2_stats.wait_spins = rotations;
    }

    s_frame_no++;
    s_buf = s_frame_no & 1u;
    (void)cycles;
    measure_period();
    return;
#else
    /* SINGLE CORE: THE LIST GOES OUT AS ONE DMA AND THIS CORE CARRIES ON. Previously uvm2_exec
     * did not return until the bus had nearly finished drawing the frame (batches of 64 words
     * waiting on the previous DMA), so building and drawing ran in series on the same core:
     * on our own cartridge's BIOS, 40 fps with a 20 ms list. Now: (1) the whole list is
     * accumulated; (2) we wait for the bus to finish with the PREVIOUS one; (3) the
     * uvm2_frame_gap hook, with the bus free, is where the BIOS reads the controllers and
     * drains the PSG (what core 1 does between lists on the UVM2); (4) the pacer step; (5) it
     * is fired and we return: the next frame is built while this one goes out. */
#if defined(UVM2_PIO_STREAM) && !defined(UVM2_HOST)
    vbus_list_begin();
#endif
#ifdef UVM2_CMDS_STAGE_SRAM
    /* CONTROL, in single core too. The copy lived ONLY in uvm2_core1.c, which is not compiled
     * without dual core — so the knob did nothing and a whole test went into comparing an
     * image with itself. Here the list lives in PSRAM but is replayed from SRAM, without one
     * access to the external chip while drawing and with no other core writing behind it. */
    {
        static uint8_t s_stage[UVM2_CMD_CAPACITY * 3u];
        for (uint32_t i = 0; i < s_count * 3u && i < UVM2_CMD_CAPACITY * 3u; i++)
            s_stage[i] = s_cmds[s_buf][i];
#ifdef UVM2_CMDS_IN_PSRAM
        /* THE LAST UNCHECKED LINK. We already know emit() -> PSRAM arrives intact (4890 of
         * 4891 frames). What is missing is PSRAM -> copy, which is what actually feeds the
         * executor in this image. If the copy does not match what was emitted, reading the
         * chip is what breaks even though the write is good — and that is a different
         * fault. */
        {
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < s_count * 3u; i++) { h ^= s_stage[i]; h *= 16777619u; }
            uvm2_sig_copy = h;
            uvm2_sig_copy_laps++;
            if (h != uvm2_sig_emitted) uvm2_sig_copy_bad++;
        }
#endif
        cycles = uvm2_exec(s_stage, s_count);
    }
#else
    cycles = uvm2_exec(s_cmds[s_buf], s_count);
#endif
#if defined(UVM2_PIO_STREAM) && !defined(UVM2_HOST)
    vbus_list_end();
    vbus_list_wait();      /* the PREVIOUS frame has finished on the bus */
    uvm2_frame_gap();      /* with the bus free: controllers, PSG (the BIOS defines it) */
#endif

    uvm2_stats.commands   = s_count;
    uvm2_stats.bus_cycles = cycles;
    uvm2_stats.dropped    = s_dropped;
#endif

    /* Freeze this frame's per-frame counters where a debugger can still read them
     * once uvm2_frame_begin has cleared the live ones. */
    uvm2_stats.vectors_last     = uvm2_stats.vectors;
    uvm2_stats.moves_last       = uvm2_stats.moves;
    uvm2_stats.ramp_cycles_last = uvm2_stats.ramp_cycles;

    s_count = 0;
#ifdef UVM2_CMDS_IN_PSRAM
    uvm2_sig_emitted = 2166136261u;
#endif
    s_frames++;
#ifdef UVM2_CMDS_IN_PSRAM
    uvm2_sig_emitted = 2166136261u;
#endif

    /* Lock the frame to the Vectrex clock rather than to an RP2350 timer:
     * 1.5 MHz / 50 Hz = 30000 bus cycles exactly.  The clamp stays asserted
     * through the wait; uvm2_frame_begin() releases it. */
    /* THE LOCK, AT RUN TIME. It used to be `#if UVM2_HZ`, i.e. one flash per value to answer
     * a question that takes seconds.
     *
     * AND THE QUESTION MATTERS. Observed on the console on 2026-08-24 over the grid, with the
     * geometry STILL: raising X_SETTLE — which only changes TIMINGS — makes the shimmer
     * FASTER. That is not shimmer, it is a BEAT: the picture is redrawn at F and something
     * periodic happens at a fixed G, and what you see moving is |F - G|. The suspect for G is
     * the mains at 50 Hz on the power supply and the deflection amplifier — which also fits
     * with it being almost entirely horizontal, because the ripple does not couple equally
     * into both axes.
     *
     * If that is it, LOCKING the drawing to 50 Hz leaves the ripple in the same phase every
     * frame and the displacement goes from moving to being a fixed bias, i.e. invisible. With
     * the lock on you also have to make sure the drawing FITS: a frame that overruns loses the
     * lock and lasts 40 ms, and alternating 20 and 40 ms shimmers by another route.
     *
     * 0 = free (Asteroids). != 0 = locked to that period in bus cycles. */
    s_draw_cycles = cycles;              /* how long it took to DRAW, without the filler */
#if defined(UVM2_HOST)
    /* No TIMER0 in the host harness: the old padding, by list cycles. */
    if (uvm2_pacer_cycles == 0) {
        s_frame_cycles = cycles;
    } else if (cycles < uvm2_pacer_cycles) {
        uvm2_bus_delay(uvm2_pacer_cycles - cycles);
        s_frame_cycles = uvm2_pacer_cycles;
    } else {
        uvm2_stats.overrun++;
        s_frame_cycles = cycles;
    }
#else
    /* THE PADDING IS MEASURED AGAINST THE CLOCK, NOT AGAINST THE LIST. Padding
     * `pacer - cycles` assumes that between one frame's end and the next only the list has
     * happened, and that is true when core 1 replays it in a loop (the .um2, which does not
     * come through here) and false in single core: our own cartridge's BIOS builds the list,
     * reads the controllers and services svc BETWEEN frames, and all of that was being added
     * to the period. Measured over SWD in its menu: the list padded to exactly 30000 cycles
     * and frames from 20.5 to 26.5 ms (40-48 fps, no lock, and the mains beat was visible).
     * Here the target is "the previous frame ended at T, this one ends at T + period":
     * whatever is left is waited out with TIMER0, and if we have already overshot it is a real
     * overrun and it re-locks from now. */
    {
        static uint32_t s_end_us;
        const uint32_t now = *(volatile uint32_t *)0x400B000Cu;   /* TIMER0 TIMELR */
        if (uvm2_pacer_cycles == 0) {
            s_frame_cycles = cycles;
            s_end_us = now;
        } else {
            const uint32_t period_us = (uvm2_pacer_cycles * 2u) / 3u;   /* 1.5 MHz bus */
            const uint32_t target = s_end_us + period_us;
            const int32_t remaining = (int32_t)(target - now);
            if (remaining > 0 && (uint32_t)remaining <= period_us) {
                while ((int32_t)(target - *(volatile uint32_t *)0x400B000Cu) > 0) { }
                s_frame_cycles = uvm2_pacer_cycles;
                s_end_us = target;
            } else {
                uvm2_stats.overrun++;
                s_frame_cycles = cycles;
                s_end_us = now;
            }
        }
    }
#if defined(UVM2_PIO_STREAM)
    vbus_list_fire();     /* and this core carries on: the frame goes out by DMA */
#endif
#endif

    measure_period();

#ifdef UVM2_DELAY_US
    artificial_delay();
#endif
}

#ifdef UVM2_DELAY_US
/* At the end of the frame close, which is where the cost of writing to PSRAM lands. */
#endif

uint32_t uvm2_frame_bus_cycles(void) { return s_frame_cycles; }

uint32_t uvm2_frame_count(void) { return s_frames; }
