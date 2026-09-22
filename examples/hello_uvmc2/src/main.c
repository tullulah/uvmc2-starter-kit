/* hello_uvmc2 — the smallest complete UVMC2 game.
 *
 * It exists to be COPIED. Everything a cartridge game needs and nothing else: a
 * frame loop, drawing, the controller, and one sound. If you are starting a new
 * game, start from this directory, not from game/tacscan (which is an arcade
 * emulator and carries an emulator's worth of scaffolding).
 *
 * CONTROLS
 *   button 1        thrust, in the direction the nose points
 *   buttons 2 / 3   rotate left / right
 *   stick left/right also rotates (see THE STICK GUARD below)
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

/* ── tuning ────────────────────────────────────────────────────────────────
 * Position and velocity are kept in 1/16 of a VPy unit so a single frame of
 * thrust is not a visible jump. Everything below is in those sixteenths. */
#define Q          16
#define EDGE      (120 * Q)   /* wrap here, just inside the ±127 screen */
#define THRUST      5         /* velocity added per frame of button 1 */
#define FRICTION    6         /* v -= v/FRICTION each frame: a short glide */
#define TURN        2         /* angle steps per frame while turning */

/* ── state ─────────────────────────────────────────────────────────────── */
static int s_angle;           /* 0..127 = a full circle (vpy_sin/vpy_cos units) */
static int s_x, s_y;          /* position, in 1/16 VPy units */
static int s_vx, s_vy;        /* velocity, in 1/16 VPy units per frame */
static int s_thrusting;

/* THE STICK GUARD, AND WHY IT IS HERE.
 *
 * The analog stick is read by a successive-approximation conversion against the
 * Vectrex's own comparator (uvm2_input.c). When it works, a centred stick reads
 * near zero. When it does not — a disconnected controller, or a conversion that
 * never settles — the axis sits pinned at an extreme, and a demo that rotates
 * from the stick then spins for ever and cannot be played. That is exactly how
 * the first version of this example failed on hardware.
 *
 * So the stick is not trusted until it has been SEEN near centre at least once.
 * Nobody is holding the stick over while the cartridge menu loads, so a healthy
 * stick passes this in the first frame and pays nothing; a pinned one is ignored
 * and the buttons still fly the ship.
 *
 * The readout at the bottom of the screen shows the raw values either way, so
 * "the stick does nothing" is always answerable by looking. */
static int s_stick_ok;

/* A ship, as a closed polygon in its own space, nose along +y. It is rotated and
 * translated per frame instead of being stored per orientation: at 50 Hz the
 * RP2350 has cycles to spare, and a table of rotations is memory this cartridge
 * would rather spend on the command list. */
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
    /* A flame while thrusting, so the direction of travel is visible. Drawn as
     * its own figure, which costs one blanked jump. */
    if (s_thrusting) {
        const int tx = cx + (0 * cs - (-18) * sn) / 127;
        const int ty = cy + (0 * sn + (-18) * cs) / 127;
        const int bx = cx + (-5 * cs - (-10) * sn) / 127;
        const int by = cy + (-5 * sn + (-10) * cs) / 127;
        const int ex = cx + ( 5 * cs - (-10) * sn) / 127;
        const int ey = cy + ( 5 * sn + (-10) * cs) / 127;
        vpy_draw_line(bx, by, tx, ty, 90);
        vpy_draw_line(tx, ty, ex, ey, 90);
    }
}

/* ── the frame ─────────────────────────────────────────────────────────── */
static void setup(void)
{
    s_x = 0; s_y = -20 * Q; s_angle = 0;
}

static void loop(void)
{
    /* The input snapshot was refreshed by vpy_frame_begin(), which vpy_run()
     * calls for us. vpy_j1_x/y are -127..127; vpy_j1_button(n) is 1 while
     * button n (1..4) is held. */
    const int jx = vpy_j1_x();
    const int jy = vpy_j1_y();
    /* The four buttons as one number, so the readout below can show them all.
     * Built from vpy_j1_button() rather than reading currentButtonState, which
     * lives in <vectrex/vectrexInterface.h> — this example keeps to one header. */
    const int btn = vpy_j1_button(1) | (vpy_j1_button(2) << 1)
                  | (vpy_j1_button(3) << 2) | (vpy_j1_button(4) << 3);

    if (jx > -40 && jx < 40) s_stick_ok = 1;      /* see THE STICK GUARD above */

    /* --- turn --- */
    if (vpy_j1_button(2) || (s_stick_ok && jx < -40)) s_angle = (s_angle + TURN) & 127;
    if (vpy_j1_button(3) || (s_stick_ok && jx >  40)) s_angle = (s_angle - TURN) & 127;

    /* --- thrust, along the nose --- */
    /* The nose is (0,1) rotated by s_angle, which is (-sin, cos). */
    s_thrusting = vpy_j1_button(1);
    if (s_thrusting) {
        s_vx += (-vpy_sin(s_angle) * THRUST) / 127;
        s_vy += ( vpy_cos(s_angle) * THRUST) / 127;
        vpy_tone(300, 8);                          /* PSG channel A while thrusting */
    } else {
        vpy_tone(0, 0);
    }

    /* --- drift, with enough friction that it never runs away --- */
    s_vx -= s_vx / FRICTION;
    s_vy -= s_vy / FRICTION;
    s_x  += s_vx;
    s_y  += s_vy;

    /* Wrap instead of clamping. A clamp lets the ship sit pinned against an edge
     * with nothing to show why; wrapping means it is always reachable. */
    if (s_x >  EDGE) s_x = -EDGE;
    if (s_x < -EDGE) s_x =  EDGE;
    if (s_y >  EDGE) s_y = -EDGE;
    if (s_y < -EDGE) s_y =  EDGE;

    /* --- draw --- */
    /* The title, centred. The left edge is not a guess: libvpy's font advances by
     * a per-glyph amount baked into the glyph stream, so a string's width has to
     * be MEASURED. At text size 12, "HELLO UVMC2" is 136.1 of the screen's 254
     * VPy units wide, which puts its left edge at -68:
     *
     *   cc -O2 -I<kit>/sdk/vpy-c/include -I<kit>/sdk/pitrex-sim/include \
     *      -o /tmp/tm tools/text_metrics.c <kit>/sdk/vpy-c/vpy.c
     *   /tmp/tm 12 "HELLO UVMC2"
     */
    vpy_set_text_size(12);
    vpy_print_text(-68, 96, "HELLO UVMC2");

    vpy_draw_rect(-120, -120, 240, 240, 40);      /* a dim border */
    draw_ship(s_x / Q, s_y / Q, s_angle, 110);

    /* THE INPUT, ON SCREEN. Delete these four lines once your game works — they
     * are here because "the ship will not move" and "the stick reads 127 all the
     * time" look identical from the sofa, and only one of them is a bug in your
     * code. X and Y are the raw axes, B is the button bits (1|2|4|8). */
    vpy_set_text_size(6);
    vpy_print_text(-118, -104, "X");     vpy_print_number(-104, -104, jx);
    vpy_print_text(  -50, -104, "Y");    vpy_print_number( -36, -104, jy);
    vpy_print_text(   18, -104, "B");    vpy_print_number(  32, -104, btn);
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
