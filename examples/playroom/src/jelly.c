/* jelly.c — JELLY: blow a jelly into the ring and keep it there for two seconds.
 *
 * vpysoft (sdk/vpy-c/include/vpysoft.h): the FLAG is a cloth pinned to a pole and
 * shows the wind; the JELLY is a blob — a ring of points round a centre that keeps
 * its AREA, so it squashes when it lands or is pushed and fills out again; the
 * CUBE (button 2) is a vpy3d box whose every edge is a soft spring, braced to a
 * hidden centre, dropped on the jelly.
 *
 * The stick is the wind, both ways; the jelly feels it as a steady drag, so
 * steering it is the game: too much and it overshoots the ring. Button 1 makes it
 * hop, into the wind.
 */
#include "playroom.h"
#ifdef PLAYROOM_HOST
#include <stdio.h>
#include <stdlib.h>
#endif
#include <vpysoft.h>
#include <vpyimpact.h>

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define GRAVITY       9800
#define FLOOR_HALF    2000
#define FLOOR_STEP     500
#define POLE_X      (-1500)
#define POLE_Z         500
#define POLE_H        1300
#define FLAG_W           8
#define FLAG_H           5
#define FLAG_CELL       90
#define FLAG_STIFF     200
#define BLOB_START_X (-700)
#define BLOB_DROP_Y    700
#define BLOB_N          16
#define BLOB_R         200
#define BLOB_STIFF      60
#define BLOB_PRESSURE   64
#define CUBE_HALF      140
#define CUBE_STIFF      90
#define CUBE_DROP_Y   1400
#define RING_X         800
#define RING_R         330
#define RING_SEG        16
#define HOLD_FRAMES    100     /* two seconds in the ring */
#define OUT_X         2200     /* past this the jelly is lost and comes back to the start */
#define BREEZE         250     /* mm/s, so the flag flies with the stick centred */
#define WIND_MAX      2600
#define FLAG_GAIN        8     /* soft_demo's: the wind as drag on the cloth */
/* the jelly's share of the wind, a frame: wind × BLOB_GAIN / 50 / BLOB_DIV mm/s.
 * Tuned on the host so full stick crosses the 1.5 m to the ring in a few seconds
 * and a tap moves it a little. */
#define BLOB_GAIN        1
#define BLOB_DIV         3
#define HOP_UP        2200
#define HOP_SIDE       700

#define EYE_Y          950
#define EYE_Z       (-2300)
#define LOOK_Y         350

#define BR_SOFT        110
#define BR_POLE        100
#define BR_FLOOR        40
#define BR_RING        100

static vpy_mesh s_cube_mesh;
static int s_flag = -1, s_blob = -1, s_cube = -1;
static int s_held_in, s_frame;
static int32_t s_wind;

void jelly_build(void)
{
    pr_box(&s_cube_mesh, CUBE_HALF, CUBE_HALF, CUBE_HALF);
}

static void drop_blob(void)
{
    if (s_blob >= 0) vpysoft_free(s_blob);
    s_blob = vpysoft_blob(BLOB_START_X, BLOB_DROP_Y, 0, BLOB_N, BLOB_R, BLOB_STIFF, BLOB_PRESSURE);
}

static void pin_flag(void)
{
    for (int r = 0; r < FLAG_H; r++)
        vpysoft_pin(s_flag, r * FLAG_W, POLE_X, POLE_H - r * FLAG_CELL, POLE_Z);
}

void jelly_enter(void)
{
    vpysoft_reset();
    vpysoft_set_gravity(0, -GRAVITY, 0);
    vpysoft_set_floor(1, 0, 64);
    s_flag = vpysoft_cloth(POLE_X, POLE_H, POLE_Z, FLAG_W, FLAG_H, FLAG_CELL, FLAG_STIFF);
    pin_flag();
    s_blob = -1; s_cube = -1;
    drop_blob();
    s_held_in = 0; s_frame = 0; s_wind = 0;
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
}

/* the jelly's middle: the mean of its points */
static void blob_centre(int32_t *x, int32_t *y, int32_t *z)
{
    const int n = vpysoft_points(s_blob);
    int64_t sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < n; i++) { int32_t a, b, c; vpysoft_point(s_blob, i, &a, &b, &c); sx += a; sy += b; sz += c; }
    *x = n ? (int32_t)(sx / n) : 0; *y = n ? (int32_t)(sy / n) : 0; *z = n ? (int32_t)(sz / n) : 0;
}

