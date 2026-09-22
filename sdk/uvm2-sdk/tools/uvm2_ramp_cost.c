/* WHAT IT COSTS TO BUILD ONE RAMP, on the host, from a real game's vectors.
 *
 * On the console (starwars, 2026-09-19) the three calls a port makes per segment cost
 * ~21 us: draw_intensity ~0.4, draw_move_abs_q4 ~9.4, draw_delta_q4 ~10.8. That is
 * ~1500 M33 cycles to BUILD a ramp, in SRAM, with the cache hitting 99.9% -- so it is
 * arithmetic, and arithmetic can be measured here instead of over SWD.
 *
 * The input is a frame DUMPED FROM THE GAME (aae_starwars/tools/host_test.c with
 * STARWARS_DUMP=<file>), not synthetic vectors: the builder's cost depends on the
 * length of each jump and stroke -- whether it splits, whether it re-zeros -- so
 * made-up geometry would measure the geometry and not the game.
 *
 *   cc -O2 -DUVM2_HOST -DUVM2_BENCH_NO_CORE1 <the cart's defines> -I<sdk> \
 *      -o rampcost tools/uvm2_ramp_cost.c uvm2_draw.c \
 *      <vectrex-draw>/cabi/target/release/libvectrex_draw_cabi.a stubs.c
 *   ./rampcost sw_frame.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"

uvm2_stats_t uvm2_stats;

/* ── the bus, mocked. Commands are counted, never driven. The list itself is kept so
 * it can be decoded afterwards the way uvm2_anatomy.c does: 3 bytes per command,
 * 12 bits of delay, 4 of register, 8 of data. The delay is where most of the bus time
 * is, so a count of commands alone would describe nothing. ── */
static uint32_t g_cmd, g_cyc;
static const uint8_t *g_list; static uint32_t g_n;
uint32_t uvm2_exec(const uint8_t *c, uint32_t n) { g_list = c; g_n = n; g_cmd += n; return n; }
void     uvm2_bus_delay(uint32_t c)              { g_cyc += c; }
void     uvm2_via_write(uint32_t r, uint32_t d)  { (void)r; (void)d; g_cmd++; }
uint8_t  uvm2_via_read(uint32_t r)               { (void)r; return 0; }

/* The SDK's own conversion, copied from sdk_rp2350.c so the bench sees exactly the
 * integers the BIOS sees on the cart. */
#define VPY_SCALE 127
#define VS_Q4(v) ((int)(((v) < 0 ? (long)(v) * 16 - VPY_SCALE / 2 \
                                 : (long)(v) * 16 + VPY_SCALE / 2) / VPY_SCALE))

struct seg { int32_t x0, y0, x1, y1, z; };

static const char *name(uint32_t reg)
{
    switch (reg){
    case UVM2_VIA_PORTA: return "PORT_A  (DAC: X or Y rate)";
    case UVM2_VIA_PORTB: return "PORT_B  (mux: sample Y)";
    case UVM2_VIA_T1CL:  return "T1CL    (ramp scale)";
    case UVM2_VIA_T1CH:  return "T1CH    (START the ramp)";
    case UVM2_VIA_T1LL:  return "T1LL    (WAIT for it to finish)";
    case UVM2_VIA_PCR:   return "CNTL    (beam on/off)";
    case UVM2_VIA_T1LH:  return "T1LH";
    case UVM2_VIA_SR:    return "SR      (beam blank/unblank)";
    case UVM2_VIA_ACR:   return "ACR";
    case UVM2_VIA_IFR:   return "IFR";
    case UVM2_VIA_IER:   return "IER";
    case 0x2:            return "DDRB";
    case 0x3:            return "DDRA";
    case 0x8:            return "T2CL";
    case 0x9:            return "T2CH";
    default:             return "other";
    }
}

