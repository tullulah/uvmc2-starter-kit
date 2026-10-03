/* scene_demo — a scene built from entities (vpyent), with text in the world.
 *
 * EVERYTHING SOLID IS AN ENTITY. Crates, balls and the low walls round the floor
 * are vpyent entities: each a vpyphys body, a mesh, an occluder shape and a
 * vpyimpact material in one table. ONE call draws the whole scene near to far
 * through the occluder (vpyent_draw) and ONE steps camera, physics, impact
 * sounds and effects in order (vpyent_step) — the bookkeeping physics_demo still
 * writes by hand, here not written at all. A shot dents a crate and marks it
 * through the entity, so the dent and the ring go wherever the crate goes.
 *
 * TEXT IN THE WORLD (vpy3d_text), three ways, and the orbiting camera shows the
 * difference between them:
 *   - a SIGN over the scene, turning slowly: two faces back to back, each drawn
 *     only from its front (VPY3D_TEXT_FRONT), so it never reads backwards — as
 *     it turns, "VECTREX" gives way to "3D TEXT";
 *   - words PAINTED ON THE FLOOR, foreshortened like the floor, hidden by what
 *     stands on them;
 *   - LABELS over the newest bodies (vpy3d_text_billboard), that follow them as
 *     they fall and stay square to the camera from every side.
 * All three go through the occluder: a crate in front cuts them.
 *
 * THE CAMERA IS THE LISTENER: a crate landing far across the floor is quieter
 * than one at your feet, and on a UVMC2 with its jack the hits come out in
 * stereo, on their side (vpyimpact).
 *
 * CONTROLS
 *   stick left/right   turn the camera round the scene (it also turns slowly by itself)
 *   stick up/down      raise or lower the camera
 *   button 1           drop a ball
 *   button 2           shoot through the centre of the screen: a crate is dented
 *                      and marked where it is hit, a ball is knocked away
 *   button 3           drop a crate
 *   button 4           start again
 *
 * THE READOUT: STK strokes this frame, DROP (must be 0), ENT entities alive,
 * REF creates refused (the table was full — never here: the oldest body is
 * recycled first).
 */
#include <vpy.h>
#include <vpy3d.h>
#include <vpyphys.h>
#include <vpyfx.h>
#include <vpycam.h>
#include <vpyimpact.h>
#include <vpyent.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#include <uvm2_jack.h>  /* the UVMC2's stereo DAC, where there is one */
#endif

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define GRAVITY      9800     /* mm/s²: Earth */
#define FLOOR_HALF   1200     /* the floor spans +-FLOOR_HALF in x and z */
#define WALL_H        120     /* the low walls' half height */
#define WALL_T         40     /* and half thickness */
#define CRATE         120     /* half size */
#define BALL          100     /* radius */
#define DROP_Y       1500
#define DROP_SPREAD   450     /* drops land within this of DROP_Z in z, of 0 in x */
#define DROP_Z        200     /* off-centre, away from the painted words, so they stay readable */
#define SHOT_KICK   20000     /* mm/s per unit of mass on a ball */
#define CRATE_NUDGE     6     /* a crate takes a sixth of it: it rocks, it does not fly */
#define SHOT_DENT      90     /* mm, deep on purpose: a shallow dent does not show this small */
#define DENT_R        200
#define MAX_BODIES     18     /* moving bodies kept; the oldest is recycled past this */

/* THE CAMERA ORBITS the focus at ORBIT_R, at a height the stick moves */
#define FOCUS_Y       200
#define ORBIT_R      3400
#define CAM_Y_MIN     700
#define CAM_Y_MAX    2600
#define CAM_Y_START  1500
#define AUTO_TURN       3     /* 4096ths of a turn per frame: a turn in ~27 s */
#define STICK_TURN     24     /* at full stick */
#define STICK_RISE     30     /* mm per frame at full stick */

/* THE TEXT: heights are a capital letter's, world units */
#define SIGN_Y       1050     /* the turning sign, over the scene */
#define SIGN_H        200
#define SIGN_TURN       5     /* 4096ths of a turn per frame */
#define FLOOR_TEXT_Z (-900)   /* painted near the wall the camera starts behind, clear of the drops */
#define FLOOR_TEXT_H  190
#define LABEL_H        70
#define LABEL_LIFT     90     /* how far above a body's top its label floats */
#define LABELS          6     /* the newest bodies get one: text is the dearest thing to draw */

