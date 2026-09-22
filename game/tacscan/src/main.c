/* AAE Tac/Scan on the RP2350 cartridge — entry point.
 *
 * AAE-as-a-C-game: Tac/Scan is a Sega G80 vector game, so it is a Z80 CPU (the mz80
 * core) plus Sega's own vector generator (SegaG80.c), rather than the 6502 + Atari
 * AVG most of the other ports use. See <kit>/docs/ for the whole picture.
 *
 * Execution model (from AAE, mapped):
 *   - driver[TACSCAN] (aae_machine.c) holds Tac/Scan's config: CPU_MZ80 @ 3 MHz,
 *     INT_TYPE_INT, 40 fps, VEC_COLOR.
 *   - run_cpus_to_cycles() (cpu_control.c) runs the Z80 for a frame + fires the
 *     40 Hz IRQ; the game writes the vector RAM ($E000-$EFFF) which
 *     BWVectorGenerator()/sega_generate_vector_list() walks → v_directDraw32.
 *   - run_segag80() (SegaG80.c) is per-frame housekeeping (vector gen + sound gate).
 *
 * IMPORTANT: unlike Asteroids (gamenum=0), SegaG80.c switches on gamenum==TACSCAN
 * to pick the Z80 port handlers + security, so aae_machine.c sets gamenum=TACSCAN
 * (the AAE enum) and sizes driver[] to cover that index.
 */

/* AAE externs. */
extern int  init_segag80(void);
extern void run_segag80(void);
extern void run_cpus_to_cycles(void);   /* cpu_control.c */
extern void init_cpu_config(void);      /* cpu_control.c: derive num_cpus + cyclecount */

/* Our machine setup (aae_machine.c). Builds GI[0] (Z80 memory) + GI[1] (xyt
 * PROM / sin table) and copies the embedded ROMs in — must run before init. */
extern void aae_load_tacscan_roms(void);

/* RP2350 SDK shim (sdk_rp2350.c). */
extern void v_init(void);
extern void v_WaitRecal(void);
#include "aae_romload.h"   /* aae_rom_last_error + el aviso en screen */
extern unsigned char v_readButtons(void);
extern void v_readJoystick1Analog(void);
/* ts_audio.c: read the sample bundle off the card into PSRAM. Here and not on the
 * first sound, because that read would land in the middle of a frame. */
extern void ts_audio_init(void);

/* ── WHERE THE BUILDER'S TIME GOES, MEASURED ON THE CONSOLE ─────────────────
 *
 * OFF BY DEFAULT: build with -DTS_TELEM=1 to compile it back in.
 *
 * The host says the Z80 is half the builder's work; the console disagreed — the
 * Z80 idle-spin skip cut 94.8% of its cycles and the frame did not move (41.3 ms
 * before, 41.6 after). So the split has to be measured HERE, not inferred from an
 * M1: the two halves do not scale the same way through XIP on an M33.
 *
 * Two timer reads a frame, accumulated into plain globals. No RTT — defmt-rtt
 * blocks when nobody is draining it, and that already cost a black screen.
 * Read with `probe-rs read b32 <&ts_us_cpu> 3`.
 *
 * THE FOUR PARTS ADD UP TO A WHOLE FRAME ON PURPOSE, `ts_us_wr` included. Without
 * that one the remainder has to be guessed, and guessing it is how the difference
 * between an average and a single `us_frame_last` sample got read as 15 ms of stall
 * that the buffer handshake (wait_spins = 0) says never existed.
 *
 * TIMER0 TIMELR at 0x400B000C, the same 1 MHz source uvm2_frame_end uses. */
#ifndef TS_TELEM
#define TS_TELEM 0
#endif
#if TS_TELEM
#define TS_NOW() (*(volatile unsigned int *)0x400B000CU)
volatile unsigned int ts_us_cpu, ts_us_vec, ts_us_wr, ts_us_n;
#define TS_MARK(v)          unsigned int v = TS_NOW()
#define TS_ADD(acc, from)   ((acc) += TS_NOW() - (from))
#define TS_SPAN(acc, a, b)  ((acc) += (b) - (a))
#define TS_TICK()           (ts_us_n++)
#else
#define TS_MARK(v)          ((void)0)
#define TS_ADD(acc, from)   ((void)0)
#define TS_SPAN(acc, a, b)  ((void)0)
#define TS_TICK()           ((void)0)
#endif

/* THE MACHINE'S OWN RATE, DECLARED BY THE GAME. Tac/Scan is a 40 Hz board, and that
 * is not a convention: the interrupt comes from the 15468480 Hz master crystal divided
 * by 3 and then by 0x1f788, which is 40.00 Hz exactly (SegaG80.c; MAME's segag80v.cpp
 * says set_refresh_hz(40) with the same derivation).
 *
 * IT MATTERS BECAUSE FPS = GAME SPEED HERE. The game's logic advances on that
 * interrupt, one per frame, so a faster refresh is a faster game — at the 42.8 fps it
 * was free-running at, Tac/Scan played 7% fast. Pinning 40 Hz gives the speed Sega
 * designed and, with the list at ~35000 of the 37500 cycles that fit in 25 ms, leaves
 * headroom so a heavy scene does not make the refresh breathe.
 *
 * The pacer is against the CLOCK, not against the list's cycle count (uvm2_core1.c). */
void uvm2_set_refresh(unsigned hz);

int main(void)
{
    v_init();
    uvm2_set_refresh(40);

    ts_audio_init();
    aae_load_tacscan_roms();   /* build GI[0]/GI[1] + copy ROMs (must precede init) */
    /* With no romset, do NOT emulate over zeros: that draws nothing, and a black
     * screen cannot tell "the ROM is missing" from "the game hung". Say so. */
    if (aae_rom_last_error) {
        for (;;) { v_WaitRecal(); aae_rom_error_screen(aae_rom_last_error); }
    }
    init_cpu_config();         /* num_cpus + cyclecount from driver[gamenum]        */
    init_segag80();            /* init Z80 context, port handlers, vector RAM        */

    for (;;) {
        {
            TS_MARK(w);
            v_WaitRecal();        /* seals the list, hands it over, opens the next */
            v_readButtons();
            v_readJoystick1Analog();
            TS_ADD(ts_us_wr, w);
        }
        {
            TS_MARK(a);
            run_cpus_to_cycles(); /* run the Z80 a frame; fires the 40 Hz IRQ */
            TS_MARK(b);
            run_segag80();        /* vector generation + per-frame housekeeping */
            TS_SPAN(ts_us_cpu, a, b);
            TS_ADD(ts_us_vec, b);
            TS_TICK();
        }
    }
    return 0;
}
