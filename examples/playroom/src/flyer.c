/* flyer.c — FLYER: hit the ship three times as it flies over the hills.
 *
 *   the HILLS are a height map drawn by vpy3d_terrain: rows near to far, each
 *     showing only what rises above the rows in front of it — the floating
 *     horizon, the right tool for land where the convex occluder is not;
 *   the SHIP flies a loop out over them and back, and FADES with distance
 *     (vpy3d_fog: on a one-colour tube, distance is the only depth cue there is);
 *     far off it is drawn with a plainer mesh (vpy3d_draw_lod picks it by its
 *     size on screen);
 *   its ENGINE is a continuous source on the UVMC2's jack (vpyimpact_loop): it
 *     pans as it crosses and its pitch drops as it passes — the Doppler shift.
 *     On a cartridge without the jack it is silent; the hits sound everywhere.
 *
 * The aim is a cross on the screen; a shot hits if the ship is under it, with
 * the margin growing with how big the ship looks.
 */
#include "playroom.h"
#include <vpyfx.h>
#include <vpyimpact.h>

/* ── the land: COLS × ROWS heights, CELL apart, starting in front of the eye ── */
#define COLS            17
#define ROWS            14
#define CELL           600
#define LAND_X0     (-(COLS - 1) * CELL / 2)
#define LAND_Z0        200
#define HILL_H         900    /* the highest a hill gets */

/* ── the ship's loop: an ellipse over the land, a turn in LOOP_FRAMES ─────── */
#define LOOP_CX          0
#define LOOP_CZ       4500
#define LOOP_RX       2300
#define LOOP_RZ       2900
#define LOOP_Y        1500
#define LOOP_BOB       350
#define LOOP_FRAMES    600    /* 12 s a lap */
#define SHIP_LEN       520
#define SHIP_R         320    /* the ball round it, for the level of detail and the hit */
#define NEED             3

/* THE FADE: full brightness this close to the eye, nothing from FOG_GONE */
#define FOG_FULL      2500
#define FOG_GONE     12000
/* THE DETAIL: the full mesh while the ship is at least this big on screen
 * (deflection units, vpy3d_screen_size), the plain one below it */
#define LOD_FULL      1100

/* ── the engine: a hum whose pitch Doppler-shifts (PCM path only) ─────────── */
#define ENGINE_HZ       95
#define ENGINE_VOL      12
#define SOUND_SPEED 343000    /* mm/s */
#define HEAR_NEAR     3000
#define HEAR_FAR     14000

/* ── the aim, in deflection units ─────────────────────────────────────────── */
#define AIM_SPEED      320    /* a frame at full stick */
#define AIM_LIMIT    14000
#define AIM_ARM        600
#define HIT_MARGIN     500    /* added to the ship's size on screen */
#define FLASH_FRAMES     4

#define EYE_Y         1500
#define EYE_Z       (-1400)
#define LOOK_Y         900
#define LOOK_Z        4000

#define BR_LAND         70
#define BR_SHIP        120
#define BR_AIM          90

static int16_t s_land[ROWS * COLS];
static vpy_mesh s_full, s_low;
static int s_t, s_hits, s_flash, s_gone;
static int32_t s_aim_x, s_aim_y;
static int32_t s_pos[3], s_vel[3];

/* Hills from three waves of different lengths, low near the eye so the first
 * rows do not hide the rest. Built once: the land never changes. */
void flyer_build(void)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const int32_t a = vpy_sin_q14(c * 610 + r * 230), b = vpy_sin_q14(r * 470 - c * 190 + 900);
            const int32_t d = vpy_sin_q14(c * 1330 + r * 1170);
            int32_t h = (a + b + d / 2 + VPY_Q14_ONE * 5 / 2) * HILL_H / (VPY_Q14_ONE * 5);
            h = h * (r + 2) / (ROWS + 1);            /* rising towards the back */
            s_land[r * COLS + c] = (int16_t)h;
        }
    pr_dart(&s_full, SHIP_LEN, 0);
    pr_dart(&s_low, SHIP_LEN, 1);
}

void flyer_enter(void)
{
    vpyfx_reset();
    vpyfx_seed(5);
    vpyfx_set_gravity(0, -3000, 0);
    vpyfx_set_budget(120);
    s_t = 0; s_hits = 0; s_flash = 0; s_gone = 0;
    s_aim_x = 0; s_aim_y = 2000;
    vpyimpact_set_sound_speed(SOUND_SPEED);
    vpyimpact_set_listener(0, EYE_Y, EYE_Z, 1, 0, HEAR_NEAR, HEAR_FAR);
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, LOOK_Z, 0, 1, 0);
}

/* where the ship is at step t, and its velocity in mm/s */
static void ship_at(int t, int32_t *p, int32_t *v)
{
    const int a = (int)((int64_t)t * VPY_Q14_TURN / LOOP_FRAMES) & (VPY_Q14_TURN - 1);
    const int b = (a * 3) & (VPY_Q14_TURN - 1);
    p[0] = LOOP_CX + LOOP_RX * vpy_sin_q14(a) / VPY_Q14_ONE;
    p[1] = LOOP_Y + LOOP_BOB * vpy_sin_q14(b) / VPY_Q14_ONE;
    p[2] = LOOP_CZ - LOOP_RZ * vpy_cos_q14(a) / VPY_Q14_ONE;
    /* d/dt of the above, in mm/s: radius × the angular speed, 2π × 50 / LOOP_FRAMES
     * radians a second (628/100 for 2π) */
    v[0] = (int32_t)((int64_t)LOOP_RX * vpy_cos_q14(a) / VPY_Q14_ONE * 628 * 50 / LOOP_FRAMES / 100);
    v[1] = (int32_t)((int64_t)LOOP_BOB * vpy_cos_q14(b) / VPY_Q14_ONE * 3 * 628 * 50 / LOOP_FRAMES / 100);
    v[2] = (int32_t)((int64_t)LOOP_RZ * vpy_sin_q14(a) / VPY_Q14_ONE * 628 * 50 / LOOP_FRAMES / 100);
}

