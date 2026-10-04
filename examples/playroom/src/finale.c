/* finale.c — the four pieces become a trophy, and the sky fills with fireworks.
 *
 * The pieces from the four rooms circle the middle; then each comes apart
 * (vpyfx_disintegrate) while a trophy builds itself out of flying pieces in
 * their place (vpyfx_assemble) and takes over as a mesh when it is whole. A sign
 * turns over it — two faces back to back, each drawn only from its front
 * (VPY3D_TEXT_FRONT), so it never reads backwards — and fireworks go off: a
 * burst, a ring and a bang (vpyfx, vpyimpact). Button 1 launches one more.
 */
#include "playroom.h"
#ifdef PLAYROOM_HOST
#include <stdio.h>
#include <stdlib.h>
#endif
#include <vpyfx.h>
#include <vpyimpact.h>

#define ORBIT_R        900
#define ORBIT_Y        700
#define ORBIT_TURN      20    /* 4096ths of a turn a frame */
#define CIRCLE_FRAMES  90     /* the pieces circle this long before they go */
#define CUP_SEG          8    /* round the trophy */
#define SIGN_Y        1750
#define SIGN_H         230
#define SIGN_TURN        6
#define FIREWORK_EVERY  45
#define SKY_Y         2600

#define EYE_Y         1500
#define EYE_Z       (-2900)
#define LOOK_Y        1000

#define BR_PIECE       120
#define BR_CUP         120
#define BR_SIGN        110
#define BR_FLOOR        40

enum { CIRCLING, GATHERING, DONE };
static vpy_mesh s_cup, s_pieces[N_ROOMS];
static int s_state, s_t, s_group, s_spin, s_sign;

/* THE TROPHY, turned on a lathe: a foot, a stem, a cup. Each (radius, height) is
 * a ring of CUP_SEG vertices; neighbouring rings are joined by quads. */
static const int16_t CUP_PROFILE[][2] = {
    { 420, 0 }, { 420, 90 }, { 120, 170 }, { 120, 540 }, { 290, 680 }, { 470, 990 }, { 520, 1270 },
};
#define CUP_RINGS ((int)(sizeof CUP_PROFILE / sizeof CUP_PROFILE[0]))

void finale_build(void)
{
    int v[CUP_RINGS][CUP_SEG];
    vpy3d_mesh_begin(&s_cup);
    for (int j = 0; j < CUP_RINGS; j++)
        for (int i = 0; i < CUP_SEG; i++) {
            const int a = i * VPY_Q14_TURN / CUP_SEG;
            v[j][i] = vpy3d_vertex(CUP_PROFILE[j][0] * vpy_cos_q14(a) / VPY_Q14_ONE, CUP_PROFILE[j][1],
                                   CUP_PROFILE[j][0] * vpy_sin_q14(a) / VPY_Q14_ONE);
        }
    for (int j = 0; j + 1 < CUP_RINGS; j++)
        for (int i = 0; i < CUP_SEG; i++) {
            const int n = (i + 1) % CUP_SEG;
            vpy3d_quad(v[j][n], v[j + 1][n], v[j + 1][i], v[j][i]);
        }
    {   /* closed below and on top (a full cup), so the hidden-line test has a solid */
        int base[CUP_SEG], top[CUP_SEG];
        for (int i = 0; i < CUP_SEG; i++) { base[i] = v[0][CUP_SEG - 1 - i]; top[i] = v[CUP_RINGS - 1][i]; }
        vpy3d_face(base, CUP_SEG);
        vpy3d_face(top, CUP_SEG);
    }
    vpy3d_mesh_end(VPY3D_HARD_45);

    pr_crate(&s_pieces[R_CRATES], 150);
    pr_ball(&s_pieces[R_JELLY], 160);
    pr_octahedron(&s_pieces[R_SHAPES], 190);
    pr_dart(&s_pieces[R_FLYER], 420, 0);
}

static vpy_xf piece_place(int i)
{
    const int a = (s_spin + i * VPY_Q14_TURN / N_ROOMS) & (VPY_Q14_TURN - 1);
    const vpy_xf turn = vpy3d_rot_y(s_spin * 3);
    const vpy_xf at = vpy3d_translate(ORBIT_R * vpy_sin_q14(a) / VPY_Q14_ONE, ORBIT_Y, ORBIT_R * vpy_cos_q14(a) / VPY_Q14_ONE);
    return vpy3d_mul(&at, &turn);
}

