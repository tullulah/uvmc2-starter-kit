/* meshes.c — the shapes more than one room draws. */
#include "playroom.h"

void pr_box(vpy_mesh *m, int hx, int hy, int hz)
{
    pr_box_y(m, hx, -hy, hy, hz, 0);
}

void pr_box_y(vpy_mesh *m, int hx, int y0, int y1, int hz, int cz)
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

/* THE CRATE, WITH A VERTEX IN THE MIDDLE OF EVERY FACE: undented it draws like a
 * plain box (the four triangles of a face are coplanar), but a face can only fold
 * where it has a vertex. Creases from 15 degrees (cos = 15826), so a dent shows. */
void pr_crate(vpy_mesh *m, int h)
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

/* A low sphere, outline only (HARD_60): a ball reads as round on the tube. */
#define SEG   8
#define RINGS 3
void pr_ball(vpy_mesh *m, int r)
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

/* Faces as vpyp_hull_shape takes them (a count, that many corners, a 0 to end),
 * each wound to face OUT whichever way the list has it: vpy3d hides back faces
 * by their winding. One list makes the mesh and the hull, so they agree. */
void pr_from_faces(vpy_mesh *m, const int16_t *xyz, int nv, const uint8_t *faces)
{
    int v[8];
    int32_t mean[3] = { 0, 0, 0 };
    vpy3d_mesh_begin(m);
    for (int i = 0; i < nv; i++) {
        v[i] = vpy3d_vertex(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]);
        for (int k = 0; k < 3; k++) mean[k] += xyz[i * 3 + k];
    }
    for (int k = 0; k < 3; k++) mean[k] /= nv;
    for (const uint8_t *f = faces; *f; f += 1 + *f) {
        const int16_t *a = &xyz[f[1] * 3], *b = &xyz[f[2] * 3], *c = &xyz[f[3] * 3];
        const int64_t u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const int64_t n[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
        const int out = n[0] * (a[0] - mean[0]) + n[1] * (a[1] - mean[1]) + n[2] * (a[2] - mean[2]) > 0;
        if (*f == 3) { if (out) vpy3d_tri(v[f[1]], v[f[2]], v[f[3]]); else vpy3d_tri(v[f[3]], v[f[2]], v[f[1]]); }
        else         { if (out) vpy3d_quad(v[f[1]], v[f[2]], v[f[3]], v[f[4]]); else vpy3d_quad(v[f[4]], v[f[3]], v[f[2]], v[f[1]]); }
    }
    vpy3d_mesh_end(VPY3D_HARD_45);
}

/* A pyramid's centre of mass is a quarter of its height above the base: the
 * corners are round it, which is where the body turns. */
const int16_t PR_PYR_V[5 * 3] = { -150,-75,-150,  150,-75,-150,  150,-75,150,  -150,-75,150,  0,225,0 };
const uint8_t PR_PYR_F[] = { 4, 0,1,2,3,  3, 0,1,4,  3, 1,2,4,  3, 2,3,4,  3, 3,0,4,  0 };

void pr_octahedron(vpy_mesh *m, int h)
{
    vpy3d_mesh_begin(m);
    const int px = vpy3d_vertex(h, 0, 0), nx = vpy3d_vertex(-h, 0, 0);
    const int py = vpy3d_vertex(0, h, 0), ny = vpy3d_vertex(0, -h, 0);
    const int pz = vpy3d_vertex(0, 0, h), nz = vpy3d_vertex(0, 0, -h);
    vpy3d_tri(py, pz, px); vpy3d_tri(py, px, nz); vpy3d_tri(py, nz, nx); vpy3d_tri(py, nx, pz);
    vpy3d_tri(ny, px, pz); vpy3d_tri(ny, nz, px); vpy3d_tri(ny, nx, nz); vpy3d_tri(ny, pz, nx);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

void pr_pyramid(vpy_mesh *m, int h)
{
    vpy3d_mesh_begin(m);
    const int a = vpy3d_vertex(-h, -h, -h), b = vpy3d_vertex(h, -h, -h);
    const int c = vpy3d_vertex(h, -h, h),   d = vpy3d_vertex(-h, -h, h);
    const int top = vpy3d_vertex(0, h, 0);
    vpy3d_quad(a, b, c, d);
    vpy3d_tri(top, b, a); vpy3d_tri(top, c, b); vpy3d_tri(top, d, c); vpy3d_tri(top, a, d);
    vpy3d_mesh_end(VPY3D_HARD_45);
}

int64_t pr_isqrt64(int64_t n)
{
    int64_t r = 0, bit = (int64_t)1 << 62;
    if (n <= 0) return 0;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* THE FLYER: a dart pointing along +z — a nose, two wings, a fin on top and a
 * keel below, closed by a flat back (its four corners share z, so it is planar).
 * `low` is the far version: the wings' triangle alone, an open plate. */
void pr_dart(vpy_mesh *m, int len, int low)
{
    const int16_t w = (int16_t)(len * 3 / 5), h = (int16_t)(len / 4), back = (int16_t)(-len / 2), nose = (int16_t)(len / 2);
    if (low) {
        vpy3d_mesh_begin(m);
        const int n = vpy3d_vertex(0, 0, nose), l = vpy3d_vertex(-w, 0, back), r = vpy3d_vertex(w, 0, back);
        vpy3d_tri(n, r, l);
        vpy3d_mesh_end(VPY3D_HARD_45);
        vpy3d_mesh_open(m, 1);
        return;
    }
    /* nose, left wing, right wing, fin top, keel */
    const int16_t v[5 * 3] = { 0, 0, nose,  (int16_t)-w, 0, back,  w, 0, back,  0, h, back,  0, (int16_t)(-h / 3), back };
    static const uint8_t F[] = { 3, 0,1,3,  3, 0,3,2,  3, 0,2,4,  3, 0,4,1,  4, 1,4,2,3,  0 };
    pr_from_faces(m, v, 5, F);
}
