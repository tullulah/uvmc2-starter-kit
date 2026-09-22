/*
 * uvm2_bus.h — Ultimate Vectrex Multicart 2: halt-mode Vectrex bus contract.
 *
 * The UVM2 has no HAL and no BIOS: the RP2350's GPIOs are wired straight to the
 * Vectrex cartridge bus, and a game either answers the 6809's ROM fetches or it
 * halts the 6809 and drives the VIA itself.  We always do the latter — the
 * generated code is native RP2350, so the 6809 has nothing to execute.
 *
 * Drawing therefore means "write the VIA registers the 6809 would have written",
 * phase-locked to the 1.5 MHz CLK that the 6809 keeps generating even while it
 * is halted.  Rather than writing them one at a time (which stalls the CPU on
 * every edge and leaves the beam idle during game logic), commands are RECORDED
 * into a buffer and replayed back-to-back by uvm2_exec().  This is the model the
 * multicart's own games use, and the command encoding below is deliberately
 * bit-identical to theirs so both executors are comparable.
 *
 * Command word layout:
 *      bits 31..20   delay: bus cycles to idle AFTER this write (0..4095)
 *      bits 19..16   VIA register select  → A0-A3 (GPIO8-11)
 *      bits 15..8    data byte            → D0-D7 (GPIO0-7)
 *      bits  7..0    unused
 * so (word >> 8) & 0xFFF lands directly on GPIO0-11 with no shifting in the
 * inner loop — one bus cycle per command, 667 ns.
 *
 * GPIO map (2026-04-16; GPIO8 = A0 confirmed against a reference game):
 *   0-7 D0-D7 | 8-21 A0-A13 | 22 PB6 | 23 /IRQ | 24 A14 | 25 A15
 *   26 R/W    | 27 /HALT     | 29 /NMI | 31 CLK        (28/30 are not ours)
 */
#ifndef UVM2_BUS_H
#define UVM2_BUS_H

/* ── THE BASELINE IS NOT NEGOTIABLE: DUAL CORE + PIO + DMA ───────────────────
 *
 * A UVM2 game draws with core 1 replaying the list through the PIO stream, which the DMA
 * feeds. This is not an optimisation and not a variant: it is what EVERYTHING written in
 * this SDK has been measured against, and comparing anything else to those numbers gives
 * false conclusions.
 *
 * WHY IT IS AN #error AND NOT A DEFAULT. A default can be stepped on without anyone
 * noticing, and it was: on 2026-09-09, with the whole SDK tuned against the reference
 * capture, dkong, asteroids and the VPy Snow Bros — the three games tested daily — were
 * compiling with NEITHER of the two. Snow Bros was running on SIO and a single core, and
 * nobody knew because the build said nothing. Measured in dkong that same day, on the
 * console: 51 ms drawing and 46 ms of game logic IN SERIES, i.e. 13 Hz for not having
 * core 1.
 *
 * `UVM2_PIO_STREAM` brings PIO and DMA together: it is the `bus` feature of the shared
 * crate, and there is no sub-switch to keep only one of them.
 *
 * THE WAY OUT, FOR BENCHES THAT ARE NOT GAMES. The capture players replay a recorded bus
 * capture and measure the executor in isolation: there, core 1 is exactly what you do NOT
 * want. Those declare `UVM2_BENCH_NO_CORE1` in their build block, which is a place where it
 * gets read and justified — not somewhere it gets forgotten. A GAME that declares it is
 * lying.
 *
 * THE OTHER WAY OUT IS THE VECTREX STUDIO CARTRIDGE BIOS (`UVM2_BIOS`). There the game runs
 * on core 1 and records ops into a ring; core 0 — the BIOS — builds the list with this same
 * uvm2_draw.c and executes it itself, so there is no SDK core 1 and none is needed. That is
 * that cartridge's BIOS+svc model, kept deliberately. The only board-specific thing that
 * changes in this file is the bus word, UVM2_VIA_WORD. */
