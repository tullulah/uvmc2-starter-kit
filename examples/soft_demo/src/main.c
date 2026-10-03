/* soft_demo — things that bend: a flag in the wind, a jelly blob, a jelly cube.
 *
 * vpysoft does the bodies (sdk/vpy-c/include/vpysoft.h): points joined by
 * springs on a Verlet core, the same as the ropes, with a stiffness per spring
 * so they GIVE. vpy3d draws them in a little scene: a floor, a flagpole.
 *
 *   the FLAG is a cloth pinned along its left edge to the pole; the wind blows
 *     it out and gusts make it ripple.
 *   the BLOB is a ring round a centre that keeps its AREA: dropped, it squashes
 *     on the floor and springs back round; punched, it wobbles.
 *   the CUBE is a vpy3d box whose every edge is a soft spring, braced to a hidden
 *     centre: a jelly that lands, sags and settles.
 *
 * CONTROLS
 *   stick left/right   the wind: which way and how hard (a breeze when centred)
 *   button 1           drop the blob from above
 *   button 2           punch the blob (sideways and up)
 *   button 3           drop the jelly cube
 *   button 4           start again
 *
 * THE READOUT: STK strokes this frame, DROP must be 0, STR the worst spring's
 * stretch (units longer than its length), AREA how far the blob is from its
 * area (%), SHED springs left out by the stroke budget (should stay 0 here).
 */
#include <vpy.h>
#include <vpy3d.h>
#include <vpysoft.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#endif

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define GRAVITY       9800    /* mm/s²: Earth */
#define FLOOR_HALF    1200    /* the floor grid spans +-this in x and z */
#define FLOOR_STEP     300
#define POLE_X       (-900)
#define POLE_Z         200
#define POLE_H        1400
/* The flag: FLAG_W × FLAG_H points FLAG_CELL apart, its top-left at the pole's top. */
#define FLAG_W         10
#define FLAG_H          6
#define FLAG_CELL      90
#define FLAG_STIFF    200     /* Q8: cloth that barely stretches */
#define BLOB_X        250     /* clear of the flag, which flies out to about x = 0 */
#define BLOB_DROP_Y  1300
#define BLOB_N         16     /* rim points: round enough at this size */
#define BLOB_R        200
#define BLOB_STIFF     60     /* Q8: soft rim, so it squashes visibly */
#define BLOB_PRESSURE  64     /* Q8: a quarter of the area error back each pass: it
                                * squashes on landing, then fills out again */
#define CUBE_X        750
#define CUBE_DROP_Y  1300
#define CUBE_HALF     150
#define CUBE_STIFF     90     /* Q8: a wobbly jelly */
#define PUNCH_X      1400     /* mm/s: the punch's sideways speed, towards the middle */
#define PUNCH_Y      2400     /* ...and its lift: up more than across, so it stays in view */
/* THE WIND. A breeze even with the stick centred, so the flag flies; the stick
 * adds up to WIND_MAX either way. Gusts change by up to GUST each half second,
 * and the flutter is a sideways (z) sway — a uniform push alone would only hold
 * the flag out stiff. */
#define BREEZE        700     /* mm/s */
#define WIND_MAX     2600
#define GUST          500
#define GUST_FRAMES    25
#define FLUTTER       900     /* mm/s, the z sway's amplitude */
#define FLUTTER_TURN   37     /* frames per sway: not a divisor of GUST_FRAMES */
/* THE WIND'S GRIP. A wind of w mm/s is applied as WIND_GAIN·w mm/s² — drag, not a
 * kick: at 1 the breeze was a twelfth of gravity and the flag hung limp (host
 * render, 2026-10-03); at 8 the breeze lifts it to about level and full stick
 * streams it out. */
#define WIND_GAIN       8

#define BR_SOFT       110
#define BR_POLE       100
#define BR_FLOOR       40

#define EYE_X           0
#define EYE_Y        1000
#define EYE_Z       -2300
#define LOOK_Y        550

static vpy_mesh s_cube_mesh;
static int s_flag = -1, s_blob = -1, s_cube = -1;
static int s_held[5];
static int32_t s_gust;          /* the current gust, mm/s */
static int s_frame;

