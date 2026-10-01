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
const vpyp_stats_t *vpyp_stats(void);                        /* awake, contacts, refused */
```

Spheres and boxes, a floor, restitution, friction, sleeping bodies (a pile at
rest costs almost nothing) and a contact list with the impulse of each hit —
what a dent, a spark or an impact sound reads. **Bodies turn:** a hit
off-centre spins them, boxes tip over and tumble, balls roll;
`vpyp_rotation()` gives the turn as a Q14 matrix to draw with. Inertia is
a scalar, exact for spheres and cubes; box against box tests the six face axes
but not edge against edge. The solver is sequential impulses with warm
starting and speculative contacts, so a stack holds and a fast body does not
tunnel through a thin wall. ~19 KB of code and ~80 KB of RAM, almost all of it
the contact table. `vpy-c/tools/phys_check.c` checks it against formulas (free fall,
braking distance, momentum) and behaviours; its numbers are in the header.

A full table is a limit the game handles: compare `vpyp_stats()->bodies` with
`VPYP_MAX_BODIES` (64) before adding, as `examples/physics_demo` does. A refusal
is counted either way.

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
```

Runtime knobs (all `volatile`, all with a long comment at their definition):
`uvm2_zero_jump`, `uvm2_zero_every`, `uvm2_zero_offset`, `uvm2_pacer_cycles`,
`uvm2_hold_y_min/max`, `uvm2_hold_z_min/max`, `uvm2_sweep_t1_max`.

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
void     uvm2_input_set_analog(int enable);   /* 0 = digital (default), 1 = SAR */
void     uvm2_psg_write(uint32_t reg,uint32_t value);
uint8_t  uvm2_psg_read(uint32_t reg);
```

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

The gaps (9, 17-20, 24, 25) are unassigned. `23` and `26` are worth not
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
