/*
 * vpy.c — implementation of the VPy builtin runtime on the PiTrex SDK contract.
 * See vpy.h. Everything routes through v_directDraw32 / v_writePSG etc., so it
 * behaves identically on real hardware and in the IDE simulator.
 */
#include "vpy.h"
#include <vectrex/vectrexInterface.h>
/* No <math.h>: libvpy is integer-only so it compiles to plain integer ARM that
 * both real hardware and the VPy sim (PitrexCore's ARMv6 interpreter) can run. */

#define VPY_SCALE 127   /* VPy logical unit -> PiTrex deflection unit */

/* ---- state ---- */
static int   s_cur_x = 0, s_cur_y = 0;   /* MOVE origin, in VPy units */
static int   s_intensity = 90;
static int   s_text_size = 8;   /* matches inline PITREX_TEXT_SIZE default (0 -> 8) */
static uint32_t s_rng = 0u;   /* matches the inline RAND_SEED (zeroed .bss) */

/* sin table: 128 steps per full circle, amplitude +-127 (compile-time const). */
static const int8_t s_sin[128] = {
       0,   6,  12,  19,  25,  31,  37,  43,  49,  54,  60,  65,  71,  76,  81,  85,
      90,  94,  98, 102, 106, 109, 112, 115, 117, 120, 122, 123, 125, 126, 126, 127,
     127, 127, 126, 126, 125, 123, 122, 120, 117, 115, 112, 109, 106, 102,  98,  94,
      90,  85,  81,  76,  71,  65,  60,  54,  49,  43,  37,  31,  25,  19,  12,   6,
       0,  -6, -12, -19, -25, -31, -37, -43, -49, -54, -60, -65, -71, -76, -81, -85,
     -90, -94, -98,-102,-106,-109,-112,-115,-117,-120,-122,-123,-125,-126,-126,-127,
    -127,-127,-126,-126,-125,-123,-122,-120,-117,-115,-112,-109,-106,-102, -98, -94,
     -90, -85, -81, -76, -71, -65, -60, -54, -49, -43, -37, -31, -25, -19, -12,  -6,
};
/* atan LUT for the integer atan2 octant — ported BIT-EXACT from the VPy PiTrex
 * inline codegen (`pitrex_atan_lut` in vpy_codegen builtins.rs). 33 entries map a
 * ratio 0..32 (= |smaller|*32/|larger|) to an octant angle 0..22. This is the
 * SAME quantized table the inline `pitrex_atan2` uses, so vpy_atan2 reproduces it
 * exactly (the inline's 45deg endpoint is 22, NOT the ideal 16 — matched on
 * purpose for bit-identical output). */
static const uint8_t s_atan[33] = {
    0,1,2,3,4,5,6,7,8,8,9,10,11,12,12,13,14,14,15,15,16,17,17,18,18,19,19,20,20,20,21,21,22
};

static void ensure_tables(void) { /* tables are compile-time const now */ }

/* Draw one segment in absolute VPy space (no MOVE offset). */
static void raw_line(int x0, int y0, int x1, int y1, int b)
{
    v_directDraw32((int32_t)x0 * VPY_SCALE, (int32_t)y0 * VPY_SCALE,
                   (int32_t)x1 * VPY_SCALE, (int32_t)y1 * VPY_SCALE, (uint8_t)b);
}

/* ---- lifecycle ---- */
void vpy_init(void)
{
    ensure_tables();
    vectrexinit(1);
    v_init();
    v_setRefresh(50);   /* Vectrex refresh; .vmus/.vsfx are compiled at 50 fps */
}

void vpy_frame_begin(void)
{
    v_WaitRecal();
    v_readButtons();          /* refresh currentButtonState */
    v_readJoystick1Analog();  /* refresh currentJoy1X / currentJoy1Y */
}

void vpy_run(void (*setup)(void), void (*loop)(void))
{
    vpy_init();
    if (setup) setup();
    for (;;) {
        vpy_frame_begin();
        /* VPy auto-injects MUSIC_UPDATE at loop start — mirror that here so
         * games don't have to call the sequencers manually. */
        vpy_music_update();
        vpy_sfx_update();
        if (loop) loop();
    }
}

/* ---- drawing ---- */
void vpy_set_intensity(int b) { s_intensity = b; }
void vpy_move(int x, int y)   { s_cur_x = x; s_cur_y = y; }

void vpy_draw_line(int x0, int y0, int x1, int y1, int b)
{
    raw_line(x0 + s_cur_x, y0 + s_cur_y, x1 + s_cur_x, y1 + s_cur_y, b);
}

void vpy_draw_circle(int cx, int cy, int r, int b)
{
    ensure_tables();
    int px = cx + r, py = cy;                 /* angle 0 */
    for (int i = 1; i <= 16; i++) {
        int a = (i * 128) / 16;               /* 0..128 */
        int x = cx + (r * s_sin[(a + 32) & 127]) / 127;  /* cos */
        int y = cy + (r * s_sin[a & 127]) / 127;         /* sin */
        raw_line(px, py, x, y, b);
        px = x; py = y;
    }
}

void vpy_draw_rect(int x, int y, int w, int h, int b)
{
    raw_line(x,     y,     x + w, y,     b);
    raw_line(x + w, y,     x + w, y + h, b);
    raw_line(x + w, y + h, x,     y + h, b);
    raw_line(x,     y + h, x,     y,     b);
}

void vpy_draw_filled_rect(int x, int y, int w, int h, int b)
{
    vpy_draw_rect(x, y, w, h, b);
    for (int yy = y + 3; yy < y + h; yy += 3)
        raw_line(x, yy, x + w, yy, b);
}

void vpy_draw_polygon(const int *xy, int n, int b)
{
    if (n < 2) return;
    for (int i = 0; i < n - 1; i++)
        raw_line(xy[2*i], xy[2*i+1], xy[2*i+2], xy[2*i+3], b);
    raw_line(xy[2*(n-1)], xy[2*(n-1)+1], xy[0], xy[1], b);  /* close */
}

void vpy_draw_ellipse(int cx, int cy, int rx, int ry, int b)
{
    ensure_tables();
    int px = cx + rx, py = cy;                /* angle 0 */
    for (int i = 1; i <= 16; i++) {
        int a = (i * 128) / 16;               /* 0..128 */
        int x = cx + (rx * s_sin[(a + 32) & 127]) / 127;  /* cos */
        int y = cy + (ry * s_sin[a & 127]) / 127;         /* sin */
        raw_line(px, py, x, y, b);
        px = x; py = y;
    }
}

/* ---- compiled vector sprites (.vec) --------------------------------------
 * Draw a compiled .vec path stream (from `vpy_cli compile-asset --format c`).
 * Byte format (LE, position-independent — no absolute pointers):
 *   [0..2] path_count (u16)
 *   per path: intensity, y0, x0, 0x00, 0x00, segments..., 0x02
 *     0xFF, dy, dx      -> line delta (i8, i8)
 *     0xFE, 8×i8        -> cubic bezier (ax,ay,cp1x,cp1y,cp2x,cp2y,bx,by),
 *                          center-relative; tessellated here into line segments
 *                          because the host SDK contract only has v_directDraw32.
 * Mirrors pitrex_draw_vector: the beam is seeded at (x0+ox, y0+oy) in VPy units
 * and each line adds its delta; bezier control points are sprite-origin
 * relative. Coordinates scale by VPY_SCALE (via raw_line), exactly like the ARM
 * path (which multiplies by 127). */
#ifdef VPY_RP2350
/* Per-path beam re-zero (RP2350 backend). Without it, chaining every path of
 * every shape with only relative moves lets integrator drift carry across shape
 * boundaries → each .vec wobbles as a block. Re-zeroing per path mirrors the
 * native ARM backend and kills the cross-shape drift. */
extern void v_beamNewStroke(void);
#endif

static void draw_vec_stream(const unsigned char *data, int ox, int oy,
                            int mirror, int override_b)
{
    if (!data) return;
    int path_count = (int)data[0] | ((int)data[1] << 8);
    const unsigned char *p = data + 2;

    for (int pi = 0; pi < path_count; pi++) {
#ifdef VPY_RP2350
        v_beamNewStroke();   /* fresh zero-ref per path — no cross-shape drift */
#endif
        int intensity = p[0];
        int y0 = (int8_t)p[1];
        int x0 = (int8_t)p[2];
        p += 5;                     /* intensity + y0 + x0 + 2 padding bytes */
        int b = (override_b > 0) ? override_b : intensity;

        int cx = ox + (mirror ? -x0 : x0);
        int cy = oy + y0;

        for (;;) {
            unsigned char marker = *p++;
            if (marker == 0x02) break;          /* end of path */

            if (marker == 0xFE) {               /* cubic bezier segment */
                int ax  = (int8_t)p[0], ay  = (int8_t)p[1];
                int c1x = (int8_t)p[2], c1y = (int8_t)p[3];
                int c2x = (int8_t)p[4], c2y = (int8_t)p[5];
                int bx  = (int8_t)p[6], by  = (int8_t)p[7];
                p += 8;
                if (mirror) { ax = -ax; c1x = -c1x; c2x = -c2x; bx = -bx; }
                /* Absolute (VPy-unit) control points, sprite-origin relative. */
                int P0x = ox + ax,  P0y = oy + ay;
                int P1x = ox + c1x, P1y = oy + c1y;
                int P2x = ox + c2x, P2y = oy + c2y;
                int P3x = ox + bx,  P3y = oy + by;
                /* Integer De Casteljau into 8 line segments (repeated lerp). */
                int px = P0x, py = P0y;
                const int STEPS = 8;
                for (int t = 1; t <= STEPS; t++) {
                    int abx = P0x + (P1x-P0x)*t/STEPS, aby = P0y + (P1y-P0y)*t/STEPS;
                    int bcx = P1x + (P2x-P1x)*t/STEPS, bcy = P1y + (P2y-P1y)*t/STEPS;
                    int cdx = P2x + (P3x-P2x)*t/STEPS, cdy = P2y + (P3y-P2y)*t/STEPS;
                    int abc_x = abx + (bcx-abx)*t/STEPS, abc_y = aby + (bcy-aby)*t/STEPS;
                    int bcd_x = bcx + (cdx-bcx)*t/STEPS, bcd_y = bcy + (cdy-bcy)*t/STEPS;
                    int nx = abc_x + (bcd_x-abc_x)*t/STEPS, ny = abc_y + (bcd_y-abc_y)*t/STEPS;
                    raw_line(px, py, nx, ny, b);
                    px = nx; py = ny;
                }
                cx = px; cy = py;
                continue;
            }

            /* line segment: marker (0xFF) then dy, dx */
            int dy = (int8_t)p[0];
            int dx = (int8_t)p[1];
            p += 2;
            if (mirror) dx = -dx;
            int nx = cx + dx, ny = cy + dy;
            raw_line(cx, cy, nx, ny, b);
            cx = nx; cy = ny;
        }
    }
}

