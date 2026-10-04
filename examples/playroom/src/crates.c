/* crates.c — CRATES: knock eight blocks off the pad.
 *
 * A tower of six crates and two pyramids stands on a square pad. A shot rocks a
 * crate, dents it where it lands and marks it; the third shot SHATTERS it — every
 * edge flies off spinning (vpyfx_shatter) and the blast (vpyp_blast) shoves what
 * is near, which is how the heavy ones leave the pad. A pyramid is a convex hull
 * (vpyp_hull_shape) and a shot knocks it about. Button 2 drops a heavy ball over
 * the aim: the other way to bring the tower down.
 *
 * Everything solid is a vpyent entity, so one call draws the scene near to far
 * through the occluder with marks and shadows, and one steps camera (shake and
 * hit-stop), physics, impact sounds and effects. A block counts as off when its
 * middle is outside the pad, or when it has been shattered.
 */
#include "playroom.h"
#ifdef PLAYROOM_HOST
#include <stdio.h>
#include <stdlib.h>
#endif
#include <vpyphys.h>
#include <vpyfx.h>
#include <vpycam.h>
#include <vpyimpact.h>
#include <vpyent.h>

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define GRAVITY      9800
#define PAD           750     /* the pad is +-PAD in x and z */
#define CRATE         130     /* half size */
#define GAP             4     /* between stacked crates, so the tower settles */
#define BALL          150     /* the heavy ball's radius */
#define BALL_MASS       8
#define BALL_DROP    1700
#define MAX_BALLS       3
#define N_BLOCKS        8
/* Two, not physics_demo's three: here the tower is shot from a fixed eye, and a
 * crate the first shot rocked was often out of the line of the next two (host
 * run, 2026-10-04) — the breaks never came. */
#define SHOTS_TO_BREAK  2
#define SHOT_KICK   20000     /* mm/s per unit of mass, along the ray */
/* A crate takes an eighth of it: it rocks. At a quarter (host run, 2026-10-04) the
 * top crate left on the first shot and landed far beyond the floor — three
 * shots must be able to land on the same crate. */
#define CRATE_NUDGE     8
#define SHOT_DENT      95     /* deep on purpose: a shallow dent does not show this small */
#define DENT_R        200
#define BLAST_R       750
#define BLAST_KICK   3200
#define AIM_SPEED      30     /* mm a frame at full stick */
#define AIM_X        1400
#define AIM_TOP      1500
#define AIM_ARM        60
#define FX_BUDGET     120
#define FLOOR_HALF   3000

#define IMPACT_QUIET  1500
#define IMPACT_LOUD  12000

#define EYE_Y        1300
#define EYE_Z      (-2500)
#define FOCUS_Y       300

#define BR_SOLID      110
#define BR_PAD         90
#define BR_FLOOR       40
#define BR_AIM         90

enum { K_CRATE = 1, K_PYR, K_BALL };

static vpy_mesh s_crate, s_pyr, s_ball;
static int s_pyr_shape;
static int s_block[N_BLOCKS];               /* entity ids; -1 once shattered */
static uint8_t s_hits[VPYENT_MAX];
static int s_balls[MAX_BALLS], s_nballs;
static int32_t s_aim_x, s_aim_y;

void crates_build(void)
{
    pr_crate(&s_crate, CRATE);
    pr_from_faces(&s_pyr, PR_PYR_V, 5, PR_PYR_F);
    pr_ball(&s_ball, BALL);
}

static int add_crate(int32_t x, int32_t y, int32_t z)
{
    const int body = vpyp_add_box(x, y, z, CRATE, CRATE, CRATE, 2);
    if (body < 0) return -1;
    const int e = vpyent_create(&s_crate, body);
    if (e < 0) { vpyp_remove(body); return -1; }
    vpyp_set_material(body, 40, 150);
    vpyent_set_kind(e, K_CRATE);
    vpyent_set_brightness(e, BR_SOLID);
    vpyent_set_occluder(e, VPYENT_OCC_BOX, CRATE, CRATE, CRATE);   /* 14 vertices: it hides as a box */
    vpyent_set_material(e, VPYI_WOOD);
    s_hits[e] = 0;
    return e;
}

static int add_pyramid(int32_t x, int32_t z)
{
    const int body = vpyp_add_hull(x, 75, z, s_pyr_shape, 2);       /* its base 75 below its middle */
    if (body < 0) return -1;
    const int e = vpyent_create(&s_pyr, body);
    if (e < 0) { vpyp_remove(body); return -1; }
    vpyp_set_material(body, 40, 150);
    vpyent_set_kind(e, K_PYR);
    vpyent_set_brightness(e, BR_SOLID);
    vpyent_set_material(e, VPYI_WOOD);
    return e;
}

