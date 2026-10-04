/* hub.c — the plaza: a figure that walks, four doors, four pedestals, the steps.
 *
 * THE FIGURE is bone_demo's: thirteen rigid bones, each its own box, posed by a
 * walk and a run clip blended by the stick (vpyb_apply_blend), its feet put on
 * the ground by the two-bone IK (vpyb_ik) — which is why the steps in the middle
 * are there. Here it also STANDS: the walk's angles are scaled by how much it is
 * moving, rebuilt every frame (13 bones × 4 keys, nothing for the CPU), so with
 * the stick centred it eases to a stand instead of freezing mid-stride.
 *
 * THE CAMERA follows it with vpycam (a dead zone, a lead, smoothing) from behind,
 * turning after it a little late, so a turn reads as a turn.
 *
 * THE PEDESTALS hold the pieces earned. A piece just earned ASSEMBLES on its
 * pedestal when the figure comes back (vpyfx_assemble), then turns there.
 */
#include "playroom.h"
#include <vpybone.h>
#include <vpycam.h>
#include <vpyfx.h>

/* ── the figure, in world units (mm): bone_demo's, unchanged ─────────────── */
#define THIGH        420
#define SHIN         420
#define FOOT_LEN     170
#define FOOT_H        50
#define HIP_W        110
#define TORSO_H      560
#define SHOULDER_W   210
#define UPPER_ARM    310
#define FOREARM      290
#define NECK         600
#define HIP_HEIGHT   (THIGH + SHIN + FOOT_H - 40)

#define DEG(d) ((d) * VPY_Q14_TURN / 360)

#define WALK_SPEED    44      /* mm a frame */
#define RUN_SPEED    105
#define TURN_SPEED    DEG(3)  /* a frame at full stick */
#define PELVIS_EASE    4
#define GAIT_EASE      6      /* the walk/run/stand amounts close 1/this of the gap a frame */

/* ── the plaza. Composition, not measurement. ─────────────────────────────── */
#define DOOR_R      5200      /* the doors stand on this circle, facing the middle */
#define DOOR_HALF    450      /* half the opening */
#define DOOR_H      1100
#define PED_R       3700      /* the pedestals, on the same bearings */
#define PED_H        500
#define BOUND_R     5000      /* how far out the figure may walk, except into a door */
#define PED_KEEP     330      /* and how close to a pedestal */
#define FLOOR_HALF  6000
#define FLOOR_STEP  1000
/* the steps in the middle: two levels, square */
#define STEP_RISE    150
#define STEP1        850      /* half size of the lower level */
#define STEP2        450      /* and of the top */
#define FINALE_R     330      /* stand this close to the middle, on top, to start the finale */
#define START_Z    (-3000)
#define BACK_FROM   1100      /* back from a room, the figure stands this far short of its pedestal */
#define BACK_SIDE    700      /* and this far to its right */

#define CAM_BACK    3800
#define CAM_UP      2300
#define CAM_TURN_EASE 12

#define NEAR_SIGN   3000      /* the count over the steps hides when the camera is closer */

#define BR_BODY      110
#define BR_FLOOR      40
#define BR_STEP       80
#define BR_DOOR      100
#define BR_SIGN      110
#define BR_PED        80
#define BR_PIECE     120

/* the doors' bearings, left to right as seen from the start: degrees from +z */
static const int DOOR_DEG[N_ROOMS] = { -54, -18, 18, 54 };
static const char *const DOOR_NAME[N_ROOMS] = { "CRATES", "JELLY", "SHAPES", "FLYER" };

enum { B_PELVIS, B_TORSO, B_HEAD, B_LUARM, B_LFARM, B_RUARM, B_RFARM,
       B_LTHIGH, B_LSHIN, B_LFOOT, B_RTHIGH, B_RSHIN, B_RFOOT, N_BONES };

