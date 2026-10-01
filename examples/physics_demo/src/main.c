/* physics_demo — crates and balls under real gravity, a shot, and the impacts.
 *
 * vpyphys does the physics (sdk/vpy-c/include/vpyphys.h): bodies fall, bounce,
 * slide, stack, come to rest and go to sleep. vpy3d draws them, with mesh
 * occlusion on so a crate in front hides what is behind it. vpyfx does the
 * effects: every strong contact throws sparks, more the harder the hit — the
 * contact list's `impulse` is what anything reacting to a crash reads.
 *
 * CRATES DENT. A hard knock pushes a crate's face in where it landed, deeper the
 * harder it was, and the dent stays; each crate gets its own copy of the mesh
 * the first time (vpy3d_mesh_copy) and the dent is vpy3d_mesh_dent. A shot
 * dents a crate where it hits, and the third shot SHATTERS it: each of its
 * edges, dents and all, flies off spinning, and the blast scatters the rest.
 *
 * CONTROLS
 *   stick      move the aim (the cross on the floor)
 *   button 1   drop a ball over the aim
 *   button 2   shoot: a crate it hits is dented, and shatters on the third
 *              hit; a ball it hits is knocked away
 *   button 3   drop a crate over the aim
 *   button 4   start again
 *
 * THE READOUT: BOD bodies alive, AWK the ones awake (sleeping ones cost almost
 * nothing), CON contacts this step, FX effect pieces in the air, STK strokes,
 * DROP must be 0. When the body
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
#include <vpyfx.h>
#include <vpycam.h>
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
#define BLAST_R       700     /* a shattering crate pushes what is this close */
#define BLAST_KICK   3000     /* mm/s, at the centre of the blast */
#define DENT_IMPULSE 5000     /* a contact harder than this dents a crate */
#define DENT_PER     2500     /* ...by 1 mm for every this much more */
#define DENT_MAX       70     /* and never deeper than this (mm) */
#define DENT_R        170     /* how far round the point a dent reaches (mm) */
#define SHOT_DENT      55     /* a shot's dent (mm) */
#define SHOTS_TO_BREAK  3
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
/* Each body slot's own crate mesh, made the first time that crate is dented
 * and reset in place when the slot holds a new crate — so recycling slots never
 * eats the vpy3d pools (sized in the Makefile for one per slot). */
static vpy_mesh s_own[VPYP_MAX_BODIES];
static uint8_t  s_dented[VPYP_MAX_BODIES], s_hits[VPYP_MAX_BODIES];

enum { K_CRATE, K_BALL, K_WALL_X, K_WALL_Z };
static uint8_t s_kind[VPYP_MAX_BODIES];
static int s_dropped[VPYP_MAX_BODIES], s_ndropped;   /* oldest first */

static int32_t s_aim_x, s_aim_z;
static int s_held[5];

/* Effects get this many strokes a frame, at the lowest priority. Chosen with
 * DEMO_BODIES so the worst frame stays inside what one frame holds. */
#define FX_BUDGET      120

/* ── meshes ─────────────────────────────────────────────────────────────── */
/* THE CRATE, WITH A VERTEX IN THE MIDDLE OF EVERY FACE. Undented it draws
 * exactly like a plain box — the four triangles of a face are coplanar, so the
 * lines between them are hidden — but a face can only fold where it has a
 * vertex, so these are what let a dent show as a fold. Creases from 15 degrees
 * (cos = 15826), so even a shallow dent shows. */
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
    if (id >= 0) { s_kind[id] = K_CRATE; vpyp_set_material(id, 40, 150); s_dented[id] = 0; s_hits[id] = 0; }
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
    vpycam_reset(0, 200, 0);
    vpyfx_reset();
    vpyfx_seed(1);
    vpyfx_set_gravity(0, -GRAVITY, 0);
    vpyfx_set_floor(1, 0, 90);
    vpyfx_set_budget(FX_BUDGET);
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

/* THE BLAST: the pieces of a shattered crate are effects and pass through
 * everything, so what shoves the neighbours is vpyp_blast — every moving body
 * within BLAST_R thrown away from the blow, spinning — and what you see of it is
 * a ring racing out across the floor. */
