/* occlusion_demo — one solid hiding another, with vpy3d's occluder on and off.
 *
 * A display with no depth buffer shows a second solid behind the first by
 * drawing straight through it: vpy3d removes hidden lines WITHIN a mesh, never
 * between two. This draws three boxes overlapping in depth over a floor grid,
 * and lets the occluder be switched, so the difference is one button away and
 * the same pose can be photographed both ways.
 *
 * CONTROLS
 *   button 1   occlusion on / off
 *   button 2   freeze / resume the camera (same pose for both screenshots)
 *
 * HOW IT IS DRAWN, which is the half of the occluder that is the caller's job
 * (sdk/vpy-c/include/vpy3d.h, "ONE SOLID HIDING ANOTHER"):
 *
 *   1. vpy3d_occl_reset() once a frame.
 *   2. The boxes NEAR TO FAR. Each one's lines go through vpy3d_occl_line, and
 *      only after it is drawn is it added as an occluder — never before, or it
 *      would cut itself.
 *   3. The floor LAST, so every box can hide it.
 *
 * The boxes are drawn edge by edge and not with vpy3d_draw_mesh, because a
 * mesh is never occluded: only strokes through vpy3d_occl_line are cut. Each
 * box removes its own hidden edges the cheap way the vpy3d header describes —
 * an axis-aligned face is turned towards us when the eye is further out along
 * its axis than the face is.
 *
 * THE READOUT. CUT is vpy3d's occl_cut: lines that lost a part this frame. With
 * occlusion on and the boxes overlapping it must be above zero — zero would
 * mean the order is wrong, not that nothing was hidden. STK is the strokes in
 * the frame, so the cost of the occluder is on screen too. DROP must be 0, or
 * the picture is not the frame that was built.
 */
#include <vpy.h>
#include <vpy3d.h>
#include <uvm2_bus.h>

/* ── the scene, in world units ──────────────────────────────────────────────
 * Sizes are composition, chosen to overlap on screen from every angle of the
 * orbit; none of them is a measurement. */
#define BOX_H        700      /* half size of each box */
#define FLOOR_Y        0
#define FLOOR_HALF  3500      /* the grid spans +-this in x and z */
#define FLOOR_STEP   700
#define CAM_R       6500      /* orbit radius */
#define CAM_Y       2600      /* eye height */
#define CAM_TURN       3      /* Q14 angle units per frame: ~27 s a turn at 50 Hz */

#define BR_BOX       110
#define BR_FLOOR      45

typedef struct { int32_t c[3]; } box_t;

static const box_t s_boxes[] = {
    { { -900, BOX_H,  700 } },
    { {    0, BOX_H,    0 } },
    { {  900, BOX_H, -700 } },
};
#define NBOX ((int)(sizeof s_boxes / sizeof s_boxes[0]))

static int s_occlude = 1;
static int s_frozen;
static int s_angle = 300;
static int s_held1, s_held2;

static void line(int32_t ax, int32_t ay, int32_t az,
                 int32_t bx, int32_t by, int32_t bz, int br)
{
    if (s_occlude) vpy3d_occl_line(ax, ay, az, bx, by, bz, br);
    else           vpy3d_line_world(ax, ay, az, bx, by, bz, br);
}

/* Is the face on `axis`, on the + side (`plus`) or the - side, turned to us? */
static int face_visible(const box_t *b, const int32_t *eye, int axis, int plus)
{
    return plus ? eye[axis] > b->c[axis] + BOX_H
                : eye[axis] < b->c[axis] - BOX_H;
}

/* Corner k: bit 0 picks +x, bit 1 +y, bit 2 +z. */
static void corner(const box_t *b, int k, int32_t *p)
{
    for (int a = 0; a < 3; a++)
        p[a] = b->c[a] + (((k >> a) & 1) ? BOX_H : -BOX_H);
}

static void draw_box(const box_t *b, const int32_t *eye)
{
    /* The twelve edges: from every corner, along every axis where its bit is 0.
     * An edge along axis a is shared by one face on each of the other two axes,
     * on the side its corner's bits say; it is drawn if either face is seen. */
    for (int k = 0; k < 8; k++) {
        for (int a = 0; a < 3; a++) {
            if ((k >> a) & 1) continue;
            const int u = (a + 1) % 3, v = (a + 2) % 3;
            if (!face_visible(b, eye, u, (k >> u) & 1) &&
                !face_visible(b, eye, v, (k >> v) & 1)) continue;
            int32_t p[3], q[3];
            corner(b, k, p);
            corner(b, k | (1 << a), q);
            line(p[0], p[1], p[2], q[0], q[1], q[2], BR_BOX);
        }
    }
    if (s_occlude) {
        int32_t cs[8][3];
        for (int k = 0; k < 8; k++) corner(b, k, cs[k]);
        vpy3d_occl_add((const int32_t (*)[3])cs, 8);
    }
}

static void draw_floor(void)
{
    for (int32_t t = -FLOOR_HALF; t <= FLOOR_HALF; t += FLOOR_STEP) {
        line(t, FLOOR_Y, -FLOOR_HALF, t, FLOOR_Y, FLOOR_HALF, BR_FLOOR);
        line(-FLOOR_HALF, FLOOR_Y, t, FLOOR_HALF, FLOOR_Y, t, BR_FLOOR);
    }
}

static int64_t dist2(const box_t *b, const int32_t *eye)
{
    int64_t s = 0;
    for (int a = 0; a < 3; a++) {
        const int64_t d = (int64_t)b->c[a] - eye[a];
        s += d * d;
    }
    return s;
}

static void setup(void) {}

static void loop(void)
{
    const int b1 = vpy_j1_button(1), b2 = vpy_j1_button(2);
    if (b1 && !s_held1) s_occlude = !s_occlude;
    if (b2 && !s_held2) s_frozen = !s_frozen;
    s_held1 = b1; s_held2 = b2;
    if (!s_frozen) s_angle = (s_angle + CAM_TURN) % VPY_Q14_TURN;

    const int32_t ex = (int32_t)(((int64_t)CAM_R * vpy_sin_q14(s_angle)) / VPY_Q14_ONE);
    const int32_t ez = (int32_t)(-((int64_t)CAM_R * vpy_cos_q14(s_angle)) / VPY_Q14_ONE);
    vpy3d_look_at(ex, CAM_Y, ez, 0, BOX_H / 2, 0, 0, 1, 0);
    int32_t eye[3];
    vpy3d_eye(eye);

    /* Near to far: three boxes, so an insertion sort by distance. */
    int order[NBOX];
    for (int i = 0; i < NBOX; i++) {
        int j = i;
        while (j > 0 && dist2(&s_boxes[order[j - 1]], eye) > dist2(&s_boxes[i], eye)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    vpy3d_reset_counts();
    vpy3d_occl_reset();
    for (int i = 0; i < NBOX; i++) draw_box(&s_boxes[order[i]], eye);
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
                                                         (long)(ds->dropped + uvm2_stats.dropped));
    if (s_frozen) vpy_print_text(60, 108, "HOLD");
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
