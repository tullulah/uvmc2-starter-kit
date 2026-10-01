/* occlusion_demo — one solid hiding another, with vpy3d's occluder on and off.
 *
 * A display with no depth buffer shows a second solid behind the first by
 * drawing straight through it: vpy3d removes hidden lines WITHIN a mesh, never
 * between two. This draws three MOVING meshes — boxes turning, each at its own
 * rate, the middle one swinging through the other two in depth — over a floor
 * grid, and lets the occluder be
 * switched, so the difference is one button away and the same pose can be
 * photographed both ways.
 *
 * CONTROLS
 *   button 1   occlusion on / off
 *   button 2   freeze / resume (same pose for both screenshots)
 *
 * HOW IT IS DRAWN, which is the half of the occluder that is the caller's job
 * (sdk/vpy-c/include/vpy3d.h, "ONE SOLID HIDING ANOTHER"):
 *
 *   1. vpy3d_set_mesh_occlusion(1), so vpy3d_draw_mesh's strokes are cut too.
 *      It is off by default: a game may rely on a mesh never being cut.
 *   2. vpy3d_occl_reset() once a frame. The occluders are rebuilt every frame,
 *      which is why moving solids need nothing special.
 *   3. The boxes NEAR TO FAR, re-sorted every frame. Each is drawn with
 *      vpy3d_draw_mesh and only THEN added with vpy3d_occl_add_mesh — never
 *      before, or it would cut itself.
 *   4. The floor LAST, through vpy3d_occl_line, so every box can hide it.
 *
 * THE READOUT. CUT is vpy3d's occl_cut: lines that lost a part this frame. With
 * occlusion on and the boxes overlapping it must be above zero — zero would
 * mean the order is wrong, not that nothing was hidden. STK is the strokes in
 * the frame, so the cost of the occluder is on screen too. DROP must be 0, or
 * the picture is not the frame that was built.
 */
#include <vpy.h>
#include <vpy3d.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#endif

/* ── the scene, in world units ──────────────────────────────────────────────
 * Sizes and rates are composition, chosen to overlap on screen from every angle
 * of the orbit; none of them is a measurement. */
#define BOX_H        700      /* half size of each box */
#define FLOOR_Y        0
#define FLOOR_HALF  3500      /* the grid spans +-this in x and z */
#define FLOOR_STEP   700
#define CAM_R       6500      /* orbit radius */
#define CAM_Y       2600      /* eye height */
#define CAM_TURN       3      /* Q14 angle units per frame: ~27 s a turn at 50 Hz */

#define BR_BOX       110
#define BR_FLOOR      45

/* spin: Q14 units per frame. swing: how far the box travels in z, there and
 * back, so it passes in front of and behind the others and the near-to-far
 * order changes while it moves. */
typedef struct { int32_t c[3]; int spin; int32_t swing; } box_t;

static const box_t s_boxes[] = {
    { { -1000, BOX_H,  0 },  5,    0 },
    { {     0, BOX_H,  0 }, -3, 2400 },
    { {  1000, BOX_H,  0 },  2,    0 },
};
#define SWING_RATE     6      /* Q14 units per frame: ~14 s there and back */
#define NBOX ((int)(sizeof s_boxes / sizeof s_boxes[0]))

static vpy_mesh s_cube;
static int s_occlude = 1;
static int s_frozen;
static int s_angle = 300;
static int s_t;                  /* frames run, for the boxes' own turning */
static int s_held1, s_held2;

static void build_cube(void)
{
    int v[8];
    vpy3d_mesh_begin(&s_cube);
    for (int k = 0; k < 8; k++)   /* bit 0 +x, bit 1 +y, bit 2 +z */
        v[k] = vpy3d_vertex((k & 1) ? BOX_H : -BOX_H,
                            (k & 2) ? BOX_H : -BOX_H,
                            (k & 4) ? BOX_H : -BOX_H);
    vpy3d_quad(v[0], v[4], v[6], v[2]);   /* -x */
    vpy3d_quad(v[1], v[3], v[7], v[5]);   /* +x */
    vpy3d_quad(v[0], v[1], v[5], v[4]);   /* -y */
    vpy3d_quad(v[2], v[6], v[7], v[3]);   /* +y */
    vpy3d_quad(v[0], v[2], v[3], v[1]);   /* -z */
    vpy3d_quad(v[4], v[5], v[7], v[6]);   /* +z */
    vpy3d_mesh_end(VPY3D_HARD_45);
}

