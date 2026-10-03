/* fx_demo — vpyfx: an object that comes apart where it is shot, and puts itself
 * back together.
 *
 * A wireframe shape turns over a floor. Shoot it and it DISINTEGRATES from the
 * point the shot landed: every edge is cut into short pieces, and a wave spreads
 * out from the hit — the pieces it has not reached yet stay in place, so the
 * object is still whole there, and the ones it has reached fly off, spin, fall
 * and fade. When the last piece is gone the next shape ASSEMBLES: its pieces fly
 * in from all round and settle on its edges, one edge after another, and the
 * moment the last one is in place the pieces give way to the real mesh.
 *
 * Both effects are vpyfx (sdk/vpy-c/include/vpyfx.h); the shot is a vpy3d ray
 * against the mesh, so it hits what is drawn.
 *
 * CONTROLS
 *   stick      move the aim
 *   button 1   fire: the shape comes apart from where it is hit
 *   button 2   blow it apart all at once, from its middle (no wave)
 *   button 3   dissolve it from its middle: the wave, without a shot
 * Whatever breaks it, the next shape assembles once the last piece is gone.
 *   button 4   start again
 *
 * THE READOUT: PCS pieces alive, SHED pieces the stroke budget left out (0 here:
 * the budget is set to the most pieces a shape makes), STK strokes this frame,
 * DROP must be 0.
 */
#include <vpy.h>
#include <vpy3d.h>
#include <vpyfx.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#endif

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define SHAPE_Y        420    /* the shape's centre above the floor */
#define SHAPE_HALF     230
#define SPIN_Y           9    /* 4096ths of a turn a frame: a turn in ~9 s */
#define TILT_X         300    /* a fixed tilt, so the top face shows */
#define FLOOR_HALF    1200
#define FLOOR_STEP     300
#define AIM_SPEED       24    /* mm a frame at full stick */
#define AIM_X          800
#define AIM_TOP       1000
#define AIM_ARM         50    /* the cross's half size */
#define SHOT_RANGE   10000

/* THE EFFECTS. PER_EDGE 8: a cube is 96 pieces, the octahedron 96, the pyramid
 * 64 — under the pool (VPYFX_MAX 192) with room for the sparks of the hit. */
#define PER_EDGE         8
#define FLY_SPEED      900    /* mm/s, the fastest a piece leaves */
#define FLY_SPIN      1500    /* 4096ths of a turn per second */
#define WAVE          1400    /* mm/s: crosses the shape in about a third of a second */
#define FLY_LIFE        90    /* steps: under 2 s, give or take a quarter */
#define GRAVITY       2500    /* mm/s²: floaty, so the pieces hang before they land */
#define BOUNCE_Q8       64
#define GATHER_SCATTER 900    /* how far out the pieces start */
#define GATHER_FRAMES   45    /* each piece's flight in */
#define GATHER_STAGGER  35    /* the edges set off one after another across this */
#define PAUSE           25    /* frames of empty floor before the next shape comes */
#define SPARKS          12
#define SPARK_SPEED   1500
#define SPARK_LIFE      20

#define BR_SHAPE       120    /* the mesh and its pieces alike, so the hand-over is seamless */
#define BR_FLOOR        40
#define BR_AIM          90
#define BR_SPARK       127

#define EYE_Y          900
#define EYE_Z        -2400
#define LOOK_Y         350

enum { N_SHAPES = 3 };
static vpy_mesh s_mesh[N_SHAPES];
enum { SOLID, BREAKING, GATHERING };
static int s_state, s_shape, s_group, s_wait, s_angle;
static int32_t s_aim_x, s_aim_y;
static int s_held[5];

/* ── meshes ─────────────────────────────────────────────────────────────── */
static void build_cube(vpy_mesh *m, int h)
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

