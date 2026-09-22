/* uvm2_core1.c — the second core, replaying frames while the game thinks.
 *
 * This is the split the multicart's own games have used from the start:
 *
 *     core 0   builds a command list into one of two buffers, publishes it
 *     core 1   replays it over the bus, reads the controls, ticks the audio
 *
 * WHAT IT BUYS, AND WHAT IT DOES NOT
 *
 * It does NOT make drawing faster. The replay is paced by the Vectrex's own
 * 1.5 MHz clock — 30000 bus cycles is a 50 Hz frame by definition — and no
 * amount of CPU makes a bus cycle shorter. dkong's stream measured 69718 cycles
 * (46.5 ms), and that number is untouched by this file.
 *
 * What it buys is the overlap. Single-core, a frame is
 *
 *     [ emulate the Z80 ][ build the list ][ replay 46.5 ms ][ pace ]
 *
 * strictly in series, so the game's own time is added on top of the beam's.
 * Split, the emulation for frame n+1 runs while the beam is still drawing frame
 * n, and the frame costs max(logic, replay) instead of their sum.
 *
 * WHO OWNS THE BUS
 *
 * Core 1, exclusively, from the moment it starts. That is the whole discipline
 * here and it is not negotiable: two cores driving the same GPIO would put two
 * writers on the VIA with no arbitration, which is the same fault we shipped on
 * the cartridge when 40 games claimed dual-core while compiled single.
 *
 * It works out cleanly because every bus contact on this target was already
 * funnelled into SYS_WAIT_RECAL: the replay, the button/axis reads and the audio
 * tick. Those move here wholesale. The game keeps reading buttons and axes
 * through the syscalls, which have always answered from a cache rather than from
 * the wire, so nothing on the game side changes.
 *
 * The two exceptions are the syscalls that write the bus at an arbitrary moment:
 * SYS_PSG_WRITE goes through a small queue drained here at the frame boundary,
 * and the raw SYS_BUS_READ/WRITE pair is refused (see uvm2_svc.c) because it has
 * no meaning while another core owns the pins.
 */

#include "uvm2_bus.h"

extern uint32_t uvm2_sig_written[2], uvm2_sig_n[2];

/* Disagreements between what core 0 wrote and what core 1 reads out of the SAME buffer. If
 * this does not climb, cross-core coherence is ruled out and the fault is one of timing. */
uint32_t uvm2_sig_failures, uvm2_sig_compared, uvm2_sig_read, uvm2_sig_last_ok;
#include "uvm2_draw.h"
#include "uvm2_input.h"
#include "uvm2_audio.h"

#ifdef UVM2_DUAL_CORE

#include "pico/multicore.h"
#include "pico/time.h"

/* Published by uvm2_draw.c. `request` counts frames core 0 has finished
 * building, `done` frames core 1 has finished replaying; the buffer for frame n
 * is n & 1. Both are plain counters that only ever increase, so a torn read is
 * impossible on a 32-bit load and no lock is needed — the ordering is carried by
 * the barrier core 0 issues before publishing. */
extern volatile uint32_t uvm2_frame_request;
extern volatile uint32_t uvm2_frame_done;
extern const uint8_t    *uvm2_frame_buffer(uint32_t frame);
extern uint32_t          uvm2_frame_length(uint32_t frame);

/* THE FRAME PERIOD, MEASURED, so the ratio above is computed against something real. The
 * pacer fills it in when it closes each pass, so the ratio uses the PREVIOUS frame's period
 * — one frame of lag in a diagnostic changes no decision, and measuring it before the frame
 * has ended is impossible. */
static uint32_t s_cycle_us;

/* PSG writes the game issued mid-frame, drained between frames.
 *
 * A ring rather than a flag per register: a game that writes the same register
 * twice in a frame means both writes, and .vmus playback does exactly that on
 * the volume registers. Single producer (core 0), single consumer (core 1), so
 * head and tail need no lock. Overflow drops the OLDEST rather than the newest,
 * because for a PSG the newest write is the current state and the one that must
 * survive. */
