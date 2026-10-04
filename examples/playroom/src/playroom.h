/* playroom.h — what the rooms of the playroom share. */
#ifndef PLAYROOM_H
#define PLAYROOM_H

#include <stdint.h>
#include <vpy.h>
#include <vpy3d.h>

/* ── the rooms ─────────────────────────────────────────────────────────── */
enum { R_CRATES = 0, R_JELLY, R_SHAPES, R_FLYER, N_ROOMS, R_FINALE = N_ROOMS };

/* Each room: enter() builds its world from nothing (every room resets the
 * modules it uses, so no state leaks from the last one), frame() runs one frame
 * and returns 1 once its goal is met. Meshes are built once, at start-up
 * (build()), because vpy3d's pools are never given back. */
void crates_build(void);  void crates_enter(void);  int crates_frame(void);
void jelly_build(void);   void jelly_enter(void);   int jelly_frame(void);
void shapes_build(void);  void shapes_enter(void);  int shapes_frame(void);
void flyer_build(void);   void flyer_enter(void);   int flyer_frame(void);
void finale_build(void);  void finale_enter(void);  int finale_frame(void);

/* The hub: returns the room walked into (R_*), or -1. `from` is the room just
 * left (-1 at the start): the figure stands in front of its door. With
 * control 0 the stick does nothing (behind the title card). */
void hub_build(void);
void hub_enter(int from);
int  hub_frame(int control);

/* ── what main.c gives every room ──────────────────────────────────────── */
int  pr_pressed(int n);                 /* button n went down this frame */
int  pr_done(int room);                 /* has its piece been earned */
int  pr_done_count(void);
/* The line at the top of the screen while a room plays: what to do, and how far
 * along it is ("3 OF 8"). */
void pr_objective(const char *what, int have, int need);
/* Text centred on x = 0 at height y, in the 2D font at `size`. */
void pr_text_centre(int y, const char *s, int size, int br);

/* ── shared meshes (meshes.c) ──────────────────────────────────────────── */
void pr_box(vpy_mesh *m, int hx, int hy, int hz);
void pr_box_y(vpy_mesh *m, int hx, int y0, int y1, int hz, int cz);   /* hung from y = 0 */
void pr_crate(vpy_mesh *m, int h);       /* a box with a vertex mid-face, so it dents */
void pr_ball(vpy_mesh *m, int r);
void pr_from_faces(vpy_mesh *m, const int16_t *xyz, int nv, const uint8_t *faces);
void pr_octahedron(vpy_mesh *m, int h);
void pr_pyramid(vpy_mesh *m, int h);
void pr_dart(vpy_mesh *m, int len, int low);  /* the flyer; low = the far version */

/* the pyramid as a hull: its corners and faces, the same for mesh and body */
extern const int16_t PR_PYR_V[5 * 3];
extern const uint8_t PR_PYR_F[];

/* ── the stereo jack (UVMC2): set up once in main.c ─────────────────────── */
void pr_sound_frame(void);              /* feeds the DAC what it can take */

int64_t pr_isqrt64(int64_t n);

#define BR_TEXT    100
#define BR_HINT     80

#endif