static vpyb_skeleton s_sk;            /* static: ~2 KB, and core 0 has 4 KB of stack */
static vpy_mesh s_pelvis, s_torso, s_head, s_uarm, s_farm, s_thigh, s_shin, s_foot;
static vpy_mesh s_post, s_lintel, s_ped;
static vpy_mesh s_piece[N_ROOMS];

/* ── the clips: bone_demo's angle tables, scaled by how much it moves ────── */
#define WALK_LEN 32
#define RUN_LEN  20
#define KEYS      4
typedef struct { int thigh[KEYS], shin[KEYS], arm[KEYS], farm, lean; uint16_t len; } gait_t;
static const gait_t WALK = {
    { DEG(-25), DEG(0), DEG(22), DEG(0) },
    { DEG(5), DEG(8), DEG(15), DEG(45) },
    { DEG(18), DEG(0), DEG(-18), DEG(0) },
    DEG(-15), DEG(3), WALK_LEN
};
static const gait_t RUN = {
    { DEG(-42), DEG(-5), DEG(35), DEG(0) },
    { DEG(12), DEG(20), DEG(40), DEG(95) },
    { DEG(35), DEG(0), DEG(-35), DEG(0) },
    DEG(-85), DEG(12), RUN_LEN
};
static vpyb_key s_keys[2][N_BONES][KEYS];
static vpyb_track s_tracks[2][N_BONES];
static vpyb_clip s_clip[2];

#define WAVE_LEN 24
static vpyb_key s_wave_keys[2][KEYS];
static vpyb_track s_wave_tracks[N_BONES];
static vpyb_clip s_wave;

static vpyb_quat rx(int a) { return vpyb_quat_axis(1, 0, 0, a); }
static vpyb_quat rz(int a) { return vpyb_quat_axis(0, 0, 1, a); }

static void track(vpyb_key *k, vpyb_track *t, const int *ang, uint16_t len, int shift, int amp_q8)
{
    for (int i = 0; i < KEYS; i++) {
        const int j = (i + shift) % KEYS;
        k[i].frame = (uint16_t)(i * len / KEYS);
        k[i].q = rx(ang[j] * amp_q8 / 256);
    }
    t->keys = k; t->n = KEYS;
}
static void one_key(vpyb_key *k, vpyb_track *t, vpyb_quat q)
{
    k[0].frame = 0; k[0].q = q;
    t->keys = k; t->n = 1;
}

/* clip c from gait g at amp_q8 of its swing (0: standing straight) */
static void build_clip(int c, const gait_t *g, int amp_q8)
{
    vpyb_key (*k)[KEYS] = s_keys[c];
    vpyb_track *t = s_tracks[c];
    track(k[B_LTHIGH], &t[B_LTHIGH], g->thigh, g->len, 0, amp_q8);
    track(k[B_LSHIN],  &t[B_LSHIN],  g->shin,  g->len, 0, amp_q8);
    track(k[B_RTHIGH], &t[B_RTHIGH], g->thigh, g->len, 2, amp_q8);
    track(k[B_RSHIN],  &t[B_RSHIN],  g->shin,  g->len, 2, amp_q8);
    track(k[B_LUARM],  &t[B_LUARM],  g->arm,   g->len, 2, amp_q8);
    track(k[B_RUARM],  &t[B_RUARM],  g->arm,   g->len, 0, amp_q8);
    one_key(k[B_LFARM], &t[B_LFARM], rx(g->farm * amp_q8 / 256));
    one_key(k[B_RFARM], &t[B_RFARM], rx(g->farm * amp_q8 / 256));
    one_key(k[B_TORSO], &t[B_TORSO], rx(g->lean * amp_q8 / 256));
    s_clip[c].tracks = t; s_clip[c].bones = N_BONES; s_clip[c].length = g->len; s_clip[c].loop = 1;
}