void vpy_draw_vector(const unsigned char *data, int x, int y)
{
    draw_vec_stream(data, x, y, 0, 0);
}

void vpy_draw_vector_ex(const unsigned char *data, int x, int y, int mirror, int intensity)
{
    draw_vec_stream(data, x, y, mirror ? 1 : 0, intensity);
}

/* ---- SDK vector font (ported from pitrex-baremetal vectorFont.i, the ACTIVE
 * BLOW_UP=15 table) ---------------------------------------------------------
 * This is the SAME font the inline PiTrex path renders via v_printString, so
 * PRINT_TEXT looks identical on hardware, the C-import WASM sim, and the VPy
 * PitrexCore sim — all drawn through v_directDraw32 (we do NOT add
 * v_printString to the SDK contract).
 *
 * Each glyph is a stream of [pattern, dy, dx] signed-byte triples (the dy/dx
 * deltas are already ×BLOW_UP=15), terminated by a lone 0x01. Reproducing
 * v_printString's pipeline:
 *   pattern != 0  -> draw a stroke  cursor -> cursor + delta*SCALEFONT
 *   pattern == 0  -> move only (no beam), advance the cursor
 * with SCALEFONT = textSize*1.5. The loop continues while the NEXT entry's
 * pattern byte is <= 0; the 0x01 endmarker (positive) stops it. The trailing
 * move in each glyph bakes in the inter-character advance. */
static const signed char F_Folder[] = {-1,120,0,-1,0,60,-1,-60,0,0,0,-15,-1,-60,0,0,0,30,-1,45,0,-1,15,-15,-1,0,-15,0,-60,-45,-1,0,75,0,0,15,1};
static const signed char F_ABC_0[] = {-1,45,0,-1,45,0,-1,30,30,-1,-30,30,-1,-45,0,-1,0,-60,0,0,60,-1,-45,0,0,0,30,1};
static const signed char F_ABC_1[] = {-1,120,0,-1,0,30,-1,-15,30,-1,-30,-15,0,0,-45,-1,0,45,-1,-45,15,-1,-30,-30,-1,0,-30,0,0,90,1};
static const signed char F_ABC_2[] = {0,120,60,-1,0,-60,-1,-120,0,-1,0,60,0,0,30,1};
static const signed char F_ABC_3[] = {-1,120,0,-1,0,30,-1,-45,30,-1,-30,0,-1,-45,-30,-1,0,-30,0,0,90,1};
static const signed char F_ABC_4[] = {0,120,60,-1,0,-60,-1,-45,0,0,0,60,-1,0,-60,-1,-75,0,-1,0,60,0,0,30,1};
static const signed char F_ABC_5[] = {-1,75,0,-1,0,60,0,0,-60,-1,45,0,-1,0,60,0,-120,30,1};
static const signed char F_ABC_6[] = {0,105,60,-1,15,0,-1,0,-60,-1,-120,0,-1,0,60,-1,75,0,-1,0,-30,0,-75,60,1};
static const signed char F_ABC_7[] = {-1,120,0,0,-45,0,-1,0,60,0,45,0,-1,-120,0,0,0,30,1};
static const signed char F_ABC_8[] = {-1,0,60,0,0,-30,-1,120,0,0,0,-30,-1,0,60,0,-120,30,1};
static const signed char F_ABC_9[] = {0,30,0,-1,-30,15,-1,0,45,-1,120,0,-1,0,-30,0,-120,60,1};
static const signed char F_ABC_10[] = {-1,120,0,0,-45,0,-1,45,60,0,-45,-60,-1,-75,60,0,0,30,1};
static const signed char F_ABC_11[] = {0,120,0,-1,-120,0,-1,0,60,0,0,30,1};
static const signed char F_ABC_12[] = {-1,120,0,-1,-45,30,-1,45,30,-1,-120,0,0,0,30,1};
static const signed char F_ABC_13[] = {-1,120,0,-1,-120,60,-1,120,0,0,-120,30,1};
static const signed char F_ABC_14[] = {0,0,0,-1,120,0,-1,0,60,-1,-120,0,-1,0,-60,0,0,90,1};
static const signed char F_ABC_15[] = {-1,120,0,-1,0,60,-1,-45,0,-1,0,-60,0,-75,90,1};
static const signed char F_ABC_16[] = {0,0,30,-1,0,-30,-1,120,0,-1,0,60,-1,-90,0,-1,-30,-30,0,30,0,-1,-30,30,0,0,30,1};
static const signed char F_ABC_17[] = {-1,120,0,-1,0,60,-1,-45,0,-1,0,-60,-1,-75,60,0,0,30,1};
static const signed char F_ABC_18[] = {0,120,60,-1,0,-60,-1,-45,0,-1,0,60,-1,-75,0,-1,0,-60,0,0,90,1};
static const signed char F_ABC_19[] = {0,0,30,-1,120,0,0,0,-30,-1,0,60,0,-120,30,1};
static const signed char F_ABC_20[] = {0,120,0,-1,-120,0,-1,0,60,-1,120,0,0,-120,30,1};
static const signed char F_ABC_21[] = {0,120,0,-1,-120,30,-1,120,30,0,-120,30,1};
static const signed char F_ABC_22[] = {0,120,0,-1,-120,0,-1,45,30,-1,-45,30,-1,120,0,0,-120,30,1};
static const signed char F_ABC_23[] = {-1,120,60,0,0,-60,-1,-120,60,0,0,30,1};
static const signed char F_ABC_24[] = {0,120,0,-1,-45,30,-1,45,30,0,-45,-30,-1,-75,0,0,0,60,1};
static const signed char F_ABC_25[] = {0,120,0,-1,0,60,-1,-120,-60,-1,0,60,0,0,30,1};
static const signed char F_ABC_26[] = {-1,0,30,-1,30,0,-1,0,-30,-1,-30,0,0,0,90,1};
static const signed char F_ABC_27[] = {0,0,90,1};
static const signed char F_ABC_28[] = {-1,0,30,-1,30,0,-1,0,-30,-1,-30,0,0,45,15,-1,75,0,0,-120,60,1};
static const signed char F_ABC_29[] = {0,120,45,-1,-120,0,0,0,45,1};
static const signed char F_ABC_30[] = {0,120,0,-1,0,60,-1,-45,0,-1,0,-60,-1,-75,0,-1,0,60,0,0,30,1};
static const signed char F_ABC_31[] = {0,120,0,-1,0,60,-1,-45,0,-1,0,-60,0,0,60,-1,-75,0,-1,0,-60,0,0,90,1};
static const signed char F_ABC_32[] = {0,0,60,-1,120,0,0,0,-60,-1,-45,0,-1,0,60,0,-75,30,1};
static const signed char F_ABC_33[] = {-1,0,60,-1,75,0,-1,0,-60,-1,45,0,-1,0,60,0,-120,30,1};
static const signed char F_ABC_34[] = {-1,120,0,-1,0,60,0,-45,-60,-1,0,60,-1,-75,0,-1,0,-60,0,0,90,1};
static const signed char F_ABC_35[] = {0,0,60,-1,120,0,-1,0,-60,0,-120,90,1};
static const signed char F_ABC_36[] = {-1,120,0,-1,0,60,-1,-120,0,-1,0,-60,0,75,0,-1,0,60,0,-75,30,1};
static const signed char F_ABC_37[] = {0,75,60,-1,0,-60,-1,45,0,-1,0,60,-1,-120,0,0,0,30,1};
static const signed char F_ABC_38[] = {-1,120,0,-1,0,60,-1,-120,0,-1,0,-60,0,0,90,1};
static const signed char F_ABC_39[] = {0,30,15,-1,30,-15,-1,30,15,0,-30,45,-1,0,-60,0,-60,90,1};
static const signed char F_ABC_40[] = {0,60,0,-1,0,60,-1,30,-15,0,-60,0,-1,30,15,0,-60,30,1};

/* ASCII(0x20..)->glyph pointer, exactly as the SDK ABC[] index. */
static const signed char *const FONT_ABC[143] = {
    F_ABC_27,F_ABC_28,F_ABC_27,F_ABC_27,F_Folder,F_ABC_27,F_ABC_27,F_ABC_27,
    F_ABC_27,F_ABC_27,F_ABC_27,F_ABC_27,F_ABC_27,F_ABC_27,F_ABC_26,F_ABC_27,
    F_ABC_38,F_ABC_29,F_ABC_30,F_ABC_31,F_ABC_32,F_ABC_33,F_ABC_34,F_ABC_35,
    F_ABC_36,F_ABC_37,F_ABC_27,F_ABC_27,F_ABC_39,F_ABC_27,F_ABC_40,F_ABC_27,
    F_ABC_27,F_ABC_0,F_ABC_1,F_ABC_2,F_ABC_3,F_ABC_4,F_ABC_5,F_ABC_6,
    F_ABC_7,F_ABC_8,F_ABC_9,F_ABC_10,F_ABC_11,F_ABC_12,F_ABC_13,F_ABC_14,
    F_ABC_15,F_ABC_16,F_ABC_17,F_ABC_18,F_ABC_19,F_ABC_20,F_ABC_21,F_ABC_22,
    F_ABC_23,F_ABC_24,F_ABC_25,F_ABC_27,F_ABC_27,F_ABC_27,F_ABC_27,F_ABC_27,
    F_ABC_27,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
    F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,F_ABC_26,
};
#define FONT_ABC_N 143