#if !defined(UVM2_BENCH_NO_CORE1) && !defined(UVM2_BIOS)
#  if !defined(UVM2_DUAL_CORE)
#    error "UVM2: UVM2_DUAL_CORE is missing. A game draws with core 1; if this is a bench that measures the executor in isolation, declare UVM2_BENCH_NO_CORE1 in its build block and say why."
#  endif
#  if !defined(UVM2_PIO_STREAM)
#    error "UVM2: UVM2_PIO_STREAM is missing (it brings PIO and DMA). The SIO path exists to bisect, not to play; if this is a bench, declare UVM2_BENCH_NO_CORE1."
#  endif
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── RP2350 SIO ──────────────────────────────────────────────────────────────
 * NOT the RP2040 offsets: RP2350 interleaves GPIO_HI_* (for GPIO32-47), so
 * every OUT/OE register moves.  (RP2350 datasheet §3.1.11.) */
#define UVM2_SIO_BASE       0xD0000000u
#define UVM2_MMIO(addr)     (*(volatile uint32_t *)(uintptr_t)(addr))
#define UVM2_REG(off)       UVM2_MMIO(UVM2_SIO_BASE + (off))
#define UVM2_CPUID          UVM2_REG(0x000)   /* 0 or 1: which core is running */
#define UVM2_GPIO_IN        UVM2_REG(0x004)
#define UVM2_GPIO_OUT       UVM2_REG(0x010)
#define UVM2_GPIO_OUT_SET   UVM2_REG(0x018)
#define UVM2_GPIO_OUT_CLR   UVM2_REG(0x020)
#define UVM2_GPIO_OUT_XOR   UVM2_REG(0x028)
#define UVM2_GPIO_OE_SET    UVM2_REG(0x038)
#define UVM2_GPIO_OE_CLR    UVM2_REG(0x040)

/* ── Pin masks ─────────────────────────────────────────────────────────────── */
#define UVM2_DATA_MASK      0x000000FFu   /* D0-D7   GPIO0-7   */
#define UVM2_ADDR_LO_MASK   0x003FFF00u   /* A0-A13  GPIO8-21  */
#define UVM2_A14_MASK       0x01000000u
#define UVM2_A15_MASK       0x02000000u
#define UVM2_RW_MASK        0x04000000u   /* 1 = read, 0 = write */
#define UVM2_HALT_MASK      0x08000000u   /* drive LOW to own the bus */
#define UVM2_CLK_MASK       0x80000000u
#define UVM2_PB6_MASK       0x00400000u

/* Everything we drive (data + full address + R/W + /HALT). */
#define UVM2_OUT_MASK       (UVM2_DATA_MASK | UVM2_ADDR_LO_MASK | \
                             UVM2_A14_MASK | UVM2_A15_MASK |      \
                             UVM2_RW_MASK  | UVM2_HALT_MASK)
/* Bus lines only — never touches /HALT, which stays asserted for good. */
#define UVM2_BUS_MASK       (UVM2_OUT_MASK & ~UVM2_HALT_MASK)

/* $D000 = A15|A14|A12; A12 is GPIO20 because GPIO8 = A0. */
#define UVM2_VIA_BASE_BITS  (UVM2_A15_MASK | UVM2_A14_MASK | (1u << 20))
/* Idle/park address: $8000 is unmapped on the Vectrex, so a parked write cycle
 * reaches no device.  Parking at $D00x instead would re-run the last VIA write
 * (or, with R/W high, keep clearing IFR flags) for as long as the bus idles. */
#define UVM2_PARK_BITS      UVM2_A15_MASK
/* The bus word for ONE write to the VIA, built from the 12 bits (reg<<8 | data) of a list
 * command. THE LIST IS THE SAME on both boards; only this changes.
 *   UVM2:                data on GP0-7, A0-A13 on GP8-21, A14/A15 on GP24/25.
 *   Vectrex Studio cart: A0-A14 on GP4-18, A15 on GP19, data on GP21-28 (board.rs). */