static void build_wave(void)
{
    for (int i = 0; i < KEYS; i++) {
        s_wave_keys[0][i].frame = (uint16_t)(i * WAVE_LEN / KEYS);
        s_wave_keys[0][i].q = rz(DEG(-150));
        s_wave_keys[1][i].frame = (uint16_t)(i * WAVE_LEN / KEYS);
        s_wave_keys[1][i].q = rz((i & 1) ? DEG(-35) : DEG(35));
    }
    s_wave_tracks[B_RUARM].keys = s_wave_keys[0]; s_wave_tracks[B_RUARM].n = KEYS;
    s_wave_tracks[B_RFARM].keys = s_wave_keys[1]; s_wave_tracks[B_RFARM].n = KEYS;
    s_wave.tracks = s_wave_tracks; s_wave.bones = N_BONES; s_wave.length = WAVE_LEN; s_wave.loop = 1;
}

/* ── the ground: the two steps in the middle, flat everywhere else ───────── */
static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
static int32_t ground(int32_t x, int32_t z)
{
    const int32_t m = iabs(x) > iabs(z) ? iabs(x) : iabs(z);
    return m < STEP2 ? 2 * STEP_RISE : m < STEP1 ? STEP_RISE : 0;
}

/* ── state ───────────────────────────────────────────────────────────────── */
static int32_t s_x, s_z, s_pelvis_y, s_vx, s_vz;
static int     s_heading, s_cam_heading;
static int32_t s_phase_q16;
static int     s_amp_q8, s_run_q8, s_wave_t, s_time;
static int     s_spin[N_ROOMS];           /* each piece's own turn: still while it assembles */
static uint8_t s_shown[N_ROOMS];
static int     s_gathering = -1, s_group;

static int32_t door_x(int i, int32_t r) { return (int32_t)((int64_t)r * vpy_sin_q14(DEG(DOOR_DEG[i])) / VPY_Q14_ONE); }
static int32_t door_z(int i, int32_t r) { return (int32_t)((int64_t)r * vpy_cos_q14(DEG(DOOR_DEG[i])) / VPY_Q14_ONE); }

static vpy_xf piece_place(int i)
{
    const vpy_xf turn = vpy3d_rot_y(s_spin[i]);
    const vpy_xf at = vpy3d_translate(door_x(i, PED_R), PED_H + 260 + (i == R_FLYER ? 0 : 0), door_z(i, PED_R));
    return vpy3d_mul(&at, &turn);
}

void hub_build(void)
{
    pr_box_y(&s_pelvis, HIP_W + 40, -60, 60, 80, 0);
    pr_box_y(&s_torso, 170, 40, TORSO_H, 90, 0);
    pr_box_y(&s_head, 100, 0, 220, 110, 10);
    pr_box_y(&s_uarm, 50, -UPPER_ARM, 30, 50, 0);
    pr_box_y(&s_farm, 45, -FOREARM, 0, 45, 0);
    pr_box_y(&s_thigh, 70, -THIGH, 30, 70, 0);
    pr_box_y(&s_shin, 55, -SHIN, 0, 55, 0);
    pr_box_y(&s_foot, 55, -FOOT_H, 10, FOOT_LEN / 2, FOOT_LEN / 2 - 30);

    vpyb_init(&s_sk);
    vpyb_add(&s_sk, -1, 0, 0, 0, &s_pelvis);
    vpyb_add(&s_sk, B_PELVIS, 0, 60, 0, &s_torso);
    vpyb_add(&s_sk, B_TORSO, 0, NECK, 0, &s_head);
    vpyb_add(&s_sk, B_TORSO,  SHOULDER_W, TORSO_H - 50, 0, &s_uarm);
    vpyb_add(&s_sk, B_LUARM, 0, -UPPER_ARM, 0, &s_farm);
    vpyb_add(&s_sk, B_TORSO, -SHOULDER_W, TORSO_H - 50, 0, &s_uarm);
    vpyb_add(&s_sk, B_RUARM, 0, -UPPER_ARM, 0, &s_farm);
    vpyb_add(&s_sk, B_PELVIS,  HIP_W, -40, 0, &s_thigh);
    vpyb_add(&s_sk, B_LTHIGH, 0, -THIGH, 0, &s_shin);
    vpyb_add(&s_sk, B_LSHIN, 0, -SHIN, 0, &s_foot);
    vpyb_add(&s_sk, B_PELVIS, -HIP_W, -40, 0, &s_thigh);
    vpyb_add(&s_sk, B_RTHIGH, 0, -THIGH, 0, &s_shin);
    vpyb_add(&s_sk, B_RSHIN, 0, -SHIN, 0, &s_foot);
    build_wave();

    pr_box_y(&s_post, 70, 0, DOOR_H, 70, 0);
    pr_box_y(&s_lintel, DOOR_HALF + 70, DOOR_H, DOOR_H + 140, 70, 0);
    pr_box_y(&s_ped, 160, 0, PED_H, 160, 0);
    pr_crate(&s_piece[R_CRATES], 150);
    pr_ball(&s_piece[R_JELLY], 160);
    pr_octahedron(&s_piece[R_SHAPES], 190);
    pr_dart(&s_piece[R_FLYER], 420, 0);
}