#define BR_SOLID      110
#define BR_WALL        70
#define BR_FLOOR       40
#define BR_TEXT        90
#define BR_PAINT       60
#define BR_LABEL       80
#define BR_AIM         90
#define FX_BUDGET      90     /* effect strokes a frame, shed first */

/* the impact sound's range (mass × mm/s) and how it falls off from the camera */
#define IMPACT_QUIET  1500
#define IMPACT_LOUD  12000
#define HEAR_NEAR    3000
#define HEAR_FAR     9000

enum { K_WALL = 1, K_CRATE, K_BALL };

static vpy_mesh s_crate, s_ball, s_wall_x, s_wall_z;
/* the moving bodies, oldest first, and the number each one's label shows */
static int s_dyn[MAX_BODIES], s_ndyn;
static int s_num[VPYENT_MAX], s_next_num;
static int s_ang, s_cam_y = CAM_Y_START, s_sign;
static int s_held[5];
static int32_t s_eye[3];

/* ── meshes ─────────────────────────────────────────────────────────────── */
/* A crate with a vertex in the middle of every face: undented it draws like a plain
 * box, but a face can only fold where it has a vertex, so these let a dent show. Its
 * 14 vertices are too many for the mesh occluder, so its entity hides as a BOX. */
static void build_crate(vpy_mesh *m, int h)
{
    int v[8];
    static const int F[6][4] = { {0,4,6,2}, {1,3,7,5}, {0,1,5,4}, {2,6,7,3}, {0,2,3,1}, {4,5,7,6} };
    static const int C[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };
    vpy3d_mesh_begin(m);
    for (int k = 0; k < 8; k++) v[k] = vpy3d_vertex((k & 1) ? h : -h, (k & 2) ? h : -h, (k & 4) ? h : -h);
    for (int f = 0; f < 6; f++) {
        const int c = vpy3d_vertex(C[f][0] * h, C[f][1] * h, C[f][2] * h);
        for (int i = 0; i < 4; i++) vpy3d_tri(c, v[F[f][i]], v[F[f][(i + 1) % 4]]);
    }
    vpy3d_mesh_end(15826);
}

