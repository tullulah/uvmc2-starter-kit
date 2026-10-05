# 08 — API reference

Complete surface, in the three layers a game can address. Everything higher is
written in terms of everything lower and they can be mixed freely.

* **Level 1 — libvpy** (`sdk/vpy-c/include/vpy.h`): a game library. Most games
  should live here.
* **Level 2 — the PiTrex/host contract** (`sdk/pitrex-sim/include/vectrex/vectrexInterface.h`):
  the backend-neutral surface. Write to this and the same source also builds for
  a desktop harness and the WASM simulator.
* **Level 3 — the SDK** (`sdk/uvm2-sdk/*.h`): the cartridge runtime itself.

### Units, in one place

| space | range | used by |
|---|---|---|
| VPy logical | x, y ∈ [−127, +127], **+y up**, origin centre | libvpy |
| PiTrex | VPy × 127, so ≈ ±16 000 | `v_directDraw32` |
| device | the SDK's own unit | `uvm2_draw_move_abs`, `uvm2_zero_jump` |
| subunit (q4) | 1/16 of a device unit | `*_q4`, everything inside `uvm2_draw.c` |
| brightness | 0..127; **0 means do not draw** | everywhere |
| angle | 0..127 = a full circle | `vpy_sin/cos/atan2` |
| bus cycle | 667 ns; 30 000 = a 50 Hz frame | `uvm2_exec`, `uvm2_stats` |

---

## Level 1 — libvpy

`#include <vpy.h>`, and add `$(VPY_C_SDK)/vpy.c` to `UVM2_SRCS`. Integer-only on
purpose (no `<math.h>`), so it compiles to plain integer ARM that hardware, host
and simulator all run identically.

### Lifecycle

```c
void vpy_init(void);          /* vectrexinit + v_init + v_setRefresh */
void vpy_frame_begin(void);   /* v_WaitRecal + refresh the input snapshot */
void vpy_run(void (*setup)(void), void (*loop)(void));   /* never returns */
```

`vpy_run` is the whole main loop: it initialises, calls `setup()` once, then per
frame calls `vpy_frame_begin()`, `vpy_music_update()`, `vpy_sfx_update()` and
your `loop()`.

### Drawing

```c
void vpy_set_intensity(int b);
void vpy_move(int x, int y);                                  /* set the cursor */
void vpy_draw_line(int x0,int y0,int x1,int y1,int b);
void vpy_draw_circle(int cx,int cy,int r,int b);              /* 16 segments */
void vpy_draw_ellipse(int cx,int cy,int rx,int ry,int b);     /* 16 segments */
void vpy_draw_rect(int x,int y,int w,int h,int b);            /* x,y = lower-left */
void vpy_draw_filled_rect(int x,int y,int w,int h,int b);     /* hatched, 3 units apart */
void vpy_draw_polygon(const int *xy,int n,int b);             /* n vertices, xy[2n] */
void vpy_draw_line_dev(int32_t x0,int32_t y0,int32_t x1,int32_t y1,int b);  /* deflection units */
```

**Every drawing call goes through a stroke buffer** (1024 strokes) and reaches
`v_directDraw32` only when the frame is flushed: by `vpy_run`, `vpy_frame_begin`
or `vpy_wait_recal`. A game that calls `v_WaitRecal()` directly never flushes and
draws nothing. `vpy_draw_stats()->frames` stuck at 0 is the sign.

```c
void vpy_flush(void);
void vpy_wait_recal(void);                 /* flush, then v_WaitRecal */
void vpy_set_priority(int p);              /* VPY_PRI_LOW 0 / NORMAL 128 / KEEP 255 */
int  vpy_pending_strokes(void);
const vpy_draw_stats_t *vpy_draw_stats(void);   /* frames, strokes, peak, shed, dropped, clamped */
int  vpy_sin_q14(int a), vpy_cos_q14(int a);    /* 4096 steps per turn, 16384 = 1.0 */
```

When the buffer is full, a stroke evicts a lower-priority one or is dropped. That
is the game library choosing, not the SDK: the SDK draws what it is asked
(invariant 4). It is always counted, so if `shed` or `dropped` is non-zero, the
frame on screen is not the frame the game built.

### 3D — `vpy3d.h`

A small 3D layer on top of the same stroke buffer: meshes with hidden-line
removal *within* each mesh, a camera, a projection and a screen clip. Integer
only. The header is the reference; this is the shape of it.

```c
int  vpy3d_look_at(int32_t ex,int32_t ey,int32_t ez, int32_t tx,int32_t ty,int32_t tz,
                   int32_t ux,int32_t uy,int32_t uz);
void vpy3d_set_focal(int32_t f), vpy3d_set_near(int32_t n), vpy3d_set_clip(int32_t h);
void vpy3d_set_clip_xy(int32_t hx,int32_t hy);       /* the window per axis; default 15500 square */
void vpy3d_set_aspect(int32_t num,int32_t den);      /* x scale; default 1/1, see below */
int  vpy3d_h_half_angle(void), vpy3d_v_half_angle(void);   /* what is on screen, Q14 units */
void vpy3d_draw_mesh(const vpy_mesh *m,const vpy_xf *place,int br);
void vpy3d_line_world(int32_t ax,int32_t ay,int32_t az, int32_t bx,int32_t by,int32_t bz,int br);

/* shading: depth, shadows, size */
int  vpy3d_fog(int br,int32_t x,int32_t y,int32_t z,int32_t full_until,int32_t gone_at);
int  vpy3d_shadow(const int32_t (*corners)[3],int n,int32_t lx,int32_t ly,int32_t lz,
                  int32_t floor_y,int br);                    /* hull on the floor */
int32_t vpy3d_screen_size(int32_t x,int32_t y,int32_t z,int32_t radius);   /* for LOD */
int  vpy3d_terrain(const int16_t *h,int cols,int rows,int32_t x0,int32_t z0,int32_t cell,int br);
                                                 /* a height map, its hidden lines hidden */

/* the pools: what is built after a mark is given back by releasing to it */
vpy3d_pool_mark_t vpy3d_pool_mark(void);
int  vpy3d_pool_release(vpy3d_pool_mark_t mark);     /* 0: mid-build, or a mark past the pools */

/* dents: an object's own mesh, pushed in where it was hit */
int  vpy3d_mesh_copy(vpy_mesh *dst,const vpy_mesh *src);       /* again = reset, no pool */
void vpy3d_mesh_dent(vpy_mesh *m,int32_t px,int32_t py,int32_t pz,
                     int32_t dx,int32_t dy,int32_t dz,int32_t depth,int32_t radius);
void vpy3d_world_to_model(const vpy_xf *place,int32_t wx,int32_t wy,int32_t wz,
                          int32_t *mx,int32_t *my,int32_t *mz);

/* where a ray meets a mesh, and marks where shots landed */
int  vpy3d_ray_mesh(const vpy_mesh *m,const vpy_xf *place,int32_t ox,int32_t oy,int32_t oz,
                    int32_t dx,int32_t dy,int32_t dz,int32_t max_dist,vpy3d_hit *out);  /* face or -1 */
void vpy3d_marks_clear(vpy3d_marks *mk);                       /* one vpy3d_marks per object */
void vpy3d_marks_add(vpy3d_marks *mk,const vpy_xf *place,int32_t x,int32_t y,int32_t z,
                     int32_t nx,int32_t ny,int32_t nz);        /* a vpyp_hit / vpy3d_hit as it is */
int  vpy3d_marks_draw(const vpy3d_marks *mk,const vpy_xf *place,int32_t radius,int br,
                      int style);                              /* VPY3D_MARK_RING / _CRACK */

/* text in the world: the vector font on a plane, or square to the camera */
int  vpy3d_text(const char *s,const vpy_xf *place,int32_t height,int br,int flags);
int  vpy3d_text_billboard(const char *s,int32_t x,int32_t y,int32_t z,int32_t height,int br,int flags);
                                         /* VPY3D_TEXT_OCCLUDE | _CENTRE | _FRONT */
/* stereo, one picture per eye (for the 3D Imager); the console's screen shape */
void vpy3d_set_stereo(int eye,int32_t half_separation,int32_t converge);  /* -1, +1; 0 = off */
int  vpy3d_use_console_window(void);     /* the calibrated window instead of the 15500 square */

/* level of detail: the version a ball of `radius` round place->t still deserves */
int  vpy3d_lod_pick(const vpy_xf *place,int32_t radius,const int32_t *min_size,int n);  /* -1: none */
int  vpy3d_draw_lod(const vpy_mesh *const *meshes,const int32_t *min_size,int n,
                    const vpy_xf *place,int32_t radius,int br);

/* morphing: two poses of one model */
int  vpy3d_mesh_blend(vpy_mesh *dst,const vpy_mesh *a,const vpy_mesh *b,int32_t t_q14);

/* one solid hiding another */
void vpy3d_occl_reset(void);                                   /* once a frame */
int  vpy3d_occl_add(const int32_t (*corners)[3],int n);        /* 3..8 world corners */
int  vpy3d_occl_add_mesh(const vpy_mesh *m,const vpy_xf *place);   /* <= 8 vertices */
void vpy3d_occl_line(int32_t ax,int32_t ay,int32_t az, int32_t bx,int32_t by,int32_t bz,int br);
void vpy3d_occl_line_cam(const int32_t *a,const int32_t *b,int br);   /* camera space */
void vpy3d_set_mesh_occlusion(int on);                         /* draw_mesh cut too; default off */
int  vpy3d_occl_count(void);
```