void hub_enter(int from)
{
    if (from >= 0 && from < N_ROOMS) {
        /* on the plaza side of the room's pedestal, LOOKING AT IT: the camera
         * behind the figure then has the pedestal and the door in view, which is
         * where a piece just earned assembles. Facing the middle instead put the
         * camera outside the door, and the door hid it all (host run, 2026-10-04). */
        /* and a step to the side, so the figure does not stand in front of it */
        const int a = DEG(DOOR_DEG[from]);
        s_x = door_x(from, PED_R - BACK_FROM) + BACK_SIDE * vpy_cos_q14(a) / VPY_Q14_ONE;
        s_z = door_z(from, PED_R - BACK_FROM) - BACK_SIDE * vpy_sin_q14(a) / VPY_Q14_ONE;
        s_heading = DEG(DOOR_DEG[from]) & (VPY_Q14_TURN - 1);
    } else {
        s_x = 0; s_z = START_Z; s_heading = 0;
    }
    s_cam_heading = s_heading;
    s_phase_q16 = 0; s_amp_q8 = 0; s_run_q8 = 0; s_wave_t = 0; s_vx = s_vz = 0;
    s_pelvis_y = HIP_HEIGHT + ground(s_x, s_z);
    vpycam_reset(s_x, s_pelvis_y, s_z);
    vpycam_follow_config(150, 150, 150, 6, 6);

    vpyfx_reset();
    vpyfx_seed(7);
    vpyfx_set_budget(VPYFX_MAX);
    s_gathering = -1;
    /* A PIECE JUST EARNED assembles on its pedestal, in front of the figure */
    if (pr_done(from) && !s_shown[from]) {
        const vpy_xf at = piece_place(from);
        s_group = vpyfx_assemble(&s_piece[from], &at, 6, 1600, 45, 30, BR_PIECE);
        if (s_group) s_gathering = from; else s_shown[from] = 1;
    }
}

/* A leg onto the ground (bone_demo's plant) */
static void plant(int thigh, int foot, const int32_t fwd[3])
{
    const int32_t *a = s_sk.pos[foot];
    const int32_t g = ground(a[0], a[2]);
    int32_t target[3] = { a[0], a[1] + g, a[2] };
    if (target[1] < g + FOOT_H) target[1] = g + FOOT_H;
    const int32_t *h = s_sk.pos[thigh];
    const int32_t pole[3] = { h[0] + fwd[0], h[1] - THIGH, h[2] + fwd[2] };
    vpyb_ik(&s_sk, thigh, target, pole);
}

/* ── where the figure may go ──────────────────────────────────────────────── */
/* Returns the door it walked into, or -1; otherwise keeps it inside the plaza and
 * off the pedestals. */