/* toupper + range-map to a glyph pointer, exactly like the SDK ABC[] lookup
 * (`ABC[toupper(*string)-0x20]`); out-of-range chars fall back to space. */
static const signed char *font_glyph(unsigned char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    int idx = (int)c - 0x20;
    if (idx < 0 || idx >= FONT_ABC_N) idx = 0;   /* -> space (F_ABC_27) */
    return FONT_ABC[idx];
}

/* textSize used by the print routines: SET_TEXT_SIZE value, or 8 by default
 * (matches the inline PITREX_TEXT_SIZE default = m6809 "normal"). */
static int font_text_size(void) { return (s_text_size > 0) ? s_text_size : 8; }

/* Core glyph-stream renderer, reproducing the inline PiTrex print sequence
 * (builtins.rs pitrex_print_text) followed by v_printString's pipeline, but
 * drawing each stroke via v_directDraw32 in raw deflection units:
 *   baseline = y - 8                       (inline cap_height shift, VPy units)
 *   startX   = ((x*127) >> 7) * 128        (inline 127/128 pre-scale, then *128)
 * Each stroke endpoint = trunc(cursor + delta * textSize * 1.5); computed
 * exactly in integer as (cursor*2 + delta*textSize*3) / 2 (C /2 truncates
 * toward zero, matching v_printString's double->int truncation). */
static void font_draw_string(int x, int y, const char *s)
{
    int ts = font_text_size();
    int yb = y - 8;
    int startX = ((x  * 127) >> 7) * 128;
    int startY = ((yb * 127) >> 7) * 128;
    for (; *s; s++) {
        const signed char *list = font_glyph((unsigned char)*s);
        do {
            int pat = list[0];
            int nx = (startX * 2 + (int)list[2] * ts * 3) / 2;
            int ny = (startY * 2 + (int)list[1] * ts * 3) / 2;
            if (pat != 0)
                v_directDraw32(startX, startY, nx, ny, (uint8_t)s_intensity);
            startX = nx;
            startY = ny;
            list += 3;
        } while ((int)list[0] <= 0);
    }
}

void vpy_set_text_size(int s) { s_text_size = s; }

void vpy_print_text(int x, int y, const char *s) { font_draw_string(x, y, s); }

void vpy_print_number(int x, int y, long n)
{
    int ts = font_text_size();
    int neg = (n < 0);
    long v = neg ? -n : n;

    /* Format a fixed 4-digit field (1000s/100s/10s/1s), '-' prefix if negative,
     * exactly like the inline pitrex_print_number buffer layout. */
    char buf[8];
    int i = 0;
    if (neg) buf[i++] = '-';
    buf[i++] = (char)('0' + (int)((v / 1000) % 10));
    buf[i++] = (char)('0' + (int)((v / 100)  % 10));
    buf[i++] = (char)('0' + (int)((v / 10)   % 10));
    buf[i++] = (char)('0' + (int)( v         % 10));
    buf[i]   = 0;

    /* The SDK vector font maps '-' to a blank space glyph, so the inline path
     * draws the minus as a manual horizontal stroke in raw deflection units:
     *   x0 = x*127, x1 = x0 + 8*ts, y = (y-8)*127 + 6*ts. Reproduce it here. */
    if (neg) {
        int x0   = x * 127;
        int ymid = (y - 8) * 127 + 6 * ts;
        v_directDraw32(x0, ymid, x0 + 8 * ts, ymid, (uint8_t)s_intensity);
    }

    /* Leading-zero suppression: the inline scan starts at buf[0]; for negatives
     * that is '-' (not '0') so it stops immediately — i.e. suppression applies
     * to positive values only. Reproduce that quirk for a matching layout. */
    const char *p = buf;
    while (p[0] == '0' && p[1] != 0) p++;
    font_draw_string(x, y, p);
}

/* ---- input ----
 * Read the SDK input globals DIRECTLY. They are refreshed every frame by
 * v_readButtons() / v_readJoystick1Analog() — called by BOTH vpy_frame_begin()
 * (the C vpy_run path) AND the VPy pitrex game loop before the loop body. This
 * makes these functions bit-identical to the inline VPy codegen (pitrex_j1_x/y
 * ldrsb currentJoy1X/Y; pitrex_j1_btnN reads bit N-1 of currentButtonState) and
 * avoids the stale-snapshot bug: the old s_jx/s_jy/s_btn snapshot was only taken
 * by vpy_frame_begin, which the VPy game loop never calls. */
int vpy_j1_x(void) { return (int)currentJoy1X; }
int vpy_j1_y(void) { return (int)currentJoy1Y; }
int vpy_j1_button(int n) { return (n >= 1 && n <= 4) ? ((currentButtonState >> (n - 1)) & 1) : 0; }
void vpy_update_buttons(void) { v_readButtons(); v_readJoystick1Analog(); }

/* ---- math ---- */
int vpy_abs(int v) { return v < 0 ? -v : v; }
int vpy_min(int a, int b) { return a < b ? a : b; }
int vpy_max(int a, int b) { return a > b ? a : b; }
int vpy_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int vpy_sin(int a) { ensure_tables(); return s_sin[((a % 128) + 128) & 127]; }
int vpy_cos(int a) { ensure_tables(); return s_sin[(((a % 128) + 128) + 32) & 127]; }
int vpy_sqrt(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }   /* integer Newton */
    return x;
}
/* Integer atan2 -> full-circle angle 0..127. BIT-EXACT port of the VPy PiTrex
 * inline `pitrex_atan2(r0=y, r1=x)`:
 *   octant split on |x| >= |y| (shallow) vs |y| > |x| (steep);
 *   ratio = |smaller| * 32 / |larger| (integer div, truncates toward zero — both
 *   operands non-negative, identical to ARM __aeabi_idiv here);
 *   shallow angle = s_atan[ratio], steep angle = 32 - s_atan[ratio];
 *   then the same quadrant map (Q4 = 128-a) and & 127.
 * The ratio is clamped to 32 to mirror the inline's `cmp/movgt` guard before the
 * table index. */
int vpy_atan2(int y, int x) {
    int ax = x < 0 ? -x : x;
    int ay = y < 0 ? -y : y;
    if (ax == 0 && ay == 0) return 0;
    int a;                                 /* octant angle 0..32 */
    if (ax >= ay) {                        /* shallow: |y| <= |x| (0..45deg) */
        int r = ax ? (ay * 32) / ax : 0;
        if (r > 32) r = 32;
        a = s_atan[r];
    } else {                               /* steep: |y| > |x| (45..90deg) */
        int r = ay ? (ax * 32) / ay : 0;
        if (r > 32) r = 32;
        a = 32 - s_atan[r];
    }
    int ang;
    if      (x >= 0 && y >= 0) ang = a;            /* Q1 */
    else if (x <  0 && y >= 0) ang = 64 - a;       /* Q2 */
    else if (x <  0 && y <  0) ang = 64 + a;       /* Q3 */
    else                        ang = 128 - a;      /* Q4 (x>=0, y<0) */
    return ang & 127;
}

/* PRNG — BIT-EXACT port of the VPy PiTrex inline `pitrex_random`:
 *   state = state * 1664525 + 1013904223  (32-bit wraparound)
 *   output = (state >> 16) & 0xFFFF        (16-bit, 0..65535)
 * The inline seed lives in a zeroed .bss word (RAND_SEED), so the initial state
 * is 0 — s_rng starts at 0 to match, giving an identical sequence from reset. */
void vpy_seed(unsigned s) { s_rng = s; }
int vpy_rand(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return (int)((s_rng >> 16) & 0xFFFFu);
}
/* rand_range — BIT-EXACT port of inline `pitrex_rand_range`:
 *   range = hi - lo + 1;  return lo + rand() % range.
 * The inline has no lo/hi guard (it relies on ARM aeabi div-by-zero -> 0 for the
 * degenerate hi < lo case); we guard only the truly-undefined range <= 0 path to
 * avoid C modulo-by-zero UB. For the real contract (hi >= lo) the guard never
 * fires and the result is identical to the inline. */
int vpy_rand_range(int lo, int hi) {
    int range = hi - lo + 1;
    if (range <= 0) return lo;
    return lo + vpy_rand() % range;
}

/* ---- sound ---- */
void vpy_tone(int period, int volume)
{
    if (volume <= 0) { v_writePSG(8, 0x00); return; }
    v_writePSG(0, period & 0xff);
    v_writePSG(1, (period >> 8) & 0x0f);
    v_writePSG(8, volume & 0x0f);
    v_writePSG(7, 0x3e);           /* enable tone A only */
}
void vpy_beep(int on) { if (on) vpy_tone(0xD5, 0x0f); else vpy_tone(0, 0); }

/* ---- compiled music / SFX sequencer -----------------------------------------
 * Plays a compiled PSG event stream (see compile-asset). Byte format (LE):
 *   MUSIC: [0..4]=num_events, [4..8]=loop_event_byte_offset, events at base+8.
 *   SFX:   [0..4]=num_events, events at base+4.
 *   event = [delay, num_writes, (reg,val)*num_writes]
 *          num_writes==0xFF -> loop (music), ==0 -> end.
 * Sequencer model (mirrors the ARM pitrex_music_update / pitrex_sfx_update):
 *   the FIRST event fires immediately (delay starts 0); each event's own delay
 *   byte is the wait BEFORE the NEXT event, read after the current one fires. */

static const unsigned char *s_mus_base = 0;   /* stream base (for loop) */
static const unsigned char *s_mus_ptr  = 0;   /* cursor: current event */
static int s_mus_playing = 0;
static int s_mus_delay   = 0;                  /* frames left before next event */
/* One-shot timer-priming flag reproducing the inline PiTrex BCM-CLO sequencer:
 * `pitrex_music_update`'s FIRST update-while-playing only captures the timer
 * baseline (pmu_init_clo, PSG_MUSIC_LAST_CLO==0) and fires nothing that frame.
 * That baseline is zeroed once at program start, so the priming frame happens
 * exactly once per program, delaying the whole music timeline by one frame.
 * libvpy is otherwise a pure one-tick-per-frame sequencer, so without this it
 * would fire the first music event one frame early; the flag makes the two
 * paths emit identical PSG-write sequences (headless-verified). */