#ifdef UVM2_BIOS
#define UVM2_VIA_WORD(out) \
    (((0xD000u | (((out) >> 8) & 0xFu)) << 4) | (((out) & 0xFFu) << 21))
#else
#define UVM2_VIA_WORD(out) (UVM2_VIA_BASE_BITS | (out))
#endif

/* The 12 bits of a command that map onto GPIO0-11. */
#define UVM2_CMD_GPIO_MASK  0x00000FFFu

/* ── VIA 6522 registers (index only — the base is implicit) ────────────────── */
enum {
    UVM2_VIA_PORTB = 0x0, UVM2_VIA_PORTA = 0x1,
    UVM2_VIA_DDRB  = 0x2, UVM2_VIA_DDRA  = 0x3,
    UVM2_VIA_T1CL  = 0x4, UVM2_VIA_T1CH  = 0x5,
    UVM2_VIA_T1LL  = 0x6, UVM2_VIA_T1LH  = 0x7,
    UVM2_VIA_T2CL  = 0x8, UVM2_VIA_T2CH  = 0x9,
    UVM2_VIA_SR    = 0xA, UVM2_VIA_ACR   = 0xB,
    UVM2_VIA_PCR   = 0xC, UVM2_VIA_IFR   = 0xD,
    UVM2_VIA_IER   = 0xE,
};

/* ── Port B bits (Vectrex wiring) ──────────────────────────────────────────── */
#define UVM2_PB_MUX_DISABLE 0x01u   /* 1 = sample/hold off (mux disabled)     */
#define UVM2_PB_MUX_SEL0    0x02u
#define UVM2_PB_MUX_SEL1    0x04u
#define UVM2_PB_RAMP_OFF    0x80u   /* /RAMP: 1 = integrators frozen          */
#define UVM2_PB_IDLE        (UVM2_PB_RAMP_OFF | UVM2_PB_MUX_DISABLE)
/* Mux channel select (with MUX_DISABLE clear the DAC value is sampled into it) */
#define UVM2_MUX_Y          0x00u
#define UVM2_MUX_ZEROREF    UVM2_PB_MUX_SEL0
#define UVM2_MUX_Z          UVM2_PB_MUX_SEL1

/* ── PCR (VIA_cntl) bits ───────────────────────────────────────────────────── */
#define UVM2_PCR_IDLE       0xCCu   /* CA2 (/ZERO) high, CB2 (/BLANK) low     */
#define UVM2_PCR_ZERO_OFF   0x02u   /* set  → /ZERO released                   */
#define UVM2_PCR_BLANK_OFF  0x20u   /* set  → beam lit                         */

/* ── Command encoding (byte-compatible with the multicart's own) ───────────── */
#define UVM2_CMD(reg, data, delay)                     \
    (((uint32_t)(delay) << 20) | ((uint32_t)(reg) << 16) | \
     (((uint32_t)(data) & 0xFFu) << 8))
#define UVM2_CMD_MAX_DELAY  4095u

/* THE LIST IS STORED IN 3 BYTES PER COMMAND, not 4.
 *
 * UVM2_CMD leaves bits 0-7 UNUSED: the information is exactly 24 bits (delay 12, register 4,
 * data 8). Storing four bytes per command throws one away in four, and on the UVM2 that is
 * paid in SRAM — the image lives in 496 KB and dkong's list is 64 KB with the game 3788
 * bytes from the ceiling.
 *
 * With 3 bytes, the same 64 KB holds 21845 commands instead of 16384, or today's 8192 take
 * 24 KB instead of 32. And nothing is lost: it is the same value, shifted.
 *
 * The cost is reading it byte by byte in the executor, and there is time to spare there:
 * each command is one Vectrex bus cycle, 667 ns, against a handful of CPU cycles at
 * 150 MHz. */
