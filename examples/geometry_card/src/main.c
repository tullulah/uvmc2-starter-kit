/* geometry_card — what shape is a unit on the glass, and where does the glass end?
 *
 * A test card to be PHOTOGRAPHED. A photograph of a CRT gives geometry, not
 * brightness, and geometry is the whole question here, so it is the right tool.
 *
 * THE QUESTION. vpy3d's projection (2026-09-30) scales x by 4/3, on the argument
 * that the size pots stretch each axis to its own edge of a 4:3 tube on end, so
 * one y unit covers 1.33 times the glass of one x unit. The PiTrex contract says
 * the opposite: one unit is one unit on both axes, and the screen is a portrait
 * WINDOW, roughly x:[-18000,18000] by y:[-24000,24000]
 * (pitrex-sim/include/vectrex/vectrexInterface.h). The BIOS draws x and y with a
 * single scale factor, and its circles are round. This card decides between the
 * two on a real console — and it has to be more than one console.
 *
 * EVERYTHING IS IN DEVICE UNITS, through vpy_draw_line_dev, i.e. exactly what
 * v_directDraw32 receives. No vpy3d, no logical-unit scaling: whatever the card
 * shows is the beam, not a projection.
 *
 * WHAT TO READ OFF THE PHOTO
 *
 *   1. THE SQUARE AND THE CIRCLE (16000 x 16000, r 8000, with the diagonals).
 *      Measure the square's width and height on the photo. 1.00 means a unit is
 *      the same on both axes and the 4:3 correction is wrong. 1.33 wide means
 *      the 4:3 argument holds. The circle and the 45-degree diagonals say the
 *      same thing in a way that can be judged by eye.
 *
 *   2. THE RULERS, along both axes. A tick every 1000 units, a long one every
 *      2000, and a label every 4000 in thousands. Where the ticks go off the
 *      glass is the visible window in device units, per axis — the numbers a
 *      per-axis clip would use. They run past the contract window on purpose.
 *
 *   3. TWO RECTANGLES. The brighter is the contract window (±18000 x ±24000);
 *      the dim one is vpy3d's square clip (±15500), i.e. the only part of the
 *      glass a vpy3d game can draw on today.
 *
 * The photo needs the bezel or the glass edge in frame, and the camera square on
 * to the tube: a camera at an angle turns a square into a trapezoid, and the
 * diagonals are there to show it when it happens.
 *
 * CONTROLS
 *   button 1   brightness: 60 -> 90 -> 127, for the exposure
 *
 * THE BOTTOM LINE is the evidence that the frame is the frame: if DROP or SHED
 * is not zero, the card on screen is not the card that was built, and the photo
 * is not evidence of anything. CLMP counts coordinates libvpy pulled back to its
 * int16 range; it must be 0 too.
 */
#include <vpy.h>
#include <uvm2_bus.h>

/* ── the card, in device units ─────────────────────────────────────────────── */
#define SQ_HALF     8000      /* the square is 16000 a side: well inside any window */
#define CIRCLE_SEG  64
#define TICK_STEP   1000
#define TICK_SHORT  250       /* a tick's half length */
#define TICK_LONG   600
#define LABEL_STEP  4000
/* How far the rulers run. Past the contract's window (18000 / 24000) by enough
 * that the glass, not the ruler, is what ends first — and inside libvpy's
 * VPY_DEV_MAX of 32000, so CLMP stays at 0. */
#define RULE_X      26000
#define RULE_Y      30000
/* The two windows being compared. */
#define CONTRACT_X  18000     /* vectrexInterface.h: "roughly x:[-18000,18000]" */
#define CONTRACT_Y  24000     /*                     "y:[-24000,24000]"         */
#define VPY3D_CLIP  15500     /* vpy3d.c: s_clip, the same in x and y */

/* The Q14 trig's units (vpy.h: "4096 steps per turn, 16384 = 1.0"). Newer SDKs
 * name them in vpy.h; #ifndef so this builds against either. */
#ifndef VPY_Q14_TURN
#define VPY_Q14_TURN 4096
#endif
#ifndef VPY_Q14_ONE
#define VPY_Q14_ONE  16384
#endif

/* VPy's logical unit, for placing text: logical * 127 = device. vpy.c VPY_SCALE. */
#define DEV_PER_LOGICAL 127