static int s_mus_primed  = 0;

static const unsigned char *s_sfx_ptr = 0;
static int s_sfx_active = 0;
static int s_sfx_delay  = 0;

/* Shadow of PSG mixer reg 7 so SFX (channel C) can read-modify-write it without
 * silencing music on channels A/B (the ARM runtime read-modify-writes reg 7). */
static uint8_t s_psg_mixer = 0x3f;

/* ...and the channel-C bits the RUNNING SFX asked for, so the merge works in
 * BOTH directions. It used to work in one: an SFX merged itself over the
 * music's A/B bits, but a music event writing reg 7 afterwards wrote it raw and
 * took channel C straight back, cutting the effect off mid-flight until its own
 * next mixer write — which a diff-encoded stream may never make. Any track with
 * noise percussion writes reg 7 on every drum hit, so those cut effects within
 * a couple of frames. */
static uint8_t s_sfx_cbits = 0x24;    /* 0x24 = tone C and noise C disabled */

static uint32_t rd_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write a PSG register, tracking the mixer shadow. */
static void psg_write(uint8_t reg, uint8_t val)
{
    if (reg == 7) {
        /* While an SFX runs it owns channel C: a music write sets everything
         * else but leaves those two bits as the effect left them. */
        if (s_sfx_active)
            val = (uint8_t)((val & 0xdb) | (s_sfx_cbits & 0x24));
        s_psg_mixer = val;
    }
    v_writePSG(reg, val);
}

#ifdef VPY_RP2350
/* RP2350 backend: the .vmus sequencer runs on core 1 (BIOS svc #21/#22), so the
 * tempo is independent of core-0 draw load. libvpy just hands over / stops the
 * track; AUDIO_UPDATE is a no-op. (PiTrex/WASM keep the software sequencer.) */
extern void v_playMusic(const unsigned char *vmus);
extern void v_stopMusic(void);
#endif

void vpy_play_music(const unsigned char *data)
{
    if (!data) return;
    /* Guard: same track already playing -> no-op (prevents per-frame restart). */
    if (s_mus_playing && s_mus_base == data) return;
    s_mus_base    = data;
    s_mus_playing = 1;
#ifdef VPY_RP2350
    v_playMusic(data);          /* hand the .vmus track to the core-1 player */
#else
    s_mus_ptr     = data + 8;   /* first event follows the 8-byte header */
    s_mus_delay   = 0;          /* first event fires immediately */
#endif
}

void vpy_stop_music(void)
{
    s_mus_playing = 0;
#ifdef VPY_RP2350
    v_stopMusic();              /* core-1 player stops + silences */
#else
    psg_write(8, 0);            /* channel A volume */
    psg_write(9, 0);            /* channel B volume */
    psg_write(10, 0);           /* channel C volume */
    psg_write(7, 0x3f);         /* mixer: all disabled */
#endif
}

void vpy_music_update(void)
{
#ifdef VPY_RP2350
    return;   /* core 1 sequences the .vmus; AUDIO_UPDATE is a no-op on RP2350 */
#else
    if (!s_mus_playing || !s_mus_ptr) return;
    /* Inline BCM-CLO parity: the very first update-while-playing primes the
     * (virtual) timer baseline and fires nothing — see s_mus_primed. */
    if (!s_mus_primed) { s_mus_primed = 1; return; }
    if (s_mus_delay > 0) { s_mus_delay--; return; }

    const unsigned char *p = s_mus_ptr;
    uint8_t num_writes = p[1];

    if (num_writes == 0x00) {          /* end marker */
        vpy_stop_music();
        return;
    }
    if (num_writes == 0xff) {          /* loop marker */
        uint32_t off = rd_le32(s_mus_base + 4);
        s_mus_ptr   = s_mus_base + off;
        s_mus_delay = s_mus_ptr[0];    /* delay of the loop-start event */
        return;
    }

    /* Fire this event: write each (reg,val) pair. */
    const unsigned char *w = p + 2;
    for (uint8_t i = 0; i < num_writes; i++) {
        psg_write(w[0], w[1]);
        w += 2;
    }
    /* Advance to next event; its delay byte is the wait before it fires. */
    s_mus_ptr   = w;
    s_mus_delay = w[0];
#endif
}

void vpy_play_sfx(const unsigned char *data)
{
    if (!data) return;
    s_sfx_ptr    = data + 4;   /* first event follows the 4-byte header */
    s_sfx_active = 1;
    s_sfx_delay  = 0;
}

void vpy_sfx_update(void)
{
    if (!s_sfx_active || !s_sfx_ptr) return;
    if (s_sfx_delay > 0) { s_sfx_delay--; return; }

    const unsigned char *p = s_sfx_ptr;
    uint8_t num_writes = p[1];

    if (num_writes == 0x00) {          /* end of SFX */
        psg_write(10, 0);              /* mute channel C */
        s_sfx_active = 0;              /* before the mixer write: */
        s_sfx_cbits  = 0x24;           /* channel C is the music's again */
        psg_write(7, (uint8_t)(s_psg_mixer | 0x24));
        return;
    }

    const unsigned char *w = p + 2;
    for (uint8_t i = 0; i < num_writes; i++) {
        uint8_t reg = w[0], val = w[1];
        if (reg == 7) {
            /* Merge only channel-C mixer bits (0x24) from the SFX; keep the
             * music's A/B bits from the current mixer shadow. */
            s_sfx_cbits = (uint8_t)(val & 0x24);
            val = (uint8_t)((s_psg_mixer & 0xdb) | (val & 0x24));
        }
        psg_write(reg, val);
        w += 2;
    }
    s_sfx_ptr   = w;
    s_sfx_delay = w[0];
}

/* ---- compiled levels (.vplay) ------------------------------------------------
 * Interprets the level byte image produced by `vpy_cli compile-asset
 * <file>.vplay --format c`. The layout mirrors the ARM/PiTrex level format
 * (pitrex_load_level / pitrex_show_level / pitrex_update_level) byte-for-byte at
 * the object level, so this is a straight C port of that runtime. The only two
 * differences (a static C array can't hold absolute addresses) are:
 *   - header layer pointers are byte OFFSETS from the image base
 *   - object sprite field is an INDEX into the sprite pointer table supplied to
 *     vpy_load_level (0xFFFFFFFF = no sprite / enemy marker → not drawn here).
 *
 * Header (36 bytes):
 *   +0  xMin i16  +2 xMax i16  +4 yMin i16  +6 yMax i16
 *   +8  bgCount u8 +9 gpCount u8 +10 fgCount u8 +11 pad
 *   +12 bgObjectsOff u32 +16 gpObjectsOff u32 +20 fgObjectsOff u32
 *   +24 scrollLeft i16 +26 scrollRight i16 +28 scrollTop i16 +30 scrollBottom i16
 *   +32 groundBottomOffset i16 +34 pad
 * Object (20 bytes):
 *   +0 x i16 +2 y i16 +4 scale u8 +5 intensity u8 +6 flags u8 +7 type u8
 *   +8 sprite_index u32 +12 half_w u8 +13 half_h u8 +14 vel_x i8 +15 vel_y i8
 *   +16 coll_mesh u32 (unused in C — collision queries deferred)
 * GP mutable buffer entry (8 bytes): x i16, y i16, vx i16, vy i16. */

#define VPY_LEVEL_MAX_GP 64   /* mutable gameplay-object slots */

static const unsigned char       *s_level      = 0;    /* header base */
static const unsigned char *const *s_sprites   = 0;    /* sprite pointer table */
static int  s_cam_x = 0, s_cam_y = 0;
static int  s_gp_count = 0;
static short s_gp_buf[VPY_LEVEL_MAX_GP][4];             /* x, y, vx, vy per GP object */