#define UVM2_CMD_PACK(w)       ((w) >> 8)          /* 32 bits -> the 24 that matter */
#define UVM2_CMD_REG_DATA(v)   ((v) & UVM2_CMD_GPIO_MASK)
#define UVM2_CMD_DELAY(v)      ((v) >> 12)

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/* Bring-up, in three steps because their order is load-bearing:
 *   uvm2_cpu_init()  .bss, vector table, firmware interrupts off.  MUST be the
 *                    first C executed — every static is garbage until it runs.
 *   uvm2_bus_pads()  pads/function select.  CLK becomes readable here, which is
 *                    what lets a switched-off console be detected rather than
 *                    hanging on the first edge wait.
 *   uvm2_bus_halt()  park the bus, enable the drivers, assert /HALT for good.
 * uvm2_bus_init() runs all three for callers that need no diagnostics between. */
void uvm2_cpu_init(void);
void uvm2_bus_pads(void);
void uvm2_bus_halt(void);
void uvm2_bus_init(void);

/* ── Command stream ────────────────────────────────────────────────────────── */

/* ALWAYS IN SRAM. See the .time_critical note in memmap_psram.ld: executing from PSRAM, a
 * cache miss inside the E window pushes the write into the next period and the drawing
 * shimmers. In normal (SRAM) images this changes nothing. */
/* UVM2_HOST builds this file into a host harness (tools/), where the section name is not
 * a valid mach-o specifier and there is no SRAM to pin anything to. */
#ifdef UVM2_HOST
#define UVM2_RAMFUNC
#else
#define UVM2_RAMFUNC __attribute__((section(".time_critical.uvm2"), noinline))
#endif

/* Replay `count` commands back-to-back, one bus cycle each plus their delays.
 * R/W is held low for the whole batch (as in the reference executor) and the
 * bus is parked at $8000 on exit.  Returns the bus cycles consumed. */
UVM2_RAMFUNC uint32_t uvm2_exec(const uint8_t *cmds, uint32_t count);

/* CPU cycles per E period, in Q8 (100.0 cycles = 25600). Filled in by uvm2_measure_e(),
 * which must be called with the bus already taken. It is the ratio that decides whether the
 * PIO stream's phase calibration carries across boards — see uvm2_bus.c. */
extern uint32_t uvm2_cycles_per_e_q8;
void uvm2_measure_e(void);

/* Idle for `cycles` Vectrex bus cycles (667 ns each) — clock-independent, which
 * is what beam/integrator timing needs. */
UVM2_RAMFUNC void uvm2_bus_delay(uint32_t cycles);

/* ── Single accesses (outside the command stream) ───────────────────────────
 * Reads cannot be recorded — they need the data bus turned around mid-cycle —
 * so input polling runs directly, exactly as the reference does after replaying
 * its frame.  uvm2_via_write is the one-off equivalent of a single command. */
UVM2_RAMFUNC void    uvm2_via_write(uint32_t reg, uint32_t data);
UVM2_RAMFUNC uint8_t uvm2_via_read(uint32_t reg);

/* ── Instrumentation ───────────────────────────────────────────────────────
 * A 50 Hz frame is 30000 bus cycles.  These let a game (or the IDE) report how
 * much of that budget the last frame actually spent, which is the only fair way
 * to compare this command-stream model against the syscall-per-write one. */