static int walls(void)
{
    for (int i = 0; i < N_ROOMS; i++) {
        /* in the door's own frame: along its opening, and out through it */
        const int32_t sx = vpy_sin_q14(DEG(DOOR_DEG[i])), cz = vpy_cos_q14(DEG(DOOR_DEG[i]));
        const int32_t along = (int32_t)(((int64_t)s_x * cz - (int64_t)s_z * sx) / VPY_Q14_ONE);
        const int32_t out = (int32_t)(((int64_t)s_x * sx + (int64_t)s_z * cz) / VPY_Q14_ONE);
        if (iabs(along) < DOOR_HALF - 80 && out > DOOR_R - 100) return i;
        /* the pedestal: pushed out of its circle */
        const int32_t dx = s_x - door_x(i, PED_R), dz = s_z - door_z(i, PED_R);
        const int64_t d2 = (int64_t)dx * dx + (int64_t)dz * dz;
        if (d2 < (int64_t)PED_KEEP * PED_KEEP) {
            const int32_t d = (int32_t)pr_isqrt64(d2);
            if (d > 0) { s_x = door_x(i, PED_R) + dx * PED_KEEP / d; s_z = door_z(i, PED_R) + dz * PED_KEEP / d; }
        }
    }
    /* the edge of the plaza, except within a door's opening (handled above) */
    const int64_t r2 = (int64_t)s_x * s_x + (int64_t)s_z * s_z;
    if (r2 > (int64_t)BOUND_R * BOUND_R) {
        int in_door = 0;
        for (int i = 0; i < N_ROOMS; i++) {
            const int32_t sx = vpy_sin_q14(DEG(DOOR_DEG[i])), cz = vpy_cos_q14(DEG(DOOR_DEG[i]));
            const int32_t along = (int32_t)(((int64_t)s_x * cz - (int64_t)s_z * sx) / VPY_Q14_ONE);
            if (iabs(along) < DOOR_HALF - 80) in_door = 1;
        }
        if (!in_door) {
            const int32_t r = (int32_t)pr_isqrt64(r2);
            s_x = (int32_t)((int64_t)s_x * BOUND_R / r);
            s_z = (int32_t)((int64_t)s_z * BOUND_R / r);
        }
    }
    return -1;
}

/* ── drawing ──────────────────────────────────────────────────────────────── */
static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_STEP) {
        vpy3d_line_world(t, 0, -FLOOR_HALF, t, 0, FLOOR_HALF, BR_FLOOR);
        vpy3d_line_world(-FLOOR_HALF, 0, t, FLOOR_HALF, 0, t, BR_FLOOR);
    }
}

/* a square outline at height y, half size h, and its corners down to y0 */
static void draw_level(int32_t h, int32_t y, int32_t y0)
{
    static const int8_t C[5][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1}, {-1,-1} };
    for (int i = 0; i < 4; i++) {
        vpy3d_line_world(C[i][0] * h, y, C[i][1] * h, C[i + 1][0] * h, y, C[i + 1][1] * h, BR_STEP);
        vpy3d_line_world(C[i][0] * h, y0, C[i][1] * h, C[i][0] * h, y, C[i][1] * h, BR_STEP);
    }
}