/* ── meshes ─────────────────────────────────────────────────────────────── */
static void build_box(vpy_mesh *m, int h)
{
    int v[8];
    vpy3d_mesh_begin(m);
    for (int k = 0; k < 8; k++)
        v[k] = vpy3d_vertex((k & 1) ? h : -h, (k & 2) ? h : -h, (k & 4) ? h : -h);
    vpy3d_quad(v[0], v[4], v[6], v[2]);  vpy3d_quad(v[1], v[3], v[7], v[5]);
    vpy3d_quad(v[0], v[1], v[5], v[4]);  vpy3d_quad(v[2], v[6], v[7], v[3]);
    vpy3d_quad(v[0], v[2], v[3], v[1]);  vpy3d_quad(v[4], v[5], v[7], v[6]);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

/* ── the world ──────────────────────────────────────────────────────────── */
static void drop_blob(void)
{
    if (s_blob >= 0) vpysoft_free(s_blob);
    s_blob = vpysoft_blob(BLOB_X, BLOB_DROP_Y, 0, BLOB_N, BLOB_R, BLOB_STIFF, BLOB_PRESSURE);
}

static void drop_cube(void)
{
    if (s_cube >= 0) vpysoft_free(s_cube);
    s_cube = vpysoft_mesh(&s_cube_mesh, CUBE_X, CUBE_DROP_Y, 0, CUBE_STIFF);
}

/* the flag's left column held to the pole, every frame */
static void pin_flag(void)
{
    for (int r = 0; r < FLAG_H; r++)
        vpysoft_pin(s_flag, r * FLAG_W, POLE_X, POLE_H - r * FLAG_CELL, POLE_Z);
}

static void setup_world(void)
{
    vpysoft_reset();
    vpysoft_set_gravity(0, -GRAVITY, 0);
    vpysoft_set_floor(1, 0, 64);
    s_flag = vpysoft_cloth(POLE_X, POLE_H, POLE_Z, FLAG_W, FLAG_H, FLAG_CELL, FLAG_STIFF);
    pin_flag();
    s_blob = -1; s_cube = -1;
    drop_blob();
    drop_cube();
    s_gust = 0;
    vpy_seed(1);
}

/* ── input ──────────────────────────────────────────────────────────────── */
static int pressed(int n)
{
    const int b = vpy_j1_button(n);
    const int edge = b && !s_held[n];
    s_held[n] = b;
    return edge;
}

/* ── drawing ────────────────────────────────────────────────────────────── */
static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_STEP) {
        vpy3d_line_world(t, 0, -FLOOR_HALF, t, 0, FLOOR_HALF, BR_FLOOR);
        vpy3d_line_world(-FLOOR_HALF, 0, t, FLOOR_HALF, 0, t, BR_FLOOR);
    }
}

static void setup(void)
{
    build_box(&s_cube_mesh, CUBE_HALF);
    setup_world();
}

static void loop(void)
{
    /* input */
    if (pressed(1)) drop_blob();
    if (pressed(2) && s_blob >= 0) {
        /* towards the middle of the floor, whichever side it is on, so it stays in view */
        int32_t bx, by, bz; vpysoft_point(s_blob, 0, &bx, &by, &bz);
        vpysoft_push(s_blob, bx > 0 ? -PUNCH_X : PUNCH_X, PUNCH_Y, 0);
    }
    if (pressed(3)) drop_cube();
    if (pressed(4)) setup_world();

    /* the wind: breeze + stick + a gust that changes every half second, and a
     * sideways sway so the cloth ripples rather than standing out flat */
    const int jx = vpy_j1_x();
    if (s_frame % GUST_FRAMES == 0) s_gust = (int32_t)(vpy_rand() % (2 * GUST + 1)) - GUST;
    const int32_t wind = BREEZE + jx * WIND_MAX / 127 + s_gust;
    const int32_t sway = FLUTTER * vpy_sin_q14((s_frame % FLUTTER_TURN) * VPY_Q14_TURN / FLUTTER_TURN) / VPY_Q14_ONE;
    /* vpysoft_push is a change of speed, so the wind is applied a fiftieth a frame:
     * a steady force, not a kick */
    vpysoft_push(s_flag, wind * WIND_GAIN / 50, 0, sway * WIND_GAIN / 50);
    s_frame++;

    pin_flag();
    vpysoft_step();

    /* draw */
    vpy3d_look_at(EYE_X, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
    vpy3d_line_world(POLE_X, 0, POLE_Z, POLE_X, POLE_H + 60, POLE_Z, BR_POLE);
    uint32_t shed = 0;
    vpysoft_draw(s_flag, BR_SOFT, 0);  shed += vpysoft_stats()->shed;
    if (s_blob >= 0) { vpysoft_draw(s_blob, BR_SOFT, 0); shed += vpysoft_stats()->shed; }
    if (s_cube >= 0) { vpysoft_draw(s_cube, BR_SOFT, 0); shed += vpysoft_stats()->shed; }
    draw_floor();

    /* readout */
    const vpysoft_stats_t *ss = vpysoft_stats();
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_set_text_size(6);
    vpy_print_text(-118, 108, "STR");  vpy_print_number(-96, 108, (long)ss->stretch);
    vpy_print_text( -40, 108, "AREA"); vpy_print_number(-12, 108, (long)(ss->area_error_q8 * 100 / 256));
    vpy_print_text(  50, 108, "SHED"); vpy_print_number( 78, 108, (long)shed);
    vpy_print_text(-118, -112, "STK"); vpy_print_number(-96, -112, (long)ds->strokes);
    vpy_print_text( -40, -112, "DROP"); vpy_print_number(-12, -112, (long)(ds->dropped
#ifndef VPY_DUAL_CORE
                                                                         + uvm2_stats.dropped
#endif
                                                                         ));
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