typedef struct {
    uint32_t commands;      /* commands replayed last frame                   */
    uint32_t bus_cycles;    /* bus cycles they consumed (writes + delays)     */
    uint32_t vectors;       /* lit segments drawn last frame                  */
    uint32_t overrun;       /* frames whose stream exceeded the 50 Hz budget  */
    uint32_t moves;         /* blanked repositions last frame (beam travel)   */
    uint32_t ramp_cycles;   /* cycles spent with the integrators running      */
    /* Commands DROPPED because the list was full, last frame. A silent cap reads as "the
     * drawing is broken" and sends you to debug the wrong place: that happened on
     * 2026-08-18 with the T1 model, which reached exactly 8192 and lost the tail of the
     * frame. If this is not zero, NOTHING you see on screen is conclusive. */
    uint32_t dropped;
    /* How many times it has recalibrated, CUMULATIVE. If this does not climb, Recalibrate
     * IS NOT BEING CALLED — and "it does not work" and "it does not run" are two different
     * investigations. Our own cartridge's firmware carries the same counter (RECALS) for
     * the same reason. */
    uint32_t recals;
    /* Bus cycles of the last frame, counted by the executor. Kept separately from
     * bus_cycles because the single-core path overwrites that one. */
    uint32_t exec_cycles;

    /* Snapshots of the three above, taken in uvm2_frame_end and never cleared.
     *
     * The live counters are reset in uvm2_frame_begin and fill up as the frame is
     * built, so a debugger sampling at an arbitrary moment mostly catches them at
     * zero — a game that spends 40 ms emulating a CPU and 2 ms drawing is in the
     * "reset, not yet drawn" window almost always. Read these instead; they hold
     * the last COMPLETE frame for as long as it takes to look. */
    uint32_t vectors_last;
    uint32_t moves_last;
    uint32_t ramp_cycles_last;
    uint32_t wait_spins;    /* passes core 0 spent waiting for the buffer */
    uint32_t us_exec;       /* core 1: microseconds executing the list     */
    uint32_t us_input;      /* core 1: reading controllers and axes        */
    uint32_t us_rest;       /* core 1: everything else in the loop         */
    uint32_t us_wait;       /* core 1: waiting for core 0 to publish       */

    /* ── REAL TIME PER FRAME ─────────────────────────────────────────────────
     *
     * WHAT FOR. The padding in uvm2_frame_end is computed from the BUS CYCLES OF THE
     * DRAWING, so however long the game's emulation takes does NOT enter the count: the
     * frame period ends up being `emulation + 20 ms` instead of `max(20 ms, everything)`.
     * What the emulated CPU costs is ADDED to the frame instead of being absorbed by it.
     *
     * Consequence: the real refresh drops below 50 Hz and, worse, VARIES with the scene —
     * few rocks, short frame; many rocks, long frame. That is stutter, and none of the
     * existing counters sees it: `overrun` measures only the bus stream (13 of 3677 with
     * the screen shimmering), and neither does `bus_cycles`, because the emulation happens
     * BETWEEN frames.
     *
     * us_frame_* is the COMPLETE period, measured from one frame_end to the next. If the
     * model is right: min ~20000 on empty scenes, max well above it with many vectors, and
     * a high vectors_at_max. If it all comes out pinned at 20000, the model is false and
     * you have to look somewhere else. */
    uint32_t us_frame_last;
    uint32_t us_frame_min;
    uint32_t us_frame_max;
    uint32_t vectors_at_max; /* vectors of the slowest frame: ties time to scene */
    uint32_t slow_frames;    /* periods above 20.5 ms */
    uint32_t measured_frames;

    /* ── THE HEARTBEAT ───────────────────────────────────────────────────────
     *
     * `slow_frames` says HOW MANY and `us_frame_max` says HOW MUCH, but what you see on
     * screen is a rhythm: "it blinks once a second, sometimes twice". A counter cannot tell
     * 50 bad frames in a row from one bad frame every 50, and those are different faults.
     *
     * `slow_gap_*` measures the FRAMES BETWEEN two consecutive slow ones: if it comes out
     * around 50, the heartbeat is 1 Hz and you should look for something with a one-second
     * period; if it comes out at 5, it is something else. `hist_frame` gives the whole
     * shape of the distribution, which is what separates "everything a bit long" from
     * "nearly all pinned and one enormous one".
     *
     * And `at_max_*` is the photograph of the SLOWEST frame: it says whether it went into
     * replaying the list (us_exec), reading the controllers (us_input), waiting for core 0
     * (us_wait) or into the rest of the loop. Without that you only know THAT there was a
     * stutter, not WHOSE it was. */
    uint32_t hist_frame[8];  /* <20.5 <21 <22 <24 <28 <36 <52 and the rest, in ms */
    uint32_t slow_gap_last;  /* frames since the previous slow one */
    uint32_t slow_gap_min;
    uint32_t slow_gap_max;
    uint32_t at_max_us_exec;
    uint32_t at_max_us_input;
    uint32_t at_max_us_rest;
    uint32_t at_max_us_wait;
    uint32_t at_max_dropped;
    uint32_t at_max_commands;

    /* THE RATIO BETWEEN WHAT THE LIST ASKS FOR AND HOW LONG IT TAKES, in hundredths:
     * 100 = the bus is running at its nominal 1.5 MHz, 50 = at half. It separates "the list
     * is too big" from "something is slowing the executor down", which are opposite faults
     * and used to be indistinguishable. See uvm2_core1.c. */
    uint32_t exec_ratio_last, exec_ratio_min, exec_ratio_max;
    uint32_t hist_ratio[8];  /* <50 <70 <85 <95 <105 <130 <200 and the rest */

    /* THE FRAME'S BOUNDING BOX and its jump relative to the previous one, in hundredths
     * (100 = unchanged). A frame drawn at a different SCALE jumps here, and no counting
     * metric sees it. */
    uint32_t box_w, box_h;
    uint32_t box_ratio_last, box_ratio_min, box_ratio_max, jump_box;
    /* The last 6 jumps, with the 3 ratios that come AFTER each one: it tells a fault (it
     * goes up and comes back) from an animation (it goes up and stays). */
    uint32_t box_peak[6][4];
    /* Firings of avg_mgo (the AVG's list generation) in the frame being built, and their
     * distribution: >1 means the geometry is accumulated TWICE into the same list. */
    uint32_t mgo_per_frame;
    uint32_t mgo_hist[4];    /* 0, 1, 2, 3 or more firings per frame */

    /* CORE 0'S TIME, SPLIT IN TWO. They are different attacks: `us_emul` is the 6502 and the
     * AVG (the port's code) and `us_list` is building the command list (our SDK). Without
     * separating them, optimising is blind. Accumulated in microseconds and with their own
     * counter, so the mean can be taken without probing. */
    uint32_t us_emul_acc, us_emul_n;     /* 6502 + AVG, without the list */
    uint32_t us_list_acc, us_list_n;     /* BUILDING the list */
    uint32_t us_pub_acc,  us_pub_n;      /* publishing it — INCLUDES waiting for core 1 */
    /* CORE 1'S TIME, WITH A SINGLE WRITER. `us_exec` is written by core 1 AND by
     * uvm2_svc.c, so reading it does not say whose it is — and we built a whole diagnosis
     * on top of that number. These belong to core 1 and to nobody else. */
    uint32_t us_c1_exec_acc, us_c1_exec_n;
    uint32_t us_c1_wait_acc;             /* what core 1 waited for core 0 */
    /* RAW, WITH NOTHING SUBTRACTED. `us_emul` comes from taking the 6502's pass and
     * subtracting however much `us_list`/`us_pub` grew inside it, and that subtraction gave
     * a mean of 0.00 ms — i.e. what was discounted equalled or exceeded what elapsed, and
     * the clamp hid it. A number computed by subtracting accumulators written by SOMEONE
     * ELSE is not a measurement: when it comes out absurd you cannot tell which of the three
     * is lying. These two are the gross figure and the discount separately, each with a
     * single writer, and `us_emul` stays as a derived value. */
    uint32_t us_cpu_raw_acc, us_cpu_raw_n;   /* the 6502's whole pass */
    uint32_t us_cpu_sub_acc;                 /* what gets discounted from it (list+pub) */
    /* FRAME CLOSE ON CORE 0. In dual core `uvm2_svc.c` used to write this into `us_exec`,
     * the same field core 1 uses for the real drawing: last writer wins and the reader
     * cannot tell whose it is. Now each has its own. */
    uint32_t us_frame_end;
    /* Periods where core 1 ran WITHOUT a list and read the controllers on its own count
     * (uvm2_core1.c): a game that is not drawing still sees its buttons. At the end, so as
     * not to move any offsets. */
    uint32_t idle_frames;
    /* .vsmp samples put into the last frame's list (uvm2_smp.c). With a voice
     * active it should be around `frame_cycles / (1.5 MHz / UVM2_SMP_HZ)`: 160 at
     * 8 kHz in a 50 Hz frame. A zero while audio was asked for means there was no
     * gap to inject into — a list with no strokes — not that the player is
     * stopped; two different faults. At the end of the struct, to not move any
     * offsets. */
    uint32_t samples;
} uvm2_stats_t;

