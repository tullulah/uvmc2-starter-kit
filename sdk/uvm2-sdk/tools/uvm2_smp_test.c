/* Host harness for injecting .vsmp samples into the command list.
 *
 * Checks over the packed list that every sample lands just before a T1CL (beam
 * parked), that Port A is restored, and — the thing that actually decides the
 * design — WHAT FRACTION OF THE FRAME it covers, because only the drawing has gaps
 * to inject into.
 *
 *   cc -O2 -DUVM2_HOST -DUVM2_BENCH_NO_CORE1 -DUVM2_SUBUNITS \
 *      -DUVM2_ZERO_SETTLE_E=40 -DUVM2_DAC_ZERO=0 -DUVM2_DRAW_SCALE=127 \
 *      -DUVM2_T1_TRANSPORT=110 -DUVM2_BLANK_SETTLE_E=10 -I. -o /tmp/smp \
 *      tools/uvm2_smp_test.c uvm2_draw.c uvm2_smp.c \
 *      ../vectrex-draw/cabi/target/release/libvectrex_draw_cabi.a
 *
 * WHAT IT MEASURED (2026-09-18, with the BIOS defines):
 *
 *     scene          refresh    frame covered
 *      40 strokes     50 Hz          5%
 *     127 strokes     50 Hz         14%
 *     300 strokes     50 Hz         38%
 *     127 strokes     free          91%
 *     300 strokes     free          91%
 *
 * And that is where this thing's one rule of use comes from: A GAME WITH SAMPLES
 * RUNS AT FREE REFRESH. With a fixed refresh, `frame_filler` spends the frame's
 * leftover on its zero-reference alternation — where the ramp is deliberately OPEN
 * and Port A is the discharge current, so nothing can be injected there — and what
 * is left of drawing is a fraction of the frame that also changes with the scene:
 * the audio would fade in and out with whatever is on screen. With free refresh
 * there is no leftover, the list is drawing end to end and the coverage is 91%
 * whatever the scene. The cartridge BIOS already compiles this way (build.rs:
 * UVM2_HZ=0); a .um2 image asks for it with `make uvm2 UVM2_HZ=0`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"
#include "uvm2_smp.h"

uvm2_stats_t uvm2_stats;
volatile int uvm2_have_calibration = 0;
int  uvm2_config_load(void) { return 0; }
void rust_eh_personality(void) { }
void uvm2_bus_delay(uint32_t c) { (void)c; }
void uvm2_via_write(uint32_t r, uint32_t d) { (void)r; (void)d; }

static const uint8_t *g_list; static uint32_t g_n;
uint32_t uvm2_exec(const uint8_t *c, uint32_t n) { g_list = c; g_n = n; return 0; }

#define SMP_N 40000
static uint8_t g_vsmp[8 + SMP_N / 2];

static void vsmp_ramp(uint32_t rate)
{
    g_vsmp[0] = rate & 0xFF; g_vsmp[1] = (rate >> 8) & 0xFF;
    g_vsmp[2] = (rate >> 16) & 0xFF; g_vsmp[3] = rate >> 24;
    g_vsmp[4] = SMP_N & 0xFF; g_vsmp[5] = (SMP_N >> 8) & 0xFF;
    g_vsmp[6] = (SMP_N >> 16) & 0xFF; g_vsmp[7] = 0;
    for (int i = 0; i < SMP_N; i++) {
        uint8_t v = (uint8_t)(i % 16);
        if (i % 2 == 0) g_vsmp[8 + i / 2] = v;
        else            g_vsmp[8 + i / 2] |= (uint8_t)(v << 4);
    }
}

struct cmd { uint32_t reg, data, gap, cycle; };
static struct cmd g_cmd[400000];
static uint32_t g_ncmd;

static void decodifica(void)
{
    uint32_t cycle = 0;
    g_ncmd = 0;
    for (uint32_t i = 0; i < g_n && g_ncmd < 400000; i++) {
        uint32_t v = (uint32_t)g_list[i*3] | ((uint32_t)g_list[i*3+1] << 8)
                   | ((uint32_t)g_list[i*3+2] << 16);
        g_cmd[g_ncmd].reg   = (v >> 8) & 0xF;
        g_cmd[g_ncmd].data  = v & 0xFF;
        g_cmd[g_ncmd].gap = UVM2_CMD_DELAY(v);
        g_cmd[g_ncmd].cycle = cycle;
        cycle += 1 + g_cmd[g_ncmd].gap;
        g_ncmd++;
    }
}

/* `n` chained strokes of `len` units, like a game's scenery. */
static void scene(int n, int len)
{
    uvm2_frame_begin();
    uvm2_draw_intensity(0x5F);
    uvm2_draw_move(-60, -40);
    for (int i = 0; i < n; i++) uvm2_draw_delta(len, (i & 1) ? len : -len);
    uvm2_frame_end();
    decodifica();
}