static const int s_levels[] = { 60, 90, 127 };
static int s_level = 1;
static int s_held;

static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int b)
{
    vpy_draw_line_dev(x0, y0, x1, y1, b);
}

static void rect(int32_t hx, int32_t hy, int b)
{
    line(-hx, -hy,  hx, -hy, b);
    line( hx, -hy,  hx,  hy, b);
    line( hx,  hy, -hx,  hy, b);
    line(-hx,  hy, -hx, -hy, b);
}

/* A label in thousands, at a device-unit position. libvpy's text is in logical
 * units, so the position is divided down; the label's own offset is a
 * readability choice, not a measurement. No sign: the font draws '-' as a
 * blank, and the card is symmetric. */
static void label(int32_t x, int32_t y, int32_t value)
{
    char buf[8];
    int  i = 0;
    int32_t v = (value < 0 ? -value : value) / 1000;
    if (v >= 10) buf[i++] = (char)('0' + v / 10);
    buf[i++] = (char)('0' + v % 10);
    buf[i] = 0;
    vpy_print_text((int)(x / DEV_PER_LOGICAL), (int)(y / DEV_PER_LOGICAL), buf);
}

static void rulers(int b)
{
    line(-RULE_X, 0, RULE_X, 0, b);
    line(0, -RULE_Y, 0, RULE_Y, b);
    for (int32_t t = TICK_STEP; t <= RULE_X; t += TICK_STEP) {
        const int32_t h = (t % (2 * TICK_STEP)) ? TICK_SHORT : TICK_LONG;
        line( t, -h,  t, h, b);
        line(-t, -h, -t, h, b);
        if (t % LABEL_STEP == 0) {
            label( t - 400, -1500,  t);
            label(-t - 700, -1500, -t);
        }
    }
    for (int32_t t = TICK_STEP; t <= RULE_Y; t += TICK_STEP) {
        const int32_t h = (t % (2 * TICK_STEP)) ? TICK_SHORT : TICK_LONG;
        line(-h,  t, h,  t, b);
        line(-h, -t, h, -t, b);
        if (t % LABEL_STEP == 0) {
            label(1000,  t + 300,  t);
            label(1000, -t + 300, -t);
        }
    }
}

static void square_and_circle(int b)
{
    rect(SQ_HALF, SQ_HALF, b);
    line(-SQ_HALF, -SQ_HALF, SQ_HALF,  SQ_HALF, b);
    line(-SQ_HALF,  SQ_HALF, SQ_HALF, -SQ_HALF, b);
    int32_t px = SQ_HALF, py = 0;
    for (int i = 1; i <= CIRCLE_SEG; i++) {
        const int a = i * VPY_Q14_TURN / CIRCLE_SEG;
        const int32_t x = (int32_t)(((int64_t)SQ_HALF * vpy_cos_q14(a)) / VPY_Q14_ONE);
        const int32_t y = (int32_t)(((int64_t)SQ_HALF * vpy_sin_q14(a)) / VPY_Q14_ONE);
        line(px, py, x, y, b);
        px = x; py = y;
    }
}

static void readout(void)
{
    const vpy_draw_stats_t *s = vpy_draw_stats();
    vpy_set_text_size(6);
    /* Logical units; inside the ±15500 square on purpose, so the readout is on
     * the glass on any console that can show anything at all, and in the lower
     * left quadrant, clear of the rulers and the square. */
    vpy_print_text(-115, -76, "DROP");  vpy_print_number(-85, -76, (long)(s->dropped + uvm2_stats.dropped));
    vpy_print_text(-115, -88, "SHED");  vpy_print_number(-85, -88, (long)s->shed);
    vpy_print_text(-115, -100, "CLMP"); vpy_print_number(-85, -100, (long)s->clamped);
}

static void setup(void) {}

static void loop(void)
{
    const int b1 = vpy_j1_button(1);
    if (b1 && !s_held) s_level = (s_level + 1) % (int)(sizeof s_levels / sizeof s_levels[0]);
    s_held = b1;
    const int b = s_levels[s_level];

    rect(CONTRACT_X, CONTRACT_Y, b * 2 / 3);
    rect(VPY3D_CLIP, VPY3D_CLIP, b / 3);
    rulers(b);
    square_and_circle(b);

    vpy_set_text_size(8);
    vpy_print_text(-115, 100, "GEOMETRY");
    readout();
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
