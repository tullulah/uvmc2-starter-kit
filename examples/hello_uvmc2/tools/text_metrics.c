/* text_metrics.c — how wide is a string, in VPy units, at a given text size?
 *
 * libvpy's font advances by a per-glyph amount baked into the glyph stream, so the
 * width of a string is not "characters x constant" and cannot be worked out by
 * looking. Centring text by guessing is how `hello_uvmc2` ended up with a title
 * that was both tiny and off to one side.
 *
 * This links the REAL vpy.c against a fake sink that records the extents, so the
 * numbers are the ones the cartridge will draw.
 *
 *   cc -O2 -I<kit>/sdk/vpy-c/include -I<kit>/sdk/pitrex-sim/include \
 *      -o /tmp/tm tools/text_metrics.c <kit>/sdk/vpy-c/vpy.c
 *   /tmp/tm 12 "HELLO UVMC2"
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vpy.h>

/* ---- the fake SDK: record what would be drawn ---- */
uint8_t currentButtonState = 0;
int8_t  currentJoy1X = 0, currentJoy1Y = 0;

static long lo_x, hi_x, lo_y, hi_y;
static int  seen;

static void note(long x, long y)
{
    if (!seen) { lo_x = hi_x = x; lo_y = hi_y = y; seen = 1; return; }
    if (x < lo_x) lo_x = x;   if (x > hi_x) hi_x = x;
    if (y < lo_y) lo_y = y;   if (y > hi_y) hi_y = y;
}

void v_directDraw32(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t z)
{
    if (!z) return;                 /* z == 0 is not drawn */
    note(x0, y0); note(x1, y1);
}
void vectrexinit(int m) { (void)m; }
void v_init(void) {}
void v_setRefresh(int hz) { (void)hz; }
void v_WaitRecal(void) {}
void v_setColour(uint32_t c) { (void)c; }
uint8_t v_readButtons(void) { return 0; }
void v_readJoystick1Analog(void) {}
uint32_t v_millis(void) { return 0; }
void v_writePSG(uint8_t r, uint8_t v) { (void)r; (void)v; }
void v_setSoundAY(uint8_t r, uint8_t v) { (void)r; (void)v; }
void v_playSample(int i, int v, int l) { (void)i; (void)v; (void)l; }
void v_stopSample(int v) { (void)v; }
int  v_samplePlaying(int v) { (void)v; return 0; }
void v_beamNewStroke(void) {}

int main(int argc, char **argv)
{
    const int  ts = argc > 1 ? atoi(argv[1]) : 8;
    const char *s = argc > 2 ? argv[2] : "HELLO UVMC2";

    vpy_set_intensity(127);
    vpy_set_text_size(ts);
    /* Drawn from x = 0 so the extents ARE the offsets from the origin. */
    vpy_print_text(0, 0, s);

    if (!seen) { printf("nothing drawn\n"); return 1; }

    /* v_directDraw32 works in PiTrex units: VPy x 127. */
    const double W = (double)(hi_x - lo_x) / 127.0;
    const double H = (double)(hi_y - lo_y) / 127.0;
    printf("text size %d, \"%s\"\n", ts, s);
    printf("  raw   x %ld..%ld   y %ld..%ld\n", lo_x, hi_x, lo_y, hi_y);
    printf("  VPy   width %.1f   height %.1f   (screen is 254 x 254)\n", W, H);
    printf("  left edge for a CENTRED line: x = %d\n", (int)(-(W / 2.0) - lo_x / 127.0));
    return 0;
}