static int16_t rd_i16(const unsigned char *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

void vpy_set_camera_x(int x) { s_cam_x = x; }
void vpy_set_camera_y(int y) { s_cam_y = y; }
int  vpy_get_camera_x(void)  { return s_cam_x; }
int  vpy_get_camera_y(void)  { return s_cam_y; }

/* ---- level scalar accessors (Phase 1 of the LEVELS bridge) ----------------
 * Bit-exact ports of the inline pitrex_get_scroll_limit_* / pitrex_get_level_
 * floor_y. They read the immutable level header (offsets identical to the ARM
 * `_NAME_LEVEL`): scroll limits i16 at +24/+26/+28/+30, groundBottomOffset i16
 * at +32. No level loaded → 0 (matches the inline zeroed .bss). These read the
 * SAME s_level/s_cam state as vpy_show/update_level, so once the level group is
 * bridged the state lives in exactly one place. NOTE: these are not wired to the
 * codegen bridge yet — the LEVELS group flips ATOMICALLY together with the enemy
 * runtime (see pitrex/libvpy.rs); until then this is dead C code. */
int vpy_get_scroll_limit_left(void)   { return s_level ? (int)rd_i16(s_level + 24) : 0; }
int vpy_get_scroll_limit_right(void)  { return s_level ? (int)rd_i16(s_level + 26) : 0; }
int vpy_get_scroll_limit_top(void)    { return s_level ? (int)rd_i16(s_level + 28) : 0; }
int vpy_get_scroll_limit_bottom(void) { return s_level ? (int)rd_i16(s_level + 30) : 0; }

/* floor_surface_world_y = camera_y - 128 + groundBottomOffset (header +32).
 * Matches pitrex_get_level_floor_y exactly; 0 when no level is loaded. */
int vpy_get_level_floor_y(void)
{
    if (!s_level) return 0;
    return s_cam_y - 128 + (int)rd_i16(s_level + 32);
}

/* Bit-exact ports of pitrex_level_collision_y / _x, reading the position-
 * independent collision mesh (object +16 = byte offset from the level base, 0 =
 * AABB fallback). Mesh block: u32 floor_count; floors[](i16 x1,y1,x2,y2);
 * u32 wall_count; walls[](i16 x,y_min,x,y_max). GP object mutable position comes
 * from s_gp_buf; half_w/half_h/flags/mesh from the level image (single state). */
int vpy_level_collision_y(int px, int py, int hh)
{
    const unsigned char *lvl = s_level;
    if (!lvl || s_gp_count == 0) return -10000;
    int player_feet = py - hh;
    const unsigned char *o = lvl + rd_le32(lvl + 16);   /* GP ROM objects */
    int best = -32767;
    for (int i = 0; i < s_gp_count; i++, o += 20) {
        if (!(o[6] & 0x10)) continue;                    /* collidable */
        int half_w = o[12];
        int ox = s_gp_buf[i][0];
        int dxb = px - ox; if (dxb < 0) dxb = -dxb;
        if (dxb > half_w) continue;                      /* x broadphase */
        uint32_t mesh_off = rd_le32(o + 16);
        if (mesh_off != 0) {
            const unsigned char *m = lvl + mesh_off;
            int seg_count = (int)rd_le32(m); m += 4;
            int local_px = px - s_gp_buf[i][0];
            int obj_world_y = s_gp_buf[i][1];
            for (int s = 0; s < seg_count; s++, m += 8) {
                int x1 = rd_i16(m), y1 = rd_i16(m + 2), x2 = rd_i16(m + 4), y2 = rd_i16(m + 6);
                if (y1 != y2) continue;                  /* non-horizontal */
                int lo = x1 < x2 ? x1 : x2, hi = x1 < x2 ? x2 : x1;
                if (local_px < lo || local_px > hi) continue;
                int world_seg_y = y1 + obj_world_y;
                if (world_seg_y > player_feet) continue; /* above feet */
                if (world_seg_y <= best) continue;       /* not better */
                best = world_seg_y;
            }
        } else {
            int obj_top = s_gp_buf[i][1] + o[13];         /* world_y + half_h */
            int player_center = player_feet + hh;
            if (obj_top > player_center) continue;
            if (obj_top <= best) continue;
            best = obj_top;
        }
    }
    if (best == -32767) return -10000;
    return best + hh;
}

int vpy_level_collision_x(int px, int py, int hw, int hy)
{
    const unsigned char *lvl = s_level;
    if (!lvl || s_gp_count == 0) return 0;
    const unsigned char *o = lvl + rd_le32(lvl + 16);
    int best = 0;
    for (int i = 0; i < s_gp_count; i++, o += 20) {
        if (!(o[6] & 0x10)) continue;                    /* collidable */
        uint32_t mesh_off = rd_le32(o + 16);
        if (mesh_off != 0) {
            const unsigned char *m = lvl + mesh_off;
            int floor_count = (int)rd_le32(m); m += 4;
            m += floor_count * 8;                        /* skip floors */
            int wall_count = (int)rd_le32(m); m += 4;
            for (int w = 0; w < wall_count; w++, m += 8) {
                int wx = rd_i16(m), wy_min = rd_i16(m + 2), wy_max = rd_i16(m + 6);
                int owx = s_gp_buf[i][0], owy = s_gp_buf[i][1];
                int world_wall_x = wx + owx;
                int world_y_min = wy_min + owy, world_y_max = wy_max + owy;
                if (py <= world_y_min - hy) continue;    /* below wall */
                if (py >= world_y_max + hy) continue;    /* above wall */
                int dxr = px - world_wall_x;
                int adx = dxr < 0 ? -dxr : dxr;
                if (adx >= hw) continue;
                int push = hw - adx;
                if (dxr < 0) push = -push;
                best = push;
            }
        } else {
            int obj_hh = o[13], owy = s_gp_buf[i][1];
            int dy = py - owy, ady = dy < 0 ? -dy : dy;
            if (ady >= obj_hh + hy) continue;            /* y-overlap */
            int obj_hw = o[12], owx = s_gp_buf[i][0];
            int dxr = px - owx, total_hw = hw + obj_hw;
            int adxr = dxr < 0 ? -dxr : dxr;
            if (adxr >= total_hw) continue;              /* x-overlap */
            int push = total_hw - adxr;
            if (dxr < 0) push = -push;
            best = push;
        }
    }
    return best;
}

/* no-tree-vectorize: at -Ofast/armv8 gcc auto-vectorizes the 8-byte GP-object
 * copy (x,y,vx,vy → s_gp_buf) into NEON `vldr d16 / vst1.64 {d16},[r2:64]!`.
 * That is the ONLY NEON in all of libvpy, and the VPy PitrexArm32 sim (which
 * runs libvpy for the bridged builtins) has no VFP/NEON register model. Pin
 * this one function to scalar codegen so libvpy stays NEON-free and the sim can
 * execute it — the body is already integer-only; this only changes the copy to
 * plain strh/str, no semantic change. (Prerequisite for a future LEVELS bridge;
 * levels are otherwise DEFERRED — the camera/level state is shared with the
 * still-inline enemy runtime, see pitrex/libvpy.rs.) */
/* GCC-only optimizer hint; Clang (emcc) has no optimize() attribute and would
 * warn -Wunknown-attributes. Guard it so only the GCC/ARM build applies it. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-vectorize")))
#endif
void vpy_load_level(const unsigned char *level, const unsigned char *const *sprites)
{
    s_level   = level;
    s_sprites = sprites;
    s_gp_count = 0;
    if (!level) return;

    int gp = level[9];
    if (gp > VPY_LEVEL_MAX_GP) gp = VPY_LEVEL_MAX_GP;
    s_gp_count = gp;

    /* Copy GP object positions/velocities into the mutable buffer (positions
     * mutate via UPDATE_LEVEL; the ROM image stays the source of sprite/type). */
    uint32_t gp_off = rd_le32(level + 16);
    const unsigned char *o = level + gp_off;
    for (int i = 0; i < gp; i++) {
        s_gp_buf[i][0] = rd_i16(o + 0);          /* x */
        s_gp_buf[i][1] = rd_i16(o + 2);          /* y */
        s_gp_buf[i][2] = (short)(int8_t)o[14];   /* vx (init) */
        s_gp_buf[i][3] = (short)(int8_t)o[15];   /* vy (init) */
        o += 20;
    }
}

/* Draw one object: sprite via index, offset by (x - cam_x, y - cam_y), culled
 * to the screen. Mirrors the per-object body of pitrex_show_level. */
static void level_draw_obj(const unsigned char *o, int wx, int wy)
{
    uint32_t sidx = rd_le32(o + 8);
    if (sidx == 0xFFFFFFFFu || !s_sprites) return;        /* no sprite / enemy */
    const unsigned char *sprite = s_sprites[sidx];
    if (!sprite) return;

    int ox = wx - s_cam_x;
    int oy = wy - s_cam_y;
    /* Cull off-screen (13-unit buffer past the ±127 screen edge), like ARM. */
    if (vpy_abs(ox) > 180 || vpy_abs(oy) > 140) return;

    int intensity = o[5];
    vpy_draw_vector_ex(sprite, ox, oy, 0, intensity);
}

void vpy_show_level(void)
{
    const unsigned char *lvl = s_level;
    if (!lvl) return;

    /* BG layer — positions straight from the ROM image. */
    int bg = lvl[8];
    const unsigned char *o = lvl + rd_le32(lvl + 12);
    for (int i = 0; i < bg; i++, o += 20)
        level_draw_obj(o, rd_i16(o + 0), rd_i16(o + 2));

    /* GP layer — positions from the mutable buffer; skip enemy-type objects
     * (type byte == 1), which a future enemy runtime would draw instead. */
    const unsigned char *g = lvl + rd_le32(lvl + 16);
    for (int i = 0; i < s_gp_count; i++, g += 20) {
        if (g[7] == 1) continue;
        level_draw_obj(g, s_gp_buf[i][0], s_gp_buf[i][1]);
    }

    /* FG layer — positions straight from the ROM image. */
    int fg = lvl[10];
    o = lvl + rd_le32(lvl + 20);
    for (int i = 0; i < fg; i++, o += 20)
        level_draw_obj(o, rd_i16(o + 0), rd_i16(o + 2));
}

void vpy_update_level(void)
{
    const unsigned char *lvl = s_level;
    if (!lvl || s_gp_count == 0) return;

    int y_min = rd_i16(lvl + 4);
    int y_max = rd_i16(lvl + 6);
    const unsigned char *o = lvl + rd_le32(lvl + 16);   /* ROM GP (for flags) */

    for (int i = 0; i < s_gp_count; i++, o += 20) {
        unsigned char flags = o[6];
        if (!(flags & 0x01)) continue;                  /* physics disabled */

        int x  = s_gp_buf[i][0];
        int y  = s_gp_buf[i][1];
        int vx = s_gp_buf[i][2];
        int vy = s_gp_buf[i][3];

        if (flags & 0x02) {                             /* gravity */
            vy -= 1;
            if (vy < -32) vy = -32;                      /* clamp fall speed */
        }
        x += vx;
        y += vy;
        if (y < y_min) { y = y_min; vy = 0; }           /* hit floor */
        if (y > y_max) { y = y_max; vy = 0; }           /* hit ceiling */

        s_gp_buf[i][0] = (short)x;
        s_gp_buf[i][1] = (short)y;
        s_gp_buf[i][3] = (short)vy;
    }
}

/* ---- enemy runtime (Phase 2 of the LEVELS bridge; NOT yet codegen-wired) ----
 * Bit-exact port of the RNG-FREE inline enemy paths (pitrex_spawn_enemies,
 * pitrex_update_enemies' waypoint + area-bounce patrol, pitrex_draw_enemies,
 * pitrex_kill_enemy). The wander AI (ai_type==4) is a turn-3 TODO stub below.
 *
 * Position-independent enemy image `_NAME_ENEMIES_C` (emitted by
 * levelres.rs::emit_enemies_c_bytes — sprite refs are INDICES into a companion
 * sprite table, so NO absolute pointers / no inline `_NAME_VECTORS` reader):
 *   u16 count
 *   per enemy (sequential, self-delimiting via wp_count / area_count):
 *     u16 sprite_index      -> s_enemy_sprites[idx]  (a `_{SPRITE}_VEC` image)
 *     i16 spawn_x, i16 spawn_y
 *     u8  ai_type, u8 wp_count, u8 mirror_on_patrol, u8 default_facing,
 *     u8  is_anim, i8 feet_offset
 *     waypoints[wp_count]:  i16 x, i16 y
 *     u16 area_count; areas[area_count]: i16 y(@x_min), i16 x_min, i16 x_max, i16 y2(@x_max)
 *     u16 trans_count; trans[trans_count]: u8 from,to,type,pad, i16 from_x,to_x
 * The mutable enemy pool lives ONLY here (single state home once bridged). */