static uint32_t list_cycles(void)
{
    return g_ncmd ? g_cmd[g_ncmd-1].cycle + 1 + g_cmd[g_ncmd-1].gap : 0;
}

/* Where the drawing stops: the ACR=0x18 with which `frame_filler` opens its
 * zero-reference alternation. It is the LAST ACR in the list — via_setup writes
 * another one in the prologue, and looking for the first gave zero (the bug this
 * had when it was written). With no fixed rhythm there is none and the drawing is
 * the whole list. */
static uint32_t draw_cycles(void)
{
    uint32_t corte = 0;
    for (uint32_t i = 1; i < g_ncmd; i++)
        if (g_cmd[i].reg == UVM2_VIA_ACR && g_cmd[i].data == 0x18u) corte = g_cmd[i].cycle;
    return corte ? corte : list_cycles();
}

/* Count the SAMPLE data writes, which means tracking which PSG register is
 * latched: the mixer write the emitter does once per list ends in exactly the same
 * BDIR sequence as a sample, and counting those too made this report a period of 6
 * cycles and a bogus placement failure. A `PORT_A = r` followed by `PORT_B` with
 * BDIR|BC1 latches register r; only data writes while UVM2_SMP_REG is latched are
 * samples. */
static uint32_t count_samples(uint32_t *hmin, uint32_t *hmax, uint32_t *hmed,
                              uint32_t *bad_place, uint32_t *bad_restore)
{
    uint32_t n = 0, prev = 0, sum = 0;
    int latched = -1;
    *hmin = 0xFFFFFFFFu; *hmax = 0; *bad_place = 0; *bad_restore = 0;
    for (uint32_t i = 0; i < g_ncmd; i++) {
        if (g_cmd[i].reg != UVM2_VIA_PORTB) continue;

        if ((g_cmd[i].data & 0x18u) == 0x18u) {          /* address latch */
            if (i >= 1 && g_cmd[i-1].reg == UVM2_VIA_PORTA) latched = (int)g_cmd[i-1].data;
            continue;
        }
        if ((g_cmd[i].data & 0x18u) != 0x10u) continue;   /* not a data write */
        if (latched != (int)UVM2_SMP_REG) continue;       /* the mixer, not a sample */
        n++;

        /* TWO VALID SHAPES, because there are two places a sample can go in.
         *
         * Inside a stroke, just before T1CL, the timer has already expired and PB7 is
         * high: data, BDIR down, restore Port A, T1CL.
         *
         * In the inter-frame rhythm the ramp is OPEN (ACR=0x18 takes PB7 off T1), so
         * the emitter freezes it first and lets it go afterwards: data, BDIR down,
         * Port B back, restore Port A.
         *
         * What has to hold in BOTH is the invariant, not the exact sequence: BDIR goes
         * low on the next command, and within a few more the DAC gets a Port A value
         * that was already on the bus before the sample. Checking the literal shape
         * instead reported a failure for placements that are correct. */
        if (!(i + 1 < g_ncmd && g_cmd[i+1].reg == UVM2_VIA_PORTB)) (*bad_place)++;
        else {
            int restored = 0;
            for (uint32_t k = i + 2; k < i + 5 && k < g_ncmd; k++)
                if (g_cmd[k].reg == UVM2_VIA_PORTA) { restored = 1; break; }
            if (!restored) (*bad_place)++;
        }
        /* the restored Port A has to be one that was already on the bus BEFORE the
         * sample, not the sample's own value */
        if (i + 2 < g_ncmd && g_cmd[i+2].reg == UVM2_VIA_PORTA) {
            int seen = 0;
            for (uint32_t k = i; k-- > 0; )
                if (g_cmd[k].reg == UVM2_VIA_PORTA && g_cmd[k].data == g_cmd[i+2].data) { seen = 1; break; }
            if (!seen) (*bad_restore)++;
        }
        if (n > 1) {
            uint32_t d = g_cmd[i].cycle - prev;
            if (d < *hmin) *hmin = d;
            if (d > *hmax) *hmax = d;
            sum += d;
        }
        prev = g_cmd[i].cycle;
    }
    *hmed = n > 1 ? sum / (n - 1) : 0;
    return n;
}

