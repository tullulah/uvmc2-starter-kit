/* physics_demo — crates and balls under real gravity, a shot, and the impacts.
 *
 * vpyphys does the physics (sdk/vpy-c/include/vpyphys.h): bodies fall, bounce,
 * slide, stack, come to rest and go to sleep. vpy3d draws them, with mesh
 * occlusion on so a crate in front hides what is behind it. Every strong
 * contact leaves a spark where it happened, as bright as the hit was hard —
 * the contact list's `impulse` is what anything reacting to a crash reads.
 *
 * CONTROLS
 *   stick      move the aim (the cross on the floor)
 *   button 1   drop a ball over the aim
 *   button 2   shoot: a ray from the camera to the aim pushes what it meets
 *   button 3   drop a crate over the aim
 *   button 4   start again
 *
 * THE READOUT: BOD bodies alive, AWK the ones awake (sleeping ones cost almost
 * nothing), CON contacts this step, STK strokes, DROP must be 0. When the body
 * table is full the oldest dropped body is recycled; vpyphys counts a refusal
 * either way, so a full table is never silent.
 *
 * Bodies TURN: a shot off-centre spins a crate, crates tip over edges and
 * tumble, balls roll — each ball carries a ring that turns with it, because a
 * sphere's outline alone cannot show it rolling.
 */
#include <vpy.h>
#include <vpy3d.h>
#include <vpyphys.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#endif

/* ── the scene, in world units (mm). Composition, not measurement. ───────── */
#define GRAVITY      9800     /* mm/s²: Earth */
#define PIT          1400     /* the floor spans +-PIT in x and z */
#define WALL_H        250
#define WALL_T         60
#define CRATE         130     /* half size */
#define BALL          110     /* radius */
#define DROP_Y       1600
#define SHOT_KICK   20000     /* mm/s per unit of mass, along the ray */
#define AIM_SPEED      40     /* mm per frame at full stick */
/* At most this many bodies at once, of vpyphys's 64. MEASURED, not chosen: with
 * all 64 alive and tumbling the frame peaked at 955 strokes, over the ~940 one
 * frame holds; each ball's ring costs about four. 48 keeps the worst frame
 * inside it. */
#define DEMO_BODIES    48

#define BR_SOLID      110
#define BR_FLOOR       40
#define BR_AIM         90

#define EYE_X           0
#define EYE_Y        2300
#define EYE_Z       -4200

static vpy_mesh s_crate, s_ball, s_wall_x, s_wall_z;

enum { K_CRATE, K_BALL, K_WALL_X, K_WALL_Z };
static uint8_t s_kind[VPYP_MAX_BODIES];
static int s_dropped[VPYP_MAX_BODIES], s_ndropped;   /* oldest first */

static int32_t s_aim_x, s_aim_z;
static int s_held[5];

/* sparks: where a hard contact happened, fading */
#define SPARKS 16
static struct { int32_t x, y, z; int life, br; } s_spark[SPARKS];
static struct { int32_t x, y, z; int life; } s_tracer;

/* ── meshes ─────────────────────────────────────────────────────────────── */
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

/* A low sphere: 8 around, 3 rings and two poles. HARD_60 so only its outline
 * is drawn, not the facets — a ball reads as round on the tube. */
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

/* ── the world ──────────────────────────────────────────────────────────── */
static int add_crate(int32_t x, int32_t y, int32_t z)
{
    const int id = vpyp_add_box(x, y, z, CRATE, CRATE, CRATE, 2);
    if (id >= 0) { s_kind[id] = K_CRATE; vpyp_set_material(id, 40, 150); }
    return id;
}
static int add_ball(int32_t x, int32_t y, int32_t z)
{
    const int id = vpyp_add_sphere(x, y, z, BALL, 1);
    if (id >= 0) { s_kind[id] = K_BALL; vpyp_set_material(id, 150, 90); }
    return id;
}