static void draw_middle(void)
{
    draw_level(STEP1, STEP_RISE, 0);
    draw_level(STEP2, 2 * STEP_RISE, STEP_RISE);
    char t[16] = "PIECES 0 OF 4";
    t[7] = (char)('0' + pr_done_count());
    if (pr_done_count() == N_ROOMS) {
        /* open: a ring on the top that pulses, and the word over it */
        const int br = 70 + (vpy_sin_q14(s_time * 120) + VPY_Q14_ONE) * 57 / (2 * VPY_Q14_ONE);
        int32_t px = FINALE_R, pz = 0;
        for (int i = 1; i <= 16; i++) {
            const int a = i * VPY_Q14_TURN / 16;
            const int32_t x = FINALE_R * vpy_cos_q14(a) / VPY_Q14_ONE, z = FINALE_R * vpy_sin_q14(a) / VPY_Q14_ONE;
            vpy3d_line_world(px, 2 * STEP_RISE + 2, pz, x, 2 * STEP_RISE + 2, z, br);
            px = x; pz = z;
        }
        vpy3d_text_billboard("FINALE", 0, 1300, 0, 260, BR_SIGN, VPY3D_TEXT_CENTRE);
    } else {
        /* the count over the steps — only from a distance: the camera passes right
         * through it on the way across, and a word that size up close is a wall of
         * strokes */
        int32_t eye[3]; vpy3d_eye(eye);
        if ((int64_t)eye[0] * eye[0] + (int64_t)eye[2] * eye[2] > (int64_t)NEAR_SIGN * NEAR_SIGN)
            vpy3d_text_billboard(t, 0, 1500, 0, 180, BR_HINT, VPY3D_TEXT_CENTRE);
    }
}

static void draw_door(int i)
{
    const vpy_xf turn = vpy3d_rot_y(DEG(DOOR_DEG[i]));
    vpy_xf at = turn;
    at.t[0] = door_x(i, DOOR_R); at.t[1] = 0; at.t[2] = door_z(i, DOOR_R);
    /* the posts sit at the opening's two ends: along the door's own x */
    for (int side = -1; side <= 1; side += 2) {
        vpy_xf p = at;
        p.t[0] += side * (DOOR_HALF + 70) * turn.m[0] / VPY_Q14_ONE;
        p.t[2] += side * (DOOR_HALF + 70) * turn.m[6] / VPY_Q14_ONE;
        vpy3d_draw_mesh(&s_post, &p, BR_DOOR);
    }
    vpy3d_draw_mesh(&s_lintel, &at, BR_DOOR);
    /* the name over it, read from the middle */
    vpy_xf sign = at; sign.t[1] = DOOR_H + 360;
    vpy3d_text(DOOR_NAME[i], &sign, 230, pr_done(i) ? BR_HINT : BR_SIGN, VPY3D_TEXT_CENTRE | VPY3D_TEXT_FRONT);
}

static void draw_pedestal(int i)
{
    const vpy_xf at = vpy3d_translate(door_x(i, PED_R), 0, door_z(i, PED_R));
    vpy3d_draw_mesh(&s_ped, &at, BR_PED);
    if (pr_done(i) && s_shown[i]) {
        vpy_xf p = piece_place(i);
        p.t[1] += 50 * vpy_sin_q14(s_time * 40 + i * 1000) / VPY_Q14_ONE;   /* a slow bob */
        vpy3d_draw_mesh(&s_piece[i], &p, BR_PIECE);
    }
}