static void line(int32_t ax, int32_t ay, int32_t az,
                 int32_t bx, int32_t by, int32_t bz, int br)
{
    if (s_occlude) vpy3d_occl_line(ax, ay, az, bx, by, bz, br);
    else           vpy3d_line_world(ax, ay, az, bx, by, bz, br);
}

static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_STEP) {
        line(t, FLOOR_Y, -FLOOR_HALF, t, FLOOR_Y, FLOOR_HALF, BR_FLOOR);
        line(-FLOOR_HALF, FLOOR_Y, t, FLOOR_HALF, FLOOR_Y, t, BR_FLOOR);
    }
}

/* Where box i is this frame. */
static void box_at(int i, int32_t *p)
{
    const box_t *b = &s_boxes[i];
    p[0] = b->c[0];
    p[1] = b->c[1];
    p[2] = b->c[2] + (int32_t)(((int64_t)b->swing *
                       vpy_sin_q14((s_t * SWING_RATE) & (VPY_Q14_TURN - 1))) / VPY_Q14_ONE);
}

static int64_t dist2(int i, const int32_t *eye)
{
    int32_t p[3];
    box_at(i, p);
    int64_t s = 0;
    for (int a = 0; a < 3; a++) {
        const int64_t d = (int64_t)p[a] - eye[a];
        s += d * d;
    }
    return s;
}

static void setup(void) { build_cube(); }

static void loop(void)
{
    const int b1 = vpy_j1_button(1), b2 = vpy_j1_button(2);
    if (b1 && !s_held1) s_occlude = !s_occlude;
    if (b2 && !s_held2) s_frozen = !s_frozen;
    s_held1 = b1; s_held2 = b2;
    if (!s_frozen) {
        s_angle = (s_angle + CAM_TURN) % VPY_Q14_TURN;
        s_t++;
    }

    const int32_t ex = (int32_t)(((int64_t)CAM_R * vpy_sin_q14(s_angle)) / VPY_Q14_ONE);
    const int32_t ez = (int32_t)(-((int64_t)CAM_R * vpy_cos_q14(s_angle)) / VPY_Q14_ONE);
    vpy3d_look_at(ex, CAM_Y, ez, 0, BOX_H / 2, 0, 0, 1, 0);
    int32_t eye[3];
    vpy3d_eye(eye);

    /* Near to far, EVERY FRAME: the middle box swings through the others, so
     * the order is not fixed. Three boxes, so an insertion sort by distance. */
    int order[NBOX];
    for (int i = 0; i < NBOX; i++) {
        int j = i;
        while (j > 0 && dist2(order[j - 1], eye) > dist2(i, eye)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    vpy3d_reset_counts();
    vpy3d_set_mesh_occlusion(s_occlude);
    vpy3d_occl_reset();
    for (int i = 0; i < NBOX; i++) {
        const box_t *b = &s_boxes[order[i]];
        int32_t p[3];
        box_at(order[i], p);
        const vpy_xf at = vpy3d_translate(p[0], p[1], p[2]);
        const vpy_xf rot = vpy3d_rot_y((s_t * b->spin) & (VPY_Q14_TURN - 1));
        const vpy_xf place = vpy3d_mul(&at, &rot);
        vpy3d_draw_mesh(&s_cube, &place, BR_BOX);
        if (s_occlude) vpy3d_occl_add_mesh(&s_cube, &place);   /* AFTER drawing it */
    }
    draw_floor();

    /* Logical units for the text; inside vpy3d's 15500 window. */
    vpy_set_text_size(8);
    vpy_print_text(-115, 108, s_occlude ? "OCCLUSION ON" : "OCCLUSION OFF");
    vpy_set_text_size(6);
    const vpy3d_stats_t *st = vpy3d_stats();
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_print_text(-115, -100, "CUT");  vpy_print_number(-90, -100, (long)st->occl_cut);
    vpy_print_text( -40, -100, "STK");  vpy_print_number(-15, -100, (long)ds->strokes);
    vpy_print_text(  35, -100, "DROP"); vpy_print_number( 65, -100,
                                                         (long)(ds->dropped
#ifndef VPY_DUAL_CORE
                                                                 + uvm2_stats.dropped
#endif
                                                                 ));
    if (s_frozen) vpy_print_text(60, 108, "HOLD");
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