#define VPY_MAX_ENEMIES 32
#define VPY_PATROL_SPEED 1

/* Sprite-index sentinel for "no sprite" (null in the inline type_data). */
#define VPY_SPR_NONE 0xFFFF

typedef struct {
    int x, y;                 /* world position (VPy units) */
    int active;
    int ai_type, wp_count, mirror_on_patrol, default_facing, is_anim;
    int sprite_index, feet_offset;
    /* wander sprite swaps (from inline type_data): idle sprite on IDLE, walk
     * sprite on WALK. VPY_SPR_NONE => the inline null pointer => swap is a no-op. */
    int idle_sprite_index, idle_is_anim, walk_sprite_index, walk_is_anim;
    int dir;                  /* 0=left, 1=right */
    int cur_target;           /* waypoint idx (patrol) / target_x (wander pool+14) */
    int sm_state, sub_state, cur_area;
    int idle_timer;           /* pool+8 multiplex: idle_timer/vy/from_x (wander) */
    int w_type;               /* wander transition type (pool+15) */
    int anim_frame_idx, anim_ticks_left;
    const unsigned char *wp_base;   /* into the enemy image: first waypoint pair */
    const unsigned char *areas;     /* into the enemy image: at area_count u16 */
    const unsigned char *sm_data;   /* PI state-machine blob (state sprites + event table), 0 if none */
} VpyEnemy;

static VpyEnemy s_enemies[VPY_MAX_ENEMIES];
static int s_enemy_count = 0;
static const unsigned char *const *s_enemy_sprites = 0;

static int rd_u16(const unsigned char *p) { return (int)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

/* Area-snap cost (mirrors the inline spawn cost fn): |area.y - spawn_y|,
 * +1024 if spawn_x outside [x_min,x_max], +4096 if area.y > spawn_y. Lowest
 * cost wins; ties keep the earlier index (strict `<`). */
static int area_snap_index(const unsigned char *areas, int area_count,
                           int spawn_x, int spawn_y)
{
    /* areas points at: u16 area_count, then areas[]: i16 y, x_min, x_max, y2
     * (8 bytes). The snap COST uses y (@x_min), matching the asm backends. */
    const unsigned char *ap = areas + 2;
    int best_idx = 0, best_cost = 0x10000;
    for (int i = 0; i < area_count; i++, ap += 8) {
        int ay = rd_i16(ap);
        int xmin = rd_i16(ap + 2);
        int xmax = rd_i16(ap + 4);
        int cost = ay - spawn_y; if (cost < 0) cost = -cost;
        if (spawn_x < xmin || spawn_x > xmax) cost += 1024;
        if (ay > spawn_y) cost += 4096;
        if (cost < best_cost) { best_cost = cost; best_idx = i; }
    }
    return best_idx;
}

/* Surface height of an 8-byte area record at world-X `x`. Flat area (y2==y1 or
 * degenerate x-range) → y1. Otherwise linear interpolation between the endpoints
 * (x_min,y1)-(x_max,y2), clamped to the area's x-range. The exact integer form
 *   y1 + (y2 - y1) * (x - x_min) / (x_max - x_min)
 * (signed, truncating division) is what the m6809/ARM/pitrex backends replicate,
 * so this is the single source of truth for sloped-walkable-area Y. */
static int surface_y_at(const unsigned char *area, int x)
{
    int y1 = rd_i16(area);
    int x_min = rd_i16(area + 2);
    int x_max = rd_i16(area + 4);
    int y2 = rd_i16(area + 6);
    if (y2 == y1 || x_max <= x_min) return y1;   /* flat / degenerate */
    if (x <= x_min) return y1;
    if (x >= x_max) return y2;
    return y1 + (int)((long)(y2 - y1) * (long)(x - x_min) / (long)(x_max - x_min));
}

/* PI anim descriptor `_{ANIM}_ANIMC`: [0]=frame_count, then per frame
 * [dur(u8), vec_index(u16)] (3 bytes). Matches emit in levelres.rs. */
static int anim_frame0_dur(const unsigned char *desc) { return desc ? desc[1] : 0; }

/* Set an enemy's current sprite (mirrors pitrex_wander_set_sprite): null index
 * (VPY_SPR_NONE) is a no-op; otherwise store sprite + is_anim, reset the anim
 * frame, and prime anim_ticks_left from the descriptor's frame-0 duration. */
static void enemy_set_sprite(VpyEnemy *en, int idx, int is_anim)
{
    if (idx == VPY_SPR_NONE) return;
    en->sprite_index = idx;
    en->is_anim = is_anim;
    en->anim_frame_idx = 0;
    if (is_anim && s_enemy_sprites)
        en->anim_ticks_left = anim_frame0_dur(s_enemy_sprites[idx]);
}

/* no-tree-vectorize: keep libvpy NEON-free for the PitrexArm32 sim (gcc -Ofast
 * vectorizes the pool-entry init into vmov.i32/vstr d16). See vpy_load_level. */
#if defined(__GNUC__) && !defined(__clang__)   /* GCC-only; Clang lacks optimize() (see vpy_load_level) */
__attribute__((optimize("no-tree-vectorize")))
#endif
void vpy_spawn_enemies(const unsigned char *img, const unsigned char *const *sprites)
{
    s_enemy_count = 0;
    s_enemy_sprites = sprites;
    if (!img) return;

    int total = rd_u16(img);
    const unsigned char *p = img + 2;
    int cam_y = s_cam_y;
    int y_min = cam_y - 150, y_max = cam_y + 150;
    int spawned = 0;

    for (int e = 0; e < total && spawned < VPY_MAX_ENEMIES; e++) {
        int sprite_index = rd_u16(p);
        int spawn_x = rd_i16(p + 2);
        int spawn_y = rd_i16(p + 4);
        int ai_type = p[6];
        int wp_count = p[7];
        int mirror = p[8];
        int facing = p[9];
        int is_anim = p[10];
        int feet_offset = (int)(int8_t)p[11];
        /* wander sprite-swap slots (turn 3): idle @12(u16)+14(u8), walk @15(u16)+17(u8). */
        int idle_sprite_index = rd_u16(p + 12);
        int idle_is_anim = p[14];
        int walk_sprite_index = rd_u16(p + 15);
        int walk_is_anim = p[17];
        int sm_index = rd_u16(p + 18);                      /* PI state-machine blob index (0xFFFF=none) */
        const unsigned char *wp_base = p + 20;
        const unsigned char *ap = wp_base + wp_count * 4;   /* -> area_count u16 */
        int area_count = rd_u16(ap);
        const unsigned char *areas = ap;                     /* points at area_count */
        const unsigned char *tp = ap + 2 + area_count * 8;   /* -> trans_count u16 */
        int trans_count = rd_u16(tp);
        const unsigned char *next = tp + 2 + trans_count * 8;

        /* Y-range spawn filter (matches inline). */
        if (spawn_y < y_min || spawn_y > y_max) { p = next; continue; }

        VpyEnemy *en = &s_enemies[spawned];
        en->x = spawn_x; en->y = spawn_y;
        en->active = 1;
        en->ai_type = ai_type; en->wp_count = wp_count;
        en->mirror_on_patrol = mirror; en->default_facing = facing;
        en->is_anim = is_anim;
        en->sprite_index = sprite_index; en->feet_offset = feet_offset;
        en->idle_sprite_index = idle_sprite_index; en->idle_is_anim = idle_is_anim;
        en->walk_sprite_index = walk_sprite_index; en->walk_is_anim = walk_is_anim;
        en->dir = 1;                 /* right */
        en->cur_target = 0;
        en->sm_state = 0; en->sub_state = 0; en->cur_area = 0; en->idle_timer = 0;
        en->w_type = 0;
        en->anim_frame_idx = 0; en->anim_ticks_left = 0;
        en->wp_base = wp_base;
        en->areas = (area_count > 0) ? areas : 0;
        /* PI state-machine blob (state sprites + event table), by sprite-table
         * index; 0xFFFF => this enemy type has no state machine. */
        en->sm_data = (sm_index != 0xFFFF && sprites) ? sprites[sm_index] : 0;

        /* Area-snap (wander OR patrol with no waypoints), matches inline. */
        int want_snap = (ai_type == 4) || (ai_type == 1 && wp_count == 0);
        if (want_snap && area_count > 0) {
            int bi = area_snap_index(areas, area_count, spawn_x, spawn_y);
            en->cur_area = bi;
            int ay = surface_y_at(areas + 2 + bi * 8, spawn_x);
            en->y = ay + feet_offset;
        }
        /* vanim init: prime frame-0 duration for an animated default sprite. */
        if (is_anim && area_count >= 0)
            en->anim_ticks_left = anim_frame0_dur(sprites ? sprites[sprite_index] : 0);

        p = next;
        spawned++;
    }
    s_enemy_count = spawned;
}

void vpy_kill_enemy(int idx)
{
    if (idx >= 0 && idx < s_enemy_count) s_enemies[idx].active = 0;
}

/* ── Enemy pool accessors ────────────────────────────────────────────────────
 * Bit-exact ports of the inline pitrex_{get,set}_enemy_* call-site bodies (they
 * read/write the 32-byte inline pool at active@12/x@4/y@6/sm_state@18/dir@26).
 * Here they read/write the SAME s_enemies pool the bridged spawn/update/draw
 * own, so enemy state has ONE home. The inline spawn clears every pool slot's
 * active byte, so slots >= s_enemy_count read back 0 — reproduced by the
 * bound-check. Getters mirror the inline load width (active/state: unsigned
 * byte; x/y: sign-extended i16). Setters mirror strh (x/y, 16-bit) / strb
 * (state/dir, low byte). */
int vpy_get_enemy_active(int idx)
{
    if (idx < 0 || idx >= s_enemy_count) return 0;
    return s_enemies[idx].active;
}
int vpy_get_enemy_x(int idx)
{
    if (idx < 0 || idx >= s_enemy_count) return 0;
    return s_enemies[idx].x;
}
int vpy_get_enemy_y(int idx)
{
    if (idx < 0 || idx >= s_enemy_count) return 0;
    return s_enemies[idx].y;
}
int vpy_get_enemy_state(int idx)
{
    if (idx < 0 || idx >= s_enemy_count) return 0;
    return s_enemies[idx].sm_state & 0xFF;
}
void vpy_set_enemy_x(int idx, int x)
{
    if (idx < 0 || idx >= s_enemy_count) return;
    s_enemies[idx].x = (int)(short)x;          /* inline strh: 16-bit, sign-extended on read */
}
void vpy_set_enemy_y(int idx, int y)
{
    if (idx < 0 || idx >= s_enemy_count) return;
    s_enemies[idx].y = (int)(short)y;
}
void vpy_set_enemy_state(int idx, int st)
{
    if (idx < 0 || idx >= s_enemy_count) return;
    s_enemies[idx].sm_state = st & 0xFF;        /* inline strb */
}
void vpy_set_enemy_dir(int idx, int dir)
{
    if (idx < 0 || idx >= s_enemy_count) return;
    s_enemies[idx].dir = dir & 0xFF;            /* inline strb (0=left,1=right) */
}

/* vpy_enemy_fire_event(idx, hash) — dispatch a named event (FNV-1a u8 hash,
 * computed at the call site) through the enemy's state machine, bit-exact vs
 * inline pitrex_enemy_fire_event. The PI state-machine blob (en->sm_data) event
 * table lives at offset 28 + sm_state*20: [event_count u8, 3 pad, then up to 4 x
 * (hash u8, target_state u8, 2 pad)]. First hash match writes target -> sm_state.
 * (The inline reads the same structure at type_data + 44 + sm_state*20; only the
 * header size — hence the base offset — differs, the dispatch loop is identical.) */
void vpy_enemy_fire_event(int idx, int hash)
{
    if (idx < 0 || idx >= s_enemy_count) return;
    VpyEnemy *en = &s_enemies[idx];
    const unsigned char *sm = en->sm_data;
    if (!sm) return;
    int st = en->sm_state & 0xFF;
    const unsigned char *base = sm + 28 + st * 20;
    int ec = base[0];
    const unsigned char *e = base + 4;
    for (int i = 0; i < ec; i++, e += 4) {
        if (e[0] == (hash & 0xFF)) { en->sm_state = e[1]; return; }
    }
}

/* vpy_enemy_fire_event_str(idx, "onSnowHit") — the ergonomic C entry point that
 * matches the VPy `ENEMY_FIRE_EVENT(idx, "name")` builtin. The VPy codegen hashes
 * the event NAME at compile time (FNV-1a, truncated to 8 bits) and calls
 * vpy_enemy_fire_event(idx, hash); C has the string at runtime, so we compute the
 * SAME hash here. The constants/order MUST match pitrex/expressions.rs +
 * assets.rs::fnv1a_u8 exactly (offset basis 2166136261, prime 16777619, XOR each
 * byte after multiply, keep low 8 bits) — a mismatch means the SM transition
 * (whose event table stores those hashes) never fires. */
static int vpy_fnv1a_u8(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h * 16777619u) ^ (uint8_t)*s;
    return (int)(h & 0xFF);
}