static void setup_world(void)
{
    vpyp_reset();
    vpyp_set_gravity(0, -GRAVITY, 0);
    vpyp_set_floor(1, 0, 60, 160);
    /* four low walls, static */
    int w;
    w = vpyp_add_box(0, WALL_H, PIT + WALL_T, PIT + WALL_T, WALL_H, WALL_T, 0);  s_kind[w] = K_WALL_X;
    w = vpyp_add_box(0, WALL_H, -PIT - WALL_T, PIT + WALL_T, WALL_H, WALL_T, 0); s_kind[w] = K_WALL_X;
    w = vpyp_add_box(PIT + WALL_T, WALL_H, 0, WALL_T, WALL_H, PIT, 0);           s_kind[w] = K_WALL_Z;
    w = vpyp_add_box(-PIT - WALL_T, WALL_H, 0, WALL_T, WALL_H, PIT, 0);          s_kind[w] = K_WALL_Z;
    /* a pyramid of crates, three, two, one, with a small gap so it settles */
    for (int row = 0; row < 3; row++)
        for (int i = 0; i < 3 - row; i++)
            add_crate((i * 2 - (2 - row)) * (CRATE + 4), CRATE + row * (2 * CRATE + 4), 300);
    /* and a few balls already in the air */
    add_ball(-700, 900, -300);
    add_ball(650, 1300, -100);
    add_ball(200, 1900, 300);
    s_ndropped = 0;
    s_aim_x = 0; s_aim_z = -200;
    for (int i = 0; i < SPARKS; i++) s_spark[i].life = 0;
    s_tracer.life = 0;
}

/* Add, recycling the oldest dropped body first when the table is full — asked
 * BEFORE adding, so vpyphys never has to refuse (its `refused` stays 0). */
static void drop(int ball)
{
    int alive = 0;
    for (int id = 0; id < VPYP_MAX_BODIES; id++) alive += vpyp_alive(id);
    if (alive >= DEMO_BODIES && s_ndropped > 0) {
        vpyp_remove(s_dropped[0]);
        for (int i = 1; i < s_ndropped; i++) s_dropped[i - 1] = s_dropped[i];
        s_ndropped--;
    }
    const int id = ball ? add_ball(s_aim_x, DROP_Y, s_aim_z) : add_crate(s_aim_x, DROP_Y, s_aim_z);
    if (id >= 0) s_dropped[s_ndropped++] = id;
}

/* Integer square root of a 64-bit value: the length of the shot's ray. */
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