static void blast(int32_t cx, int32_t cy, int32_t cz)
{
    vpyp_blast(cx, cy, cz, BLAST_R, BLAST_KICK, 0xFF);
    vpyfx_ring(cx, 2, cz, 0, 1, 0, 60, 2600, 16, 22, 110);
}

/* the mesh a crate is drawn with: its own once it has been dented */
static const vpy_mesh *crate_mesh(int id) { return s_dented[id] ? &s_own[id] : &s_crate; }

/* Dent crate `id` at world point p, pushed along world direction d. */
static void dent(int id, int32_t px, int32_t py, int32_t pz,
                 int32_t dx, int32_t dy, int32_t dz, int32_t depth)
{
    if (s_kind[id] != K_CRATE || depth <= 0) return;
    if (!s_dented[id]) {
        if (!vpy3d_mesh_copy(&s_own[id], &s_crate)) return;    /* pools full: counted by vpy3d */
        s_dented[id] = 1;
    }
    int32_t x, y, z; vpyp_position(id, &x, &y, &z);
    vpy_xf at = vpy3d_translate(x, y, z);
    vpyp_rotation(id, at.m);
    int32_t mx, my, mz, ux, uy, uz;
    vpy3d_world_to_model(&at, px, py, pz, &mx, &my, &mz);
    vpy_xf turn = at; turn.t[0] = turn.t[1] = turn.t[2] = 0;   /* a direction: no translation */
    vpy3d_world_to_model(&turn, dx, dy, dz, &ux, &uy, &uz);
    vpy3d_mesh_dent(&s_own[id], mx, my, mz, ux, uy, uz, depth, DENT_R);
}

static void shatter(int id, int32_t hx, int32_t hy, int32_t hz)
{
    int32_t x, y, z, vx, vy, vz;
    vpyp_position(id, &x, &y, &z);
    vpyp_velocity(id, &vx, &vy, &vz);
    vpy_xf at = vpy3d_translate(x, y, z);
    vpyp_rotation(id, at.m);
    vpyfx_shatter(crate_mesh(id), &at, vx, vy, vz, hx, hy, hz, 2200, 3000, 150, 120);
    vpyp_remove(id);
    blast(hx, hy, hz);
    vpycam_shake(60, 18);      /* the camera feels it */
    vpycam_hitstop(3);         /* and time holds for three frames, so the hit lands */
    for (int i = 0; i < s_ndropped; i++)
        if (s_dropped[i] == id) {
            for (int j = i + 1; j < s_ndropped; j++) s_dropped[j - 1] = s_dropped[j];
            s_ndropped--;
            break;
        }
}

