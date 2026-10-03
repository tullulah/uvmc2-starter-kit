/* bone_demo — vpybone: an articulated figure walking and running up and down steps.
 *
 * THE FIGURE is thirteen rigid bones (sdk/vpy-c/include/vpybone.h): a pelvis, a
 * torso, a head, two arms of two bones and two legs of three (thigh, shin, foot).
 * Each bone is its own small box mesh, hung from its joint, so a limb turns as a
 * piece and nothing stretches — the cheap and exact kind of skinning, and on a
 * vector screen a limb drawn as a box reads perfectly well.
 *
 * TWO CLIPS, WALK AND RUN, keyframed per bone and BLENDED by the stick: centred it
 * walks, pushed up it runs, and anything between is a mix (vpyb_apply_blend). The
 * two clips have different lengths, so they share a PHASE rather than a time:
 * each is sampled at the same fraction of its own cycle, which keeps the feet in
 * step while the blend moves — two clips at their own clocks would cross their
 * legs halfway through.
 *
 * THE FEET STAND ON THE STEPS. The clips are made for flat ground; after posing,
 * each leg is handed to the two-bone IK (vpyb_ik) with the ankle's target lifted
 * by the height of the ground under it, so the figure climbs three steps and
 * comes down three without a foot sinking into a riser. The pelvis rides on the
 * lower of the two feet, eased so a step does not jolt it. IKS on the readout
 * counts targets the leg could not reach: it should stay 0 on these steps.
 *
 * CONTROLS
 *   stick up   from walking (centre) to running (pushed all the way)
 *   stick l/r  turn
 *   button 1   draw the bones as lines instead of boxes (and back)
 *   button 2   held: wave (a third clip, on the right arm only, over the walk)
 *   button 4   start again
 *
 * THE READOUT: STK strokes this frame, DROP must be 0, BON bones posed, IKS feet
 * the IK could not reach, RUN the run blend (0 walk .. 256 run).
 */
#include <vpy.h>
#include <vpy3d.h>
#include <vpybone.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#endif

/* ── the figure, in world units (mm). Composition, not measurement. ───────── */
#define THIGH        420
#define SHIN         420
#define FOOT_LEN     170      /* the foot box, forward from the ankle */
#define FOOT_H        50      /* how far the ankle sits above the sole */
#define HIP_W        110      /* half the hip width */
#define TORSO_H      560
#define SHOULDER_W   210
#define UPPER_ARM    310
#define FOREARM      290
#define NECK         600      /* torso joint to head joint */
#define HIP_HEIGHT   (THIGH + SHIN + FOOT_H - 40)   /* the knees never lock straight */

/* degrees into the 4096-per-turn angles vpy_sin_q14 and vpyb_quat_axis use */
#define DEG(d) ((d) * VPY_Q14_TURN / 360)

/* ── the steps: three up and three down, repeating along z, in a band of x ── */
#define STEP_RISE    150
#define STEP_RUN     600
#define STAIR_HALF_W 1200
#define STAIR_START  1500     /* flat run-up before the first step */
#define STAIR_PERIOD 6000     /* the pattern repeats this often along z */

#define WALK_SPEED    44      /* mm a frame: one walk cycle covers ~two strides */
#define RUN_SPEED    105
#define TURN_SPEED    DEG(3)  /* a frame at full stick */
#define PELVIS_EASE    4      /* the pelvis moves 1/this of the way to its height a frame */

#define CAM_SIDE    3000      /* the camera: this far to the figure's left */
#define CAM_BACK    1200      /* this far behind it */
#define CAM_UP       700      /* and this far above the pelvis */

#define BR_BODY      110
#define BR_GROUND     45
#define BR_STEP       80

/* ── bones, in table order (a parent before its children) ─────────────────── */
enum { B_PELVIS, B_TORSO, B_HEAD, B_LUARM, B_LFARM, B_RUARM, B_RFARM,
       B_LTHIGH, B_LSHIN, B_LFOOT, B_RTHIGH, B_RSHIN, B_RFOOT, N_BONES };

static vpyb_skeleton s_sk;            /* static: ~2 KB, and core 0 has 4 KB of stack */
static vpy_mesh s_pelvis, s_torso, s_head, s_uarm, s_farm, s_thigh, s_shin, s_foot;