**One unit is one unit on both axes; the glass is portrait.** Measured
2026-10-01 by photograph on one console, with `examples/geometry_card` (device
units straight to `v_directDraw32`): a 16000-unit square is square on the glass
to within ~10%, and the visible window was about **±18000 × ±20500** — narrower
than the contract's "roughly ±24000" in y. So the portrait shape belongs in the
*window*, not in the scale, as the PiTrex contract and the BIOS assume.

The projection's x scale (`vpy3d_set_aspect`) defaults to 1/1. For one day
(2026-09-30) it defaulted to 4/3, from a service-manual argument that was never
measured; the photograph refuted it. The knob stays for a console whose size
pots are off. The clip defaults to the 15500 square every vpy3d game has been
composed in; `vpy3d_set_clip_xy` opens the taller window. Neither default moves
on one console's evidence — photograph the card on a second one first.
`vpy-c/tools/aspect_check.c` is the host witness: a world square projects with
w/h = 1.000, and each half angle follows its own axis's clip. Read the field of
view with `vpy3d_h/v_half_angle` rather than writing "58 degrees" a second time.

**The pools.** Every mesh's vertices, faces and edges live in shared pools sized
at compile time (`VPY3D_POOL_V/F/FV/E`), which only grow. A game that builds
everything at start-up never needs more; one whose meshes change — a level's
geometry — marks the pools after building what lasts (`vpy3d_pool_mark`) and
releases to the mark on entering the next level (`vpy3d_pool_release`), dropping
every pointer to the meshes built since first. A build that does not fit is left
empty (it draws nothing), `mesh_end` returns 0 and `vpy3d_stats()->overflow`
counts it — not zero means broken. `vpy-c/tools/pool_check.c` holds it to that.

**Dents.** A mesh is shared by everything drawn with it, so an object that can
be dented takes its own copy first (`vpy3d_mesh_copy`); copying again into it
resets it without using more pool, which is how a game recycles objects.
`vpy3d_mesh_dent` pushes the vertices near a point in and works the creases out
again, so a flat face shows the fold — but a face only bends where it has
vertices: give a dentable box a vertex in the middle of each face (it still
draws like a plain box). Dents are drawing only; a physics body keeps its
shape. `vpy-c/tools/dent_check.c` holds it to that.

**Marks.** A dent alone cannot be seen on something a few millimetres across on
the tube (measured on the console, 2026-10-01); a small bright ring on the face
that was hit can. `vpy3d_marks_add` stores the hit in the object's own space, so
the mark turns with it; `vpy3d_marks_draw` draws the rings (or cracks) through
the occluder, skipping faces turned away — call it after the object and before
adding the object as an occluder. A full set gives up its oldest mark.
`vpy3d_ray_mesh` finds the face a ray meets on any mesh, dented or morphed as it
is now; `vpyp_raycast` stays the one for bodies. `vpy-c/tools/mesh_check.c`
checks both and the LOD pick.

**Text in the world.** `vpy3d_text` draws the same vector font PRINT_TEXT uses, on
a plane placed by a `vpy_xf`: x along the text, y up the letters, read from the
plane's -z side; `height` is a capital's height in world units, one stroke per
font stroke. `VPY3D_TEXT_FRONT` hides it seen from behind (where it would read
backwards); `vpy3d_text_billboard` puts it square to the camera at a point — a
label over an object. `vpy-c/tools/text3d_check.c` checks size, centring,
foreshortening on a floor, the billboard from two sides and occlusion.

**Stereo.** `vpy3d_set_stereo(eye, half_separation, converge)` moves the camera
half the separation to one side, looking parallel, and shifts the picture so the
convergence plane has no parallax: what is nearer comes out of the screen. Draw
the scene once per eye. It is the picture half of the 3D Imager; the goggles'
driver (the wheel's speed and its sync) is not in the SDK yet — see `TODO.md`.

**The console's screen shape.** A console calibrated for it (`aspect_q8`, `win_x`,
`win_y` in `uvm2.cfg`, [12](12-calibrating-a-console.md)) has its aspect used by
vpy3d on its own, unless the game calls `vpy3d_set_aspect`; its visible window
only when the game asks with `vpy3d_use_console_window`, because a wider window
changes what a game composed in the 15500 square shows. Not under the debug
cartridge's BIOS, whose games do not link the configuration.
`vpy-c/tools/shape_check.c` checks it.

**Occlusion between solids is the caller's job, done with a silhouette.** Hidden
lines are removed within a mesh, never between two: with no depth buffer, a
second solid in front of the first is simply not there, and you see through it.
Any game with two solids on screen has this until it uses the occluder. A convex
solid's silhouette is the convex hull of its projected corners; a line behind it
is the line minus the part inside the hull, exact for convex occluders. With no
occluder added, `vpy3d_occl_line` *is* `vpy3d_line_world` plus one compare, so a
game can send every stroke through it.

There is **no depth test**. Order makes a silhouette mean "behind":

```c
vpy3d_occl_reset();
/* draw the nearest solid */      vpy3d_occl_add_mesh(&m, &at);   /* after, never before */
/* draw the next one, near to far, each line through vpy3d_occl_line */
```

**Meshes, moving or not.** Occluders are rebuilt every frame, so solids that
move, turn or change depth order need nothing special. With
`vpy3d_set_mesh_occlusion(1)`, the strokes `vpy3d_draw_mesh` emits are cut like
any line, after the mesh's own hidden-line removal: draw a mesh, then
`vpy3d_occl_add_mesh` it, near to far.

Things at the same depth are drawn as a group and only then added — otherwise
the first one drawn bites a piece out of its neighbour. The one guard built in:
a line wholly nearer than an occluder's nearest corner is never cut by it.

What it does **not** cut, each of which looks like a broken occluder:

| | |
|---|---|
| `vpy3d_draw_mesh`, by default | Cut only after `vpy3d_set_mesh_occlusion(1)`. Off by default because a game may rely on a mesh never being cut. |
| a visible piece < 1/48 of the line's screen length | Dropped as a sliver; about 2% of the screen on a full-width stroke. |
| an occluder with a corner behind the near plane | Refused (`vpy3d_occl_add` returns 0, `occl_refused` counts it) — that frame it hides nothing. |
| the 65th occluder | Refused, counted in `occl_full`. Add the nearest first. |
| an occluder many screens wide | The projection saturates and the hull arithmetic overflows. Clamp it to the visible window before adding it. |

A line with an end behind the near plane *is* cut: it is clipped to the near
plane first, with `vpy3d_line_cam`'s own arithmetic, and the rest is tested.

`vpy3d_stats()` counts it per frame (zeroed by `vpy3d_reset_counts`):
`occl_cut` is lines that lost a part — **the proof it ran**: with solids behind
silhouettes on screen and `occl_cut` at zero, the order is wrong, not the
occluder. `occl_refused` and `occl_full` count every occluder not taken.

It came from kuroishi (2026-09-24, "the waves look transparent while they
move") and moved into the SDK when hakaba needed the same thing.
`examples/occlusion_demo` is the worked example in this kit: three turning
meshes and a floor, drawn near to far, with button 1 switching the occluder off
to compare.

### Physics — `vpyphys.h`

Rigid bodies under gravity, collisions between them, and a ray cast. Integer
only and **deterministic**: the same calls give the same positions on every
target, bit for bit, so replays and host harnesses hold. It draws nothing; the
game reads positions back and draws them (with vpy3d meshes, or in 2D with z = 0).
Add `$(VPY_C_SDK)/vpyphys.c` to `UVM2_SRCS`.

```c
void vpyp_reset(void);
void vpyp_set_gravity(int32_t gx,int32_t gy,int32_t gz);     /* units/s²: 9800 mm/s² */
void vpyp_set_floor(int on,int32_t y,int restitution_q8,int friction_q8);
int  vpyp_add_sphere(int32_t x,int32_t y,int32_t z,int32_t r,int32_t mass);     /* mass 0 = static */
int  vpyp_add_box(int32_t x,int32_t y,int32_t z,int32_t hx,int32_t hy,int32_t hz,int32_t mass);
int  vpyp_hull_shape(const int16_t *xyz,int nverts,const uint8_t *faces);   /* checked; or -1 */
int  vpyp_add_hull(int32_t x,int32_t y,int32_t z,int shape,int32_t mass);
int  vpyp_hull_error(void);                                  /* why a shape was refused */
int  vpyp_ball_joint(int a,int b,int32_t px,int32_t py,int32_t pz);         /* b -1: the world */
int  vpyp_hinge(int a,int b,int32_t px,int32_t py,int32_t pz,int32_t ax,int32_t ay,int32_t az);
void vpyp_joint_remove(int joint);
void vpyp_set_material(int id,int restitution_q8,int friction_q8);
void vpyp_set_mask(int id,uint8_t mask);                     /* who collides with whom */
void vpyp_set_velocity(int id,int32_t vx,int32_t vy,int32_t vz);
void vpyp_apply_impulse(int id,int32_t ix,int32_t iy,int32_t iz);
void vpyp_apply_impulse_at(int id,int32_t ix,int32_t iy,int32_t iz, int32_t px,int32_t py,int32_t pz);
void vpyp_set_rotation(int id,int32_t ax,int32_t ay,int32_t az,int angle);   /* 4096 per turn */
void vpyp_set_spin(int id,int32_t wx,int32_t wy,int32_t wz);  /* 4096ths of a turn per second */
void vpyp_lock_rotation(int id,int on);                      /* stay upright */
void vpyp_step(void);                                        /* once per frame at 50 Hz */
void vpyp_position(int id,int32_t *x,int32_t *y,int32_t *z);
void vpyp_rotation(int id,int32_t m[9]);                     /* Q14, straight into vpy_xf.m */
int  vpyp_contact_count(void);  const vpyp_contact *vpyp_contact_get(int i);   /* .impulse: how hard */
int  vpyp_raycast(int32_t ox,int32_t oy,int32_t oz, int32_t dx,int32_t dy,int32_t dz,
                  int32_t max_dist,uint8_t mask,vpyp_hit *out);
int  vpyp_blast(int32_t cx,int32_t cy,int32_t cz,int32_t radius,int32_t speed,uint8_t mask);
const vpyp_stats_t *vpyp_stats(void);                        /* awake, contacts, refused */
```

Spheres, boxes and convex hulls, a floor, restitution, friction, sleeping bodies (a pile at
rest costs almost nothing) and a contact list with the impulse of each hit —
what a dent, a spark or an impact sound reads. **Bodies turn:** a hit
off-centre spins them, boxes tip over and tumble, balls roll;
`vpyp_rotation()` gives the turn as a Q14 matrix to draw with. Inertia is
a scalar, exact for spheres and cubes. Box against box tests all fifteen
separating axes, edge against edge included. The solver is sequential impulses
with warm starting and speculative contacts, so a stack holds and a fast body
does not tunnel through a thin wall. ~42 KB of code and ~99 KB of RAM, most of it
the contact table.

**Hulls** are any convex solid: register the shape once (corners round the
body's centre, faces as a count and that many corner indices — a mesh's own
faces will do) and add as many bodies of it as you like. The shape is checked —
flat faces, convex, the centre inside, the tables big enough — and refused with
a reason rather than simulated wrong. Keep them small: a hull pair tests every
face of both and every pair of edge directions.

**Joints.** A ball joint holds two bodies (or a body and the world) together at
a point; a hinge also keeps an axis lined up — a door, a wheel, a flail. Two
joined bodies do not collide; removing a body removes its joints. No limits and
no motor. A joint is held by impulses and then by putting the positions back,
and `vpyp_stats()->joint_stretch` says how far apart the worst one is. `vpy-c/tools/phys_check.c` checks it against formulas (free fall,
braking distance, momentum) and behaviours; its numbers are in the header.

A full table is a limit the game handles: compare `vpyp_stats()->bodies` with
`VPYP_MAX_BODIES` (64) before adding, as `examples/physics_demo` does. A refusal
is counted either way.

### Impact sounds — `vpyimpact.h`

The sound of things hitting each other, synthesised on the PSG from the hit — no
samples. Each impact is a short envelope of noise and a falling tone, played
through libvpy's own SFX player on channel C, so it shares the PSG with music like
any other effect. Add `$(VPY_C_SDK)/vpyimpact.c` to `UVM2_SRCS`.

```c
enum { VPYI_SOFT, VPYI_WOOD, VPYI_METAL, VPYI_NONE = 255 };
void vpyimpact_set_range(int32_t quiet,int32_t loud);   /* impulses: silent below, full from */
int  vpyimpact_hit(int32_t impulse,int material);       /* one hit; 1 if it sounds */
int  vpyimpact_hit_at(int32_t impulse,int material,int32_t x,int32_t y,int32_t z);  /* placed */
void vpyimpact_set_listener(int32_t x,int32_t y,int32_t z,int32_t right_x,int32_t right_z,
                            int32_t near_dist,int32_t far_dist);   /* the camera, usually */
void vpyimpact_set_pcm(vpyimpact_sink sink,int rate,int psg_too);  /* stereo: the UVMC2's jack */
void vpyimpact_pcm(int n);                               /* every frame: n = what the DAC takes */
int  vpyimpact_loop(int slot,int32_t x,int32_t y,int32_t z,int32_t vx,int32_t vy,int32_t vz,
                    int hz,int volume);                  /* an engine: Doppler-shifted */
int  vpyimpact_contacts(const uint8_t *material_of,int floor_material);  /* after vpyp_step */
void vpyimpact_step(void);                               /* once per frame */
const vpyimpact_stats_t *vpyimpact_stats(void);          /* played, skipped, quiet */
```

The volume follows the contact's impulse (square-root shaped, so a middling knock
is still heard); a resting body reports a little every step and stays under
`quiet`. A material per body decides the voice — wood knocks, soft bumps, metal
rings — and when two meet, the one that rings more decides. One channel, so a hit
replaces the one playing only if it is at least as loud as what that one has
left; the rest are counted as skipped. `vpy-c/tools/impact_check.c` checks it
through the real SFX player. The voices are starting values, heard on one
console's speaker (2026-10-02) and not tuned further.