extern uvm2_stats_t uvm2_stats;

/* Bus cycles consumed by single accesses (input, PSG) — see uvm2_bus.c.  The
 * frame pacing must subtract these or it overshoots by whatever they cost. */
extern uint32_t uvm2_single_cycles;

/* THE REFRESH RATE IS NOT A LAW, IT IS A HABIT.
 *
 * The Vectrex has no vsync: it is a VECTOR monitor and it redraws when it is told to. The
 * 50 Hz comes from the BIOS doing that, not from the electronics. And the vector arcade
 * machines had no fixed refresh either — Asteroids redrew as soon as it finished its list,
 * so it dimmed a little when the screen filled up with rocks.
 *
 * It can be changed here: 25000 = 60 Hz, 30000 = 50 Hz. With asteroids drawing in 11745
 * cycles, there is still room to spare at 60. And it is the direct test of whether the
 * problem is that WE are holding it back: if it looks better at 60, it was.
 *
 * MIND THIS WHEN CHANGING IT: refreshing more often also makes it BRIGHTER (more passes per
 * second over the same phosphor), so the intensity may need adjusting, and that adjustment
 * must not be confused with the result of the test. */
#ifndef UVM2_HZ
#define UVM2_HZ 50
#endif

/* THE BUS CLOCK, with a name. It used to be written out by hand as 1500000 in the four
 * places below; with a name you can see that all four derive from the SAME clock and are
 * not four separate numbers. */