/* the ship turned to fly along its velocity, level: its nose (+z) forward, its
 * wings (x) to the side */
static vpy_xf ship_place(void)
{
    vpy_xf at = vpy3d_identity();
    const int64_t l = pr_isqrt64((int64_t)s_vel[0] * s_vel[0] + (int64_t)s_vel[2] * s_vel[2]);
    const int32_t fx = l ? (int32_t)(s_vel[0] * (int64_t)VPY_Q14_ONE / l) : 0;
    const int32_t fz = l ? (int32_t)(s_vel[2] * (int64_t)VPY_Q14_ONE / l) : VPY_Q14_ONE;
    /* columns: right (fz, 0, -fx), up (0, 1, 0), forward (fx, 0, fz) */
    at.m[0] = fz;  at.m[1] = 0;           at.m[2] = fx;
    at.m[3] = 0;   at.m[4] = VPY_Q14_ONE; at.m[5] = 0;
    at.m[6] = -fx; at.m[7] = 0;           at.m[8] = fz;
    at.t[0] = s_pos[0]; at.t[1] = s_pos[1]; at.t[2] = s_pos[2];
    return at;
}

static void fire(void)
{
    s_flash = FLASH_FRAMES;
    if (s_gone) return;
    int32_t cam[3], sx, sy;
    vpy3d_to_camera(s_pos[0], s_pos[1], s_pos[2], cam);
    if (!vpy3d_project(cam, &sx, &sy)) return;
    const int32_t r = vpy3d_screen_size(s_pos[0], s_pos[1], s_pos[2], SHIP_R) + HIT_MARGIN;
    if (sx - s_aim_x > r || s_aim_x - sx > r || sy - s_aim_y > r || s_aim_y - sy > r) return;
    s_hits++;
    vpyfx_burst(s_pos[0], s_pos[1], s_pos[2], s_vel[0] / 2, s_vel[1] / 2, s_vel[2] / 2, 14, 1800, 20, 127);
    vpyimpact_hit_at(16000, VPYI_METAL, s_pos[0], s_pos[1], s_pos[2]);
    if (s_hits >= NEED) {
        const vpy_xf at = ship_place();
        vpyfx_shatter(&s_full, &at, s_vel[0], s_vel[1], s_vel[2], s_pos[0], s_pos[1], s_pos[2], 2500, 4000, 120, 127);
        vpyfx_ring(s_pos[0], s_pos[1], s_pos[2], 0, 1, 0, 80, 3000, 16, 25, 120);
        vpyimpact_loop(0, 0, 0, 0, 0, 0, 0, ENGINE_HZ, 0);    /* the engine stops */
        s_gone = 1;
    }
}

int flyer_frame(void)
{
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, LOOK_Z, 0, 1, 0);
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    if (jx > 30 || jx < -30) s_aim_x += jx * AIM_SPEED / 127;
    if (jy > 30 || jy < -30) s_aim_y += jy * AIM_SPEED / 127;
    if (s_aim_x >  AIM_LIMIT) s_aim_x =  AIM_LIMIT;
    if (s_aim_x < -AIM_LIMIT) s_aim_x = -AIM_LIMIT;
    if (s_aim_y >  AIM_LIMIT) s_aim_y =  AIM_LIMIT;
    if (s_aim_y < -AIM_LIMIT) s_aim_y = -AIM_LIMIT;

    if (!s_gone) {
        s_t++;
        ship_at(s_t, s_pos, s_vel);
        vpyimpact_loop(0, s_pos[0], s_pos[1], s_pos[2], s_vel[0], s_vel[1], s_vel[2], ENGINE_HZ, ENGINE_VOL);
    }
    if (pr_pressed(1)) fire();
    vpyfx_step();
    vpyimpact_step();

    /* draw: the cross and the shot first, then the ship, its pieces, the land */
    vpy_draw_line_dev(s_aim_x - AIM_ARM, s_aim_y, s_aim_x + AIM_ARM, s_aim_y, BR_AIM);
    vpy_draw_line_dev(s_aim_x, s_aim_y - AIM_ARM, s_aim_x, s_aim_y + AIM_ARM, BR_AIM);
    if (s_flash > 0) {
        /* the shot: from the bottom of the screen to the cross, fading */
        vpy_draw_line_dev(0, -15000, s_aim_x, s_aim_y, 40 + s_flash * 20);
        s_flash--;
    }
    if (!s_gone) {
        const vpy_xf at = ship_place();
        const int br = vpy3d_fog(BR_SHIP, s_pos[0], s_pos[1], s_pos[2], FOG_FULL, FOG_GONE);
        static const vpy_mesh *const LEVELS[2] = { &s_full, &s_low };
        static const int32_t MIN_SIZE[2] = { LOD_FULL, 1 };
        if (br > 0) vpy3d_draw_lod(LEVELS, MIN_SIZE, 2, &at, SHIP_R, br);
    }
    vpyfx_draw(0);
    vpy3d_terrain(s_land, COLS, ROWS, LAND_X0, LAND_Z0, CELL, BR_LAND);
    pr_objective("HITS ON THE FLYER", s_hits, NEED);
    return s_gone;
}
