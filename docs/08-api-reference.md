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
```

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

### Text, LED, clock — `uvm2_text.h`, `uvm2_led.h`

```c
void uvm2_print_text(int x,int y,const char *str,int scale,int intensity);

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
int      uvm2_sd_create(const char *path,const unsigned char *data,uint32_t n);   /* 1 cluster */
int      uvm2_sd_overwrite(const char *path,const unsigned char *data,uint32_t n);/* 1 sector, in place */
int      uvm2_sd_write(const char *path,const unsigned char *data,uint32_t n);    /* any size */
extern int uvm2_sd_error;   /* OK / NO_CARD / NO_INIT / NO_FAT / MISSING / TOO_BIG */
extern struct uvm2_sd_diag uvm2_sd_diag;   /* what the mount understood about the disk */

int  uvm2_psram_init(void);        /* reset, check ID, arm the XIP window -> 1 if present */
void uvm2_psram_enable_xip(void);
int  uvm2_psram_probe(void);       /* diagnostic only; may leave the chip in QPI */
#define UVM2_PSRAM_NO_CACHE 0x15000000u   /* verify through THIS, not 0x11000000 */
```

Paths take one subdirectory, 8.3 names only, matched case-insensitively on the
base name. `uvm2_sd_read` treats "does not fit" as a failure; `uvm2_sd_read_from`
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
};
int  uvm2_config_load(void);
int  uvm2_config_save(void);
void uvm2_config_current(struct uvm2_config *c);
void uvm2_config_apply(const struct uvm2_config *c);
int  uvm2_config_wizard(void);
void uvm2_config_game(const char *name,unsigned settings);
extern volatile int uvm2_have_calibration;
```

These belong to the machine the cartridge is plugged into, not to the game. A
game normally only calls `uvm2_config_game()` to declare which settings its menu
should offer.

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
| 0 | `RESET0REF` | 14 | `READ_BTN_RAW` |
| 1 | `WAIT_RECAL` | 15 | `MOVE_ABS` |
| 2 | `SET_INTENSITY` | 16 | `PRINT_TEXT` |
| 3 | `MOVE` | 21 | `PLAY_MUSIC` |
| 4 | `DRAW_DELTA` | 22 | `STOP_MUSIC` |
| 5 | `PSG_WRITE` | 23 | `PLAY_SFX` |
| 6 | `PSG_SILENCE` | 26 | `RASTER_TEXT` |
| 7 | `READ_BUTTONS` | 27 | `DRAW_GAPPED` |
| 8 | `BUS_WRITE` | 28 | `MOVE_Q4` |
| 10 | `SAMPLE_POS` | 29 | `DRAW_DELTA_Q4` |
| 11 | `BUS_READ` | 12 | `PSG_READ` |
| 13 | `READ_AXES` | | |

Drawing syscalls only **record**; `WAIT_RECAL` is what makes a frame happen.
Input is sampled once per frame inside `WAIT_RECAL` and cached, so reading it
costs nothing and never fights the beam. `BUS_READ`/`BUS_WRITE` are refused under
dual core — another core owns the pins.

On the `.um2` path the drawing does not go through `svc` at all: the SDK is in
the image, so `sdk_rp2350.c` calls `uvm2_draw_*` directly. What remains on `svc`
is sound, music, samples and raster text.