static void build_octahedron(vpy_mesh *m, int h)
{
    vpy3d_mesh_begin(m);
    const int px = vpy3d_vertex(h, 0, 0), nx = vpy3d_vertex(-h, 0, 0);
    const int py = vpy3d_vertex(0, h, 0), ny = vpy3d_vertex(0, -h, 0);
    const int pz = vpy3d_vertex(0, 0, h), nz = vpy3d_vertex(0, 0, -h);
    vpy3d_tri(py, pz, px); vpy3d_tri(py, px, nz); vpy3d_tri(py, nz, nx); vpy3d_tri(py, nx, pz);
    vpy3d_tri(ny, px, pz); vpy3d_tri(ny, nz, px); vpy3d_tri(ny, nx, nz); vpy3d_tri(ny, pz, nx);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

static void build_pyramid(vpy_mesh *m, int h)
{
    vpy3d_mesh_begin(m);
    const int a = vpy3d_vertex(-h, -h, -h), b = vpy3d_vertex(h, -h, -h);
    const int c = vpy3d_vertex(h, -h, h),   d = vpy3d_vertex(-h, -h, h);
    const int top = vpy3d_vertex(0, h, 0);
    vpy3d_quad(a, b, c, d);
    vpy3d_tri(top, b, a); vpy3d_tri(top, c, b); vpy3d_tri(top, d, c); vpy3d_tri(top, a, d);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

/* where the shape is this frame */
static vpy_xf place(void)
{
    const vpy_xf t = vpy3d_translate(0, SHAPE_Y, 0);
    const vpy_xf ry = vpy3d_rot_y(s_angle), rx = vpy3d_rot_x(TILT_X);
    const vpy_xf r = vpy3d_mul(&ry, &rx);
    return vpy3d_mul(&t, &r);
}

/* ── the effects ────────────────────────────────────────────────────────── */
static void come_apart(int32_t x, int32_t y, int32_t z, int32_t wave)
{
    const vpy_xf at = place();
    vpyfx_disintegrate(&s_mesh[s_shape], &at, x, y, z, PER_EDGE, FLY_SPEED, FLY_SPIN, wave, FLY_LIFE, BR_SHAPE);
    s_state = BREAKING;
}

static void come_together(void)
{
    const vpy_xf at = place();      /* the turn stops while it assembles: the slots are fixed */
    s_group = vpyfx_assemble(&s_mesh[s_shape], &at, PER_EDGE, GATHER_SCATTER, GATHER_FRAMES, GATHER_STAGGER, BR_SHAPE);
    s_state = s_group ? GATHERING : SOLID;   /* nothing made (pool exhausted): just show it */
}

static void fire(void)
{
    int32_t eye[3]; vpy3d_eye(eye);
    const vpy_xf at = place();
    vpy3d_hit h;
    if (vpy3d_ray_mesh(&s_mesh[s_shape], &at, eye[0], eye[1], eye[2],
                       s_aim_x - eye[0], s_aim_y - eye[1], 0 - eye[2], SHOT_RANGE, &h) < 0)
        return;                                 /* a miss: nothing to break */
    vpyfx_burst(h.x, h.y, h.z, 0, 0, 0, SPARKS, SPARK_SPEED, SPARK_LIFE, BR_SPARK);
    come_apart(h.x, h.y, h.z, WAVE);
}

static void start(void)
{
    vpyfx_reset();
    vpyfx_seed(1);
    vpyfx_set_gravity(0, -GRAVITY, 0);
    vpyfx_set_floor(1, 0, BOUNCE_Q8);
    vpyfx_set_budget(VPYFX_MAX);   /* every piece: this demo is the effect */
    s_shape = 0; s_angle = 0; s_aim_x = 0; s_aim_y = SHAPE_Y;
    come_together();
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
    build_cube(&s_mesh[0], SHAPE_HALF);
    build_octahedron(&s_mesh[1], SHAPE_HALF * 5 / 4);
    build_pyramid(&s_mesh[2], SHAPE_HALF);
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);
    start();
}

static void loop(void)
{
    vpy3d_look_at(0, EYE_Y, EYE_Z, 0, LOOK_Y, 0, 0, 1, 0);

    /* input */
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    if (jx > 30 || jx < -30) s_aim_x += jx * AIM_SPEED / 127;
    if (jy > 30 || jy < -30) s_aim_y += jy * AIM_SPEED / 127;
    if (s_aim_x >  AIM_X) s_aim_x =  AIM_X;
    if (s_aim_x < -AIM_X) s_aim_x = -AIM_X;
    if (s_aim_y > AIM_TOP) s_aim_y = AIM_TOP;
    if (s_aim_y < 0) s_aim_y = 0;
    const int b1 = pressed(1), b2 = pressed(2), b3 = pressed(3);
    if (pressed(4)) start();
    else if (s_state == SOLID) {
        if (b1) fire();
        else if (b2 || b3) come_apart(0, SHAPE_Y, 0, b3 ? WAVE : 0);
    }

    /* the state */
    if (s_state == SOLID) s_angle = (s_angle + SPIN_Y) & 4095;
    vpyfx_step();
    if (s_state == BREAKING && vpyfx_stats()->alive == 0) {
        if (s_wait <= 0) s_wait = PAUSE;
        else if (--s_wait == 0) {
            s_shape = (s_shape + 1) % N_SHAPES;
            come_together();
        }
    }

    /* draw: the shape, or its pieces — never both, except the frame they swap */
    const vpy_xf at = place();
    if (s_state == GATHERING && vpyfx_assembled(s_group)) {
        vpyfx_release(s_group);                 /* the mesh takes over this very frame */
        s_state = SOLID;
    }
    if (s_state == SOLID) vpy3d_draw_mesh(&s_mesh[s_shape], &at, BR_SHAPE);
    vpyfx_draw(0);
    draw_floor();
    vpy3d_line_world(s_aim_x - AIM_ARM, s_aim_y, 0, s_aim_x + AIM_ARM, s_aim_y, 0, BR_AIM);
    vpy3d_line_world(s_aim_x, s_aim_y - AIM_ARM, 0, s_aim_x, s_aim_y + AIM_ARM, 0, BR_AIM);

    /* readout */
    const vpyfx_stats_t *fs = vpyfx_stats();
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_set_text_size(6);
    vpy_print_text(-118, 108, "PCS");  vpy_print_number(-96, 108, (long)fs->alive);
    vpy_print_text(  50, 108, "SHED"); vpy_print_number( 78, 108, (long)fs->shed);
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
