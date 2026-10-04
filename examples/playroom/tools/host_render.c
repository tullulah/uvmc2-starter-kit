/* host_render — the playroom on the host: a scripted session, chosen frames to SVG.
 *
 *   cd examples/playroom
 *   cc -O2 -w -Itools/host -Isrc -I../../sdk/vpy-c/include -I../../sdk/pitrex-sim/include \
 *      tools/host_render.c src/*.c ../../sdk/vpy-c/vpy.c ../../sdk/vpy-c/vpy3d.c \
 *      ../../sdk/vpy-c/vpyphys.c ../../sdk/vpy-c/vpyfx.c ../../sdk/vpy-c/vpycam.c \
 *      ../../sdk/vpy-c/vpyimpact.c ../../sdk/vpy-c/vpyent.c ../../sdk/vpy-c/vpysoft.c \
 *      ../../sdk/vpy-c/vpybone.c ../../sdk/vpy-c/vpyik.c ../../sdk/vpy-c/vpyease.c -o /tmp/pr_host
 *   IN='0-5:b=1; 10-200:jy=100' FRAMES=60,150 OUT=/tmp/pr /tmp/pr_host   -> /tmp/pr_60.svg ...
 *
 * IN is the script: entries "first-last:key=value,..." separated by ';', where
 * the keys are jx and jy (the stick, -127..127) and b (the buttons, bit 0 =
 * button 1). A frame no entry covers has the stick centred and no button. The
 * strokes are caught where libvpy hands them to the hardware (v_directDraw32),
 * so what is drawn here is what the cartridge would draw. It prints the most
 * strokes any frame took (it must stay under ~940), and LAST=n runs to frame n.
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
    char buf[512]; strncpy(buf, s, 511); buf[511] = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) { const int v = atoi(t); if (v == fr) return 1; if (v > last) last = v; }
    return 0;
}
static void script(int fr, int *jx, int *jy, int *b)
{
    *jx = *jy = *b = 0;
    const char *s = getenv("IN");
    if (!s) return;
    char buf[2048]; strncpy(buf, s, 2047); buf[2047] = 0;
    char *save1;
    for (char *e = strtok_r(buf, ";", &save1); e; e = strtok_r(NULL, ";", &save1)) {
        int a, z; char *colon = strchr(e, ':');
        if (!colon) continue;
        if (sscanf(e, " %d-%d", &a, &z) != 2) { if (sscanf(e, " %d", &a) != 1) continue; z = a; }
        if (fr < a || fr > z) continue;
        char *save2;
        for (char *kv = strtok_r(colon + 1, ",", &save2); kv; kv = strtok_r(NULL, ",", &save2)) {
            while (*kv == ' ') kv++;
            if (!strncmp(kv, "jx=", 3)) *jx = atoi(kv + 3);
            else if (!strncmp(kv, "jy=", 3)) *jy = atoi(kv + 3);
            else if (!strncmp(kv, "b=", 2)) *b = atoi(kv + 2);
        }
    }
}
void vectrexinit(int m) { (void)m; } void v_init(void) {} void v_setRefresh(int h) { (void)h; }
void v_directDraw32(int32_t a, int32_t b, int32_t c, int32_t d, uint8_t e)
{
    n++;
    if (!wanted(frame)) return;
    if (!f) {
        char p[256]; snprintf(p, sizeof p, "%s_%d.svg", getenv("OUT") ? getenv("OUT") : "/tmp/pr", frame);
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
    if (getenv("LAST") && atoi(getenv("LAST")) > last) last = atoi(getenv("LAST"));
    if (++frame > last && frame > 1) { printf("max %d strokes (frame %d)\n", maxn, maxframe); exit(0); }
}
uint8_t v_readButtons(void)
{
    int jx, jy, b; script(frame, &jx, &jy, &b);
    currentButtonState = (uint8_t)b; currentJoy1X = (int8_t)jx; currentJoy1Y = (int8_t)jy;
    return currentButtonState;
}
void v_setColour(uint32_t r) { (void)r; } void v_readJoystick1Analog(void) {} uint32_t v_millis(void) { return 0; }
void v_setSoundAY(uint8_t r, uint8_t v) { (void)r; (void)v; } void v_writePSG(uint8_t r, uint8_t v) { (void)r; (void)v; }
void v_playSample(int a, int b, int c) { (void)a; (void)b; (void)c; } void v_stopSample(int v) { (void)v; }
int v_samplePlaying(int v) { (void)v; return 0; }