int hub_frame(int control)
{
    s_time++;
    const int jx = control ? vpy_j1_x() : 0, jy = control ? vpy_j1_y() : 0;

    /* how much it moves: nothing with the stick centred, a walk pushed a little,
     * a run pushed all the way */
    const int f = jy > 20 ? (jy - 20) * 256 / 107 : 0;
    const int want_amp = f > 0 ? 256 : 0;
    const int want_run = f > 128 ? (f - 128) * 2 : 0;
    s_amp_q8 += (want_amp - s_amp_q8) / GAIT_EASE;
    s_run_q8 += (want_run - s_run_q8) / GAIT_EASE;
    if (s_amp_q8 < 3 && !want_amp) s_amp_q8 = 0;
    if (jx > 30 || jx < -30) s_heading = (s_heading - jx * TURN_SPEED / 127) & (VPY_Q14_TURN - 1);

    const int32_t len = WALK_LEN + (RUN_LEN - WALK_LEN) * s_run_q8 / 256;
    s_phase_q16 = (s_phase_q16 + 65536 / len * s_amp_q8 / 256) & 0xFFFF;
    const int32_t speed = (WALK_SPEED + (RUN_SPEED - WALK_SPEED) * s_run_q8 / 256) * s_amp_q8 / 256;
    const int32_t fwd[3] = { vpy_sin_q14(s_heading), 0, vpy_cos_q14(s_heading) };
    s_vx = speed * fwd[0] / VPY_Q14_ONE;
    s_vz = speed * fwd[2] / VPY_Q14_ONE;
    s_x += s_vx; s_z += s_vz;
    const int door = walls();
    const int on_top = pr_done_count() == N_ROOMS && iabs(s_x) < FINALE_R && iabs(s_z) < FINALE_R;

    /* the pose */
    build_clip(0, &WALK, s_amp_q8);
    build_clip(1, &RUN, s_amp_q8);
    vpyb_apply_blend(&s_sk, &s_clip[0], s_phase_q16 * WALK_LEN >> 8, &s_clip[1], s_phase_q16 * RUN_LEN >> 8, s_run_q8);
    if (vpy_j1_button(2) && control) { vpyb_apply(&s_sk, &s_wave, s_wave_t << 8); s_wave_t++; }
    const int bob = ((vpy_cos_q14((int)(s_phase_q16 * 2 * VPY_Q14_TURN >> 16)) * (12 + s_run_q8 / 8)) >> 14) * s_amp_q8 / 256;
    const int32_t dl = HIP_W * fwd[2] / VPY_Q14_ONE, dz = -HIP_W * fwd[0] / VPY_Q14_ONE;
    const int32_t gl = ground(s_x + dl, s_z + dz), gr = ground(s_x - dl, s_z - dz);
    const int32_t want_y = HIP_HEIGHT - s_run_q8 / 3 + bob + (gl < gr ? gl : gr);
    s_pelvis_y += (want_y - s_pelvis_y) / PELVIS_EASE;
    const int32_t pos[3] = { s_x, s_pelvis_y, s_z };
    vpyb_pose(&s_sk, pos, vpyb_quat_axis(0, 1, 0, s_heading));
    const int32_t ahead[3] = { fwd[0] * 1000 / VPY_Q14_ONE, 0, fwd[2] * 1000 / VPY_Q14_ONE };
    plant(B_LTHIGH, B_LFOOT, ahead);
    plant(B_RTHIGH, B_RFOOT, ahead);

    /* the camera: behind it, turning after it */
    int dh = (s_heading - s_cam_heading) & (VPY_Q14_TURN - 1);
    if (dh >= VPY_Q14_TURN / 2) dh -= VPY_Q14_TURN;
    s_cam_heading = (s_cam_heading + dh / CAM_TURN_EASE) & (VPY_Q14_TURN - 1);
    vpycam_follow(s_x, s_pelvis_y, s_z, s_vx, 0, s_vz);
    vpycam_step();
    vpycam_look_at(-CAM_BACK * vpy_sin_q14(s_cam_heading) / VPY_Q14_ONE, CAM_UP,
                   -CAM_BACK * vpy_cos_q14(s_cam_heading) / VPY_Q14_ONE);

    /* a piece assembling: when it is all there, the mesh takes over */
    vpyfx_step();
    if (s_gathering >= 0 && vpyfx_assembled(s_group)) {
        vpyfx_release(s_group);
        s_shown[s_gathering] = 1;
        s_gathering = -1;
    }
    for (int i = 0; i < N_ROOMS; i++) if (s_shown[i]) s_spin[i] = (s_spin[i] + 14) & (VPY_Q14_TURN - 1);

    /* draw */
    vpyb_draw(&s_sk, BR_BODY, VPYB_DRAW_MESH);
    for (int i = 0; i < N_ROOMS; i++) { draw_pedestal(i); draw_door(i); }
    vpyfx_draw(0);
    draw_middle();
    draw_floor();
    if (control) pr_objective(pr_done_count() == N_ROOMS ? "GO TO THE MIDDLE" : "WALK INTO A DOOR", 0, 0);

    if (!control) return -1;
    if (door >= 0) return door;
    return on_top ? R_FINALE : -1;
}