static int probe(const char *title, int n, int len, unsigned hz)
{
    uint32_t hmin, hmax, hmed, ms, mr, n_smp;
    uint32_t list, draw, cmds_mute, cmds_sound;

    uvm2_set_refresh(hz);
    uvm2_smp_stop(UVM2_SMP_ALL);
    scene(n, len);                       /* control, with no audio */
    cmds_mute = g_ncmd;

    vsmp_ramp(UVM2_SMP_HZ);
    uvm2_smp_play(g_vsmp, 0, 1);
    scene(n, len);
    cmds_sound = g_ncmd;
    list  = list_cycles();
    draw = draw_cycles();
    n_smp  = count_samples(&hmin, &hmax, &hmed, &ms, &mr);

    printf("%-28s strokes %3d x %2d u,  refresh %u Hz\n", title, n, len, hz);
    printf("   list %6u cycles   drawing %6u (%2.0f%%)   commands %5u (+%d)\n",
           list, draw, list ? 100.0 * draw / list : 0.0,
           cmds_sound, (int)cmds_sound - (int)cmds_mute);
    printf("   samples %4u   expected(drawing) %4u   expected(frame) %4u\n",
           n_smp, draw / (UVM2_BUS_HZ / UVM2_SMP_HZ), list / (UVM2_BUS_HZ / UVM2_SMP_HZ));
    printf("   period  mean %4u  min %4u  max %5u   (asked %u)\n",
           hmed, hmin == 0xFFFFFFFFu ? 0 : hmin, hmax, UVM2_BUS_HZ / UVM2_SMP_HZ);
    printf("   place %s   restore %s   frame covered %2.0f%%\n\n",
           ms ? "FAIL" : "ok", mr ? "FAIL" : "ok",
           list && n_smp ? 100.0 * (n_smp * (UVM2_BUS_HZ / UVM2_SMP_HZ)) / list : 0.0);
    return (ms || mr) ? 1 : 0;
}

/* THE PITCH, WHICH IS THE THING THAT CAN FAIL SILENTLY.
 *
 * The injection opportunities fall unevenly (every ~32 cycles, and only the first
 * one past the period is used), so the OUTPUT rate is not the one asked for: 196
 * cycles are measured where 187 were requested. That does not matter AS LONG AS the
 * cursor advances by elapsed cycles and not one sample per emission — if it went
 * one at a time the sample would sound 5% flat and nobody would notice by reading
 * the list.
 *
 * Measured black-box: a non-looping sample of `n` frames has to last exactly
 * `n * 1.5 MHz / rate` bus cycles, whatever the scene. */
static int pitch_test(unsigned rate_file)
{
    const uint32_t expected = (uint32_t)(((uint64_t)SMP_N * UVM2_BUS_HZ) / rate_file);
    uint32_t elapsed = 0;
    int frames = 0;

    uvm2_set_refresh(0);
    uvm2_smp_stop(UVM2_SMP_ALL);
    vsmp_ramp(rate_file);
    uvm2_smp_play(g_vsmp, 0, 0);          /* no looping: it has to end by itself */

    while (uvm2_smp_playing(0) && frames < 20000) {
        scene(127, 8);
        elapsed += list_cycles();
        frames++;
    }

    const double error = 100.0 * ((double)elapsed - expected) / expected;
    printf("pitch: %u Hz file, %d samples -> %u cycles (expected %u), error %+.2f%%\n",
           rate_file, SMP_N, elapsed, expected, error);
    printf("      %s\n\n", (error > 1.0 || error < -1.0) ? "FAIL: out of tune"
                                                         : "ok (within 1%)");
    return (error > 1.0 || error < -1.0) ? 1 : 0;
}

int main(void)
{
    int bad = 0;
    uvm2_draw_init();

    /* Scenes from small to full, and both refresh policies. The question is not
     * whether the injection works — `place` says that — but WHAT FRACTION OF THE
     * FRAME it covers. */
    bad |= probe("small scene, 50 Hz",   40,  3, 50);
    bad |= probe("medium scene, 50 Hz",    127,  8, 50);
    bad |= probe("full scene, 50 Hz",    300, 12, 50);
    bad |= probe("medium scene, FREE",    127,  8,  0);
    bad |= probe("full scene, FREE",    300, 12,  0);

    /* Two file rates either side of the output rate: the resampling has to give
     * the same pitch for both. */
    bad |= pitch_test(6000);
    bad |= pitch_test(11025);
    bad |= pitch_test(12000);   /* what the bundle now ships at */
    printf("%s\n", bad ? "THERE ARE FAILURES" : "every placement check ok");
    return bad;
}