static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_STEP) {
        vpy3d_line_world(t, 0, -FLOOR_HALF, t, 0, FLOOR_HALF, BR_FLOOR);
        vpy3d_line_world(-FLOOR_HALF, 0, t, FLOOR_HALF, 0, t, BR_FLOOR);
    }
}

static void draw_ring(int br)
{
    int32_t px = RING_X + RING_R, pz = 0;
    for (int i = 1; i <= RING_SEG; i++) {
        const int a = i * VPY_Q14_TURN / RING_SEG;
        const int32_t x = RING_X + RING_R * vpy_cos_q14(a) / VPY_Q14_ONE, z = RING_R * vpy_sin_q14(a) / VPY_Q14_ONE;
        vpy3d_line_world(px, 3, pz, x, 3, z, br);
        px = x; pz = z;
    }
}

/* how long it has been in the ring, as a bar under the objective */
static void draw_hold_bar(void)
{
    const int w = 80, x0 = -w / 2, y = 104;
    vpy_draw_line(x0, y, x0 + w, y, BR_HINT);
    vpy_draw_line(x0, y - 4, x0 + w, y - 4, BR_HINT);
    vpy_draw_line(x0, y, x0, y - 4, BR_HINT);
    vpy_draw_line(x0 + w, y, x0 + w, y - 4, BR_HINT);
    if (s_held_in > 0) {
        const int fill = w * s_held_in / HOLD_FRAMES;
        vpy_draw_line(x0, y - 2, x0 + fill, y - 2, 127);
    }
}

int jelly_frame(void)
{
    s_frame++;
    /* the wind: the stick both ways, on top of a breeze, eased so it gusts in */
    const int jx = vpy_j1_x();
    const int32_t want = (jx > 15 || jx < -15) ? jx * WIND_MAX / 127 : 0;
    s_wind += (want - s_wind) / 8;
    if (s_wind - want < 8 && want - s_wind < 8) s_wind = want;   /* do not creep: settle */
    const int32_t sway = 600 * vpy_sin_q14((s_frame % 37) * VPY_Q14_TURN / 37) / VPY_Q14_ONE;
    vpysoft_push(s_flag, (BREEZE + s_wind) * FLAG_GAIN / 50, 0, sway * FLAG_GAIN / 50);
    vpysoft_push(s_blob, s_wind * BLOB_GAIN / 50 / BLOB_DIV, 0, 0);
    if (pr_pressed(1)) {
        vpysoft_push(s_blob, s_wind > 0 ? HOP_SIDE : s_wind < 0 ? -HOP_SIDE : 0, HOP_UP, 0);
        vpyimpact_hit(6000, VPYI_SOFT);
    }
    if (pr_pressed(2)) {
        int32_t bx, by, bz; blob_centre(&bx, &by, &bz);
        if (s_cube >= 0) vpysoft_free(s_cube);
        s_cube = vpysoft_mesh(&s_cube_mesh, bx, CUBE_DROP_Y, bz, CUBE_STIFF);
    }
    pin_flag();
    vpysoft_step();
    vpyimpact_step();

    /* where the jelly is: lost off the side, in the ring, or neither */
    int32_t bx, by, bz; blob_centre(&bx, &by, &bz);
    if (bx > OUT_X || bx < -OUT_X) { drop_blob(); s_held_in = 0; }
    const int64_t dx = bx - RING_X, dz = bz;
    const int in = dx * dx + dz * dz < (int64_t)(RING_R - BLOB_R / 3) * (RING_R - BLOB_R / 3) && by < BLOB_R + 150;
    s_held_in = in ? s_held_in + 1 : 0;
#ifdef PLAYROOM_HOST
    if (getenv("TRACE") && s_frame % 10 == 0) printf("jelly f%d x %d y %d wind %d in %d\n", s_frame, bx, by, s_wind, s_held_in);
#endif

    /* draw */
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
    vpy3d_line_world(POLE_X, 0, POLE_Z, POLE_X, POLE_H + 60, POLE_Z, BR_POLE);
    vpysoft_draw(s_flag, BR_SOFT, 0);
    vpysoft_draw(s_blob, BR_SOFT, 0);
    if (s_cube >= 0) vpysoft_draw(s_cube, BR_SOFT, 0);
    draw_ring(in ? 70 + (s_frame & 8) * 7 : BR_RING);          /* it flickers while held */
    draw_floor();
    pr_objective("KEEP THE JELLY IN THE RING", 0, 0);
    draw_hold_bar();
    return s_held_in >= HOLD_FRAMES;
}
