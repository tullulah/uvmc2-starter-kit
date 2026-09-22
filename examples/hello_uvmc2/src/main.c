/* hello_uvmc2 — the smallest complete UVMC2 game.
 *
 * It exists to be COPIED. Everything a cartridge game needs and nothing else: a
 * frame loop, drawing, the controller, and one sound. If you are starting a new
 * game, start from this directory, not from game/tacscan (which is an arcade
 * emulator and carries an emulator's worth of scaffolding).
 *
 * WHAT DRAWS IT. The game only ever calls libvpy (vpy.c), which calls
 * v_directDraw32 in sdk_rp2350.c, which calls the SDK inside the image
 * (uvm2_draw.c). The SDK turns each stroke into VIA writes with delays — the
 * COMMAND LIST — and on this build core 1 replays that list onto the Vectrex bus
 * through PIO and DMA while core 0 is already building the next frame. None of
 * that is visible from here, which is the point: see <kit>/docs/ for what
 * happens underneath.
 *
 * COORDINATES. VPy's logical screen is x,y in [-127, +127], with (0,0) at the
 * centre and +y UP. Brightness is 0..127; 0 means "do not draw".
 */
#include <vpy.h>

/* ── state ─────────────────────────────────────────────────────────────── */
static int  s_angle;        /* 0..127 = a full circle (vpy_sin/vpy_cos units) */
static int  s_x, s_y;       /* the ship, in VPy units */
static int  s_beeping;

/* A ship, as a closed polygon in its own space. It is rotated and translated
 * per frame instead of being stored per orientation: at 50 Hz the RP2350 has
 * cycles to spare, and a table of rotations is memory this cartridge would
 * rather spend on the command list. */
static const int SHIP[][2] = { { 0, 14 }, { -9, -10 }, { 0, -5 }, { 9, -10 } };
#define SHIP_N ((int)(sizeof SHIP / sizeof SHIP[0]))

static void draw_ship(int cx, int cy, int a, int b)
{
    const int sn = vpy_sin(a), cs = vpy_cos(a);   /* -127..127 */
    int px = 0, py = 0;
    for (int i = 0; i <= SHIP_N; i++) {
        const int k = i % SHIP_N;                 /* the last edge closes the shape */
        const int x = cx + (SHIP[k][0] * cs - SHIP[k][1] * sn) / 127;
        const int y = cy + (SHIP[k][0] * sn + SHIP[k][1] * cs) / 127;
        /* Chained strokes: each one starts where the previous ended, so the beam
         * pays no blanked jump between them. That is the single cheapest habit
         * there is on this hardware — see docs/04-drawing.md. */
        if (i) vpy_draw_line(px, py, x, y, b);
        px = x; py = y;
    }
}

/* ── the frame ─────────────────────────────────────────────────────────── */
static void setup(void)
{
    s_x = 0; s_y = -20; s_angle = 0;
    vpy_set_text_size(2);
}

static void loop(void)
{
    /* The input snapshot was refreshed by vpy_frame_begin(), which vpy_run()
     * calls for us. currentJoy1X/Y are -127..127; currentButtonState has one bit
     * per button; vpy_j1_button(n) returns 0/1 for n = 1..4. */
    const int jx = vpy_j1_x();
    const int jy = vpy_j1_y();

    if (jx >  32) s_angle = (s_angle - 1) & 127;
    if (jx < -32) s_angle = (s_angle + 1) & 127;
    if (jy >  32) { s_x += vpy_sin(s_angle) / 32; s_y += vpy_cos(s_angle) / 32; }

    s_x = vpy_clamp(s_x, -110, 110);
    s_y = vpy_clamp(s_y, -110, 110);

    /* Button 1: a tone straight into the PSG. `vpy_tone` writes channel A's
     * period and volume; a Vectrex has three square-wave channels and no DAC. */
    if (vpy_j1_button(1)) {
        if (!s_beeping) { vpy_tone(400, 12); s_beeping = 1; }
    } else if (s_beeping) {
        vpy_tone(0, 0); s_beeping = 0;
    }

    /* Drawing. Order matters a little and brightness matters a lot: everything
     * drawn in one frame shares the beam's time, so a frame with more strokes is
     * a frame that takes longer — there is no vsync to hide behind. */
    vpy_print_text(-60, 100, "HELLO UVMC2");
    vpy_draw_rect(-120, -120, 240, 240, 40);      /* a dim border */
    draw_ship(s_x, s_y, s_angle, 110);
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