**Where it happened.** With a listener set, a hit is quieter the further away it
is — full up to `near`, nothing from `far` — on every cartridge, and contacts are
placed where they touch. **On a cartridge with a DAC** (the UVMC2's jack: give
`uvm2_jack_write_lr` as the sink, and call `vpyimpact_pcm(uvm2_jack_space())`
every frame) the same voices are also rendered as 16-bit stereo, panned to their
side, up to four at once; and **loops** — continuous sources such as an engine —
whose pitch follows the Doppler shift against the listener. Measured on the host:
a 400 Hz loop approaching at 40 m/s sounds at 452 Hz (theory 453), receding at
358 (358). `physics_demo` uses all of it on a UVMC2 with its jack. Not yet heard
through a jack.

### Effects — `vpyfx.h`

Sparks and debris, one stroke per piece. A **spark** is a point drawn as a
streak along its velocity; a **stick** is a rigid segment that moves and spins.
`vpyfx_shatter()` turns every edge of a mesh into a stick thrown out from the
point of the blow — the classic vector explosion, costing exactly the edges the
object had. `vpyfx_disintegrate()` is the finer version: every edge cut into
short pieces that wait in place until a wave from the hit reaches them, so the
object comes apart from where it was struck; `vpyfx_assemble()` runs it the
other way, pieces flying in and settling exactly on the edges, edge after edge.
Add `$(VPY_C_SDK)/vpyfx.c` to `UVM2_SRCS`.

```c
void vpyfx_reset(void);  void vpyfx_seed(uint32_t s);
void vpyfx_set_gravity(int32_t gx,int32_t gy,int32_t gz);     /* units/s² */
void vpyfx_set_floor(int on,int32_t y,int bounce_q8);
void vpyfx_set_budget(int strokes_per_frame);                 /* default 160 */
int  vpyfx_burst(int32_t x,int32_t y,int32_t z, int32_t vx,int32_t vy,int32_t vz,
                 int count,int32_t speed,int life,int br);
int  vpyfx_shatter(const vpy_mesh *m,const vpy_xf *place, int32_t vx,int32_t vy,int32_t vz,
                   int32_t cx,int32_t cy,int32_t cz, int32_t speed,int32_t spin,int life,int br);
int  vpyfx_disintegrate(const vpy_mesh *m,const vpy_xf *place, int32_t cx,int32_t cy,int32_t cz,
                        int per_edge,int32_t speed,int32_t spin,int32_t wave,int life,int br);
int  vpyfx_assemble(const vpy_mesh *m,const vpy_xf *place, int per_edge,int32_t scatter,
                    int frames,int stagger,int br);           /* returns a group id */
int  vpyfx_assembled(int group);  void vpyfx_release(int group);
int  vpyfx_ring(int32_t cx,int32_t cy,int32_t cz, int32_t nx,int32_t ny,int32_t nz,
                int32_t r0,int32_t speed,int segments,int life,int br);   /* a shockwave to see */
int  vpyfx_line(int32_t ax,int32_t ay,int32_t az, int32_t bx,int32_t by,int32_t bz,
                int life,int br);                             /* stays, fades: a trail */
void vpyfx_step(void);
void vpyfx_draw(int occlude);   void vpyfx_draw2d(void);
const vpyfx_stats_t *vpyfx_stats(void);                       /* alive, drawn, shed, recycled */
```

