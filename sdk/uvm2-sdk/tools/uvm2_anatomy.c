/* THE ANATOMY OF ONE OPERATION, decoding the REAL command list.
 *
 * The earlier harness (uvm2_op_cost.c) counted `g_cyc += n`: ONE cycle per command, and
 * threw the delay field away. But the delay is exactly where the time is — the executor
 * spends 1 + delay per command. Measured that way, the 85 cycles per operation seen on the
 * console cannot be explained, and an anatomy whose total does not add up to what the
 * hardware measures describes nothing.
 *
 * Here a chain of vectors is emitted with the REAL emitter and the REAL timings, the packed
 * list is decoded (3 bytes: delay 12, register 4, data 8) and every cycle is attributed to
 * the stage that spends it.
 *
 *   cc -O2 -DUVM2_HOST -I. -o /tmp/anat tools/uvm2_anatomy.c uvm2_draw.c \
 *      <vectrex-draw>/cabi/target/release/libvectrex_draw_cabi.a
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"

uvm2_stats_t uvm2_stats;
const uint8_t *uvm2_frame_buffer(uint32_t);
uint32_t uvm2_exec(const uint8_t *c, uint32_t n);
void     uvm2_bus_delay(uint32_t c) { (void)c; }
void     uvm2_via_write(uint32_t r, uint32_t d) { (void)r; (void)d; }

/* the list the emitter has just built, exactly as the executor would read it */
static const uint8_t *g_list; static uint32_t g_n;
uint32_t uvm2_exec(const uint8_t *c, uint32_t n){ g_list = c; g_n = n; return 0; }

static const char *name(uint32_t reg)
{
    switch (reg){
    case UVM2_VIA_PORTA: return "PORT_A  (DAC: X or Y rate)";
    case UVM2_VIA_PORTB: return "PORT_B  (mux: sample Y)";
    case UVM2_VIA_T1CL:  return "T1CL    (the ramp's scale)";
    case UVM2_VIA_T1CH:  return "T1CH    (STARTS the ramp)";
    case UVM2_VIA_T1LL:  return "T1LL    (WAITS for it to finish)";
    case UVM2_VIA_PCR:   return "CNTL    (unblanks/blanks the beam)";
    default:             return "other";
    }
}

int main(void)
{
    uvm2_draw_init();
    uvm2_frame_begin();
    /* a typical chain: short strokes linked end to end, like a ladder or the scenery. The
     * length matters: t1 grows with it and the ramp is the term that SCALES, everything
     * else is fixed. */
    const int N = 32, L = 8;
    uvm2_draw_move(0, 0);
    for (int i = 0; i < N; i++) uvm2_draw_delta(L, (i & 1) ? L : -L);
    uvm2_frame_end();

    uint32_t wrt[16] = {0}, wait[16] = {0}, cnt[16] = {0};
    uint32_t total = 0;
    for (uint32_t i = 0; i < g_n; i++){
        uint32_t v = (uint32_t)g_list[i*3] | ((uint32_t)g_list[i*3+1] << 8)
                   | ((uint32_t)g_list[i*3+2] << 16);
        uint32_t reg = (v >> 8) & 0xF, d = UVM2_CMD_DELAY(v);
        wrt[reg] += 1; wait[reg] += d; cnt[reg]++;
        total += 1 + d;
    }
    printf("  a chain of %d vectors of %d units, %u commands, %u bus cycles\n",
           N, L, g_n, total);
    printf("  -> %.1f cycles per operation\n\n", (double)total / N);
    printf("  %-34s  cmds     write    wait   total   %%\n", "stage");
    for (int r = 0; r < 16; r++){
        if (!cnt[r]) continue;
        uint32_t t = wrt[r] + wait[r];
        printf("  %-34s %5u %10u %7u %7u  %4.1f%%\n",
               name((uint32_t)r), cnt[r], wrt[r], wait[r], t, 100.0*t/total);
    }
    uint32_t we = 0, wr = 0;
    for (int r = 0; r < 16; r++){ we += wrt[r]; wr += wait[r]; }
    /* THE SEQUENCE, command by command. A table of totals says HOW MUCH; this says WHAT,
     * and it is the only thing that shows a delay hanging off the wrong register. */
    printf("\n  commands 40..61: two chained vectors half way along\n");
    for (uint32_t i = 40; i < g_n && i < 62; i++){
        uint32_t v = (uint32_t)g_list[i*3] | ((uint32_t)g_list[i*3+1] << 8)
                   | ((uint32_t)g_list[i*3+2] << 16);
        printf("    %2u  %-34s data %3u   wait %u\n",
               i, name((v >> 8) & 0xF), v & 0xFF, UVM2_CMD_DELAY(v));
    }
    printf("\n  WRITES %u cycles (%.1f%%)   WAITS %u cycles (%.1f%%)\n",
           we, 100.0*we/total, wr, 100.0*wr/total);
    return 0;
}
