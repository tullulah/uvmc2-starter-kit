/* host_render — soft_demo on the host: a scripted session, chosen frames to SVG.
 *
 *   cd examples/soft_demo
 *   cc -O2 -w -Itools/host -I../../sdk/vpy-c/include -I../../sdk/pitrex-sim/include \
 *      tools/host_render.c src/main.c ../../sdk/vpy-c/vpy.c ../../sdk/vpy-c/vpy3d.c \
 *      ../../sdk/vpy-c/vpysoft.c -o /tmp/soft_host
 *   FRAMES=60,150,215 OUT=/tmp/soft /tmp/soft_host        -> /tmp/soft_60.svg ...
 *
 * The strokes are caught where libvpy hands them to the hardware (v_directDraw32,
 * deflection units, +y up), so what is drawn here is what the cartridge would
 * draw. It prints every frame's stroke count it saw the most of — the frame must
 * stay under ~940. The script, in frames at 50 Hz:
 *     0-99   breeze only          100-199  stick right (strong wind)
 *     200    button 1 (drop blob) 260      button 2 (punch)
 *     300    button 3 (drop cube) 350-449  stick left (wind the other way)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "uvm2_bus.h"

uvm2_stats_t uvm2_stats;
uint8_t currentButtonState; int8_t currentJoy1X, currentJoy1Y;
static int frame, n, maxn, maxframe, last;
static FILE *f;
static int wanted(int fr)
{
    const char *s = getenv("FRAMES");
    if (!s) return 0;
    char buf[256]; strncpy(buf, s, 255); buf[255] = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) { const int v = atoi(t); if (v == fr) return 1; if (v > last) last = v; }
    return 0;
}
void vectrexinit(int m) { (void)m; } void v_init(void) {} void v_setRefresh(int h) { (void)h; }
void v_directDraw32(int32_t a, int32_t b, int32_t c, int32_t d, uint8_t e)
{
    n++;
    if (!wanted(frame)) return;
    if (!f) {
        char p[256]; snprintf(p, sizeof p, "%s_%d.svg", getenv("OUT") ? getenv("OUT") : "/tmp/soft", frame);
        f = fopen(p, "w");
        fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' viewBox='-17000 -17000 34000 34000' width='600' height='600' style='background:#000'><g transform='scale(1,-1)'>");
    }
    fprintf(f, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='rgb(%d,255,%d)' stroke-width='90'/>", a, b, c, d, e, e);
}
void v_WaitRecal(void)
{
    if (n > maxn) { maxn = n; maxframe = frame; }
    if (f) { fprintf(f, "</g></svg>"); fclose(f); f = 0; printf("frame %d: %d strokes\n", frame, n); }
    n = 0;
    wanted(-1);
    if (++frame > last && frame > 1) { printf("max %d strokes (frame %d)\n", maxn, maxframe); exit(0); }
}
uint8_t v_readButtons(void)
{
    currentButtonState = frame == 200 ? 1 : frame == 260 ? 2 : frame == 300 ? 4 : 0;
    currentJoy1X = (frame >= 100 && frame < 200) ? 127 : (frame >= 350 && frame < 450) ? -127 : 0;
    currentJoy1Y = 0;
    return currentButtonState;
}
void v_setColour(uint32_t r) { (void)r; } void v_readJoystick1Analog(void) {} uint32_t v_millis(void) { return 0; }
void v_setSoundAY(uint8_t r, uint8_t v) { (void)r; (void)v; } void v_writePSG(uint8_t r, uint8_t v) { (void)r; (void)v; }
void v_playSample(int a, int b, int c) { (void)a; (void)b; (void)c; } void v_stopSample(int v) { (void)v; }
int v_samplePlaying(int v) { (void)v; return 0; }