void vpy_enemy_fire_event_str(int idx, const char *event)
{
    if (!event) return;
    vpy_enemy_fire_event(idx, vpy_fnv1a_u8(event));
}

/* ── Standalone animation draw: DRAW_ANIM ────────────────────────────────────
 * Bit-exact port of the inline pitrex_draw_anim tick/advance state machine, over
 * the position-independent anim descriptor compiled from a .vanim by
 * `vpy_cli compile-asset <name>.vanim --format c` (VanimResource::compile_to_c_bytes).
 * Descriptor layout (little-endian):
 *   [0] frame_count  [1] loop_flag  [2] base_ref_count  [3] pad
 *   base_ref_count × u16 vec_index          (drawn every call, before the tick)
 *   frame_count    × u16 frame_offset       (byte offset to each frame block)
 *   per frame block: [0] duration_ticks [1] vec_ref_count, then vec_ref_count × u16 vec_index
 * `sprites[i]` is a compiled `.vec` byte array.
 *
 * State: a SINGLE global cursor (frame_idx, ticks_left) shared by all DRAW_ANIM
 * calls, exactly like the inline PITREX_ANIM_STATE_BUF (one animation ticked per
 * frame). ticks_left==0 means "uninitialized" → the first call primes frame 0.
 * speed_mul is fixed at 1 (the common DRAW_ANIM(name, x, y, mirror) form). Vecs
 * are drawn with per-path brightness (intensity 0), matching the inline path
 * where pitrex_draw_vector_ex reads per-path brightness. */
static int s_anim_frame_idx = 0;
static int s_anim_ticks_left = 0;   /* 0 = uninitialized */

static const unsigned char *anim_frame_block(const unsigned char *anim,
                                             int base_ref_count, int fi)
{
    const unsigned char *frame_off_tbl = anim + 4 + base_ref_count * 2;
    return anim + rd_u16(frame_off_tbl + fi * 2);
}

void vpy_draw_anim(const unsigned char *anim, const unsigned char *const *sprites,
                   int x, int y, int mirror)
{
    if (!anim || !sprites) return;
    int frame_count    = anim[0];
    int loop_flag      = anim[1];
    int base_ref_count = anim[2];
    int mir = mirror ? 1 : 0;
    if (frame_count <= 0) return;

    int fi = s_anim_frame_idx;
    int tl = s_anim_ticks_left;

    /* base refs are drawn every call, before tick management (per-path bright). */
    const unsigned char *base_tbl = anim + 4;
    for (int b = 0; b < base_ref_count; b++) {
        const unsigned char *v = sprites[rd_u16(base_tbl + b * 2)];
        if (v) vpy_draw_vector_ex(v, x, y, mir, 0);
    }

    /* tick management — bit-exact pitrex_draw_anim par_tick/advance/freeze. */
    if (tl == 0) {
        /* first call: init frame 0 (fi already 0). */
        tl = anim_frame_block(anim, base_ref_count, fi)[0];
        if (tl < 1) tl = 1;
        s_anim_ticks_left = tl;
    } else {
        tl -= 1;
        if (tl > 0) {
            s_anim_ticks_left = tl;                  /* still ticking, same frame */
        } else {
            fi += 1;                                 /* ticks exhausted: advance */
            if (fi >= frame_count) {
                if (loop_flag) {
                    fi = 0;                          /* loop: wrap to frame 0 */
                    s_anim_frame_idx = fi;
                    tl = anim_frame_block(anim, base_ref_count, fi)[0];
                    if (tl < 1) tl = 1;
                    s_anim_ticks_left = tl;
                } else {
                    fi = frame_count - 1;            /* freeze: stay on last frame */
                    s_anim_frame_idx = fi;
                    s_anim_ticks_left = 1;
                }
            } else {
                s_anim_frame_idx = fi;
                tl = anim_frame_block(anim, base_ref_count, fi)[0];
                if (tl < 1) tl = 1;
                s_anim_ticks_left = tl;
            }
        }
    }

    /* draw the current frame's vec refs (per-path brightness). */
    const unsigned char *fp = anim_frame_block(anim, base_ref_count, fi);
    int vec_ref_count = fp[1];
    const unsigned char *vtbl = fp + 2;
    for (int k = 0; k < vec_ref_count; k++) {
        const unsigned char *v = sprites[rd_u16(vtbl + k * 2)];
        if (v) vpy_draw_vector_ex(v, x, y, mir, 0);
    }
}

