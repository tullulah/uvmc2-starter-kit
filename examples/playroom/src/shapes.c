/* shapes.c — SHAPES: shoot five shapes to pieces.
 *
 * fx_demo, made a game. A wireframe shape turns and drifts from side to side; a
 * shot is a vpy3d ray against its mesh, and where it lands the shape
 * DISINTEGRATES (vpyfx_disintegrate): every edge cut into short pieces, a wave
 * spreading from the hit, the far side still whole until the wave gets there.
 * When the last piece has gone the next shape ASSEMBLES (vpyfx_assemble), edge
 * after edge, and the pieces give way to the real mesh the frame they are all in
 * place. A ring goes out from every hit (vpyfx_ring) and it rings like metal
 * (vpyimpact).
 */
#include "playroom.h"
#include <vpyfx.h>
#include <vpyimpact.h>

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define SHAPE_Y        420
#define SHAPE_HALF     230
#define SPIN_Y           9    /* 4096ths of a turn a frame */
#define TILT_X         300
#define DRIFT          700    /* mm either side */
#define DRIFT_TURN     300    /* frames for one swing there and back */
#define FLOOR_HALF    1800
#define FLOOR_STEP     300
#define AIM_SPEED       24
#define AIM_X         1000
#define AIM_TOP       1000
#define AIM_ARM         50
#define SHOT_RANGE   10000
#define NEED             5

/* the effects: fx_demo's, which are measured there (8 per edge: 96 pieces a cube) */
#define PER_EDGE         8
#define FLY_SPEED      900
#define FLY_SPIN      1500
#define WAVE          1400
#define FLY_LIFE        90
#define GRAVITY       2500
#define BOUNCE_Q8       64
#define GATHER_SCATTER 900
#define GATHER_FRAMES   45
#define GATHER_STAGGER  35
#define PAUSE           25

#define BR_SHAPE       120
#define BR_FLOOR        40
#define BR_AIM          90

#define EYE_Y          900
#define EYE_Z        -2600
#define LOOK_Y         350

enum { N_SHAPES = 3 };
static vpy_mesh s_mesh[N_SHAPES];
enum { SOLID, BREAKING, GATHERING };
static int s_state, s_shape, s_group, s_wait, s_angle, s_drift_t, s_broken;
static int32_t s_aim_x, s_aim_y;

void shapes_build(void)
{
    pr_box(&s_mesh[0], SHAPE_HALF, SHAPE_HALF, SHAPE_HALF);
    pr_octahedron(&s_mesh[1], SHAPE_HALF * 5 / 4);
    pr_pyramid(&s_mesh[2], SHAPE_HALF);
}

/* where the shape is: it turns and drifts only while it is whole, so the slots
 * an assembly flies to are where the mesh then takes over */
static vpy_xf place(void)
{
    const int32_t x = DRIFT * vpy_sin_q14(s_drift_t * VPY_Q14_TURN / DRIFT_TURN) / VPY_Q14_ONE;
    const vpy_xf t = vpy3d_translate(x, SHAPE_Y, 0);
    const vpy_xf ry = vpy3d_rot_y(s_angle), rx = vpy3d_rot_x(TILT_X);
    const vpy_xf r = vpy3d_mul(&ry, &rx);
    return vpy3d_mul(&t, &r);
}

static void come_together(void)
{
    const vpy_xf at = place();
    s_group = vpyfx_assemble(&s_mesh[s_shape], &at, PER_EDGE, GATHER_SCATTER, GATHER_FRAMES, GATHER_STAGGER, BR_SHAPE);
    s_state = s_group ? GATHERING : SOLID;
}

void shapes_enter(void)
{
    vpyfx_reset();
    vpyfx_seed(11);
    vpyfx_set_gravity(0, -GRAVITY, 0);
    vpyfx_set_floor(1, 0, BOUNCE_Q8);
    vpyfx_set_budget(VPYFX_MAX);       /* every piece: the effect is the point here */
    s_shape = 0; s_angle = 0; s_drift_t = 0; s_broken = 0; s_wait = 0;
    s_aim_x = 0; s_aim_y = SHAPE_Y;
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
    come_together();
}

static void fire(void)
{
    int32_t eye[3]; vpy3d_eye(eye);
    const vpy_xf at = place();
    vpy3d_hit h;
    if (vpy3d_ray_mesh(&s_mesh[s_shape], &at, eye[0], eye[1], eye[2],
                       s_aim_x - eye[0], s_aim_y - eye[1], 0 - eye[2], SHOT_RANGE, &h) < 0)
        return;
    vpyfx_burst(h.x, h.y, h.z, 0, 0, 0, 10, 1500, 20, 127);
    vpyfx_ring(h.x, h.y, h.z, h.nx, h.ny, h.nz, 30, 1600, 12, 18, 110);
    vpyfx_disintegrate(&s_mesh[s_shape], &at, h.x, h.y, h.z, PER_EDGE, FLY_SPEED, FLY_SPIN, WAVE, FLY_LIFE, BR_SHAPE);
    vpyimpact_hit(14000, VPYI_METAL);
    s_state = BREAKING;
    s_broken++;
}

int shapes_frame(void)
{
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    if (jx > 30 || jx < -30) s_aim_x += jx * AIM_SPEED / 127;
    if (jy > 30 || jy < -30) s_aim_y += jy * AIM_SPEED / 127;
    if (s_aim_x >  AIM_X) s_aim_x =  AIM_X;
    if (s_aim_x < -AIM_X) s_aim_x = -AIM_X;
    if (s_aim_y > AIM_TOP) s_aim_y = AIM_TOP;
    if (s_aim_y < 0) s_aim_y = 0;
    if (pr_pressed(1) && s_state == SOLID) fire();

    if (s_state == SOLID) { s_angle = (s_angle + SPIN_Y) & 4095; s_drift_t++; }
    vpyfx_step();
    vpyimpact_step();
    if (s_state == BREAKING && vpyfx_stats()->alive == 0) {
        if (s_wait <= 0) s_wait = PAUSE;
        else if (--s_wait == 0) { s_shape = (s_shape + 1) % N_SHAPES; come_together(); }
    }

    const vpy_xf at = place();
    if (s_state == GATHERING && vpyfx_assembled(s_group)) {
        vpyfx_release(s_group);                 /* the mesh takes over this very frame */
        s_state = SOLID;
    }
    if (s_state == SOLID) vpy3d_draw_mesh(&s_mesh[s_shape], &at, BR_SHAPE);
    vpyfx_draw(0);
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_STEP) {
        vpy3d_line_world(t, 0, -FLOOR_HALF, t, 0, FLOOR_HALF, BR_FLOOR);
        vpy3d_line_world(-FLOOR_HALF, 0, t, FLOOR_HALF, 0, t, BR_FLOOR);
    }
    vpy3d_line_world(s_aim_x - AIM_ARM, s_aim_y, 0, s_aim_x + AIM_ARM, s_aim_y, 0, BR_AIM);
    vpy3d_line_world(s_aim_x, s_aim_y - AIM_ARM, 0, s_aim_x, s_aim_y + AIM_ARM, 0, BR_AIM);
    pr_objective("SHAPES BROKEN", s_broken, NEED);
    return s_broken >= NEED;
}