static vpy_xf cup_place(void)
{
    const vpy_xf turn = vpy3d_rot_y(s_spin);
    const vpy_xf at = vpy3d_translate(0, 0, 0);
    return vpy3d_mul(&at, &turn);
}

void finale_enter(void)
{
    vpyfx_reset();
    vpyfx_seed(42);
    vpyfx_set_gravity(0, -2000, 0);
    vpyfx_set_floor(1, 0, 60);
    vpyfx_set_budget(VPYFX_MAX);
    s_state = CIRCLING; s_t = 0; s_spin = 0; s_sign = 0;
    vpyimpact_set_listener(0, EYE_Y, EYE_Z, 1, 0, 4000, 12000);
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
}

static void firework(void)
{
    const int32_t x = vpy_rand_range(-2200, 2200), y = SKY_Y + vpy_rand_range(-400, 300), z = vpy_rand_range(800, 2500);
    vpyfx_burst(x, y, z, 0, 0, 0, 24, 2200, 40, 127);
    vpyfx_ring(x, y, z, 0, 0, 1, 40, 1800, 12, 24, 100);
    vpyimpact_hit_at(18000, VPYI_METAL, x, y, z);
}

int finale_frame(void)
{
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
    s_t++;
    if (s_state != GATHERING) s_spin = (s_spin + ORBIT_TURN) & (VPY_Q14_TURN - 1);
    s_sign = (s_sign + SIGN_TURN) & (VPY_Q14_TURN - 1);

    if (s_state == CIRCLING && s_t >= CIRCLE_FRAMES) {
        /* every piece comes apart where it is, and the trophy builds in the middle */
        for (int i = 0; i < N_ROOMS; i++) {
            const vpy_xf at = piece_place(i);
            vpyfx_disintegrate(&s_pieces[i], &at, at.t[0], at.t[1], at.t[2], 2, 1200, 2000, 0, 50, BR_PIECE);
        }
        const vpy_xf at = cup_place();
        s_group = vpyfx_assemble(&s_cup, &at, 1, 1500, 60, 40, BR_CUP);
        s_state = s_group ? GATHERING : DONE;
        vpyimpact_hit(12000, VPYI_METAL);
    }
    if (s_state == GATHERING && vpyfx_assembled(s_group)) {
        vpyfx_release(s_group);
        s_state = DONE;
        firework();
    }
    if (s_state == DONE && (s_t % FIREWORK_EVERY == 0 || pr_pressed(1))) firework();
    vpyfx_step();
    vpyimpact_step();
#ifdef PLAYROOM_HOST
    if (getenv("TRACE") && (s_t == CIRCLE_FRAMES + 1 || s_t == CIRCLE_FRAMES + 60))
        printf("finale f%d: pieces alive %u, recycled %u (pool %d)\n", s_t, vpyfx_stats()->alive, vpyfx_stats()->recycled, VPYFX_MAX);
#endif

    /* draw */
    if (s_state == CIRCLING)
        for (int i = 0; i < N_ROOMS; i++) { const vpy_xf at = piece_place(i); vpy3d_draw_mesh(&s_pieces[i], &at, BR_PIECE); }
    if (s_state == DONE) {
        const vpy_xf at = cup_place();
        vpy3d_draw_mesh(&s_cup, &at, BR_CUP);
        vpy_xf a = vpy3d_rot_y(s_sign);
        a.t[1] = SIGN_Y;
        vpy3d_text("WELL DONE", &a, SIGN_H, BR_SIGN, VPY3D_TEXT_CENTRE | VPY3D_TEXT_FRONT);
        vpy_xf b = vpy3d_rot_y(s_sign + VPY_Q14_TURN / 2);
        b.t[1] = SIGN_Y;
        vpy3d_text("PLAYROOM", &b, SIGN_H, BR_SIGN, VPY3D_TEXT_CENTRE | VPY3D_TEXT_FRONT);
    }
    vpyfx_draw(0);
    for (int32_t t = -2000; t <= 2000; t += 500) {
        vpy3d_line_world(t, 0, -1000, t, 0, 2000, BR_FLOOR);
        if (t >= -1000) vpy3d_line_world(-2000, 0, t, 2000, 0, t, BR_FLOOR);
    }
    pr_objective(s_state == DONE ? "1 > FIREWORK   4 > HUB" : "ALL FOUR PIECES", 0, 0);
    return 0;
}