Effects draw at `VPY_PRI_LOW` within their stroke budget, so they are the first
thing shed when a frame is full — never the scenery — and what is left out is
counted. A disintegration or an assembly costs `per_edge` strokes per edge (a
cube at 8 is 96), so for one that is the point of the scene raise the budget,
or the far side of the object is what gets shed. When `vpyfx_assembled(g)` says
every piece is in place, draw the mesh and `vpyfx_release(g)` in the same frame:
the pieces sit exactly on the edges, so the hand-over does not show. Deterministic, with a seeded generator. Pieces pass through solid
bodies; what shoves the neighbours of an explosion is `vpyp_blast`, and
`vpyfx_ring` is the shockwave you see. `vpy-c/tools/fx_check.c` holds
it to its header. Uses vpy3d (`vpy3d_mesh_edge` reads a mesh's edges).

### Camera and easing — `vpycam.h`, `vpyease.h`

```c
void vpycam_reset(int32_t fx,int32_t fy,int32_t fz);
void vpycam_follow_config(int32_t dead_x,int32_t dead_y,int32_t dead_z,int lead_frames,int smooth);
void vpycam_follow(int32_t tx,int32_t ty,int32_t tz, int32_t vx,int32_t vy,int32_t vz);
void vpycam_shake(int32_t amount,int frames);   void vpycam_hitstop(int frames);
int  vpycam_stopped(void);                      /* skip the simulation while 1 */
void vpycam_step(void);                         /* once a frame */
int  vpycam_look_at(int32_t ox,int32_t oy,int32_t oz);   /* vpy3d camera, shaken */

int32_t vpy_ease_out_cubic(int32_t t_q14);      /* and in/out quad, cubic, smoothstep, back, bounce, elastic */
int32_t vpy_tween(int32_t from,int32_t to,int frame,int frames,vpy_ease_fn curve);
```

The focus follows a target with a dead zone, a lead by its velocity and
smoothing that snaps the last few units rather than creeping (a camera that
moves a unit a frame re-zeroes the beam somewhere new every frame). Every
easing curve is exactly 0 at the start and exactly 1 at the end.
`vpy-c/tools/motion_check.c` holds both to that.

### Ropes and replays — `vpyrope.h`, `vpyreplay.h`

```c
int  vpyrope_new(int32_t ax,int32_t ay,int32_t az, int32_t bx,int32_t by,int32_t bz, int links);
void vpyrope_pin(int rope,int i,int32_t x,int32_t y,int32_t z);   /* every frame, to move it */
void vpyrope_step(void);   void vpyrope_draw(int rope,int br,int occlude);

void vpyreplay_record(vpyreplay_frame *buf,int capacity,uint32_t seed);
void vpyreplay_play(const vpyreplay_frame *buf,int frames,uint32_t seed);
void vpyreplay_input(uint8_t *buttons,int8_t *jx,int8_t *jy);   /* once a frame, before reading input */
```

A rope is Verlet points held to their link length, with pins the game moves; it
draws as one chained polyline, and `vpyrope_stats()->stretch` says how far the
worst link was pulled. A replay is three bytes of input a frame and a seed —
enough, because everything that moves in libvpy is deterministic
(`tools/replay_check.c` plays 500 frames of physics back exactly).

### Soft bodies — `vpysoft.h`

Points joined by springs, that sag, wobble and flap: the same Verlet core as the
ropes, with a stiffness per spring (Q8: 256 back to length every pass, less gives).
A cloth or flag, a blob that keeps its area, or any vpy3d mesh as a jelly. Add
`$(VPY_C_SDK)/vpysoft.c` to `UVM2_SRCS`.

```c
int  vpysoft_cloth(int32_t x,int32_t y,int32_t z,int w,int h,int32_t cell,int stiff_q8);
int  vpysoft_blob(int32_t x,int32_t y,int32_t z,int n,int32_t radius,int stiff_q8,int pressure_q8);
int  vpysoft_mesh(const vpy_mesh *m,int32_t x,int32_t y,int32_t z,int stiff_q8);
void vpysoft_pin(int body,int i,int32_t x,int32_t y,int32_t z);   /* every frame to move it */
void vpysoft_push(int body,int32_t vx,int32_t vy,int32_t vz);     /* a hit, a gust */
void vpysoft_step(void);                                          /* once per frame */
void vpysoft_draw(int body,int br,int occlude);  void vpysoft_draw2d(int body,int br);
const vpysoft_stats_t *vpysoft_stats(void);      /* stretch, area_error_q8, drawn, shed, refused */
```

A blob keeps its area by pushing its rim along the area's gradient each pass:
without that pressure a dropped ring kept 85% of its area, with it 99%. A mesh gets
a hidden centre point tied to every vertex, because edges alone fold flat. Only
structural springs are drawn, one stroke each at `VPY_PRI_LOW` within a budget
(`vpysoft_set_budget`, default 160 a call), and what the budget leaves out is
counted. Bodies meet the floor only, not each other nor vpyphys bodies.
`vpy-c/tools/soft_check.c` checks it.

### Skeletal animation — `vpybone.h`

A tree of rigid bones posed by keyframed clips. Each bone is a joint at an offset
from its parent's, turned by its own rotation (a Q14 quaternion), and carries its
own small mesh: rigid parts, so nothing stretches. Add `$(VPY_C_SDK)/vpybone.c`
and `vpyik.c` to `UVM2_SRCS`.

```c
vpyb_quat vpyb_quat_axis(int32_t ax,int32_t ay,int32_t az,int angle);   /* 4096 per turn */
void vpyb_init(vpyb_skeleton *s);                          /* declare it static: ~2 KB */
int  vpyb_add(vpyb_skeleton *s,int parent,int32_t ox,int32_t oy,int32_t oz,const vpy_mesh *m);
void vpyb_pose(vpyb_skeleton *s,const int32_t pos[3],vpyb_quat rot);  /* forward kinematics */
vpy_xf vpyb_xf(const vpyb_skeleton *s,int bone);           /* a bone's frame: attach things */
int  vpyb_draw(const vpyb_skeleton *s,int br,int what);    /* VPYB_DRAW_MESH | VPYB_DRAW_LINES */
void vpyb_apply(vpyb_skeleton *s,const vpyb_clip *c,int32_t t_q8);    /* time in frames, Q8 */
void vpyb_apply_blend(vpyb_skeleton *s,const vpyb_clip *a,int32_t ta,
                      const vpyb_clip *b,int32_t tb,int w_q8);         /* walk into run */
int  vpyb_ik(vpyb_skeleton *s,int upper,const int32_t target[3],const int32_t pole[3]);
const vpyb_stats_t *vpyb_stats(void);                      /* refused, ik_refused, ik_stretched */
```

A clip is one track of keys (frame, rotation) per bone, sampled with normalised
linear interpolation, looping or held at its ends; two clips blend with a weight.
`vpyb_ik` hands a limb (a bone, its child and grandchild: hip, knee, ankle) to the
two-bone IK so the end lands on a target, then poses again: feet on uneven ground.
Quaternions are Q14, so an IK end lands within ~3 units on bones ~450 long. No
skinning — rigid parts, as the TODO asked first. `vpy-c/tools/bone_check.c` checks
it against trigonometry.

### Entities — `vpyent.h`

The bookkeeping a 3D game with physics writes by hand, done once: an entity is a
transform (a vpyphys body's, or one the game sets), a mesh (shared, or its own copy
once dented), an occluder shape, its shot marks, a vpyimpact material, a kind and a
user pointer. A fixed table (`VPYENT_MAX`, 64), every refusal counted. Add
`$(VPY_C_SDK)/vpyent.c` with vpy3d, vpyphys, vpyimpact, vpyfx and vpycam.

```c
int  vpyent_create(const vpy_mesh *mesh,int body);       /* body VPYP_NONE: set_place */
void vpyent_destroy(int e);                               /* and its body */
void vpyent_set_occluder(int e,int kind,int32_t hx,int32_t hy,int32_t hz);  /* MESH BOX SPHERE NONE */
void vpyent_set_material(int e,int material);             /* VPYI_* */
int  vpyent_dent(int e,int32_t px,int32_t py,int32_t pz,int32_t dx,int32_t dy,int32_t dz,int32_t depth,int32_t r);
void vpyent_mark(int e,int32_t x,int32_t y,int32_t z,int32_t nx,int32_t ny,int32_t nz);
int  vpyent_draw(void);                                   /* near to far, through the occluder */
int  vpyent_draw_shadows(int32_t lx,int32_t ly,int32_t lz,int32_t floor_y,int32_t lift,int br);
int  vpyent_step(int floor_material);                     /* camera, impacts, physics, fx */
const vpyent_stats_t *vpyent_stats(void);
```

**It removes the ordering trap.** The occluder only works drawn near to far, each
solid drawn and then added, marks before the occluder that would cut them —
`vpyent_draw` does that order every frame, whatever order the entities were
created in. A mesh of more than 8 vertices with no occluder shape hides nothing,
and `occl_missing` says so. `vpyent_step` reads the hit-stop before
`vpycam_step` counts it down, so `vpycam_hitstop(n)` holds exactly n frames.
`vpy-c/tools/ent_check.c` checks it.

### Inverse kinematics — `vpyik.h`

```c
int vpyik_two_bone(const int32_t root[3],const int32_t target[3],int32_t l1,int32_t l2,
                   const int32_t pole[3],int32_t joint[3],int32_t end[3]);   /* 1 = reached */
```

Where the elbow (or knee) goes so the hand (or foot) lands on a target,
bending towards a pole point; stretched straight towards it when out of reach.

### Steering and paths — `vpyai.h`

```c
void vpyai_seek(const int32_t pos[3],const int32_t target[3],int32_t speed,int32_t out[3]);
void vpyai_arrive(const int32_t pos[3],const int32_t target[3],int32_t speed,int32_t slow_radius,int32_t out[3]);
void vpyai_separate(const int32_t pos[3],const int32_t (*others)[3],int n,int32_t radius,int32_t strength,int32_t out[3]);
void vpyai_steer(int32_t v[3],const int32_t desired[3],int32_t max_change);   /* a turn rate */
int  vpyai_path(const uint8_t *grid,int w,int h,int sx,int sy,int gx,int gy,
                int diagonal,int16_t *out_xy,int max_cells);  /* A*: 0 free, 255 wall, else dearer */
```

A* returns the cheapest path — `tools/ai_check.c` compares it with Dijkstra on
196 random grids — with the same answer every time and fixed memory.

### Compiled assets

```c
void vpy_draw_vector(const unsigned char *data,int x,int y);
void vpy_draw_vector_ex(const unsigned char *data,int x,int y,int mirror,int intensity);
void vpy_draw_anim(const unsigned char *anim,
                   const unsigned char *const *sprites,int x,int y,int mirror);
```

`data` is a position-independent path stream (a compiled `.vec`); `anim` is a
frame descriptor plus a companion sprite pointer table. Bézier segments are
tessellated to lines. `intensity <= 0` keeps each path's own intensity.

> The asset compiler that produces these (`vpy_cli compile-asset`) is **not** in
> this kit. The runtime readers are, and the formats are documented in
> `vpy.c`'s comments, so a hand-written generator is a small job.

### Text

```c
void vpy_set_text_size(int s);
void vpy_print_text(int x,int y,const char *s);
void vpy_print_number(int x,int y,long n);
```

### Input

```c
int  vpy_j1_x(void);            /* -127..127 */
int  vpy_j1_y(void);            /* -127..127, + = up */
int  vpy_j1_button(int n);      /* n = 1..4 -> 0/1 */
void vpy_update_buttons(void);  /* force a re-read mid-frame */
```

These read the SDK globals directly rather than a snapshot, which is what makes
them bit-identical to the VPy code generator's inline versions.

### Math

```c
int vpy_abs, vpy_min, vpy_max, vpy_clamp;
int vpy_sin(int a), vpy_cos(int a);   /* a 0..127 = full circle -> -127..127 */
int vpy_sqrt(int v);                  /* integer Newton */
int vpy_atan2(int y,int x);           /* -> 0..127 */
int vpy_rand(void), vpy_rand_range(int lo,int hi);
void vpy_seed(unsigned s);
```

### Sound

```c
void vpy_beep(int on);
void vpy_tone(int period,int volume);            /* channel A; period 12-bit, vol 0..15 */
void vpy_play_music(const unsigned char *vmus);  /* a compiled PSG event stream */
void vpy_music_update(void);                     /* auto-called by vpy_run */
void vpy_stop_music(void);
void vpy_play_sfx(const unsigned char *vsfx);    /* one-shot, channel C */
void vpy_sfx_update(void);
```

### Levels and enemies (optional)

A tile-free scrolling-level runtime: `vpy_load_level`, `vpy_show_level`,
`vpy_update_level`, camera accessors, `vpy_level_collision_x/y`, and an enemy
pool with patrol/area/wander AI (`vpy_spawn_enemies`, `vpy_update_enemies`,
`vpy_draw_enemies`, `vpy_kill_enemy`, plus per-enemy accessors). It consumes
compiled `.vplay` images. Unused code is dropped by `--gc-sections`, so ignoring
this half costs nothing.

---

## Level 2 — the PiTrex/host contract

```c
#include <vectrex/vectrexInterface.h>

void     vectrexinit(int mode);
void     v_init(void);
void     v_setRefresh(int hz);
void     v_WaitRecal(void);       /* seal the frame, hand it over, open the next */

void     v_directDraw32(int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint8_t brightness);
void     v_setColour(uint32_t rgb);   /* 0x00RRGGBB; 0 = the display's default */

uint8_t  v_readButtons(void);         /* -> currentButtonState */
void     v_readJoystick1Analog(void); /* -> currentJoy1X / currentJoy1Y */
uint32_t v_millis(void);

void     v_writePSG(uint8_t reg,uint8_t val);
void     v_setSoundAY(uint8_t reg,uint8_t val);   /* the same, legacy name */
void     v_playSample(int idx,int voice,int loop);
void     v_stopSample(int voice);
int      v_samplePlaying(int voice);

extern uint8_t currentButtonState;    /* bit n-1 = button n */
extern int8_t  currentJoy1X, currentJoy1Y;
```

`v_directDraw32` with brightness 0 returns immediately — it is **not** a blanked
move. To reposition without drawing, just draw the next stroke from where you
want; the SDK inserts the jump.

### Cartridge extensions (`sdk/rp2350-sdk/sdk_rp2350.c`)

```c
void v_beamNewStroke(void);     /* force a re-zero: a fresh reference per figure */
void v_setIntensity(int b);
void v_directGapped(int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint8_t b,
                    const unsigned char *gaps,int n);
int  v_directSweepSR(int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint8_t b,
                     const unsigned char *pattern,int n,int step);
void v_rasterText(int x,int y,const unsigned char *s,int n);
void v_textBegin(void); void v_textEnd(void);
const void *v_sampleData(int idx);   /* WEAK: the game defines it (see ts_audio.c) */
```

`v_beamNewStroke()` matters more than its size suggests. A figure drawn as loose
strokes accumulates position error, and the symptom is degradation running
*rightwards* along a line of text — the first letters clean, the last ones ragged
and clipped. One re-zero per glyph costs ~211 cycles and fixes it. It is a no-op
on targets with no integrators, so calling it everywhere is safe.

`v_directGapped` draws **one ramp with holes in it**: `gaps` is (start, end)
pairs as fractions 0..255 of the stroke, and the SDK toggles BLANK along the way
instead of programming a ramp per lit run. That is what you need to sweep a row
of pixels; `v_rasterText` on the other hand ends in the SDK's own vector font at
a fixed scale, so text the game scales itself comes out misaligned.

---

## Level 3 — the SDK

### Drawing — `uvm2_draw.h`

```c
void uvm2_draw_init(void);
void uvm2_frame_begin(void);
void uvm2_frame_end(void);

void uvm2_draw_intensity(int brightness);      /* 0..127 */
int  uvm2_draw_intensity_current(void);
void uvm2_draw_penup(void);

void uvm2_draw_move(int dx,int dy);            /* relative jump, device units */
void uvm2_draw_delta(int dx,int dy);           /* relative lit stroke */
void uvm2_draw_move_abs(int x,int y);          /* absolute jump */
void uvm2_draw_move_q4(int dx_q4,int dy_q4);   /* the same, in 1/16 */
void uvm2_draw_move_abs_q4(int x_q4,int y_q4);
void uvm2_draw_delta_q4(int dx_q4,int dy_q4);

void uvm2_draw_reset(void);        /* re-zero the beam now */
void uvm2_draw_invalidate(void);   /* forget the cached VIA state */
void uvm2_draw_prime_holds(void);
void uvm2_draw_rotate(int enable); /* 90 degrees, for horizontal arcade games */
int  uvm2_draw_rotated(void);

void uvm2_draw_delta_patterned(int dx,int dy,const unsigned char *gaps,int n);
int  uvm2_draw_sweep_sr(int dx,int dy,const unsigned char *pattern,int n,int step);

void     uvm2_set_refresh(unsigned hz);   /* 50, 60, or 0 = free-running */
unsigned uvm2_current_refresh(void);
int      uvm2_refresh_fits(void);         /* did the last frame fit the cap? */

uint32_t uvm2_list_cycles(void);          /* bus cycles in the list so far */
uint32_t uvm2_list_commands(void);
uint32_t uvm2_frame_count(void);
uint32_t uvm2_frame_bus_cycles(void);
void     uvm2_emit_raw(uint32_t reg,uint32_t data,uint32_t gap);   /* one raw command */

/* the diagnostics HUD (uvm2_hud.c): buttons 1+4 held 2 s, or poke uvm2_hud */
extern volatile uint8_t uvm2_hud;              /* 0 off, 1 on */
extern uvm2_hud_stats_t uvm2_hud_stats;        /* cmds, cycles, vectors it added; drawn, skipped, toggles */
extern char uvm2_hud_text[2][32];              /* the two lines last drawn, as text */
uint32_t uvm2_list_room(void);                 /* commands the game may still add this frame */

/* the last closed frame's list, to the SD card (uvm2_dump.c) — core 0 only */
int  uvm2_dump_list(const char *path);                       /* 1 if written */
int  uvm2_dump_list_on_buttons(const char *path,uint8_t mask);   /* once per press */
extern uvm2_dump_diag_t uvm2_dump_diag;                      /* ok, failed, error, commands */
```

The dump is the list the console was given, for `tools/list_from_sd.py` and
`beam_sim.py` (see `07-measuring.md`). The screen goes dark while the card
writes, so it is a capture, not a per-frame call; a refusal is counted with its
reason (`UVM2_DUMP_CORE1`, `_EMPTY`, `_SD`, and then `uvm2_sd_error`).

Runtime knobs (all `volatile`, all with a long comment at their definition):
`uvm2_zero_jump`, `uvm2_zero_every`, `uvm2_zero_offset`, `uvm2_pacer_cycles`,
`uvm2_filler_clamp`, `uvm2_hold_y_min/max`, `uvm2_hold_z_min/max`, `uvm2_sweep_t1_max`.

### The bus — `uvm2_bus.h`

```c
void     uvm2_cpu_init(void);   /* .bss, vector table, firmware IRQs off — FIRST C run */
void     uvm2_bus_pads(void);   /* pads + function select; CLK becomes readable */
void     uvm2_bus_halt(void);   /* park, enable drivers, assert /HALT for good */
void     uvm2_bus_init(void);   /* all three */

uint32_t uvm2_exec(const uint8_t *cmds,uint32_t count);  /* replay; -> bus cycles */
void     uvm2_bus_delay(uint32_t cycles);
void     uvm2_via_write(uint32_t reg,uint32_t data);
uint8_t  uvm2_via_read(uint32_t reg);
void     uvm2_measure_e(void);
extern uint32_t uvm2_cycles_per_e_q8;   /* CPU cycles per E period, Q8 */
uint32_t uvm2_now_us(void);             /* TIMER0 microseconds; 0 on a host harness */

extern uvm2_stats_t uvm2_stats;
```

Command encoding and VIA register/bit names also live here:
`UVM2_CMD(reg,data,delay)`, `UVM2_CMD_PACK/REG_DATA/DELAY`, `UVM2_VIA_PORTA…IER`,
`UVM2_PB_RAMP_OFF`, `UVM2_PB_MUX_*`, `UVM2_MUX_Y/ZEROREF/Z`, `UVM2_PCR_*`.

`uvm2_stats_t` is listed field by field in [07 — Measuring](07-measuring.md).

### Input — `uvm2_input.h`

```c
uint8_t  uvm2_read_buttons(void);      /* RAW: active-low, J1 in bits 0-3, J2 in 4-7 */
uint32_t uvm2_read_axes(void);         /* four packed int8: j1x, j1y, j2x, j2y */
void     uvm2_input_set_analog(int enable);   /* 0 = -127/0/+127 (default), 1 = centred analog */
void     uvm2_psg_write(uint32_t reg,uint32_t value);
uint8_t  uvm2_psg_read(uint32_t reg);
```

The console's reset button (06, "The console's reset button"): under 1 s nothing,
1–3 s and let go restarts the game, 3 s returns to the UVMC2's menu.

```c
void uvm2_mem_write(uint32_t addr,uint32_t data);   /* any bus address; core 1, between frames */
extern volatile uint32_t uvm2_reset_seen;           /* presses of the reset button noticed */
extern volatile uint32_t uvm2_reset_held_us;        /* how long the current one has lasted */
extern volatile uint32_t uvm2_restart_refused;      /* restarts refused: .data over 4 KB */
extern volatile uint32_t uvm2_reset_held_max_us;    /* the longest "press" seen */
extern volatile uint8_t  uvm2_reset_last_t1;        /* T1 high byte the two reads agreed on (0x00: VIA in reset) */
extern volatile uint32_t uvm2_reset_polls;          /* polls made: proof the watch runs */
```

**Open (2026-10-04):** heavy games (dkong, `examples/playroom`) reset themselves on
the UVMC2 with nobody near the button — the watch sees T1 standing still where it is
not. Until that is found, such a game builds with `-DUVM2_NO_RESET_BUTTON`. Building
with `-DUVM2_RESET_WATCH_ONLY` keeps the watch and its counters but never acts on it:
the build to read the counters above with the game running.

### Sound — `uvm2_audio.h`, `uvm2_smp.h`

```c
void uvm2_play_music(const uint8_t *vmus);
void uvm2_stop_music(void);
void uvm2_play_sfx(const uint8_t *vsfx);
void uvm2_audio_tick(void);            /* one sequencer step; core 1 calls it */

void        uvm2_smp_play(const void *vsmp,unsigned voice,int loop);
void        uvm2_smp_stop(unsigned voice);          /* >= UVM2_SMP_VOICES = all */
int         uvm2_smp_playing(unsigned voice);
unsigned    uvm2_smp_pos(unsigned voice,unsigned fps);
int         uvm2_smp_bundle_load(const char *path); /* read a .vsm off the SD */
int         uvm2_smp_bundle_set(const void *base,uint32_t bytes);
const void *uvm2_smp_bundle_entry(unsigned idx);
uint32_t    uvm2_smp_bundle_count(void);
```

`UVM2_SMP_VOICES` is 10, `UVM2_SMP_REG` is 10 (channel C's volume), `UVM2_SMP_HZ`
is 16000 — the rate the *emitter* aims at, not the file's; asking for more than
the source has does not invent detail, it makes each value land closer to when it
should. It costs about 6% more bus cycles.

The rest of the header (`uvm2_smp_frame/due/needs_latch/mixer/note_mixer`) is the
injector's own interface, called from inside the draw path.

The UVMC2 also has a **16-bit audio jack** (a PT8211 on GPIO43-45), independent
of the Vectrex's sound chip. `uvm2_jack.h`:

```c
#define UVM2_JACK_RATE 32000
int  uvm2_jack_init(void);                      /* 1 = running, 0 = no jack on this board */
int  uvm2_jack_space(void);                     /* samples to write now to stay ahead */
void uvm2_jack_write(const int16_t *s,int n);   /* mono, UVM2_JACK_RATE */
void uvm2_jack_write_lr(const int16_t *l,const int16_t *r,int n);   /* stereo, same ring */
```

The DAC is stereo: one 32-bit word of the ring is a frame, left in the high half.
`uvm2_jack_write` puts the same sample in both halves; a game with two sources to
separate (Star Wars: the POKEYs and a voice) calls `uvm2_jack_write_lr`. Same
ring, same DMA, same `uvm2_jack_space()`.

It runs on the system clock measured against the Vectrex's E at boot. The per-game
`audio` setting (`UVM2_SETTING_AUDIO`, 0 = jack, 1 = console chip) is stored for
the game to read; the SDK does not route sound itself.

### Text, LED, clock — `uvm2_text.h`, `uvm2_led.h`

```c
void uvm2_print_text(int x,int y,const char *str,int scale,int intensity);
void uvm2_print_text_chained(int x,int y,const char *str,int scale,int intensity); /* no per-glyph re-zero */

void uvm2_led_init(void);
void uvm2_led_rgb(uint8_t r,uint8_t g,uint8_t b);
void uvm2_led_status(uvm2_status_t code);
/*   OFF, BOOT (blue), NO_CLOCK (red), HALTING (amber), RUNNING (green),
     OVERRUN (orange) */
uint32_t uvm2_clock_calibrate(void);   /* cycles per us, or 0 = no CLK edge arrived */
uint32_t uvm2_cycles_per_us(void);
```

`uvm2_clock_calibrate()` returning 0 is itself the diagnosis: the console is off
or the cartridge is not seated.

### Storage — `uvm2_sd.h`, `uvm2_psram.h`

```c
int      uvm2_sd_init(void);
uint32_t uvm2_sd_read(const char *path,unsigned char *dst,uint32_t max);
uint32_t uvm2_sd_read_from(const char *path,unsigned char *dst,uint32_t max,uint32_t from);
int      uvm2_sd_create(const char *path,const unsigned char *data,uint32_t n);   /* 512-byte text file */
int      uvm2_sd_overwrite(const char *path,const unsigned char *data,uint32_t n);/* 1 sector, in place */
int      uvm2_sd_write(const char *path,const unsigned char *data,uint32_t n);    /* any size */
int      uvm2_sd_open(const char *path,uvm2_sd_file *f);                         /* stream a large file */
uint32_t uvm2_sd_next(uvm2_sd_file *f,unsigned char *dst,uint32_t max);          /* 0 = end */
void     uvm2_sd_close(uvm2_sd_file *f);                                         /* optional */
extern int uvm2_sd_error;   /* OK / NO_CARD / NO_INIT / NO_FAT / MISSING / TOO_BIG / IO_ERROR */
extern struct uvm2_sd_diag uvm2_sd_diag;   /* what the mount understood about the disk */

int  uvm2_psram_init(void);        /* reset, check ID, arm the XIP window -> 1 if present */
void uvm2_psram_enable_xip(void);
int  uvm2_psram_probe(void);       /* diagnostic only; may leave the chip in QPI */
#define UVM2_PSRAM_NO_CACHE 0x15000000u   /* verify through THIS, not 0x11000000 */
```

FAT12/16/32 or exFAT (FatFs underneath). Paths are relative to the root, any
depth, long names allowed, matched case-insensitively. `uvm2_sd_read` treats "does not fit" as a failure; `uvm2_sd_read_from`
does not.

### Calibration — `uvm2_config.h`

Per-**console** beam calibration, read from and written to the SD card:

```c
struct uvm2_config {
    int32_t scale;        /* DRAW_SCALE: larger = shorter strokes */
    int32_t t1_tail_q8;   /* the ramp's real length, in 1/256 of a count */
    int32_t zero;         /* the value primed into the zero reference */
    int32_t bright;       /* default Z, 0..127 */
    int32_t hold_y_min, hold_y_max;   /* Y sample-and-hold, in E cycles */
    int32_t neg_rate_x, neg_rate_y;   /* negative-rate DAC trim, 1/256 */
    int32_t drift_x, drift_y;         /* per-jump drift, 1/256 */
    int32_t hz;           /* 50, 60, or 0 = free */
    int32_t start_menu;   /* 1 = menu on power-up */
    int32_t rotate;       /* 1 = drawing rotated 90 degrees */
    int32_t audio;        /* 0 = the jack, 1 = the console's chip (game setting) */
    int32_t aspect_q8;    /* the screen's shape: x against y, 256 = 1:1 */
    int32_t win_x, win_y; /* what the tube shows, half extents in deflection units */
};
int  uvm2_config_load(void);
int  uvm2_config_save(void);
void uvm2_config_current(struct uvm2_config *c);
void uvm2_config_apply(const struct uvm2_config *c);
int  uvm2_config_wizard(void);
void uvm2_config_boot_combo(void);          /* runtime: buttons 2+3 held at launch -> wizard */
extern volatile int32_t uvm2_boot_combo;    /* -1 unchecked, 0 not held, 1 ran, 2 could not check */
void uvm2_config_game(const char *name,unsigned settings);
extern volatile int uvm2_have_calibration;
```

These belong to the machine the cartridge is plugged into, not to the game. A
game normally only calls `uvm2_config_game()` to declare which settings its menu
should offer.

**To calibrate a console**, hold buttons 2 and 3 and launch the game with 4: the
wizard opens before the game. Button 4 saves to `config/uvm2.cfg`. What each
screen shows and which field fixes what is in
[12 — Calibrating a console](12-calibrating-a-console.md).

#### `uvm2_config_game(name, settings)`

Two files, split by **whose** settings they are:

| file | holds | written by |
|---|---|---|
| `config/uvm2.cfg` | the beam calibration (`scale` … `drift_y`): the console's, shared by every game | the calibration screen, when you save |
| `config/<NAME>.cfg` | the game's own settings, layered on top of the console's | only the settings the game declared |

`name` is the game's 8.3 base name, no extension (`"TACSCAN"`). `settings` is an
OR of:

| bit | value | the wizard offers |
|---|---|---|
| `UVM2_SETTING_HZ` | 1 | refresh 50 / 60 / 0 (free) → `hz` |
| `UVM2_SETTING_MENU` | 2 | menu on power-up; with it off, button 4 still forces the menu → `start_menu` |
| `UVM2_SETTING_ROTATE` | 4 | drawing rotated 90° for a horizontal arcade game → `rotate` |
| `UVM2_SETTING_AUDIO` | 8 | sound out of the jack or the console's chip → `audio` |

The runtime has already loaded the console's file before your `main` runs, so call
`uvm2_config_load()` again after declaring, and the game's file is layered on top.
Declare only what means something in your game: a vertical game should not offer
`ROTATE`. Not calling it at all gives one file, and no game settings in the
wizard. The buttons-2+3 wizard opens before `main`, so it shows only the
console's fields; a game's own menu that calls `uvm2_config_wizard()` shows both.

```c
uvm2_config_game("MYGAME", UVM2_SETTING_HZ | UVM2_SETTING_MENU);
uvm2_config_load();
```

### The PIO stream — `uvm2_bus_stream.h`

You should not need this; `uvm2_draw.c` drives it. It is documented because
reading it is how the timing makes sense.

```c
void     vbus_install(uint32_t out_base,uint32_t out_count,uint32_t out_dirs,uint32_t park);
uint32_t vbus_word(uint32_t bus_word);   /* a write, with its sentinel */
uint32_t vbus_repeat(uint32_t n);        /* park for n periods, in ONE word */
uint32_t vbus_silence(void);             /* one period of silence */
void     vbus_push(uint32_t word);
void     vbus_flush(void); void vbus_drain(void);
void     vbus_list_begin/end/wait/fire(void);   /* a frame as a single DMA */
uint32_t vbus_stat(uint32_t idx);
void     uvm2_stream_start(void);
```

---

## The syscall layer

Between levels 2 and 3 sits a Thumb `svc` ABI (`uvm2_svc.c`), so the *same* game
binary can run against an SDK compiled into its own image or one living in
another cartridge's BIOS. Arguments in r0-r3, AAPCS.

| # | name | # | name |
|---|---|---|---|
| 0 | `RESET0REF` | 12 | `PSG_READ` |
| 1 | `WAIT_RECAL` | 13 | `READ_AXES` |
| 2 | `SET_INTENSITY` | 14 | `READ_BTN_RAW` |
| 3 | `MOVE` | 15 | `MOVE_ABS` |
| 4 | `DRAW_DELTA` | 16 | `PRINT_TEXT` |
| 5 | `PSG_WRITE` | 21 | `PLAY_MUSIC` |
| 6 | `PSG_SILENCE` | 22 | `STOP_MUSIC` |
| 7 | `READ_BUTTONS` | 23 | `PLAY_SFX` |
| 8 | `BUS_WRITE` | 26 | `RASTER_TEXT` |
| 10 | `SAMPLE_POS` | 27 | `DRAW_GAPPED` |
| 11 | `BUS_READ` | 28 | `MOVE_Q4` |
| | | 29 | `DRAW_DELTA_Q4` |
| | | 30 | `CALIBRATE` |

The gaps (9, 17-20, 24, 25) are unassigned here; the debug cartridge's BIOS uses
some of them for its own. `CALIBRATE` (30) runs the calibration screen and returns
1 if it saved — VPy's `CALIBRATE()`; for a day (2026-10-02) the BIOS gave it 26,
which is `RASTER_TEXT` here. `23` and `26` are worth not
confusing: raster text once went out as `23`, which is `PLAY_SFX` in the other
cartridge's BIOS; the SFX player took the text pointer for a track and hung the
core (see the note in `sdk_rp2350.c`).

Drawing syscalls only **record**; `WAIT_RECAL` is what makes a frame happen.
Input is sampled once per frame inside `WAIT_RECAL` and cached, so reading it
costs nothing and never fights the beam. `BUS_READ`/`BUS_WRITE` are refused under
dual core — another core owns the pins.

On the `.um2` path the drawing does not go through `svc` at all: the SDK is in
the image, so `sdk_rp2350.c` calls `uvm2_draw_*` directly. What remains on `svc`
is sound, music, samples and raster text.