void crates_enter(void)
{
    vpyent_reset();
    vpyp_reset();
    vpyp_set_gravity(0, -GRAVITY, 0);
    vpyp_set_floor(1, 0, 60, 160);
    s_pyr_shape = vpyp_hull_shape(PR_PYR_V, 5, PR_PYR_F);      /* shapes go with every reset */
    /* the tower: three, two, one */
    int k = 0;
    for (int row = 0; row < 3; row++)
        for (int i = 0; i < 3 - row; i++)
            s_block[k++] = add_crate((i * 2 - (2 - row)) * (CRATE + GAP), CRATE + row * (2 * CRATE + GAP), 0);
    /* and a pyramid either side, a little forward, where the camera sees them */
    s_block[k++] = add_pyramid(-580, -260);
    s_block[k++] = add_pyramid(580, -260);
    s_nballs = 0;
    s_aim_x = 0; s_aim_y = 5 * CRATE;          /* on the top crate */
    vpycam_reset(0, FOCUS_Y, 0);
    vpyfx_reset();
    vpyfx_seed(3);
    vpyfx_set_gravity(0, -GRAVITY, 0);
    vpyfx_set_floor(1, 0, 90);
    vpyfx_set_budget(FX_BUDGET);
    vpyimpact_set_range(IMPACT_QUIET, IMPACT_LOUD);
    vpyimpact_set_listener(0, EYE_Y, EYE_Z, 1, 0, 4000, 9000);
}

static int off_pad(int e)
{
    if (e < 0) return 1;
    vpy_xf at; vpyent_place(e, &at);
    return at.t[0] > PAD || at.t[0] < -PAD || at.t[2] > PAD || at.t[2] < -PAD;
}

static void shatter(int e, int32_t hx, int32_t hy, int32_t hz)
{
    vpy_xf at; vpyent_place(e, &at);
    int32_t vx, vy, vz; vpyp_velocity(vpyent_body(e), &vx, &vy, &vz);
    vpyfx_shatter(&s_crate, &at, vx, vy, vz, hx, hy, hz, 2200, 3000, 150, 120);
    for (int i = 0; i < N_BLOCKS; i++) if (s_block[i] == e) s_block[i] = -1;
    vpyent_destroy(e);
    vpyimpact_hit(IMPACT_LOUD, VPYI_WOOD);          /* the crash, at full volume */
    vpyp_blast(hx, hy, hz, BLAST_R, BLAST_KICK, 0xFF);
    vpyfx_ring(hx, 2, hz, 0, 1, 0, 60, 2600, 16, 22, 110);
    vpycam_shake(70, 18);
    vpycam_hitstop(3);
}