#define PSG_QUEUE_LEN 64u
static volatile uint32_t s_psg_reg[PSG_QUEUE_LEN];
static volatile uint32_t s_psg_val[PSG_QUEUE_LEN];
static volatile uint32_t s_psg_head;      /* written by core 0 */
static volatile uint32_t s_psg_tail;      /* written by core 1 */

void uvm2_psg_queue(uint32_t reg, uint32_t value)
{
    uint32_t h = s_psg_head;
    /* Register 7 is the mixer, and the sample injector composes its own channel over
     * whatever it believes the mixer to be. Tell it, or a game that drives the PSG
     * itself gets its channels silenced once a frame -- see uvm2_audio_note_mixer.
     * Noted HERE, on the core that builds the list, because that is the core that reads
     * it back through uvm2_smp_mixer. */
    if (reg == 7u) { extern void uvm2_smp_note_mixer(uint8_t);
                     uvm2_smp_note_mixer((uint8_t)value); }
    s_psg_reg[h % PSG_QUEUE_LEN] = reg;
    s_psg_val[h % PSG_QUEUE_LEN] = value;
    __asm volatile ("dmb" ::: "memory");  /* the data before the index */
    s_psg_head = h + 1;
}

static void psg_drain(void)
{
    uint32_t h = s_psg_head;
    uint32_t t = s_psg_tail;

    /* If the producer ran away, skip forward and keep only the newest entries
     * the ring still holds — the older ones have already been overwritten. */
    if (h - t > PSG_QUEUE_LEN) t = h - PSG_QUEUE_LEN;

    __asm volatile ("dmb" ::: "memory");
    while (t != h) {
        uvm2_psg_write(s_psg_reg[t % PSG_QUEUE_LEN], s_psg_val[t % PSG_QUEUE_LEN]);
        t++;
    }
    s_psg_tail = t;
}

/* Cached controls, served to the game by SYS_READ_BUTTONS / SYS_READ_AXES.
 * Defined here in dual-core builds because this is the core that fills them.
 *
 * RELEASED IS 0xFF, AND THE INITIAL VALUE IS NOT COSMETIC. These are active low —
 * SYS_READ_BUTTONS answers `~uvm2_cached_buttons`, so a zero bit means PRESSED. Left in
 * .bss they start at 0, i.e. all four buttons held, and every game that waits for a
 * button starts by itself before core 1 gets to fill the cache at the end of frame one.
 * Single-core never showed it because core 0 fills s_buttons inside the same syscall path
 * the game polls from; dual-core fills it asynchronously, so the game can read first.
 * Seen on SnowBros 2026-08-25, the first VPy game to actually launch core 1. */
volatile uint8_t  uvm2_cached_buttons = 0xFFu;
volatile uint32_t uvm2_cached_axes;

/* Vectrex time not yet handed to the sequencer, in bus cycles. */
static uint32_t s_audio_acc;