static void shoot(void)
{
    const int32_t tx = s_aim_x, ty = CRATE, tz = s_aim_z;
    const int32_t dx = tx - EYE_X, dy = ty - EYE_Y, dz = tz - EYE_Z;
    vpyp_hit h;
    const int id = vpyp_raycast(EYE_X, EYE_Y, EYE_Z, dx, dy, dz, 20000, 0xFF, &h);
    if (id == VPYP_NONE) return;
    s_tracer.x = h.x; s_tracer.y = h.y; s_tracer.z = h.z; s_tracer.life = 6;
    if (id >= 0) {
        /* the kick along the ray, the same speed whatever the ray's length or
         * the body's mass, AT the point it hit: off-centre, it spins the body */
        const int64_t l = isqrt64((int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz);
        const int32_t m = s_kind[id] == K_CRATE ? 2 : 1;
        vpyp_apply_impulse_at(id, (int32_t)((int64_t)dx * SHOT_KICK * m / l),
                                  (int32_t)((int64_t)dy * SHOT_KICK * m / l) + SHOT_KICK * m / 4,
                                  (int32_t)((int64_t)dz * SHOT_KICK * m / l),
                              h.x, h.y, h.z);
    }
}

static void sparks_from_contacts(void)
{
    for (int i = 0; i < vpyp_contact_count(); i++) {
        const vpyp_contact *c = vpyp_contact_get(i);
        if (c->impulse < 2500) continue;
        int slot = 0;
        for (int k = 0; k < SPARKS; k++) if (s_spark[k].life < s_spark[slot].life) slot = k;
        s_spark[slot].x = c->x; s_spark[slot].y = c->y; s_spark[slot].z = c->z;
        s_spark[slot].life = 8;
        s_spark[slot].br = c->impulse > 12000 ? 127 : 60 + c->impulse * 67 / 12000;
    }
}

/* ── drawing ────────────────────────────────────────────────────────────── */
static int64_t dist2(int id)
{
    int32_t x, y, z; vpyp_position(id, &x, &y, &z);
    const int64_t dx = x - EYE_X, dy = y - EYE_Y, dz = z - EYE_Z;
    return dx * dx + dy * dy + dz * dz;
}

/* A ring round a ball, in the ball's own frame, so it turns as the ball rolls.
 * Only the segments on the side facing the camera: the ball's own silhouette
 * hides the rest, and drawing it would show straight through the ball. */
#define RING_SEG 10
static void draw_ring(const vpy_xf *at, int br)
{
    int32_t px = 0, py = 0, pz = 0;
    for (int i = 0; i <= RING_SEG; i++) {
        const int a = i * VPY_Q14_TURN / RING_SEG;
        const int32_t lx = BALL * vpy_cos_q14(a) / VPY_Q14_ONE, lz = BALL * vpy_sin_q14(a) / VPY_Q14_ONE;
        /* the ring is the ball's equator in its own frame: x and z, y = 0 */
        const int32_t wx = (at->m[0] * lx + at->m[2] * lz) / VPY_Q14_ONE;
        const int32_t wy = (at->m[3] * lx + at->m[5] * lz) / VPY_Q14_ONE;
        const int32_t wz = (at->m[6] * lx + at->m[8] * lz) / VPY_Q14_ONE;
        const int32_t qx = at->t[0] + wx, qy = at->t[1] + wy, qz = at->t[2] + wz;
        if (i) {
            /* facing the camera: the segment's middle leans towards the eye */
            const int64_t mx = (px + qx) / 2 - at->t[0], my = (py + qy) / 2 - at->t[1], mz = (pz + qz) / 2 - at->t[2];
            const int64_t ex = EYE_X - at->t[0], ey = EYE_Y - at->t[1], ez = EYE_Z - at->t[2];
            if (mx * ex + my * ey + mz * ez > 0) vpy3d_occl_line(px, py, pz, qx, qy, qz, br);
        }
        px = qx; py = qy; pz = qz;
    }
}

static void draw_body(int id)
{
    int32_t x, y, z; vpyp_position(id, &x, &y, &z);
    vpy_xf at = vpy3d_translate(x, y, z);
    vpyp_rotation(id, at.m);                 /* the turn it really has */
    const vpy_mesh *m = s_kind[id] == K_BALL ? &s_ball : s_kind[id] == K_CRATE ? &s_crate
                      : s_kind[id] == K_WALL_X ? &s_wall_x : &s_wall_z;
    vpy3d_draw_mesh(m, &at, s_kind[id] >= K_WALL_X ? BR_FLOOR + 20 : BR_SOLID);
    if (s_kind[id] == K_BALL) draw_ring(&at, BR_SOLID - 30);
    /* AFTER drawing it. A ball hides by the octahedron inside it — a little
     * less than the ball, never more — and a box by its own eight corners. */
    if (s_kind[id] == K_BALL) {
        const int32_t r = BALL;
        const int32_t c[6][3] = { { x - r, y, z }, { x + r, y, z }, { x, y - r, z },
                                  { x, y + r, z }, { x, y, z - r }, { x, y, z + r } };
        vpy3d_occl_add(c, 6);
    } else {
        vpy3d_occl_add_mesh(m, &at);
    }
}

static void draw_floor(void)
{
    for (int32_t t = -PIT; t <= PIT; t += PIT / 4) {
        vpy3d_occl_line(t, 0, -PIT, t, 0, PIT, BR_FLOOR);
        vpy3d_occl_line(-PIT, 0, t, PIT, 0, t, BR_FLOOR);
    }
}

static void draw_effects(void)
{
    const int32_t a = 90;
    vpy3d_occl_line(s_aim_x - a, 2, s_aim_z, s_aim_x + a, 2, s_aim_z, BR_AIM);
    vpy3d_occl_line(s_aim_x, 2, s_aim_z - a, s_aim_x, 2, s_aim_z + a, BR_AIM);
    for (int i = 0; i < SPARKS; i++) {
        if (!s_spark[i].life) continue;
        const int32_t r = 25 + s_spark[i].life * 12, x = s_spark[i].x, y = s_spark[i].y, z = s_spark[i].z;
        const int br = s_spark[i].br * s_spark[i].life / 8;
        vpy3d_line_world(x - r, y, z, x + r, y, z, br);
        vpy3d_line_world(x, y - r, z, x, y + r, z, br);
        vpy3d_line_world(x - r * 2 / 3, y - r * 2 / 3, z, x + r * 2 / 3, y + r * 2 / 3, z, br);
        s_spark[i].life--;
    }
    if (s_tracer.life) {
        vpy3d_line_world(EYE_X + 300, EYE_Y - 600, EYE_Z + 900,
                         s_tracer.x, s_tracer.y, s_tracer.z, 30 + s_tracer.life * 15);
        s_tracer.life--;
    }
}

static int pressed(int n)
{
    const int b = vpy_j1_button(n);
    const int edge = b && !s_held[n];
    s_held[n] = b;
    return edge;
}

static void setup(void)
{
    build_box(&s_crate, CRATE, CRATE, CRATE);
    build_ball(&s_ball, BALL);
    build_box(&s_wall_x, PIT + WALL_T, WALL_H, WALL_T);
    build_box(&s_wall_z, WALL_T, WALL_H, PIT);
    vpy3d_set_mesh_occlusion(1);
    setup_world();
}

static void loop(void)
{
    /* input */
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    if (jx > 30 || jx < -30) s_aim_x += jx * AIM_SPEED / 127;
    if (jy > 30 || jy < -30) s_aim_z += jy * AIM_SPEED / 127;
    if (s_aim_x >  PIT - 100) s_aim_x =  PIT - 100;
    if (s_aim_x < -PIT + 100) s_aim_x = -PIT + 100;
    if (s_aim_z >  PIT - 100) s_aim_z =  PIT - 100;
    if (s_aim_z < -PIT + 100) s_aim_z = -PIT + 100;
    if (pressed(1)) drop(1);
    if (pressed(2)) shoot();
    if (pressed(3)) drop(0);
    if (pressed(4)) setup_world();

    /* physics: one step per frame */
    vpyp_step();
    sparks_from_contacts();

    /* draw, near to far */
    vpy3d_look_at(EYE_X, EYE_Y, EYE_Z, 0, 200, 0, 0, 1, 0);
    vpy3d_occl_reset();
    int order[VPYP_MAX_BODIES], n = 0;
    for (int id = 0; id < VPYP_MAX_BODIES; id++) {
        if (!vpyp_alive(id)) continue;
        int j = n++;
        while (j > 0 && dist2(order[j - 1]) > dist2(id)) { order[j] = order[j - 1]; j--; }
        order[j] = id;
    }
    draw_effects();                   /* nearest of all: nothing hides the aim or a spark */
    for (int i = 0; i < n; i++) draw_body(order[i]);
    draw_floor();

    /* readout */
    const vpyp_stats_t *ps = vpyp_stats();
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_set_text_size(6);
    vpy_print_text(-118, 108, "BOD"); vpy_print_number(-96, 108, (long)ps->bodies);
    vpy_print_text( -58, 108, "AWK"); vpy_print_number(-36, 108, (long)ps->awake);
    vpy_print_text(   2, 108, "CON"); vpy_print_number( 24, 108, (long)ps->contacts);
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