#define UVM2_BUS_HZ  1500000u

/* UVM2_HZ = 0 means UNLIMITED: present as soon as the list is built, like the arcade
 * machine. There is then no fixed period to define, and the 1 is only there so the division
 * does not explode at compile time; nobody uses it, because the padding is compiled out. */
#if UVM2_HZ == 0
#define UVM2_CYCLES_PER_FRAME  (UVM2_BUS_HZ / 1u)
#else
#define UVM2_CYCLES_PER_FRAME  (UVM2_BUS_HZ / (unsigned)UVM2_HZ)
#endif

/* TWO THINGS THAT ARE NOT THE FRAME PERIOD, and which used it because at 50 Hz they
 * coincide.
 *
 * At 50 Hz UVM2_CYCLES_PER_FRAME is 30000 and everybody wrote 30000 where they meant "20 ms"
 * or "one music tick". Move UVM2_HZ and those two moved with it: at 60 Hz the music speeds
 * up by 20%, and with UVM2_HZ=0 the /HALT settling goes from 20 ms to ONE SECOND.
 * Coinciding is not being the same thing. */
#define UVM2_CYCLES_20MS   (UVM2_BUS_HZ / 50u)   /* settling after asserting /HALT */
#define UVM2_AUDIO_CYCLES  (UVM2_BUS_HZ / 50u)   /* .vmus tempo is 50 Hz, not the refresh */

#ifdef __cplusplus
}
#endif

#endif /* UVM2_BUS_H */