/* ── meshes: a box hanging from its joint ─────────────────────────────────── */
/* A box from y0 to y1 (the joint is y = 0), hx and hz half width and depth, its
 * centre z at cz. Hidden lines removed within it, like any vpy3d mesh. */
static void build_box(vpy_mesh *m, int hx, int y0, int y1, int hz, int cz)
{
    int v[8];
    vpy3d_mesh_begin(m);
    for (int k = 0; k < 8; k++)
        v[k] = vpy3d_vertex((k & 1) ? hx : -hx, (k & 2) ? y1 : y0, cz + ((k & 4) ? hz : -hz));
    vpy3d_quad(v[0], v[4], v[6], v[2]);  vpy3d_quad(v[1], v[3], v[7], v[5]);
    vpy3d_quad(v[0], v[1], v[5], v[4]);  vpy3d_quad(v[2], v[6], v[7], v[3]);
    vpy3d_quad(v[0], v[2], v[3], v[1]);  vpy3d_quad(v[4], v[5], v[7], v[6]);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

/* ── the clips ────────────────────────────────────────────────────────────────
 * Keys are quaternions, which vpyb_quat_axis computes at run time, so the key
 * tables are filled once in setup() from these angle tables. A leg swings about
 * x: a NEGATIVE angle carries the foot forward (+z), a POSITIVE one bends the
 * knee back. The right leg is the left one half a cycle later; each arm swings
 * against the leg on its own side. */
#define WALK_LEN 32
#define RUN_LEN  20
#define KEYS      4                    /* every track: four keys, a quarter cycle apart */

typedef struct { int thigh[KEYS], shin[KEYS], arm[KEYS], farm, lean; uint16_t len; } gait_t;
static const gait_t WALK = {
    { DEG(-25), DEG(0), DEG(22), DEG(0) },    /* thigh: forward, under, back, under */
    { DEG(5), DEG(8), DEG(15), DEG(45) },     /* shin: the knee folds as the leg swings through */
    { DEG(18), DEG(0), DEG(-18), DEG(0) },    /* upper arm: against the thigh */
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

/* THE WAVE: the right arm only, up and waving, over whatever the legs do. Its
 * other tracks are empty (n = 0), so vpyb_apply leaves those bones alone. */
#define WAVE_LEN 24
static vpyb_key s_wave_keys[2][KEYS];
static vpyb_track s_wave_tracks[N_BONES];
static vpyb_clip s_wave;

static vpyb_quat rx(int a) { return vpyb_quat_axis(1, 0, 0, a); }
static vpyb_quat rz(int a) { return vpyb_quat_axis(0, 0, 1, a); }

/* one bone's four keys from four angles about x, the first at frame `shift` */
static void track(vpyb_key *k, vpyb_track *t, const int *ang, uint16_t len, int shift)
{
    for (int i = 0; i < KEYS; i++) {
        const int j = (i + shift) % KEYS;            /* key i holds pose j: a phase shift */
        k[i].frame = (uint16_t)(i * len / KEYS);
        k[i].q = rx(ang[j]);
    }
    t->keys = k; t->n = KEYS;
}
static void one_key(vpyb_key *k, vpyb_track *t, vpyb_quat q)
{
    k[0].frame = 0; k[0].q = q;
    t->keys = k; t->n = 1;
}

static void build_clip(int c, const gait_t *g)
{
    vpyb_key (*k)[KEYS] = s_keys[c];
    vpyb_track *t = s_tracks[c];
    /* the opposite side is the same motion half a cycle (two keys) later */
    track(k[B_LTHIGH], &t[B_LTHIGH], g->thigh, g->len, 0);
    track(k[B_LSHIN],  &t[B_LSHIN],  g->shin,  g->len, 0);
    track(k[B_RTHIGH], &t[B_RTHIGH], g->thigh, g->len, 2);
    track(k[B_RSHIN],  &t[B_RSHIN],  g->shin,  g->len, 2);
    track(k[B_LUARM],  &t[B_LUARM],  g->arm,   g->len, 2);     /* left arm with the right leg */
    track(k[B_RUARM],  &t[B_RUARM],  g->arm,   g->len, 0);
    one_key(k[B_LFARM], &t[B_LFARM], rx(g->farm));
    one_key(k[B_RFARM], &t[B_RFARM], rx(g->farm));
    one_key(k[B_TORSO], &t[B_TORSO], rx(g->lean));
    s_clip[c].tracks = t; s_clip[c].bones = N_BONES; s_clip[c].length = g->len; s_clip[c].loop = 1;
}

static void build_wave(void)
{
    /* the right arm is on -x when the figure faces +z: turning its hanging arm
     * about z by -150 degrees lifts it up and out to that side */
    for (int i = 0; i < KEYS; i++) {
        s_wave_keys[0][i].frame = (uint16_t)(i * WAVE_LEN / KEYS);
        s_wave_keys[0][i].q = rz(DEG(-150));
        s_wave_keys[1][i].frame = (uint16_t)(i * WAVE_LEN / KEYS);
        s_wave_keys[1][i].q = rz((i & 1) ? DEG(-35) : DEG(35));   /* the forearm, side to side */
    }
    s_wave_tracks[B_RUARM].keys = s_wave_keys[0]; s_wave_tracks[B_RUARM].n = KEYS;
    s_wave_tracks[B_RFARM].keys = s_wave_keys[1]; s_wave_tracks[B_RFARM].n = KEYS;
    s_wave.tracks = s_wave_tracks; s_wave.bones = N_BONES; s_wave.length = WAVE_LEN; s_wave.loop = 1;
}

/* ── the ground ─────────────────────────────────────────────────────────────── */
static int32_t stair_z(int32_t z)              /* z inside the repeating pattern */
{
    int32_t m = z % STAIR_PERIOD;
    return m < 0 ? m + STAIR_PERIOD : m;
}
/* the height of the ground under (x, z): steps up, a landing, steps down */
static int32_t ground(int32_t x, int32_t z)
{
    if (x < -STAIR_HALF_W || x > STAIR_HALF_W) return 0;
    const int32_t m = stair_z(z) - STAIR_START;
    if (m < 0) return 0;
    const int32_t i = m / STEP_RUN;              /* 0,1,2 up, 3 landing, 4,5,6 down */
    static const int8_t LEVEL[8] = { 1, 2, 3, 3, 2, 1, 0, 0 };
    return i < 8 ? LEVEL[i] * STEP_RISE : 0;
}

/* ── state ──────────────────────────────────────────────────────────────────── */
static int32_t s_x, s_z, s_pelvis_y;
static int     s_heading;
static int32_t s_phase_q16;                    /* through the cycle, 0..65535 */
static int     s_run_q8, s_lines, s_wave_t, s_held[5];
static int32_t s_cam[3];

static int pressed(int n)
{
    const int b = vpy_j1_button(n);
    const int edge = b && !s_held[n];
    s_held[n] = b;
    return edge;
}

static void reset(void)
{
    s_x = 0; s_z = 0; s_heading = 0; s_phase_q16 = 0; s_run_q8 = 0; s_wave_t = 0;
    s_pelvis_y = HIP_HEIGHT;
    s_cam[0] = CAM_SIDE; s_cam[1] = HIP_HEIGHT + CAM_UP; s_cam[2] = -CAM_BACK;
    vpyb_reset_stats();
}

static void setup(void)
{
    build_box(&s_pelvis, HIP_W + 40, -60, 60, 80, 0);
    build_box(&s_torso, 170, 40, TORSO_H, 90, 0);
    build_box(&s_head, 100, 0, 220, 110, 10);
    build_box(&s_uarm, 50, -UPPER_ARM, 30, 50, 0);
    build_box(&s_farm, 45, -FOREARM, 0, 45, 0);
    build_box(&s_thigh, 70, -THIGH, 30, 70, 0);
    build_box(&s_shin, 55, -SHIN, 0, 55, 0);
    build_box(&s_foot, 55, -FOOT_H, 10, FOOT_LEN / 2, FOOT_LEN / 2 - 30);

    vpyb_init(&s_sk);
    vpyb_add(&s_sk, -1, 0, 0, 0, &s_pelvis);                          /* B_PELVIS */
    vpyb_add(&s_sk, B_PELVIS, 0, 60, 0, &s_torso);                    /* B_TORSO */
    vpyb_add(&s_sk, B_TORSO, 0, NECK, 0, &s_head);                    /* B_HEAD */
    vpyb_add(&s_sk, B_TORSO,  SHOULDER_W, TORSO_H - 50, 0, &s_uarm);  /* B_LUARM: left is +x facing +z */
    vpyb_add(&s_sk, B_LUARM, 0, -UPPER_ARM, 0, &s_farm);              /* B_LFARM */
    vpyb_add(&s_sk, B_TORSO, -SHOULDER_W, TORSO_H - 50, 0, &s_uarm);  /* B_RUARM */
    vpyb_add(&s_sk, B_RUARM, 0, -UPPER_ARM, 0, &s_farm);              /* B_RFARM */
    vpyb_add(&s_sk, B_PELVIS,  HIP_W, -40, 0, &s_thigh);              /* B_LTHIGH */
    vpyb_add(&s_sk, B_LTHIGH, 0, -THIGH, 0, &s_shin);                 /* B_LSHIN */
    vpyb_add(&s_sk, B_LSHIN, 0, -SHIN, 0, &s_foot);                   /* B_LFOOT: its joint is the ankle */
    vpyb_add(&s_sk, B_PELVIS, -HIP_W, -40, 0, &s_thigh);              /* B_RTHIGH */
    vpyb_add(&s_sk, B_RTHIGH, 0, -THIGH, 0, &s_shin);                 /* B_RSHIN */
    vpyb_add(&s_sk, B_RSHIN, 0, -SHIN, 0, &s_foot);                   /* B_RFOOT */

    build_clip(0, &WALK);
    build_clip(1, &RUN);
    build_wave();
    reset();
}

/* ── one frame ──────────────────────────────────────────────────────────────── */
/* A leg onto the ground: the animated ankle lifted by the ground's height under
 * it, and never below the sole; the knee bending forward, towards `pole`. */
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

static void draw_ground(void)
{
    /* a floor grid round the figure, every metre */
    const int32_t gx = s_x / 1000 * 1000, gz = s_z / 1000 * 1000;
    for (int i = -4; i <= 4; i++) {
        vpy3d_line_world(gx - 4000, 0, gz + i * 1000, gx + 4000, 0, gz + i * 1000, BR_GROUND);
        vpy3d_line_world(gx + i * 1000, 0, gz - 4000, gx + i * 1000, 0, gz + 4000, BR_GROUND);
    }
    /* the steps near the figure: each tread's outline and the risers at its ends */
    const int32_t base = s_z - stair_z(s_z);
    for (int rep = -1; rep <= 1; rep++) {
        const int32_t z0 = base + rep * STAIR_PERIOD + STAIR_START;
        if (z0 > s_z + 5000 || z0 + 8 * STEP_RUN < s_z - 4000) continue;
        for (int i = 0; i < 6; i++) {
            const int32_t za = z0 + i * STEP_RUN, zb = za + (i == 2 ? 2 : 1) * STEP_RUN;
            if (i == 3) continue;                       /* the landing is drawn with step 2 */
            const int32_t h = ground(0, za);
            const int32_t hp = ground(0, za - 1), hn = ground(0, zb);
            vpy3d_line_world(-STAIR_HALF_W, h, za, STAIR_HALF_W, h, za, BR_STEP);   /* front edge */
            vpy3d_line_world(-STAIR_HALF_W, h, zb, STAIR_HALF_W, h, zb, BR_STEP);   /* back edge */
            vpy3d_line_world(-STAIR_HALF_W, h, za, -STAIR_HALF_W, h, zb, BR_STEP);
            vpy3d_line_world( STAIR_HALF_W, h, za,  STAIR_HALF_W, h, zb, BR_STEP);
            /* the risers at both ends of the tread, down to the level beside it */
            vpy3d_line_world(-STAIR_HALF_W, hp, za, -STAIR_HALF_W, h, za, BR_STEP);
            vpy3d_line_world( STAIR_HALF_W, hp, za,  STAIR_HALF_W, h, za, BR_STEP);
            vpy3d_line_world(-STAIR_HALF_W, hn, zb, -STAIR_HALF_W, h, zb, BR_STEP);
            vpy3d_line_world( STAIR_HALF_W, hn, zb,  STAIR_HALF_W, h, zb, BR_STEP);
        }
    }
}

static void loop(void)
{
    /* input: stick up runs, left/right turns */
    const int jx = vpy_j1_x(), jy = vpy_j1_y();
    const int want_run = jy > 20 ? (jy - 20) * 256 / 107 : 0;
    s_run_q8 += (want_run - s_run_q8) / 8;                         /* eased, so the gait changes over ~8 frames */
    if (s_run_q8 > 256) s_run_q8 = 256;
    if (jx > 30 || jx < -30) s_heading -= jx * TURN_SPEED / 127;  /* stick right turns right */
    if (pressed(1)) s_lines = !s_lines;
    if (pressed(4)) reset();

    /* THE SHARED PHASE: one step through the cycle a frame, the cycle as long as
     * the blend of the two clips' lengths, so walking and running stay in step */
    const int32_t len = WALK_LEN + (RUN_LEN - WALK_LEN) * s_run_q8 / 256;
    s_phase_q16 = (s_phase_q16 + 65536 / len) & 0xFFFF;
    const int32_t speed = WALK_SPEED + (RUN_SPEED - WALK_SPEED) * s_run_q8 / 256;
    const int32_t fwd[3] = { vpy_sin_q14(s_heading), 0, vpy_cos_q14(s_heading) };   /* Q14 */
    s_x += speed * fwd[0] / 16384;
    s_z += speed * fwd[2] / 16384;

    /* the clips, at the same fraction of their own cycles */
    vpyb_apply_blend(&s_sk, &s_clip[0], s_phase_q16 * WALK_LEN >> 8,
                     &s_clip[1], s_phase_q16 * RUN_LEN >> 8, s_run_q8);
    if (vpy_j1_button(2)) { vpyb_apply(&s_sk, &s_wave, s_wave_t << 8); s_wave_t++; }

    /* the pelvis: its bob (twice a cycle, lower as the legs spread), carried on
     * the LOWER foot's ground so the other one can bend up onto a step */
    const int bob = (vpy_cos_q14((int)(s_phase_q16 * 2 * VPY_Q14_TURN >> 16)) * (12 + s_run_q8 / 8)) >> 14;
    const int32_t dl = HIP_W * fwd[2] / 16384, dz = -HIP_W * fwd[0] / 16384;  /* across the body */
    const int32_t gl = ground(s_x + dl, s_z + dz), gr = ground(s_x - dl, s_z - dz);
    const int32_t want_y = HIP_HEIGHT - s_run_q8 / 3 + bob + (gl < gr ? gl : gr);
    s_pelvis_y += (want_y - s_pelvis_y) / PELVIS_EASE;

    const int32_t pos[3] = { s_x, s_pelvis_y, s_z };
    const vpyb_quat turn = vpyb_quat_axis(0, 1, 0, s_heading);
    vpyb_pose(&s_sk, pos, turn);
    const int32_t ahead[3] = { fwd[0] * 1000 / 16384, 0, fwd[2] * 1000 / 16384 };
    plant(B_LTHIGH, B_LFOOT, ahead);
    plant(B_RTHIGH, B_RFOOT, ahead);

    /* the camera: off the figure's left side and a little behind, eased after it —
     * side on is where a walk reads: the legs scissor across the picture */
    const int32_t want_cam[3] = { s_x + (CAM_SIDE * fwd[2] - CAM_BACK * fwd[0]) / 16384, s_pelvis_y + CAM_UP,
                                  s_z + (-CAM_SIDE * fwd[0] - CAM_BACK * fwd[2]) / 16384 };
    for (int k = 0; k < 3; k++) s_cam[k] += (want_cam[k] - s_cam[k]) / 8;
    vpy3d_look_at(s_cam[0], s_cam[1], s_cam[2], s_x, s_pelvis_y - 100, s_z, 0, 1, 0);

    draw_ground();
    vpyb_draw(&s_sk, BR_BODY, s_lines ? VPYB_DRAW_LINES : VPYB_DRAW_MESH);
    if (s_lines) {
        /* the lines alone lose the head and the feet: a mark for each */
        const int32_t *h = s_sk.pos[B_HEAD];
        vpy3d_line_world(h[0], h[1], h[2], h[0], h[1] + 220, h[2], BR_BODY);
        for (int f = 0; f < 2; f++) {
            const int32_t *a = s_sk.pos[f ? B_RFOOT : B_LFOOT];
            vpy3d_line_world(a[0], a[1], a[2], a[0] + ahead[0] / 6, a[1], a[2] + ahead[2] / 6, BR_BODY);
        }
    }

    /* readout */
    const vpyb_stats_t *bs = vpyb_stats();
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_set_text_size(6);
    vpy_print_text(-118, 108, "BON"); vpy_print_number(-96, 108, (long)bs->bones);
    vpy_print_text( -40, 108, "IKS"); vpy_print_number(-18, 108, (long)bs->ik_stretched);
    vpy_print_text(  40, 108, "RUN"); vpy_print_number( 62, 108, (long)s_run_q8);
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