void uvm2_core1_gap(void);   /* per-pass hook; weak, below */
static void core1_main(void)
{
    uint32_t served = 0;

    for (;;) {
        /* REALLY TIMED, in microseconds. We got here after two rounds of guessing where
         * ~17 ms per frame that were not the beam came from: the audio was blamed and
         * measured not to be it. A clock costs less than a hypothesis. */
        uint32_t t_w0 = time_us_32();
        /* WITHOUT A LIST THERE ARE NO CONTROLS — unless we read them here. The cache is
         * refreshed after replaying a list, so a game that stops drawing never sees a
         * button change: Major Havoc pauses while button 1 is held (no IRQs, no GO, no
         * list), read the stale "pressed" byte forever and never left the pause; the
         * same starvation made dkong's "hold 50 frames" count in microseconds. The beam
         * is clamped at centre since the previous frame's close, so reading here is as
         * safe as reading after a list. Once per frame period, and the hook runs too so
         * the cart's music and SD work keep going while the game shows nothing. */
        {
            const uint32_t period_us = uvm2_pacer_cycles ? uvm2_pacer_cycles * 2u / 3u : 20000u;
            uint32_t t_idle = t_w0;
            while (uvm2_frame_request == served) {         /* nothing published yet */
                if (time_us_32() - t_idle >= period_us) {
                    t_idle = time_us_32();
                    uvm2_single_cycles = 0;
#ifndef UVM2_NO_INPUT
                    uvm2_cached_buttons = uvm2_read_buttons();
                    uvm2_cached_axes    = uvm2_read_axes();
#endif
                    uvm2_draw_invalidate();
                    uvm2_core1_gap();
                    uvm2_stats.idle_frames++;
                }
            }
        }
        uint32_t t0 = time_us_32();
        uvm2_stats.us_wait = t0 - t_w0;
        uvm2_stats.us_c1_wait_acc += t0 - t_w0;   /* what core 1 waited for core 0 */
        served++;
        __asm volatile ("dmb" ::: "memory");       /* the buffer before the count */

#ifdef UVM2_CMDS_STAGE_SRAM
        /* CONTROL: the list lives in PSRAM but is REPLAYED from SRAM.
         *
         * The hypothesis under test: an XIP cache miss (16 KB) against the PSRAM takes
         * longer than one E period, so the write loses its phase — and the 6800 bus rule
         * is about PHASE, not settling time: it does not degrade, it fails. Hence
         * scribbles mixed in with good vectors rather than a blurry drawing.
         *
         * Copying the whole thing before starting removes EVERY miss from the window with
         * the beam lit. If this cleans it up, the cause is the fetch and the real fix is
         * to bring it in by DMA into a small ring while drawing. If it does NOT clean up,
         * the data is wrong and the fetch had nothing to do with it.
         *
         * This is a CONTROL, not the solution: it spends in SRAM exactly what we were
         * trying to save. */
        static uint8_t s_stage[UVM2_CMD_CAPACITY * 3u];
        const uint8_t  *src = uvm2_frame_buffer(served);
        const uint32_t  n   = uvm2_frame_length(served);
        for (uint32_t i = 0; i < n * 3u && i < UVM2_CMD_CAPACITY * 3u; i++) s_stage[i] = src[i];
        uint32_t cycles = uvm2_exec(s_stage, n);
#else
        {   /* The other half of the signature: what core 1 is ACTUALLY going to replay. */
            const uint8_t *b = uvm2_frame_buffer(served);
            uint32_t n = uvm2_frame_length(served), h = 2166136261u;
            for (uint32_t i = 0; i < n * 3u; i++) { h ^= b[i]; h *= 16777619u; }
            uvm2_sig_read = h;
            if (n == uvm2_sig_n[served & 1u]) {
                uvm2_sig_compared++;
                if (h != uvm2_sig_written[served & 1u]) uvm2_sig_failures++;
                else                                     uvm2_sig_last_ok = h;
            }
        }
        uint32_t cycles = uvm2_exec(uvm2_frame_buffer(served),
                                    uvm2_frame_length(served));
#endif
        uint32_t t1 = time_us_32();
        uvm2_stats.us_exec = t1 - t0;
        /* AND THE SAME NUMBER IN A FIELD THAT DOES NOT SHARE A WRITER. `us_exec` is also
         * written by uvm2_svc.c with something ELSE, so reading it does not say whose it
         * is. */
        uvm2_stats.us_c1_exec_acc += t1 - t0;
        uvm2_stats.us_c1_exec_n++;

        /* HOW LONG THE LIST TAKES AGAINST WHAT IT ASKS FOR, which is the one figure that
         * separates two opposite faults and which we did not have.
         *
         * `exec_cycles` was DECLARED AND NEVER WRITTEN — a dead counter that reads exactly
         * like "not needed". The executor does return the cycles; nobody was saving them.
         *
         * The list knows what it ought to cost (uvm2_list_cycles, counted while building
         * it) and the wall clock says what it did cost. At 1.5 MHz, 1000 cycles are 667 us:
         * if the ratio comes out ~1.0 the bus is running at its own pace and the problem is
         * the SIZE of the list; if it comes out 2.0 the list fits and something is SLOWING
         * the executor down — and the suspect is core 0 emulating the 6502 against the same
         * memory.
         *
         * And this is what the bisection asks for: the fixed-geometry player (no emulation)
         * does NOT flicker and the game DOES. The difference between the two is exactly
         * core 0's load. */
        uvm2_stats.exec_cycles = cycles;
        {
            extern uint32_t uvm2_frame_cycles(uint32_t);
            const uint32_t asked = uvm2_frame_cycles(served);   /* the one just replayed */
            /* IT IS NOT MEASURED AGAINST `t1 - t0`, AND THAT WAS THE TRAP.
             *
             * On the SIO path `uvm2_exec` waits edge by edge on CLK, so its duration IS
             * the bus time and the ratio meant what it promised. With PIO+DMA the same
             * code queues words, calls `vbus_flush` and returns: the PIO keeps playing
             * afterwards. Measuring against that gave **120%**, i.e. "the bus runs 20%
             * faster than its nominal rate" — impossible, because it is locked to the
             * console's CLK. What it was measuring was how fast WE QUEUE, which is exactly
             * what the DMA is for.
             *
             * Against the FRAME PERIOD it does mean something, and it is what you want to
             * know: 100 = the bus is busy for the whole frame (drawing is the limit),
             * 50 = for half the frame the bus is idle and the limit is somewhere else.
             * Believing that 120% cost a whole session: you do not publish a number whose
             * name does not match what was measured. */
            const uint32_t us = s_cycle_us;   /* the PREVIOUS frame's: see below */
            if (asked && us) {
                /* ratio in hundredths: 100 = the bus busy for the whole frame */
                uint32_t r = (uint32_t)(((uint64_t)asked * 100u * 100u) / ((uint64_t)us * 150u));
                uvm2_stats.exec_ratio_last = r;
                if (r > uvm2_stats.exec_ratio_max) uvm2_stats.exec_ratio_max = r;
                if (uvm2_stats.exec_ratio_min == 0u || r < uvm2_stats.exec_ratio_min)
                    uvm2_stats.exec_ratio_min = r;
                /* the whole shape: 100 = the bus busy for the whole frame */
                static const uint32_t T[7] = { 50u, 70u, 85u, 95u, 105u, 130u, 200u };
                unsigned b = 7u;
                for (unsigned i = 0; i < 7u; i++) if (r < T[i]) { b = i; break; }
                uvm2_stats.hist_ratio[b]++;
            }
        }

        /* Between frames, with the beam clamped at centre by the last command of
         * the stream — the only window in which anything else may drive Port A
         * or Port B. Same window the single-core path used, same order.
         */
        uvm2_single_cycles = 0;
#ifndef UVM2_NO_INPUT
        uvm2_cached_buttons = uvm2_read_buttons();
        uvm2_cached_axes    = uvm2_read_axes();
#endif
        uint32_t t2 = time_us_32();
        uvm2_stats.us_input = t2 - t1;
        psg_drain();
        uvm2_core1_gap();
#ifndef UVM2_NO_AUDIO
        /* Advance the sequencer by VECTREX TIME ELAPSED, not once per frame. That is what
         * the single-core path does, and for a measured reason: a frame that blows through
         * the 50 Hz budget is worth two frames of music, and dkong spends 59,000 cycles
         * against 30,000. One tick per frame plays the track at the average speed.
         *
         * This is NOT a diagnosis of "the music does not play in dual core", which is still
         * open: it is that the two paths had different semantics for the same thing, and
         * that has to be equalised before anything can be compared. */
        s_audio_acc += cycles;
        while (s_audio_acc >= UVM2_AUDIO_CYCLES) {
            s_audio_acc -= UVM2_AUDIO_CYCLES;
            uvm2_audio_tick();
        }
#endif
        /* Counted, not assumed: an axis conversion costs as many bus cycles as
         * its SAR needed, and the PSG queue's depth varies with the music. */
        cycles += uvm2_single_cycles;
        /* Redraw state that the input read and the PSG writes just clobbered:
         * one CD4052 serves the pots and the beam on shared select lines, so a
         * joystick conversion lands in the beam's own sample-and-holds. */
        uvm2_draw_invalidate();

        /* THE LOCK, AT RUN TIME. THIS is where it really rules: with dual core,
         * uvm2_frame_end returns before reaching its own pacer, so the one that counts is
         * this one. (I found out because the knob's symbol did not even reach the ELF:
         * nobody referenced the other one.)
         *
         * `cycles` ALWAYS into bus_cycles, no matter what. Previously, in the locked
         * branch, it did `bus_cycles = UVM2_CYCLES_PER_FRAME`: the counter was OVERWRITTEN
         * with the budget and read 30000 whether the drawing cost half that or twice. That
         * is why it did not move while sweeping VCAP, and why I published a "IT FITS" that
         * was false.
         *
         * 0 = free (Asteroids redrew as soon as it finished its list). != 0 = fixed period
         * in bus cycles; 30000 = 50 Hz. */
        uvm2_stats.bus_cycles = cycles;

        /* THE PACE IS SET AGAINST THE CLOCK, NOT AGAINST THE LIST.
         *
         * This used to wait `pacer - cycles` bus cycles, and that assumes that between one
         * frame's end and the next only the list has happened. It is false for two reasons
         * at once: between lists this core reads the controllers, drains the PSG and serves
         * the SD, and besides, `cycles` is what the list BELIEVES it costs, not how long
         * the bus takes. The single-core path already fixed this same thing in
         * uvm2_frame_end and its note says so: "list padded to exactly 30000 cycles and
         * frames from 20.5 to 26.5 ms".
         *
         * Now the target is "the previous frame ended at T, this one ends at T + period".
         * Whatever is left over is waited out with TIMER0; if we have already overshot,
         * that is a real overrun and it re-locks from now rather than dragging the lag
         * along.
         *
         * `uvm2_pacer_cycles` is still the knob (0 = free) and is converted to microseconds
         * here: the bus is 1.5 MHz, i.e. cycles * 2/3. */
        {
            static uint32_t s_end_us;
            const uint32_t now = time_us_32();
            if (s_end_us) s_cycle_us = now - s_end_us;   /* how long this pass lasted */
            if (uvm2_pacer_cycles == 0u) {
                s_end_us = now;
            } else {
                const uint32_t period_us = (uvm2_pacer_cycles * 2u) / 3u;
                const uint32_t target    = s_end_us + period_us;
                const int32_t  remaining = (int32_t)(target - now);
                if (remaining > 0 && (uint32_t)remaining <= period_us) {
                    while ((int32_t)(target - time_us_32()) > 0) { }
                    s_end_us = target;
                } else {
                    uvm2_stats.overrun++;
                    s_end_us = now;
                }
            }
        }

        uvm2_stats.us_rest = time_us_32() - t2;
        __asm volatile ("dmb" ::: "memory");       /* the work before the flag */
        uvm2_frame_done = served;
    }
}

