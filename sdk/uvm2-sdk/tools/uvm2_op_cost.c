/* Host harness: WHAT ONE OPERATION COSTS, broken down. Links the REAL uvm2_draw.c against
 * a mocked bus and counts, per primitive, how many commands go out and how many bus cycles
 * they consume — the anatomy of the ~39 cycles of overhead that ride on every vector.
 *
 *   cc -O2 -DUVM2_HOST -I<sdk> -o costeop tools/uvm2_op_cost.c uvm2_draw.c \
 *      <vectrex-draw>/cabi/target/release/libvectrex_draw_cabi.a eh.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"
const uint8_t *uvm2_frame_buffer(uint32_t);

uvm2_stats_t uvm2_stats;
static uint32_t g_cyc, g_cmd, g_delay;

uint32_t uvm2_exec(const uint8_t *c, uint32_t n) { (void)c; g_cmd += n; g_cyc += n; return n; }
void     uvm2_bus_delay(uint32_t c)              { g_cyc += c; g_delay += c; }
void     uvm2_via_write(uint32_t r, uint32_t d)  { (void)r; (void)d; g_cmd++; g_cyc++; }
uint8_t  uvm2_via_read(uint32_t r)               { (void)r; return 0; }

static void zero(void) { g_cyc = g_cmd = g_delay = 0; }
static void di(const char *q) {
    printf("  %-34s %4u commands  %5u cycles  (%u of delay)\n", q, g_cmd, g_cyc, g_delay);
}

static uint32_t base_cmd, base_cyc;

/* Every primitive QUEUES; the list runs when the frame closes. So what is measured is
 * (frame_begin + primitive + frame_end) minus (frame_begin + frame_end). */
static void measure(const char *q, void (*f)(void))
{
    zero(); uvm2_frame_begin(); if (f) f(); uvm2_frame_end();
    if (!f) { base_cmd = g_cmd; base_cyc = g_cyc; printf("  %-30s %4u commands  %5u cycles   <- baseline\n", q, g_cmd, g_cyc); return; }
    printf("  %-30s %4d commands  %5d cycles\n", q, (int)g_cmd-(int)base_cmd, (int)g_cyc-(int)base_cyc);
}
static void p_reset(void){ uvm2_draw_reset(); }
static void p_int(void)  { uvm2_draw_intensity(0x5F); }
static void p_mov(void)  { uvm2_draw_move(40,30); }
static void p_h(void)    { uvm2_draw_delta(60,0); }
static void p_v(void)    { uvm2_draw_delta(0,60); }
static void p_d(void)    { uvm2_draw_delta(60,45); }
static void p_c(void)    { uvm2_draw_delta(4,3); }
static void p_2(void)    { uvm2_draw_delta(60,45); uvm2_draw_delta(60,45); }
static void p_mov2(void) { uvm2_draw_move(40,30); uvm2_draw_move(40,30); }

static const char *REG[16] = {"ORB","ORA","DDRB","DDRA","T1CL","T1CH","T1LL","T1LH",
                              "T2CL","T2CH","SR","ACR","PCR","IFR","IER","ORAnh"};
/* Dump the list as it stands: register, data and delay of every command. */
static void dump(const char *q, void (*f)(void))
{
    zero(); uvm2_frame_begin();
    const uint8_t *b = uvm2_frame_buffer(0);
    uint32_t before = uvm2_stats.commands;
    f(); uvm2_frame_end();
    uint32_t n = uvm2_stats.commands;
    printf("\n== %s : %u commands ==\n", q, n);
    for (uint32_t i = 0; i < n && i < 60; i++){
        uint32_t v = (uint32_t)b[i*3] | ((uint32_t)b[i*3+1] << 8) | ((uint32_t)b[i*3+2] << 16);
        printf("   %2u  %-5s data %3u  delay %4u\n",
               i, REG[(v >> 8) & 0xF], (v >> 0) & 0xFF, v >> 12);
    }
    (void)before;
}
static void nothing(void){}

int main(void)
{
    uvm2_draw_init();
    measure("nothing", 0);
    measure("reset0ref (re-zero)", p_reset);
    measure("intensity", p_int);
    measure("move 40,30", p_mov);
    measure("move 40,30 x2", p_mov2);
    measure("draw 60,0  horizontal", p_h);
    measure("draw 0,60  vertical", p_v);
    measure("draw 60,45 diagonal", p_d);
    measure("draw 4,3   short", p_c);
    measure("draw 60,45 x2 chained", p_2);
    dump("a frame with ONE single draw", p_d);
    dump("an empty frame", nothing);
    return 0;
}