static void shoot(void)
{
    const int32_t tx = s_aim_x, ty = CRATE, tz = s_aim_z;
    const int32_t dx = tx - EYE_X, dy = ty - EYE_Y, dz = tz - EYE_Z;
    vpyp_hit h;
    const int id = vpyp_raycast(EYE_X, EYE_Y, EYE_Z, dx, dy, dz, 20000, 0xFF, &h);
    if (id == VPYP_NONE) return;
    /* the shot itself: a line from the gun to the hit, fading */
    vpyfx_line(EYE_X + 300, EYE_Y - 600, EYE_Z + 900, h.x, h.y, h.z, 8, 110);
    vpyfx_burst(h.x, h.y, h.z, 0, 0, 0, 10, 2500, 16, 127);
    if (id >= 0 && s_kind[id] == K_CRATE) {
        /* A MARK where it hit, and on the third hit the crate goes: every edge
         * of it, dents and all, thrown out from where the shot landed. */
        if (++s_hits[id] >= SHOTS_TO_BREAK) { shatter(id, h.x, h.y, h.z); return; }
        dent(id, h.x, h.y, h.z, dx, dy, dz, SHOT_DENT);
    }
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

/* every hard contact throws sparks — more, and brighter, the harder the hit —
 * and a really hard one dents the crate (or crates) it hit, there */
static void sparks_from_contacts(void)
{
    for (int i = 0; i < vpyp_contact_count(); i++) {
        const vpyp_contact *c = vpyp_contact_get(i);
        if (c->impulse >= DENT_IMPULSE) {
            int32_t depth = (c->impulse - DENT_IMPULSE) / DENT_PER + 10;
            if (depth > DENT_MAX) depth = DENT_MAX;
            /* the normal points from a to b: b pushes into a along -n, a into b along +n */
            if (c->a >= 0) dent(c->a, c->x, c->y, c->z, -c->nx, -c->ny, -c->nz, depth);
            if (c->b >= 0) dent(c->b, c->x, c->y, c->z,  c->nx,  c->ny,  c->nz, depth);
        }
        if (c->impulse < 2500) continue;
        const int n = c->impulse / 1500 > 8 ? 8 : c->impulse / 1500;
        const int br = c->impulse > 12000 ? 127 : 70 + c->impulse * 57 / 12000;
        vpyfx_burst(c->x, c->y, c->z, 0, 0, 0, n, 1200, 14, br);
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
    const vpy_mesh *m = s_kind[id] == K_BALL ? &s_ball : s_kind[id] == K_CRATE ? crate_mesh(id)
                      : s_kind[id] == K_WALL_X ? &s_wall_x : &s_wall_z;
    vpy3d_draw_mesh(m, &at, s_kind[id] >= K_WALL_X ? BR_FLOOR + 20 : BR_SOLID);
    if (s_kind[id] == K_BALL) draw_ring(&at, BR_SOLID - 30);
    /* AFTER drawing it. A ball hides by the octahedron inside it — a little
     * less than the ball, never more. A crate by its eight corners, worked out
     * here because its mesh has face centres too (fourteen vertices, and the
     * occluder takes eight); a dent is not in the silhouette, which is the
     * undented box — near enough for something a few mm deep. Walls: their mesh. */
    if (s_kind[id] == K_BALL) {
        const int32_t r = BALL;
        const int32_t c[6][3] = { { x - r, y, z }, { x + r, y, z }, { x, y - r, z },
                                  { x, y + r, z }, { x, y, z - r }, { x, y, z + r } };
        vpy3d_occl_add(c, 6);
    } else if (s_kind[id] == K_CRATE) {
        int32_t c[8][3];
        for (int k = 0; k < 8; k++) {
            const int32_t l[3] = { (k & 1) ? CRATE : -CRATE, (k & 2) ? CRATE : -CRATE, (k & 4) ? CRATE : -CRATE };
            for (int r = 0; r < 3; r++)
                c[k][r] = at.t[r] + (int32_t)(((int64_t)at.m[r*3] * l[0] + (int64_t)at.m[r*3+1] * l[1] +
                                               (int64_t)at.m[r*3+2] * l[2]) >> 14);
        }
        vpy3d_occl_add((const int32_t (*)[3])c, 8);
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
    build_crate(&s_crate, CRATE);
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

    /* physics: one step per frame, except while a hit-stop holds time */
    vpycam_step();
    if (!vpycam_stopped()) {
        vpyp_step();
        sparks_from_contacts();
        vpyfx_step();
    }

    /* draw, near to far */
    vpycam_look_at(EYE_X, EYE_Y - 200, EYE_Z);       /* from the eye to (0, 200, 0), shaken */
    vpy3d_occl_reset();
    int order[VPYP_MAX_BODIES], n = 0;
    for (int id = 0; id < VPYP_MAX_BODIES; id++) {
        if (!vpyp_alive(id)) continue;
        int j = n++;
        while (j > 0 && dist2(order[j - 1]) > dist2(id)) { order[j] = order[j - 1]; j--; }
        order[j] = id;
    }
    draw_effects();                   /* the aim and the tracer: nearest of all */
    for (int i = 0; i < n; i++) draw_body(order[i]);
    vpyfx_draw(1);                    /* debris and sparks, cut by the solids in front */
    draw_floor();

    /* readout */
    const vpyp_stats_t *ps = vpyp_stats();
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_set_text_size(6);
    vpy_print_text(-118, 108, "BOD"); vpy_print_number(-96, 108, (long)ps->bodies);
    vpy_print_text( -58, 108, "AWK"); vpy_print_number(-36, 108, (long)ps->awake);
    vpy_print_text(   2, 108, "CON"); vpy_print_number( 24, 108, (long)ps->contacts);
    vpy_print_text(  62, 108, "FX");  vpy_print_number( 78, 108, (long)vpyfx_stats()->alive);
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