void vpy_update_enemies(void)
{
    for (int i = 0; i < s_enemy_count; i++) {
        VpyEnemy *en = &s_enemies[i];
        if (!en->active) continue;
        if (en->sm_state != 0) continue;          /* frozen (snowed/balled) */

        if (en->ai_type == 4) {
            /* ── WANDER (bit-exact port of pitrex_update_enemies' wander branch).
             * RNG: exactly two vpy_rand() sites, mirroring the inline call order:
             *   (a) WALK edge-reversal -> (rand & 0x3F) + 90 -> idle_timer.
             *   (b) IDLE expiry: per matching transition, rand & 3 == 0 commits.
             * vpy_rand == pitrex_random (same LCG, seed 0). CAVEAT: a program
             * using BOTH VPy rand() and wander enemies shares one s_rng stream
             * (bridged) vs two independent streams (inline) — the documented
             * rand caveat, now applying to enemies too. */
            const unsigned char *ap = en->areas;
            if (!ap) continue;
            int area_count = rd_u16(ap);
            if (area_count == 0) continue;
            const unsigned char *area_base = ap + 2;                 /* areas[] (8B stride) */
            const unsigned char *tp = area_base + area_count * 8;    /* -> trans_count u16 */
            int trans_count = rd_u16(tp);
            const unsigned char *trans_base = tp + 2;                /* trans[] (8B stride) */
            const int SPEED = VPY_PATROL_SPEED;

            if (en->sub_state == 3) {
                /* WALK_TO_TAKEOFF: walk X toward from_x (idle_timer). */
                int from_x = en->idle_timer, x = en->x, reached = 0;
                int dx = from_x - x;
                if (dx == 0) reached = 1;
                else if (dx > 0) { en->dir = 1; x += SPEED; if (x > from_x) x = from_x; en->x = x; reached = (x == from_x); }
                else             { en->dir = 0; x -= SPEED; if (x < from_x) x = from_x; en->x = x; reached = (x == from_x); }
                if (!reached) continue;
                /* takeoff -> AIRBORNE: pick vy0 by transition type. Target Y is
                 * the destination area's surface at the landing X (target_x). */
                int ty = surface_y_at(area_base + en->cur_area * 8, en->cur_target);
                int dy = ty - en->y;
                int vy0;
                if (en->w_type == 2) vy0 = -1;              /* drop */
                else if (en->w_type == 3) vy0 = 3;          /* jump_across */
                else { vy0 = 4; while (vy0 * (vy0 + 1) / 2 < dy && vy0 < 16) vy0++; }  /* jump_up */
                en->idle_timer = vy0;                        /* pool+8 = vy */
                en->sub_state = 2;                           /* AIRBORNE */
                en->dir = (en->cur_target >= en->x) ? 1 : 0; /* face target_x */
                continue;
            }
            if (en->sub_state == 2) {
                /* AIRBORNE: one clean parabola, matching the m6809 runtime.
                 * Step X toward target_x by AIR_X=2 (clamped, never overshoots)
                 * and run the parabolic Y arc (y += vy; vy -= 1; clamp vy>=-4).
                 * LAND the instant the *descending* arc crosses the target
                 * platform Y. The landing is Y-crossing-driven (not "X reached
                 * target_x, then lerp Y back up"), so the motion is a single
                 * continuous arc — no halfway stop + second hop. Termination
                 * needs no timer: after the peak, gravity pulls Y down without
                 * bound, so y<=target_y becomes (and stays) true within a
                 * bounded number of frames for every transition type; drop
                 * enters with vy<0 and lands as soon as it descends. */
                const int AIR_X = 2;
                int x = en->x, target_x = en->cur_target;
                if (x < target_x)      { x += AIR_X; if (x > target_x) x = target_x; en->x = x; }
                else if (x > target_x) { x -= AIR_X; if (x < target_x) x = target_x; en->x = x; }
                int y = en->y, vy = en->idle_timer;
                y += vy; en->y = y;
                vy -= 1; if (vy < -4) vy = -4; en->idle_timer = vy;
                /* target_y = target area's surface at the landing X + feet_offset. */
                int ty = surface_y_at(area_base + en->cur_area * 8, en->cur_target) + en->feet_offset;
                if ((en->w_type == 2 || vy < 0) && y <= ty) {
                    en->y = ty;                              /* snap onto platform */
                    en->sub_state = 0;                       /* WALK */
                }
                continue;
            }
            if (en->sub_state == 1) {
                /* IDLE: count down; on expiry pick a transition (RNG coin). */
                if (--en->idle_timer > 0) continue;
                if (trans_count == 0) {
                    en->sub_state = 0;
                    enemy_set_sprite(en, en->walk_sprite_index, en->walk_is_anim);
                    continue;
                }
                int cur = en->cur_area, hit = -1;
                for (int ti = 0; ti < trans_count; ti++) {
                    const unsigned char *t = trans_base + ti * 8;
                    if (t[0] != cur) continue;
                    if ((vpy_rand() & 3) == 0) { hit = ti; break; }
                }
                if (hit < 0) {
                    en->sub_state = 0;
                    enemy_set_sprite(en, en->walk_sprite_index, en->walk_is_anim);
                    continue;
                }
                const unsigned char *t = trans_base + hit * 8;
                en->cur_area = t[1];                          /* to (target area) */
                en->w_type = t[2];                           /* transition type */
                en->idle_timer = rd_i16(t + 4);              /* from_x (pool+8) */
                en->cur_target = rd_i16(t + 6);              /* to_x (pool+14) */
                en->sub_state = 3;                           /* WALK_TO_TAKEOFF */
                enemy_set_sprite(en, en->walk_sprite_index, en->walk_is_anim);
                continue;
            }
            /* WALK (sub_state 0): bounce X within the current area's edges.
             * On a sloped area the surface Y follows X, so re-derive Y from the
             * enemy's X every walked frame (flat area → surface_y_at == y1). */
            const unsigned char *area = area_base + en->cur_area * 8;
            int x_min = rd_i16(area + 2), x_max = rd_i16(area + 4);
            int x = en->x, edge = 0;
            if (en->dir == 1) {
                if (x >= x_max) edge = 1;
                else { x += SPEED; if (x > x_max) x = x_max; en->x = x; en->y = surface_y_at(area, x) + en->feet_offset; edge = (x == x_max); }
            } else {
                if (x <= x_min) edge = 1;
                else { x -= SPEED; if (x < x_min) x = x_min; en->x = x; en->y = surface_y_at(area, x) + en->feet_offset; edge = (x == x_min); }
            }
            if (!edge) continue;
            en->dir ^= 1;
            en->idle_timer = (vpy_rand() & 0x3F) + 90;       /* idle_timer */
            en->sub_state = 1;                                /* IDLE */
            enemy_set_sprite(en, en->idle_sprite_index, en->idle_is_anim);
            continue;
        }
        if (en->ai_type != 1) continue;           /* unsupported */

        if (en->wp_count == 0) {
            /* area-bounded X-bounce patrol (Y follows a sloped area's surface) */
            if (!en->areas) continue;
            const unsigned char *area = en->areas + 2 + en->cur_area * 8;
            int xmin = rd_i16(area + 2);
            int xmax = rd_i16(area + 4);
            int x = en->x;
            if (en->dir == 1) {
                if (x >= xmax) { en->dir ^= 1; continue; }
                x += VPY_PATROL_SPEED; if (x > xmax) x = xmax;
                en->x = x; en->y = surface_y_at(area, x) + en->feet_offset;
                if (x != xmax) continue;
            } else {
                if (x <= xmin) { en->dir ^= 1; continue; }
                x -= VPY_PATROL_SPEED; if (x < xmin) x = xmin;
                en->x = x; en->y = surface_y_at(area, x) + en->feet_offset;
                if (x != xmin) continue;
            }
            en->dir ^= 1;
            continue;
        }
        if (en->wp_count < 2) continue;           /* wp_count==1 invalid */

        /* classic waypoint patrol (X then Y toward wp[cur_target]) */
        const unsigned char *wp = en->wp_base + en->cur_target * 4;
        int tx = rd_i16(wp);
        int ty = rd_i16(wp + 2);
        int x = en->x, y = en->y;
        int S = VPY_PATROL_SPEED;

        int dx = tx - x;
        if (dx != 0) {
            en->dir = (dx > 0) ? 1 : 0;
            if (dx > 0) { if (dx <= S) x = tx; else x += S; }
            else        { int adx = -dx; if (adx <= S) x = tx; else x -= S; }
        }
        int dy = ty - y;
        if (dy != 0) {
            if (dy > 0) { if (dy <= S) y = ty; else y += S; }
            else        { int ady = -dy; if (ady <= S) y = ty; else y -= S; }
        }
        en->x = x; en->y = y;
        if (x == tx && y == ty) {
            int nt = en->cur_target + 1;
            if (nt >= en->wp_count) nt = 0;
            en->cur_target = nt;
        }
    }
}

void vpy_draw_enemies(void)
{
    int cam_x = s_cam_x, cam_y = s_cam_y;
    for (int i = 0; i < s_enemy_count; i++) {
        VpyEnemy *en = &s_enemies[i];
        if (!en->active) continue;

        /* sm_state==0 -> default sprite (en->sprite_index). sm_state>0 (frozen:
         * snowed/balled) -> the state sprite from the PI state-machine blob at
         * [4 + state*2] (u16 index) with is_anim at [20 + state], mirroring the
         * inline pitrex_draw_enemies (type_data[sm_state] sprite, null -> default). */
        int sidx = en->sprite_index;
        int is_anim = en->is_anim;
        if (en->sm_state != 0 && en->sm_data) {
            int st = en->sm_state & 0xFF;
            int st_sidx = rd_u16(en->sm_data + 4 + st * 2);
            if (st_sidx != 0xFFFF) {
                sidx = st_sidx;
                is_anim = en->sm_data[20 + st];
            }
        }
        if (!s_enemy_sprites) continue;
        const unsigned char *sprite = s_enemy_sprites[sidx];
        if (!sprite) continue;

        int ox = en->x - cam_x;
        if (ox < 0 ? (-ox > 180) : (ox > 180)) continue;
        int oy = en->y - cam_y;
        if (oy < 0 ? (-oy > 140) : (oy > 140)) continue;

        int mirror = 0;
        if (en->mirror_on_patrol)
            mirror = (en->default_facing ^ en->dir ^ 1) & 1;

        if (!is_anim) {
            /* override_b=0 -> use the .vec's own per-path brightness, matching
             * the inline pitrex_draw_vector_ex (which ignores its intensity arg
             * and reads per-path brightness unless a SET_INTENSITY override). */
            vpy_draw_vector_ex(sprite, ox, oy, mirror, 0);
        } else {
            /* Anim tick/extract — bit-exact port of pitrex_draw_enemies' vanim
             * branch, over the PI anim descriptor `_{ANIM}_ANIMC`:
             *   [0]=frame_count, then per frame [dur(u8), vec_index(u16)].
             * `sprite` here is the descriptor; each frame's vec_index resolves to
             * a static `_{FRAME}_VEC` in the same sprite table. Tick: ticks-1; if
             * >0 keep frame, else advance (wrap) and reload dur from the new frame.
             * Draw the current frame's vec. (Matches the inline tick order.) */
            const unsigned char *anim = sprite;
            int frame_count = anim[0];
            int ti = en->anim_ticks_left - 1;
            int fi = en->anim_frame_idx;
            if (ti > 0) {
                en->anim_ticks_left = ti;                /* keep frame */
            } else {
                fi++; if (fi >= frame_count) fi = 0;      /* advance, wrap */
                en->anim_frame_idx = fi;
                ti = anim[1 + fi * 3];                    /* new frame duration */
                en->anim_ticks_left = ti;
            }
            const unsigned char *fp = anim + 1 + fi * 3;
            int vec_index = fp[1] | (fp[2] << 8);         /* frame vec sprite-index */
            const unsigned char *vec = s_enemy_sprites[vec_index];
            if (vec) vpy_draw_vector_ex(vec, ox, oy, mirror, 0); /* per-path brightness */
        }
    }
}
