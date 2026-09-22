/* Host harness: what the GAPPED RAMP costs, counted through the REAL uvm2_draw.c.
 *
 * The point is to get the number without hardware. uvm2_draw.c maintains
 * uvm2_stats.ramp_cycles itself — cycles with the integrators running — so that figure is
 * the SDK's own, not this harness's; the bus is mocked only to count commands and delays.
 *
 *   cc -O2 -DUVM2_HOST -I<sdk> -o gapbudget tools/uvm2_gapped_budget.c uvm2_draw.c \
 *      <vectrex-draw>/cabi/target/release/libvectrex_draw_cabi.a
 *   ./gapbudget frame.ops
 *
 * The .ops file is a frame already merged into runs by flush_frame's rules — see
 * dkong_sbt/tools/dk_ops.py, which produces it twice, once per build:
 *
 *   Z              reset the zero reference
 *   I <b>          intensity
 *   M <dx> <dy>    blanked move
 *   D <dx> <dy>    lit draw
 *   G <dx> <dy> <n> <s0> <e0> ...   one ramp, n dark stretches as 0..255 fractions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"

uvm2_stats_t uvm2_stats;
static uint32_t g_cycles, g_commands;

uint32_t uvm2_exec(const uint8_t *cmds, uint32_t count)
{
    (void)cmds; g_commands += count; g_cycles += count; return count;
}
void    uvm2_bus_delay(uint32_t c)                  { g_cycles += c; }
void    uvm2_via_write(uint32_t r, uint32_t d)      { (void)r; (void)d; g_cycles += 1; }
uint8_t uvm2_via_read(uint32_t r)                   { (void)r; g_cycles += 1; return 0; }

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "uso: %s <frame.ops>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }

    uvm2_draw_init();
    memset(&uvm2_stats, 0, sizeof uvm2_stats);
    g_cycles = 0; g_commands = 0;
    uvm2_frame_begin();

    char line[4096];
    unsigned long n_draw = 0, n_gap = 0, n_move = 0;
    while (fgets(line, sizeof line, f)) {
        int a, b, n;
        if (line[0] == 'Z') { uvm2_draw_reset(); }
        else if (sscanf(line, "I %d", &a) == 1)            uvm2_draw_intensity(a);
        else if (sscanf(line, "M %d %d", &a, &b) == 2)   { uvm2_draw_move(a, b);  n_move++; }
        else if (sscanf(line, "D %d %d", &a, &b) == 2)   { uvm2_draw_delta(a, b); n_draw++; }
        else if (line[0] == 'G') {
            unsigned char g[32]; int got = 0;
            char *p = line + 1;
            long v[3 + 32]; int nv = 0;
            while (nv < 3 + 32) {
                char *q;
                long x = strtol(p, &q, 10);
                if (q == p) break;
                v[nv++] = x; p = q;
            }
            if (nv < 3) continue;
            a = (int)v[0]; b = (int)v[1]; n = (int)v[2];
            for (int k = 0; k < n * 2 && 3 + k < nv && got < 32; k++)
                g[got++] = (unsigned char)v[3 + k];
            uvm2_draw_delta_patterned(a, b, g, got / 2);
            n_gap++;
        }
    }
    uvm2_frame_end();
    fclose(f);

    printf("%-22s %5lu draw %4lu gapped %5lu move | %6u cmd %8u bus cyc | "
           "RAMP %7u cyc = %6.2f ms\n",
           argv[1], n_draw, n_gap, n_move, g_commands, g_cycles,
           uvm2_stats.ramp_cycles, uvm2_stats.ramp_cycles * 0.667 / 1000.0);
    return 0;
}
