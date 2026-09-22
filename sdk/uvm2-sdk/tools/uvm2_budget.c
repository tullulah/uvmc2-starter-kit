/* Host harness: links the REAL uvm2_draw.c against a mocked bus so the command
 * stream can be counted exactly, without hardware. */
#include <stdio.h>
#include <stdint.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"

uvm2_stats_t uvm2_stats;

static uint32_t g_cycles;
static uint32_t g_commands;

uint32_t uvm2_exec(const uint32_t *cmds, uint32_t count)
{
    uint32_t cycles = 0;
    for (uint32_t i = 0; i < count; i++) cycles += 1 + (cmds[i] >> 20);
    g_cycles   += cycles;
    g_commands += count;
    return cycles;
}
void uvm2_bus_delay(uint32_t c) { (void)c; }
void uvm2_via_write(uint32_t r, uint32_t d) { (void)r; (void)d; }
uint8_t uvm2_via_read(uint32_t r) { (void)r; return 0; }

/* One VPy DRAW_LINE, exactly as vpy_draw_line issues it: re-zero, set the
 * intensity, blanked move to the start, then the lit segment. */
static void vpy_line(int x0, int y0, int x1, int y1, int b)
{
    uvm2_draw_reset();
    uvm2_draw_intensity(b);
    uvm2_draw_move(x0, y0);
    uvm2_draw_delta(x1 - x0, y1 - y0);
}

static void measure(const char *label, uint32_t scale, int fixup, int len)
{
    (void)fixup;   /* the fixup was retired from the SDK: nobody called it */
    g_cycles = 0; g_commands = 0;
    uvm2_draw_set_scale(scale);
    uvm2_draw_init();

    g_cycles = 0; g_commands = 0;
    uvm2_frame_begin();
    /* 20 segments of the requested length, scattered so nothing is cached away */
    for (int i = 0; i < 20; i++) {
        int x = -100 + (i * 7) % 180;
        int y =  -80 + (i * 13) % 150;
        vpy_line(x, y, x + len, y + len / 2, 0x5F);
    }
    uvm2_frame_end();

    double per_vec = (double)g_cycles / 20.0;
    printf("  %-28s %6u cmd %7u cyc  %6.1f cyc/vec  →  %4.0f vec/frame\n",
           label, g_commands, g_cycles, per_vec, 30000.0 / per_vec);
}

/* How CrazyStones draws a shape: centre once, position once, then chain the
 * segments. No re-zero, no re-move, no intensity change per segment. */
static void measure_chained(const char *label, uint32_t scale, int fixup, int len)
{
    (void)fixup;   /* idem */
    const int SEGS = 20;

    uvm2_draw_set_scale(scale);
    uvm2_draw_init();

    g_cycles = 0; g_commands = 0;
    uvm2_frame_begin();
    uvm2_draw_reset();
    uvm2_draw_intensity(0x5F);
    uvm2_draw_move(-100, -60);
    for (int i = 0; i < SEGS; i++)
        uvm2_draw_delta((i & 1) ? len : -len, len / 2);
    uvm2_frame_end();

    double per_vec = (double)g_cycles / SEGS;
    printf("  %-28s %6u cmd %7u cyc  %6.1f cyc/vec  →  %4.0f vec/frame\n",
           label, g_commands, g_cycles, per_vec, 30000.0 / per_vec);
}

int main(void)
{
    printf("\nUVM2 command-stream budget — 50 Hz frame = %u bus cycles\n\n",
           UVM2_CYCLES_PER_FRAME);

    printf("A) Current VPy model: reset+intensity+move+draw PER SEGMENT\n");
    measure("scale 128 (stock default)", 128, 0, 100);
    measure("scale  64",                 64, 0, 100);
    measure("scale 128 + fixup (len 20)",128, 1, 20);

    printf("\nB) Stock firmware model: one centring per figure, chained segments\n");
    measure_chained("scale 128 (stock default)", 128, 0, 100);
    measure_chained("scale  64",                 64, 0, 100);
    measure_chained("scale 128 + fixup (len 20)",128, 1, 20);
    printf("\n");
    return 0;
}
