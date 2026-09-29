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
#include "aae_romload.h"   /* aae_rom_last_error + the on-screen notice */
extern unsigned char v_readButtons(void);
extern void v_readJoystick1Analog(void);
/* ts_audio.c: read the sample bundle off the card into PSRAM. Here and not on the
 * first sound, because that read would land in the middle of a frame. */
extern void ts_audio_init(void);

#ifdef UVM2_PICO_RUNTIME
/* ── WHERE THE SOUND COMES OUT, AND WHO DECIDES ────────────────────────────
 *
 * Two complete paths for the same 22 sounds:
 *
 *   console   the PSG's 4-bit volume register used as a DAC, at 12 kHz, written in the
 *             gaps of the draw list (uvm2_smp + TACSCAN.VSM, 242 KB)
 *   jack      the cartridge's PT8211 at 16 bits and 32 kHz, off the Vectrex entirely
 *             (ts_jack + TACSCAN.PCM, 2.64 MB in PSRAM)
 *
 * The player chooses, on the AUDIO line of the menu. The SDK stores that setting and
 * deliberately does not act on it: routing a game's sound is the game's business, and this
 * is the game. samples.c is where the choice is actually read, per sound. */
#include "ts_jack.h"
#include "uvm2_config.h"
#include "uvm2_text.h"
#include "uvm2_smp.h"
extern volatile int32_t uvm2_setting_audio;   /* 0 = jack, 1 = the console's chip */
extern volatile int32_t uvm2_setting_menu;    /* 1 = open the menu on power-up */
extern volatile uint8_t uvm2_cached_buttons;  /* core 1 refreshes it; RAW, active low */

/* THE JACK BUNDLE IS 2.64 MB AND THE CARD IS BIT-BANGED SPI, so it cannot come in inside
 * one frame — and it must not be read in the middle of one either. It is loaded here, with
 * a line on screen, in slices of 32 KB per pass. The frame rate sags while it does and that
 * costs nothing: there is nothing on screen to be smooth.
 *
 * ONLY IF THE PLAYER ASKED FOR THE JACK. Spending the wait on a bundle that is not going to
 * be used would be a tax on everyone who prefers the console's sound, so this also runs
 * AFTER the menu closes, when the choice has just changed to the jack and the bundle is not
 * in yet. */
static void ts_load_jack_audio(void)
{
    if (uvm2_setting_audio != 0 || ts_jack_ready()) return;
    if (!ts_jack_begin()) return;        /* no jack on this board, or no PSRAM */
    for (;;) {
        char line[24];
        int  p = 0, pc = ts_jack_load_percent();
        for (const char *t = "LOADING AUDIO "; *t; t++) line[p++] = *t;
        if (pc >= 100) line[p++] = (char)('0' + pc / 100);
        if (pc >= 10)  line[p++] = (char)('0' + (pc / 10) % 10);
        line[p++] = (char)('0' + pc % 10);
        line[p++] = '%';
        line[p] = 0;

        v_WaitRecal();
        aae_text(line, 0, 200);
        if (ts_jack_load_step()) break;
    }
}

/* THE MENU, HELD RATHER THAN TAPPED. All four buttons are in play during a game (fire,
 * grab, coin, start), so the menu needs a gesture the game itself never makes: buttons 2
 * and 3 together, held. The same pair the SDK's boot combo uses, so it is one thing to
 * remember rather than two.
 *
 * HELD FOR THREE QUARTERS OF A SECOND, because 2 and 3 are grab and coin: pressing both for
 * one frame is something a player could plausibly do, and losing the game to a menu would be
 * worse than the menu being slightly deliberate to open. */
#define TS_MENU_HOLD 30      /* frames at 40 Hz */

static void ts_open_menu(void)
{
    /* Nothing must be left sounding on the path we may be about to leave: a looping voice
     * (the tunnel, the ship's roar) would otherwise keep going on the old one for ever. */
    uvm2_smp_stop(UVM2_SMP_ALL);
    ts_jack_stop(-1);

    uvm2_config_wizard();        /* draws, adjusts, and button 4 saves to TACSCAN.CFG */
    ts_load_jack_audio();        /* the choice may have just become the jack */
}

static void ts_menu_poll(void)
{
    static int held;
    /* Active low: 0 = pressed. Buttons 2 and 3 are bits 1 and 2. */
    const uint8_t b = (uint8_t)~uvm2_cached_buttons;
    if ((b & 0x06u) == 0x06u) held++;
    else                      held = 0;
    if (held < TS_MENU_HOLD) return;
    held = 0;
    ts_open_menu();
}
#endif

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

#ifdef UVM2_PICO_RUNTIME
    /* WHICH OF ITS OWN SETTINGS THIS GAME HAS. Beam calibration belongs to the console and
     * is shared; these are the game's, and they go in config/TACSCAN.CFG layered on top.
     * Without declaring them the wizard shows only the console's fields and nothing here is
     * remembered between power-ups.
     *
     *   AUDIO   jack or the console's chip — the whole point of the menu
     *   HZ      Tac/Scan is a 40 Hz board, but the refresh cap is still the player's
     *   MENU    whether this menu opens on power-up
     *
     * No ROTATE: Tac/Scan's screen is vertical, so a horizontal/vertical switch would be a
     * line that does nothing. */
    uvm2_config_game("TACSCAN", UVM2_SETTING_AUDIO | UVM2_SETTING_HZ | UVM2_SETTING_MENU);
    /* The runtime already loaded the console's file before main; this layers the game's on
     * top of it, which is what makes the AUDIO choice survive a power cycle. */
    uvm2_config_load();
#endif

    ts_audio_init();
    aae_load_tacscan_roms();   /* build GI[0]/GI[1] + copy ROMs (must precede init) */
    /* With no romset, do NOT emulate over zeros: that draws nothing, and a black
     * screen cannot tell "the ROM is missing" from "the game hung". Say so. */
    if (aae_rom_last_error) {
        for (;;) { v_WaitRecal(); aae_rom_error_screen(aae_rom_last_error); }
    }
    init_cpu_config();         /* num_cpus + cyclecount from driver[gamenum]        */
    init_segag80();            /* init Z80 context, port handlers, vector RAM        */

#ifdef UVM2_PICO_RUNTIME
    /* AFTER the romset, so a missing ROM is reported in a second rather than after the
     * audio bundle has finished coming off the card. */
    ts_load_jack_audio();
    if (uvm2_setting_menu) ts_open_menu();
#endif

    for (;;) {
        {
            TS_MARK(w);
            v_WaitRecal();        /* seals the list, hands it over, opens the next */
            v_readButtons();
            v_readJoystick1Analog();
#ifdef UVM2_PICO_RUNTIME
            /* The mixer BEFORE the menu poll: the poll can block for the length of a whole
             * menu session, and the ring has 64 ms. Refilling first means the jack is as far
             * ahead as it can be when that happens. */
            ts_jack_update();
            ts_menu_poll();
#endif
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