static void build_box(vpy_mesh *m, int hx, int hy, int hz)
{
    int v[8];
    vpy3d_mesh_begin(m);
    for (int k = 0; k < 8; k++)
        v[k] = vpy3d_vertex((k & 1) ? hx : -hx, (k & 2) ? hy : -hy, (k & 4) ? hz : -hz);
    vpy3d_quad(v[0], v[4], v[6], v[2]);  vpy3d_quad(v[1], v[3], v[7], v[5]);
    vpy3d_quad(v[0], v[1], v[5], v[4]);  vpy3d_quad(v[2], v[6], v[7], v[3]);
    vpy3d_quad(v[0], v[2], v[3], v[1]);  vpy3d_quad(v[4], v[5], v[7], v[6]);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

/* A low sphere, outline only (HARD_60): a ball reads as round on the tube. */
#define SEG   8
#define RINGS 3
static void build_ball(vpy_mesh *m, int r)
{
    int ring[RINGS][SEG];
    vpy3d_mesh_begin(m);
    const int bottom = vpy3d_vertex(0, -r, 0), top = vpy3d_vertex(0, r, 0);
    for (int j = 0; j < RINGS; j++) {
        const int lat = (j - 1) * (VPY_Q14_TURN / 8);          /* -45, 0, +45 degrees */
        const int y = r * vpy_sin_q14(lat) / VPY_Q14_ONE, rr = r * vpy_cos_q14(lat) / VPY_Q14_ONE;
        for (int i = 0; i < SEG; i++) {
            const int a = i * VPY_Q14_TURN / SEG;
            ring[j][i] = vpy3d_vertex(rr * vpy_cos_q14(a) / VPY_Q14_ONE, y,
                                      rr * vpy_sin_q14(a) / VPY_Q14_ONE);
        }
    }
    for (int i = 0; i < SEG; i++) {
        const int n = (i + 1) % SEG;
        vpy3d_tri(bottom, ring[0][n], ring[0][i]);
        for (int j = 0; j + 1 < RINGS; j++)
            vpy3d_quad(ring[j][i], ring[j][n], ring[j + 1][n], ring[j + 1][i]);
        vpy3d_tri(top, ring[RINGS - 1][i], ring[RINGS - 1][n]);
    }
    vpy3d_mesh_end(VPY3D_HARD_60);
}

/* ── the world, as entities ─────────────────────────────────────────────── */
static void forget(int e)
{
    for (int i = 0; i < s_ndyn; i++)
        if (s_dyn[i] == e) {
            for (int j = i + 1; j < s_ndyn; j++) s_dyn[j - 1] = s_dyn[j];
            s_ndyn--;
            return;
        }
}

/* A moving body as an entity. The oldest goes first when there are MAX_BODIES —
 * asked BEFORE creating, so vpyent never has to refuse (its `refused` stays 0). */
static int add_dynamic(int kind, int32_t x, int32_t y, int32_t z)
{
    if (s_ndyn >= MAX_BODIES) { const int old = s_dyn[0]; forget(old); vpyent_destroy(old); }
    const int crate = kind == K_CRATE;
    const int body = crate ? vpyp_add_box(x, y, z, CRATE, CRATE, CRATE, 2) : vpyp_add_sphere(x, y, z, BALL, 1);
    if (body < 0) return VPYENT_NONE;
    const int e = vpyent_create(crate ? &s_crate : &s_ball, body);
    if (e < 0) { vpyp_remove(body); return VPYENT_NONE; }
    vpyent_set_kind(e, kind);
    vpyent_set_brightness(e, BR_SOLID);
    if (crate) {
        vpyp_set_material(body, 40, 150);
        vpyent_set_occluder(e, VPYENT_OCC_BOX, CRATE, CRATE, CRATE);
        vpyent_set_material(e, VPYI_WOOD);
    } else {
        vpyp_set_material(body, 150, 90);
        vpyent_set_occluder(e, VPYENT_OCC_SPHERE, BALL, 0, 0);
        vpyent_set_material(e, VPYI_SOFT);
    }
    s_num[e] = ++s_next_num;
    s_dyn[s_ndyn++] = e;
    return e;
}

static void add_wall(const vpy_mesh *m, int32_t x, int32_t z, int32_t hx, int32_t hz)
{
    const int body = vpyp_add_box(x, WALL_H, z, hx, WALL_H, hz, 0);
    if (body < 0) return;
    const int e = vpyent_create(m, body);          /* 8 vertices: it hides by its own mesh */
    if (e < 0) { vpyp_remove(body); return; }
    vpyent_set_kind(e, K_WALL);
    vpyent_set_brightness(e, BR_WALL);
}

static void setup_world(void)
{
    vpyent_reset();
    vpyp_reset();
    vpyp_set_gravity(0, -GRAVITY, 0);
    vpyp_set_floor(1, 0, 60, 160);
    s_ndyn = 0; s_next_num = 0;
    add_wall(&s_wall_x, 0,  FLOOR_HALF + WALL_T, FLOOR_HALF + WALL_T, WALL_T);
    add_wall(&s_wall_x, 0, -FLOOR_HALF - WALL_T, FLOOR_HALF + WALL_T, WALL_T);
    add_wall(&s_wall_z,  FLOOR_HALF + WALL_T, 0, WALL_T, FLOOR_HALF);
    add_wall(&s_wall_z, -FLOOR_HALF - WALL_T, 0, WALL_T, FLOOR_HALF);
    /* a small stack — two, then one on top — and two balls on their way down */
    add_dynamic(K_CRATE, -CRATE - 4, CRATE, 250);
    add_dynamic(K_CRATE,  CRATE + 4, CRATE, 250);
    add_dynamic(K_CRATE,  0, 3 * CRATE + 8, 250);
    add_dynamic(K_BALL, -600, 900, -200);
    add_dynamic(K_BALL,  550, 1300, 100);
    vpycam_reset(0, FOCUS_Y, 0);
    vpyfx_reset();
    vpyfx_seed(1);
    vpyfx_set_gravity(0, -GRAVITY, 0);
    vpyfx_set_floor(1, 0, 90);
    vpyfx_set_budget(FX_BUDGET);
    vpyimpact_reset();
    vpyimpact_set_range(IMPACT_QUIET, IMPACT_LOUD);
}

static int64_t isqrt64(int64_t n)
{
    int64_t r = 0, bit = (int64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* THE SHOT goes from the eye through the focus — the centre of the screen, where the
 * cross is. Whatever the ray meets first is hit there. */
static void shoot(void)
{
    const int32_t dx = 0 - s_eye[0], dy = FOCUS_Y - s_eye[1], dz = 0 - s_eye[2];
    vpyp_hit h;
    const int body = vpyp_raycast(s_eye[0], s_eye[1], s_eye[2], dx, dy, dz, 20000, 0xFF, &h);
    if (body == VPYP_NONE) return;
    vpyfx_burst(h.x, h.y, h.z, 0, 0, 0, 10, 2500, 16, 127);
    const int e = body >= 0 ? vpyent_of_body(body) : VPYENT_NONE;
    if (e < 0 || vpyent_kind(e) == K_WALL) return;
    const int crate = vpyent_kind(e) == K_CRATE;
    if (crate) {
        vpyent_dent(e, h.x, h.y, h.z, dx, dy, dz, SHOT_DENT, DENT_R);
        vpyent_mark(e, h.x, h.y, h.z, h.nx, h.ny, h.nz);
    }
    const int64_t l = isqrt64((int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz);
    const int32_t m = crate ? 2 : 1, kick = crate ? SHOT_KICK / CRATE_NUDGE : SHOT_KICK;
    vpyp_apply_impulse_at(body, (int32_t)((int64_t)dx * kick * m / l),
                                (int32_t)((int64_t)dy * kick * m / l) + kick * m / 4,
                                (int32_t)((int64_t)dz * kick * m / l), h.x, h.y, h.z);
}

/* ── drawing ────────────────────────────────────────────────────────────── */
/* "C12", "B3": what a label says */
static void label_text(int e, char *out)
{
    int p = 0, n = s_num[e];
    out[p++] = vpyent_kind(e) == K_CRATE ? 'C' : 'B';
    char d[6]; int k = 0;
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 5);
    while (k) out[p++] = d[--k];
    out[p] = 0;
}

/* LABELS over the newest bodies, square to the camera wherever it is. After the
 * entities, so a body in front cuts them. */
static void draw_labels(void)
{
    const int first = s_ndyn > LABELS ? s_ndyn - LABELS : 0;
    for (int i = first; i < s_ndyn; i++) {
        const int e = s_dyn[i];
        vpy_xf at; vpyent_place(e, &at);
        const int32_t top = vpyent_kind(e) == K_CRATE ? CRATE : BALL;
        char t[8]; label_text(e, t);
        vpy3d_text_billboard(t, at.t[0], at.t[1] + top + LABEL_LIFT, at.t[2], LABEL_H, BR_LABEL,
                             VPY3D_TEXT_CENTRE | VPY3D_TEXT_OCCLUDE);
    }
}

/* THE SIGN: two faces back to back, each drawn only from its front, so whichever
 * faces the camera reads the right way round. */
static void draw_sign(void)
{
    vpy_xf a = vpy3d_rot_y(s_sign);
    a.t[0] = 0; a.t[1] = SIGN_Y; a.t[2] = 0;
    vpy3d_text("VECTREX", &a, SIGN_H, BR_TEXT, VPY3D_TEXT_CENTRE | VPY3D_TEXT_FRONT | VPY3D_TEXT_OCCLUDE);
    vpy_xf b = vpy3d_rot_y(s_sign + VPY_Q14_TURN / 2);
    b.t[0] = 0; b.t[1] = SIGN_Y; b.t[2] = 0;
    vpy3d_text("3D TEXT", &b, SIGN_H, BR_TEXT, VPY3D_TEXT_CENTRE | VPY3D_TEXT_FRONT | VPY3D_TEXT_OCCLUDE);
}

/* PAINTED ON THE FLOOR: a plane turned a quarter about x, so its letters run along
 * the floor away from the camera's start, read from above. */
static void draw_floor_text(void)
{
    vpy_xf f = vpy3d_rot_x(VPY_Q14_TURN / 4);
    f.t[0] = 0; f.t[1] = 2; f.t[2] = FLOOR_TEXT_Z;
    vpy3d_text("ENTITIES", &f, FLOOR_TEXT_H, BR_PAINT, VPY3D_TEXT_CENTRE | VPY3D_TEXT_OCCLUDE);
}

static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_HALF / 3) {
        vpy3d_occl_line(t, 0, -FLOOR_HALF, t, 0, FLOOR_HALF, BR_FLOOR);
        vpy3d_occl_line(-FLOOR_HALF, 0, t, FLOOR_HALF, 0, t, BR_FLOOR);
    }
}

/* the cross at the centre of the screen, where a shot goes: nearest of all */
static void draw_aim(void)
{
    const int32_t a = 900;
    vpy_draw_line_dev(-a, 0, a, 0, BR_AIM);
    vpy_draw_line_dev(0, -a, 0, a, BR_AIM);
}

static int pressed(int n)
{
    const int b = vpy_j1_button(n);
    const int edge = b && !s_held[n];
    s_held[n] = b;
    return edge;
}

#ifndef VPY_DUAL_CORE
/* the jack, if this cartridge has one: the hits in stereo, the speaker still on */
static int s_jack;
static void jack_sink(const int16_t *l, const int16_t *r, int n) { uvm2_jack_write_lr(l, r, n); }
#endif

static void setup(void)
{
    build_crate(&s_crate, CRATE);
    build_ball(&s_ball, BALL);
    build_box(&s_wall_x, FLOOR_HALF + WALL_T, WALL_H, WALL_T);
    build_box(&s_wall_z, WALL_T, WALL_H, FLOOR_HALF);
    setup_world();
#ifndef VPY_DUAL_CORE
    s_jack = uvm2_jack_init();
    if (s_jack) vpyimpact_set_pcm(jack_sink, UVM2_JACK_RATE, 1);
#endif
}

static int32_t drop_x(void) { return vpy_rand_range(-DROP_SPREAD, DROP_SPREAD); }
static int32_t drop_z(void) { return DROP_Z + vpy_rand_range(-DROP_SPREAD, DROP_SPREAD); }

static void loop(void)
{
    /* input: the stick moves the camera, the buttons the world */
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    s_ang += AUTO_TURN;
    if (jx > 30 || jx < -30) s_ang += jx * STICK_TURN / 127;
    s_ang &= VPY_Q14_TURN - 1;
    if (jy > 30 || jy < -30) s_cam_y += jy * STICK_RISE / 127;
    if (s_cam_y < CAM_Y_MIN) s_cam_y = CAM_Y_MIN;
    if (s_cam_y > CAM_Y_MAX) s_cam_y = CAM_Y_MAX;
    s_sign = (s_sign + SIGN_TURN) & (VPY_Q14_TURN - 1);
    if (pressed(1)) add_dynamic(K_BALL, drop_x(), DROP_Y, drop_z());
    if (pressed(2)) shoot();
    if (pressed(3)) add_dynamic(K_CRATE, drop_x(), DROP_Y, drop_z());
    if (pressed(4)) setup_world();

    /* the world: camera, physics, impact sounds, effects — in that order, in one call */
    vpyent_step(VPYI_NONE);
#ifndef VPY_DUAL_CORE
    if (s_jack) vpyimpact_pcm(uvm2_jack_space());   /* what the DAC can take, every frame */
#endif

    /* the camera, and the listener with it: its right is the camera's own x axis */
    s_eye[0] = (int32_t)((int64_t)ORBIT_R * vpy_sin_q14(s_ang) / VPY_Q14_ONE);
    s_eye[1] = s_cam_y;
    s_eye[2] = (int32_t)(-(int64_t)ORBIT_R * vpy_cos_q14(s_ang) / VPY_Q14_ONE);
    vpycam_look_at(s_eye[0], s_eye[1], s_eye[2]);
    const vpy_xf *cam = vpy3d_camera();
    vpyimpact_set_listener(s_eye[0], s_eye[1], s_eye[2], cam->m[0], cam->m[2], HEAR_NEAR, HEAR_FAR);

    /* draw, near to far: the aim, then every entity, then what they may hide */
    vpy3d_occl_reset();
    draw_aim();
    vpyent_draw();
    draw_labels();
    draw_sign();
    vpyfx_draw(1);
    vpyent_draw_shadows(300, -1000, 200, 0, 60, 35);
    draw_floor_text();
    draw_floor();

    /* readout */
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    const vpyent_stats_t *es = vpyent_stats();
    vpy_set_text_size(6);
    vpy_print_text(-118, 108, "ENT"); vpy_print_number(-96, 108, (long)es->alive);
    vpy_print_text( -40, 108, "REF"); vpy_print_number(-18, 108, (long)es->refused);
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