static void shoot(void)
{
    int32_t eye[3]; vpy3d_eye(eye);
    const int32_t dx = s_aim_x - eye[0], dy = s_aim_y - eye[1], dz = 0 - eye[2];
    vpyp_hit h;
    const int body = vpyp_raycast(eye[0], eye[1], eye[2], dx, dy, dz, 20000, 0xFF, &h);
    if (body == VPYP_NONE) return;
    vpyfx_line(eye[0] + 300, eye[1] - 500, eye[2] + 600, h.x, h.y, h.z, 8, 110);   /* the tracer */
    vpyfx_burst(h.x, h.y, h.z, 0, 0, 0, 10, 2500, 16, 127);
    const int e = body >= 0 ? vpyent_of_body(body) : VPYENT_NONE;
    if (e < 0) return;
    const int kind = vpyent_kind(e);
    if (kind == K_CRATE) {
        if (++s_hits[e] >= SHOTS_TO_BREAK) { shatter(e, h.x, h.y, h.z); return; }
        vpyent_dent(e, h.x, h.y, h.z, dx, dy, dz, SHOT_DENT, DENT_R);
    }
    if (kind != K_BALL) vpyent_mark(e, h.x, h.y, h.z, h.nx, h.ny, h.nz);
    /* the kick along the ray, at the point it hit: off-centre, it spins the body */
    const int64_t l = pr_isqrt64((int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz);
    const int32_t m = kind == K_BALL ? BALL_MASS : 2;
    const int32_t kick = kind == K_CRATE ? SHOT_KICK / CRATE_NUDGE : SHOT_KICK / 2;
    vpyp_apply_impulse_at(body, (int32_t)((int64_t)dx * kick * m / l),
                                (int32_t)((int64_t)dy * kick * m / l) + (kind == K_CRATE ? 0 : kick * m / 4),
                                (int32_t)((int64_t)dz * kick * m / l), h.x, h.y, h.z);
}

static void drop_ball(void)
{
    if (s_nballs >= MAX_BALLS) {                    /* the oldest goes */
        vpyent_destroy(s_balls[0]);
        for (int i = 1; i < s_nballs; i++) s_balls[i - 1] = s_balls[i];
        s_nballs--;
    }
    const int body = vpyp_add_sphere(s_aim_x, BALL_DROP, 0, BALL, BALL_MASS);
    if (body < 0) return;
    const int e = vpyent_create(&s_ball, body);
    if (e < 0) { vpyp_remove(body); return; }
    vpyp_set_material(body, 60, 120);
    vpyent_set_kind(e, K_BALL);
    vpyent_set_brightness(e, BR_SOLID);
    vpyent_set_occluder(e, VPYENT_OCC_SPHERE, BALL, 0, 0);
    vpyent_set_material(e, VPYI_METAL);
    s_balls[s_nballs++] = e;
}

/* every hard contact throws sparks, more the harder it was */
static void sparks(void)
{
    for (int i = 0; i < vpyp_contact_count(); i++) {
        const vpyp_contact *c = vpyp_contact_get(i);
        if (c->impulse < 2500) continue;
        const int n = c->impulse / 1500 > 8 ? 8 : c->impulse / 1500;
        vpyfx_burst(c->x, c->y, c->z, 0, 0, 0, n, 1200, 14, c->impulse > 12000 ? 127 : 90);
    }
}

static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += 600) {
        vpy3d_occl_line(t, 0, -FLOOR_HALF, t, 0, FLOOR_HALF, BR_FLOOR);
        vpy3d_occl_line(-FLOOR_HALF, 0, t, FLOOR_HALF, 0, t, BR_FLOOR);
    }
}

static void draw_pad(void)
{
    vpy3d_occl_line(-PAD, 3, -PAD,  PAD, 3, -PAD, BR_PAD);
    vpy3d_occl_line( PAD, 3, -PAD,  PAD, 3,  PAD, BR_PAD);
    vpy3d_occl_line( PAD, 3,  PAD, -PAD, 3,  PAD, BR_PAD);
    vpy3d_occl_line(-PAD, 3,  PAD, -PAD, 3, -PAD, BR_PAD);
}

int crates_frame(void)
{
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    if (jx > 30 || jx < -30) s_aim_x += jx * AIM_SPEED / 127;
    if (jy > 30 || jy < -30) s_aim_y += jy * AIM_SPEED / 127;
    if (s_aim_x >  AIM_X) s_aim_x =  AIM_X;
    if (s_aim_x < -AIM_X) s_aim_x = -AIM_X;
    if (s_aim_y > AIM_TOP) s_aim_y = AIM_TOP;
    if (s_aim_y < 0) s_aim_y = 0;
    if (pr_pressed(1)) shoot();
    if (pr_pressed(2)) drop_ball();

    if (vpyent_step(VPYI_NONE)) sparks();           /* camera, physics, sounds, effects */
    vpycam_look_at(0, EYE_Y - FOCUS_Y, EYE_Z);

    /* draw: the aim in front of everything, the entities near to far, then what they hide */
    vpy3d_occl_reset();
    vpy3d_line_world(s_aim_x - AIM_ARM, s_aim_y, 0, s_aim_x + AIM_ARM, s_aim_y, 0, BR_AIM);
    vpy3d_line_world(s_aim_x, s_aim_y - AIM_ARM, 0, s_aim_x, s_aim_y + AIM_ARM, 0, BR_AIM);
    vpyent_draw();
    vpyfx_draw(1);
    vpyent_draw_shadows(300, -1000, 200, 0, 60, 35);
    draw_pad();
    draw_floor();

#ifdef PLAYROOM_HOST
    {
        static uint32_t worst;
        if (vpyp_stats()->contacts > worst) worst = vpyp_stats()->contacts;
        if (getenv("TRACE") && pr_pressed(3)) printf("crates: worst contacts %u, dropped %u, bodies refused %u, vpyent refused %u\n",
                                                    worst, vpyp_stats()->contacts_dropped, vpyp_stats()->refused, vpyent_stats()->refused);
    }
#endif
    int off = 0;
    for (int i = 0; i < N_BLOCKS; i++) off += off_pad(s_block[i]);
    pr_objective("BLOCKS OFF THE PAD", off, N_BLOCKS);
    return off == N_BLOCKS;
}