static void anatomia(void)
{
    uint32_t esc[16] = {0}, ret[16] = {0}, cnt[16] = {0}, total = 0, i;
    if (!g_list || !g_n) return;
    for (i = 0; i < g_n; i++){
        uint32_t v = (uint32_t)g_list[i*3] | ((uint32_t)g_list[i*3+1] << 8)
                   | ((uint32_t)g_list[i*3+2] << 16);
        uint32_t reg = (v >> 8) & 0xF, d = UVM2_CMD_DELAY(v);
        esc[reg] += 1; ret[reg] += d; cnt[reg]++;
        total += 1 + d;
    }
    printf("\n  where the %u bus cycles go:\n", total);
    printf("  %-34s  cmds  writes   waits   total   %%\n", "stage");
    for (i = 0; i < 16; i++){
        uint32_t t;
        if (!cnt[i]) continue;
        t = esc[i] + ret[i];
        printf("  %-34s %5u %7u %7u %7u  %4.1f%%\n",
               name(i), cnt[i], esc[i], ret[i], t, 100.0*t/total);
    }
    {
        uint32_t we = 0, wr = 0;
        for (i = 0; i < 16; i++){ we += esc[i]; wr += ret[i]; }
        printf("  WRITES %u cycles (%.1f%%)   WAITS %u cycles (%.1f%%)\n",
               we, 100.0*we/total, wr, 100.0*wr/total);
    }
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "sw_frame.bin";
    FILE *f = fopen(path, "rb");
    struct seg *s;
    long n, i, rep, REPS = 200;
    double t_i = 0, t_m = 0, t_d = 0, t0;
    long chained = 0;

    if (!f) { fprintf(stderr, "no dump at %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f) / (long)sizeof(struct seg); fseek(f, 0, SEEK_SET);
    s = malloc((size_t)n * sizeof *s);
    if (!s || fread(s, sizeof *s, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    for (i = 1; i < n; i++)
        if (s[i].x0 == s[i-1].x1 && s[i].y0 == s[i-1].y1) chained++;
    printf("%ld segments, %ld of them starting where the previous ended (%.1f%%)\n",
           n, chained, 100.0 * chained / n);

    uvm2_draw_init();

    /* One frame first, to report the shape of the list the game produces -- and WHICH
     * call emits what. The three are charged separately because they are not the same
     * animal: the builder already drops a zero-length move, so `moves` below is well
     * under one per segment, and the average cost of the move call is diluted by the
     * ones that did nothing. */
    {
        uint32_t c_i = 0, c_m = 0, c_d = 0, before;
        uint32_t y_i = 0, y_m = 0, y_d = 0, cbefore;   /* the same split, in bus cycles */
        long moved = 0;
        g_cmd = 0;
        uvm2_frame_begin();
        for (i = 0; i < n; i++) {
            int ax = VS_Q4(s[i].x0), ay = VS_Q4(s[i].y0);
            int dx = VS_Q4(s[i].x1) - ax, dy = VS_Q4(s[i].y1) - ay;
            before = uvm2_list_commands(); cbefore = uvm2_list_cycles();
            uvm2_draw_intensity((int)s[i].z);
            c_i += uvm2_list_commands() - before; y_i += uvm2_list_cycles() - cbefore;
            before = uvm2_list_commands(); cbefore = uvm2_list_cycles();
            uvm2_draw_move_abs_q4(ax, ay);
            if (uvm2_list_commands() != before) moved++;
            c_m += uvm2_list_commands() - before; y_m += uvm2_list_cycles() - cbefore;
            before = uvm2_list_commands(); cbefore = uvm2_list_cycles();
            uvm2_draw_delta_q4(dx, dy);
            c_d += uvm2_list_commands() - before; y_d += uvm2_list_cycles() - cbefore;
        }
        /* BEFORE frame_end, because that is what publishes and resets the list. This is
         * the OTHER half of the frame: what core 0 will spend on the bus replaying it. */
        uint32_t cycles = uvm2_list_cycles();
        uvm2_frame_end();
        printf("list costs %u bus cycles to replay -> %.1f ms at 1.5 MHz"
               "  (%.0f%% of a 50 Hz frame's 30000)\n",
               cycles, cycles / 1500.0, 100.0 * cycles / 30000.0);
        printf("one frame -> %u commands, %.1f per segment"
               "   (capacity %u, %.0f%% full, dropped %u)\n",
               g_cmd, (double)g_cmd / n, (unsigned)UVM2_CMD_CAPACITY,
               100.0 * g_cmd / (double)UVM2_CMD_CAPACITY, uvm2_stats.dropped);
        printf("  by call:  intensity %u   move %u (%ld of %ld segments actually moved)"
               "   stroke %u   frame begin/end %u\n",
               c_i, c_m, moved, n, c_d, g_cmd - c_i - c_m - c_d);
        printf("  stats: %u vectors, %u moves, %u recals, %u ramp cycles\n",
               uvm2_stats.vectors, uvm2_stats.moves, uvm2_stats.recals,
               uvm2_stats.ramp_cycles);
        anatomia();
        /* THE CEILING, as a formula instead of a remembered number. The Vectrex bus runs
         * at ~1.5 MHz (it is the 6809's E clock and cannot be raised), so a 50 Hz frame
         * is 30000 cycles and that is the whole budget. */
        printf("\n  bus cycles by call:  one jump %.1f   one stroke %.1f"
               "   (intensity %.2f)\n",
               (double)y_m / (moved ? moved : 1), (double)y_d / n, (double)y_i / n);
        printf("  => at 50 Hz (30000 cycles) this shape of list affords about %.0f"
               " segments/frame;\n     this frame wants %ld, so it can only run at"
               " %.1f Hz\n",
               30000.0 / ((double)(y_m + y_d + y_i) / n), n,
               1500000.0 / (double)(y_m + y_d + y_i));
    }

    /* Then the timing, over many frames so the per-call figure is stable. */
    for (rep = 0; rep < REPS; rep++) {
        uvm2_frame_begin();
        for (i = 0; i < n; i++) {
            int ax = VS_Q4(s[i].x0), ay = VS_Q4(s[i].y0);
            int dx = VS_Q4(s[i].x1) - ax, dy = VS_Q4(s[i].y1) - ay;
            t0 = now_ns(); uvm2_draw_intensity((int)s[i].z);  t_i += now_ns() - t0;
            t0 = now_ns(); uvm2_draw_move_abs_q4(ax, ay);     t_m += now_ns() - t0;
            t0 = now_ns(); uvm2_draw_delta_q4(dx, dy);        t_d += now_ns() - t0;
        }
        uvm2_frame_end();
    }
    {
        double calls = (double)n * REPS;
        printf("\nper call on this host (clock reads included, so an upper bound):\n");
        printf("  draw_intensity    %7.1f ns\n", t_i / calls);
        printf("  draw_move_abs_q4  %7.1f ns\n", t_m / calls);
        printf("  draw_delta_q4     %7.1f ns\n", t_d / calls);
        printf("  ratio move:stroke  %.2f   (the console says 9.4 : 10.8 = 0.87)\n",
               t_m / t_d);
    }
    return 0;
}