/* STOPPING CORE 1, for what cannot be done with it running: writing to flash. Erasing or
 * programming flash hangs any core that executes or reads from XIP, and the list executor
 * lives here. `multicore_reset_core1` really does stop it; to come back, `uvm2_core1_start`,
 * which already begins by resetting it. */
/* OUR OWN CARTRIDGE'S BIOS runs this same loop ON CORE 0 (there the program runs on core 1
 * and core 0 replays; it is the UVM2 mirrored with the cores swapped). The loop and a
 * per-pass hook are exposed for whatever the BIOS does between lists besides the
 * controllers and the PSG: its music player and the SD work. */
__attribute__((weak)) void uvm2_core1_gap(void) { }
void uvm2_core1_loop(void) { core1_main(); }

void uvm2_core1_stop(void)
{
    multicore_reset_core1();
}

void uvm2_core1_start(void)
{
    /* RESET BEFORE LAUNCHING, and this is what makes loading over SWD possible.
     *
     * `multicore_launch_core1` assumes core 1 is stopped. On a cold boot it is; loading a
     * new image over SWD on top of one that was already running, it is NOT — core 1 is
     * still in the PREVIOUS image's loop, the FIFO handshake never arrives and the new
     * image hangs there forever. Found on 2026-08-24 with tools/probe.sh: pc in
     * multicore_fifo_rvalid, lr in multicore_launch_core1_raw. The symptom is "it does not
     * draw", i.e. indistinguishable from a bug in the program.
     *
     * I tried to solve it from outside by powering the core down through the PSM and hung
     * the whole console. From inside it is one line, the pico-sdk provides it, and on a cold
     * boot it does nothing. With this, tools/load.sh works for dual-core images too and the
     * test cycle goes from a minute (pull the SD, copy, menu) to a few seconds. */
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
}

#endif /* UVM2_DUAL_CORE */
