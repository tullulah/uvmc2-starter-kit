/* sdk_rp2350.c — the RP2350 backend of the libvpy contract.
 *
 * libvpy (vpy.c) and the SBT/AAE ports draw EVERYTHING through v_directDraw32, make sound
 * through v_writePSG, read controllers through v_readButtons / v_readJoystick1Analog and mark
 * the frame with v_WaitRecal. This file is that contract on the two RP2350 boards:
 *
 *   - the UVM2 (.um2, UVM2_PICO_RUNTIME): the SDK (uvm2_draw.c) lives INSIDE the image and is
 *     called directly;
 *   - the Vectrex Studio cartridge (VPY_DUAL_CORE): the SDK lives in the BIOS and the game
 *     calls it through the function table the BIOS publishes (`struct uvm2_api`).
 *
 * ONE GEOMETRY PATH, IN SUBUNITS. Every v_directDraw32 is three calls into the SDK with the
 * coordinates in 1/16 of a unit: intensity, absolute jump, stroke. Everything that used to
 * live here — collinear merging, Douglas-Peucker, reordering strokes by nearest neighbour, an
 * intensity cache, a re-zero budget, clipping short strokes, a ramp with gaps — worked in
 * integers over ANOTHER encoding, and every one of those pieces was measured at some point as
 * a shimmer, a displaced stroke or missing geometry. What was validated as optimal is the
 * list in subunits exactly as uvm2_draw.c generates it, and that is the only path: no knobs.
 * What the game asks for is what gets drawn.
 */
#include <stdint.h>

/* THERE ARE ONLY TWO WAYS TO REACH THE SDK, and a build that does not say which one does not
 * compile: on the .um2 the runtime (UVM2_PICO_RUNTIME) and on our own cartridge the table
 * (VPY_DUAL_CORE). There used to be a third — draw through svc, one call at a time, in
 * integers — and it was what any old rule produced by default; with it the game never went
 * through subunits and had no configuration menu, and nobody noticed until the console. */
#if !defined(VPY_DUAL_CORE) && !defined(UVM2_PICO_RUNTIME)
#error "sdk_rp2350.c: needs -DVPY_DUAL_CORE (Vectrex Studio cartridge, through the BIOS table) or UVM2_PICO_RUNTIME (.um2). Use the rp2350-cart recipe in uvm2.mk."
#endif

/* ── The BIOS svc calls that remain (Thumb `svc #imm`; r0-r3, AAPCS). On the cartridge the
 * drawing does NOT go through svc (it goes through the table); these are sound, music and
 * raster text for the .um2, whose bridge is uvm2_svc.c. ── */
static inline void sys_reset0ref(void)       { __asm__ volatile("svc #0"  ::: "r0","r1","r2","r3","memory"); }
static inline void sys_wait_recal(void)      { __asm__ volatile("svc #1"  ::: "r0","r1","r2","r3","memory"); }
static inline void sys_set_intensity(int b)  { register int r0 __asm__("r0")=b; __asm__ volatile("svc #2" : "+r"(r0) :: "memory"); }
static inline void sys_psg_write(int reg,int val){ register int r0 __asm__("r0")=reg; register int r1 __asm__("r1")=val; __asm__ volatile("svc #5" : "+r"(r0) : "r"(r1) : "memory"); }
static inline int  sys_read_buttons(void)    { register int r0 __asm__("r0"); __asm__ volatile("svc #7"  : "=r"(r0) :: "memory"); return r0; }
static inline int  sys_read_axes(void)       { register int r0 __asm__("r0"); __asm__ volatile("svc #13" : "=r"(r0) :: "memory"); return r0; }
/* SYS_SAMPLE_POS (10): how far voice 0's cursor has got, expressed as a frame number
 * at the `fps` asked for. Asking with fps = the sample's own rate gives the cursor in
 * SAMPLES, which is what a game streaming into a ring buffer needs to know. It goes by
 * svc even on the cartridge -- like sound, music and raster text -- because only the
 * DRAW path uses the BIOS table. */
static inline int sys_sample_pos(int fps){ register int r0 __asm__("r0")=fps; __asm__ volatile("svc #10" : "+r"(r0) :: "memory"); return r0; }
static inline void sys_play_music(const void *p){ register const void *r0 __asm__("r0")=p; __asm__ volatile("svc #21" : "+r"(r0) :: "memory"); }
static inline void sys_stop_music(void)      { __asm__ volatile("svc #22" ::: "r0","r1","r2","r3","memory"); }
static inline void sys_play_sfx(const void *p){ register const void *r0 __asm__("r0")=p; __asm__ volatile("svc #23" : "+r"(r0) :: "memory"); }
/* SYS_RASTER_TEXT = 26 (23 is SYS_PLAY_SFX in the BIOS: with that number a pointer to text
 * arrived at the SFX player as a track and hung the core). */
static inline void sys_raster_text(int x,int y,const unsigned char*s,int n){ register int r0 __asm__("r0")=x; register int r1 __asm__("r1")=y; register const unsigned char* r2 __asm__("r2")=s; register int r3 __asm__("r3")=n; __asm__ volatile("svc #26" :: "r"(r0),"r"(r1),"r"(r2),"r"(r3) : "memory"); }

/* ── The input, exactly as libvpy and the ports read it (externs). ── */
uint8_t currentButtonState = 0;
int8_t  currentJoy1X = 0;
int8_t  currentJoy1Y = 0;

/* libvpy scales VPy's logical units (screen ±127) by VPY_SCALE; the SDK wants screen units
 * in 1/16. The multiplication was exact, so what the integer used to lose is preserved here:
 * a stroke of 2.5 units is still 2.5. */
#define VPY_SCALE 127
#define VS_Q4(v) ((int)(((v) < 0 ? (long)(v) * 16 - VPY_SCALE / 2 \
                                 : (long)(v) * 16 + VPY_SCALE / 2) / VPY_SCALE))

/* ── Lifecycle (the BIOS or the runtime has already set up clocks, pins and VIA). ── */
void vectrexinit(int mode) { (void)mode; }
#if defined(VPY_DUAL_CORE) && !defined(UVM2_PICO_RUNTIME)
static void dc_cfg_init(void);          /* below, with the table-based configuration API */
void v_init(void)          { dc_cfg_init(); }
#else
void v_init(void)          {}
#endif
void v_setRefresh(int hz)  { (void)hz; }   /* uvm2_set_refresh sets the pace */

#ifdef VPY_DUAL_CORE
/* THE UVM2 MODEL ON THE VECTREX STUDIO CARTRIDGE. The game runs on core 1 and builds the
 * list by calling the SDK that lives in the BIOS — the SAME functions and the same 32-bit
 * integers uvm2_draw.c receives on the .um2 — through a function table the BIOS publishes at a
 * fixed address (uvm2c.rs, `Uvm2Api`). Core 0 is the executor (uvm2_core1.c): it replays the
 * lists over PIO+DMA, reads the controllers and drains the PSG between lists. No svc per
 * vector and no op ring: what used to be here (DC_OP_*, deltas in i8 and later in packed
 * quarters) was a second encoding and its limits showed (side-to-side girders inverted). The
 * field order is the contract with the BIOS; things are only ever appended at the end. */
#include "uvm2_config.h"
struct uvm2_api {
    unsigned magic, version;
    void (*draw_intensity)(int);
    void (*draw_reset)(void);
    void (*draw_move)(int, int);
    void (*draw_delta)(int, int);
    void (*draw_move_abs_q4)(int, int);
    void (*draw_delta_q4)(int, int);
    void (*draw_delta_patterned)(int, int, const unsigned char *, int);
    void (*print_text)(int, int, const char *, int, int);
    void (*wait_recal)(void);
    unsigned (*read_buttons)(void);
    unsigned (*read_axes)(void);
    void (*psg_queue)(unsigned, unsigned);
    void (*config_current)(int32_t *);
    void (*config_apply)(const int32_t *);
    int  (*config_save)(void);
    void (*set_refresh)(unsigned);
    /* version 2: .vsmp samples (uvm2_smp.c, injected into the list). The order is
     * the contract with uvm2c.rs and only ever grows at the end; callers check
     * `version` first, because a version-1 table ends just above. */
    void (*play_sample)(const void *, unsigned, int);
    void (*stop_sample)(unsigned);
    int  (*sample_playing)(unsigned);
    const void *(*sample_bundle_entry)(unsigned);
    /* version 3: ask the BIOS for the REAL analog axes. `uvm2_read_axes` otherwise
     * returns a digital verdict scaled to the ends of the range (-127, 0, +127), which
     * is all a yoke game like Star Wars ever saw on this board. It is a request rather
     * than the default because core 1 reads the axes every frame whether the game looks
     * at them or not, and the successive-approximation read disturbs the PSG. */
    void (*set_analog)(int);
    /* version 4: a straight line whose gaps the VIA produces by itself — one byte to the
     * shift register = 8 dots. This is the BIOS's text, and in a text-heavy game it brings a
     * character down from 36.4 commands to 21.2 (measured, uvm2_raster_text.c). Check
     * `version >= 4` before calling it: an older BIOS ends at `set_analog`. */
    int (*draw_sweep_sr)(int, int, const unsigned char *, int, int);
};
#define UVM2_API        ((const struct uvm2_api *)0x20077000u)
#define UVM2_API_MAGIC  0x50415356u   /* 'VSAP' */
#define BEAM_ZERO()       UVM2_API->draw_reset()
#define BEAM_INTENSITY(b) UVM2_API->draw_intensity((int)(signed char)(b) & 0x7F)
static void api_raster(int x, int y, const unsigned char *s, int n) {
    char t[97]; int k = 0;
    while (k < n && k < 96) { t[k] = (char)s[k]; k++; }
    t[k] = 0;
    UVM2_API->print_text(x, y, t, 1, 0x5F);   /* like SYS_RASTER_TEXT in uvm2_svc.c */
}
#define BEAM_RASTER(x,y,s,n) api_raster((x),(y),(s),(n))
/* THE SDK'S CONFIGURATION API, the same shape as uvm2_config.h, over the table. */
volatile int32_t uvm2_setting_hz = 50, uvm2_setting_menu = 1;
void uvm2_config_current(struct uvm2_config *c) {
    UVM2_API->config_current((int32_t *)c);
    c->hz = uvm2_setting_hz; c->start_menu = uvm2_setting_menu;
}
void uvm2_config_apply(const struct uvm2_config *c) {
    uvm2_setting_hz = (c->hz == 60) ? 60 : (c->hz == 0) ? 0 : 50;
    uvm2_setting_menu = c->start_menu ? 1 : 0;
    UVM2_API->config_apply((const int32_t *)c);
}
int uvm2_config_save(void) {
    struct uvm2_config c; UVM2_API->config_current((int32_t *)&c);
    c.hz = uvm2_setting_hz; c.start_menu = uvm2_setting_menu;
    UVM2_API->config_apply((const int32_t *)&c);
    return UVM2_API->config_save();
}
void uvm2_set_refresh(unsigned hz) { UVM2_API->set_refresh(hz); }
/* At game startup: the settings the BIOS launched it with. */
static void dc_cfg_init(void) {
    struct uvm2_config c; UVM2_API->config_current((int32_t *)&c);
    uvm2_setting_hz = c.hz; uvm2_setting_menu = c.start_menu;
}
#else
#define BEAM_ZERO()       sys_reset0ref()
#define BEAM_INTENSITY(b) sys_set_intensity(b)
#define BEAM_RASTER(x,y,s,n) sys_raster_text((x),(y),(s),(n)) /* SYS #26 */
#endif

/* Raster text (the BIOS font / uvm2_text.c) in device coordinates (i8). */
void v_rasterText(int x, int y, const unsigned char *s, int n) { BEAM_RASTER(x, y, s, n); }
/* ONE row of raw bytes (8 pixels per byte, bit 7 leftmost), same primitive. */
void v_rasterRow(int x, int y, const unsigned char *s, int n) { BEAM_RASTER(x, y, s, n); }
/* Z intensity for whatever follows (raster rows need it set BEFOREHAND). */
void v_setIntensity(int b) { BEAM_INTENSITY((signed char)b); }

#ifdef UVM2_INPUT_COUNT
unsigned uvm2_input_count;   /* DIAGNOSTIC, see uvm2_frame_end */
#endif

/* ── THE DRAWING: three SDK calls per stroke, in 1/16 of a unit. ────────────────────────
 *
 * It is what the .um2 does and what our own cartridge does, with the same functions and the
 * same integers; only how you reach them changes (directly, or through the table). The SDK is
 * what splits by the REAL ramp limit, what decides the re-zeros (uvm2_zero_every, measured
 * against the reference capture) and what knows the console's calibration. None of that is
 * decided here. */
void uvm2_draw_move_abs_q4(int x_q4, int y_q4);
void uvm2_draw_delta_q4(int dx_q4, int dy_q4);
void uvm2_draw_delta_patterned(int dx, int dy, const unsigned char *gaps, int n);
int uvm2_draw_sweep_sr(int dx, int dy, const unsigned char *pattern, int n, int step);
void uvm2_draw_intensity(int z);
/* uvm2_smp.c, inside the image on the .um2 (on the cartridge it is reached
 * through the BIOS table). */
void uvm2_smp_play(const void *data, unsigned voice, int loop);
void uvm2_smp_stop(unsigned voice);
int  uvm2_smp_playing(unsigned voice);
const void *uvm2_smp_bundle_entry(unsigned idx);

/* ── HOW MUCH OF A FRAME IS SPENT INSIDE THE DRAW PATH ──────────────────────
 *
 * Tac/Scan measured 30.1 ms a frame in "vector generation + list building" — 86.7%
 * of everything the builder does, and 11057 M33 cycles per path. That number lumps
 * two different suspects: the port's own walk over the game's vector RAM, and the
 * three calls this function makes into the SDK. On this board those three are
 * INDIRECT calls through the BIOS table at 0x20077000, and an older note here said
 * they land in BIOS code in flash — a second XIP region per path. MEASURED AND
 * UNTRUE (2026-09-19, starwars): every pointer in that table reads back as
 * 0x2000xxxx, i.e. internal SRAM, so the callee side is not XIP at all. What IS in
 * PSRAM is the caller: this file and the port's draw path link into .game_rom.
 *
 * So the split worth having is three ways, and that is what the two counter pairs
 * below give when the port also times its call to us:
 *   port's span  minus  uvm2_us_draw  = the caller-side conversions (a port that
 *                                       hands us floats pays soft-float here)
 *   uvm2_us_draw minus  uvm2_us_api   = VS_Q4 and this function's own body
 *   uvm2_us_api                       = the BIOS list builder, in SRAM
 *
 * Off unless the build asks (-DUVM2_MEASURE_DRAW). The timer reads are APB accesses
 * and there are four per call once both pairs are on, so with 400 paths a frame they
 * add tens of microseconds of their own: the figure is an upper bound, not a
 * hairline. (Measured on starwars: turning one pair on moved a 13 ms span by less
 * than its sample-to-sample spread at ~650 paths a frame.) */
#ifdef UVM2_MEASURE_DRAW
volatile uint32_t uvm2_us_draw;    /* accumulated microseconds inside this function */
volatile uint32_t uvm2_n_draw;     /* calls counted, to get the per-path cost */
/* Of those, the three calls into the BIOS builder, one counter each. They are not the
 * same animal: the intensity call is usually a no-op (a display list rarely changes
 * brightness between segments), the move is a blanked ramp that is often zero-length
 * (connected geometry), and the delta is the lit stroke that always has to happen. A
 * single lumped figure cannot tell "the builder is expensive" from "we are asking it
 * for two ramps per segment when one would do". */
volatile uint32_t uvm2_us_api_i, uvm2_us_api_m, uvm2_us_api_d;
volatile uint32_t uvm2_n_api;
#define DRAW_NOW() (*(volatile uint32_t *)0x400B000CU)   /* TIMER0 TIMELR, 1 MHz */
#endif

/* THIS ONE RUNS FROM SRAM ON THE CART. It is called once per segment -- a thousand
 * times a frame in a dense port -- and it is the last piece of the draw path still
 * linked into .game_rom, which on the cartridge is the PSRAM XIP window. An XIP hit
 * is not an SRAM access, however good the hit rate is. rp2350_start.s copies
 * .sram_text before main(); on the .um2 the whole image already runs from SRAM, so
 * the attribute is empty there. */
#if defined(VPY_DUAL_CORE) && !defined(UVM2_PICO_RUNTIME)
#define SDK_SRAM_TEXT __attribute__((section(".sram_text")))
#else
#define SDK_SRAM_TEXT
#endif

SDK_SRAM_TEXT
void v_directDraw32(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t b)
{
#ifdef UVM2_MEASURE_DRAW
    uint32_t t0 = DRAW_NOW();
#endif
    if (b == 0) return;                       /* z=0 is a blanked jump, not a stroke */
#ifdef UVM2_INPUT_COUNT
    uvm2_input_count++;   /* DIAGNOSTIC: how many drawing calls go INTO the SDK */
#endif
#ifdef VPY_DUAL_CORE
    {
#ifdef UVM2_MEASURE_DRAW
    /* The arguments are evaluated OUTSIDE this span on purpose: VS_Q4 is the caller's
     * own arithmetic, and charging it to the BIOS would hide it. */
    int i_ = (int)b;
    int ax_ = VS_Q4(x0), ay_ = VS_Q4(y0);
    int dx_ = VS_Q4(x1) - ax_, dy_ = VS_Q4(y1) - ay_;
    uint32_t ta_ = DRAW_NOW();
    UVM2_API->draw_intensity(i_);
    uint32_t tb_ = DRAW_NOW();
    UVM2_API->draw_move_abs_q4(ax_, ay_);
    uint32_t tc_ = DRAW_NOW();
    UVM2_API->draw_delta_q4(dx_, dy_);
    uint32_t td_ = DRAW_NOW();
    uvm2_us_api_i += tb_ - ta_;
    uvm2_us_api_m += tc_ - tb_;
    uvm2_us_api_d += td_ - tc_;
    uvm2_n_api++;
#else
    UVM2_API->draw_intensity((int)b);
    UVM2_API->draw_move_abs_q4(VS_Q4(x0), VS_Q4(y0));
    UVM2_API->draw_delta_q4(VS_Q4(x1) - VS_Q4(x0), VS_Q4(y1) - VS_Q4(y0));
#endif
    }
#else
    uvm2_draw_intensity((int)b);
    uvm2_draw_move_abs_q4(VS_Q4(x0), VS_Q4(y0));
    uvm2_draw_delta_q4(VS_Q4(x1) - VS_Q4(x0), VS_Q4(y1) - VS_Q4(y0));
#endif
#ifdef UVM2_MEASURE_DRAW
    uvm2_us_draw += DRAW_NOW() - t0;
    uvm2_n_draw++;
#endif
}

/* A STROKE WITH GAPS, IN A SINGLE RAMP — the same units as v_directDraw32.
 *
 * `gaps` are (start, end) pairs as fractions 0..255 of the stroke; the SDK converts them to
 * T1 counts and toggles BLANK along the way instead of programming one ramp per segment. It
 * is what you need to sweep text: a line of pixels is ONE ramp with the font's gaps in it,
 * not one stroke per run of lit dots.
 *
 * THIS EXISTS BECAUSE `v_rasterText` IS NO USE FOR THAT. On the cartridge it ends in
 * `UVM2_API->print_text(..., 1, 0x5F)`, i.e. the SDK's VECTOR font at a fixed scale: the
 * caller sets the position and the BIOS sets the glyph and the size, so text scaled by the
 * game comes out misaligned. Through here the font and the size belong to the caller, and
 * `draw_delta_patterned` has been in the BIOS table since version 1. */
void v_directGapped(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t b,
                    const unsigned char *gaps, int n)
{
    if (b == 0) return;
#ifdef VPY_DUAL_CORE
    UVM2_API->draw_intensity((int)b);
    UVM2_API->draw_move_abs_q4(VS_Q4(x0), VS_Q4(y0));
    UVM2_API->draw_delta_patterned(VS_Q4(x1) - VS_Q4(x0), VS_Q4(y1) - VS_Q4(y0),
                                   gaps, n);
#else
    uvm2_draw_intensity((int)b);
    uvm2_draw_move_abs_q4(VS_Q4(x0), VS_Q4(y0));
    uvm2_draw_delta_patterned(VS_Q4(x1) - VS_Q4(x0), VS_Q4(y1) - VS_Q4(y0), gaps, n);
#endif
}

/* THE SHIFT-REGISTER SWEEP, same units as v_directDraw32.
 *
 * `pattern` is one byte per 8 dots, bit 7 leftmost; `step` is the T1 counts between writes
 * and is 8, which is how long the 8 shifts take at the Phi2 rate. The width of what gets
 * drawn is NOT chosen with `step` but with the distance: the caller sets (x1-x0) so that n*8
 * counts give the width it wants.
 *
 * IT RETURNS HOW MANY BYTES IT DREW, or 0 if the BIOS does not provide it (a table older than
 * v4). They need not be the `n` asked for: the cell is the SR's 8 shifts and cannot be
 * stretched, so if the ramp speed does not reach, fewer characters fit. The caller carries on
 * from where it stopped. */
int v_directSweepSR(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t b,
                    const unsigned char *pattern, int n, int step)
{
    if (b == 0) return n;
#ifdef VPY_DUAL_CORE
    if (UVM2_API->magic != UVM2_API_MAGIC || UVM2_API->version < 4u) return 0;
    UVM2_API->draw_intensity((int)b);
    UVM2_API->draw_move_abs_q4(VS_Q4(x0), VS_Q4(y0));
    return UVM2_API->draw_sweep_sr(VS_Q4(x1) - VS_Q4(x0), VS_Q4(y1) - VS_Q4(y0),
                                     pattern, n, step);
#else
    uvm2_draw_intensity((int)b);
    uvm2_draw_move_abs_q4(VS_Q4(x0), VS_Q4(y0));
    return uvm2_draw_sweep_sr(VS_Q4(x1) - VS_Q4(x0), VS_Q4(y1) - VS_Q4(y0), pattern, n, step);
#endif
}

void v_WaitRecal(void)
{
#ifdef VPY_DUAL_CORE
    UVM2_API->wait_recal();     /* closes the list, hands it to the executor, opens the next */
#else
    sys_wait_recal();
#endif
}

/* A re-zero at the game's request. The SDK already clamps the zero on its own account every
 * uvm2_zero_every jumps; this only adds one where the game asks for it. */
void v_beamNewStroke(void) { BEAM_ZERO(); }

/* Kept for compatibility: games with vector text call them. They no longer bound anything
 * (there is no merging or clipping to avoid): text is drawn like everything else. */
void v_textBegin(void) {}
void v_textEnd(void)   {}

uint8_t v_readButtons(void)
{
#ifdef VPY_DUAL_CORE
    currentButtonState = (uint8_t)UVM2_API->read_buttons();
#else
    currentButtonState = (uint8_t)sys_read_buttons();
#endif
    return currentButtonState;
}

/* A GAME THAT STEERS WITH THE STICK SAYS SO, ONCE, BEFORE IT READS AN AXIS.
 *
 * Without it the axes come back as -127, 0 or +127 and nothing between: the BIOS's
 * `s_analog` defaults to off and only the .um2 runtime ever set it, so on the cartridge
 * every yoke game got a digital verdict scaled to the ends. Measured on the board with
 * Star Wars in gameplay -- `uvm2_cached_axes` read 0x7f000000, 0x00000000, 0x81000000
 * and never anything else.
 *
 * Harmless on an older BIOS: the table grew at the end, so the version check leaves the
 * game exactly as it was rather than calling into whatever the RAM held. */
/* Voice 0's cursor, as a frame count at `fps`. See sys_sample_pos. */
int v_samplePos(int fps) { return sys_sample_pos(fps); }

void v_setAnalog(int on)
{
#ifdef VPY_DUAL_CORE
    if (UVM2_API->version >= 3) UVM2_API->set_analog(on);
#else
    (void)on;   /* the .um2 runtime turns it on in uvm2_svc.c */
#endif
}

void v_readJoystick1Analog(void)
{
#ifdef VPY_DUAL_CORE
    unsigned int a = UVM2_API->read_axes();
#else
    unsigned int a = (unsigned int)sys_read_axes();
#endif
    currentJoy1X = (int8_t)(a >> 24);
    currentJoy1Y = (int8_t)(a >> 16);
}

__attribute__((weak)) int8_t currentJoy2X = 0;
__attribute__((weak)) int8_t currentJoy2Y = 0;
__attribute__((weak)) void v_readJoystick2Analog(void)
{
#ifdef VPY_DUAL_CORE
    unsigned int a = UVM2_API->read_axes();
#else
    unsigned int a = (unsigned int)sys_read_axes();
#endif
    currentJoy2X = (int8_t)(a >> 8);
    currentJoy2Y = (int8_t)a;
}

void v_writePSG(uint8_t reg, uint8_t val)
{
#ifdef VPY_DUAL_CORE
    UVM2_API->psg_queue(reg, val);
#else
    sys_psg_write(reg, val);
#endif
}

/* ── DIGITISED SAMPLES (AAE Sega G80: Tac/Scan, Star Trek…) ───────────────────────────
 *
 * They sound through the PSG's volume DAC, with the writes PUT INTO THE DRAW LIST —
 * see the uvm2_smp.h header, which is where the model and the measurements live.
 * Only the plumbing is here, and it has one piece that is not obvious:
 *
 * AAE ASKS FOR A SOUND BY NUMBER (`v_playSample(idx, …)`, where idx indexes its own
 * sample table), and the SDK cannot know which .vsmp is which: that belongs to the
 * game. In the simulator the JS side resolves it by reading `samples/samples.json`;
 * on the cartridge `v_sampleData` resolves it, and EACH GAME defines it with its own
 * table. The weak default returns 0, so the 43 ports with no samples keep compiling
 * and keep silent without a line of change. It is the same rule as the rest of the
 * SDK: generalise at the contract, never per program. */
/* THE DEFAULT IS THE BUNDLE, which is what a port normally wants: the sounds ship
 * as a file on the card and the index is the position in it. Who READS that file
 * differs by board and that is the whole reason this is not one line:
 *
 *   - .um2: the game reads it itself at startup (uvm2_smp_bundle_load), because it
 *     owns the bus while it boots.
 *   - Vectrex Studio cartridge: the BIOS reads it in SYS_LAUNCH, next to the
 *     romset. The game runs on core 1 while core 0 drives the bus, and a card read
 *     from the other core returns garbage — so the game must not try.
 *
 * A port that would rather link its own table just defines v_sampleData itself. */
__attribute__((weak)) const void *v_sampleData(int idx)
{
    if (idx < 0) return 0;
#ifdef VPY_DUAL_CORE
    return (UVM2_API->version >= 2u) ? UVM2_API->sample_bundle_entry((unsigned)idx) : 0;
#else
    return uvm2_smp_bundle_entry((unsigned)idx);
#endif
}

void v_playSample(int idx, int voice, int loop)
{
    const void *p = v_sampleData(idx);
    if (!p) return;
#ifdef VPY_DUAL_CORE
    /* A VERSION-1 TABLE ENDS AT `refresco`. Calling `play_sample` on a BIOS from
     * back then would be jumping into whatever that RAM holds, so the number is
     * checked: an old BIOS with a new game stays quiet, which is what it did
     * before any of this. */
    if (UVM2_API->version >= 2u) UVM2_API->play_sample(p, (unsigned)voice, loop);
#else
    uvm2_smp_play(p, (unsigned)voice, loop);
#endif
}

void v_stopSample(int voice)
{
#ifdef VPY_DUAL_CORE
    if (UVM2_API->version >= 2u) UVM2_API->stop_sample((unsigned)voice);
#else
    uvm2_smp_stop((unsigned)voice);
#endif
}

int v_samplePlaying(int voice)
{
#ifdef VPY_DUAL_CORE
    return (UVM2_API->version >= 2u) ? UVM2_API->sample_playing((unsigned)voice) : 0;
#else
    return uvm2_smp_playing((unsigned)voice);
#endif
}

void v_playMusic(const unsigned char *vmus) { sys_play_music(vmus); }
void v_stopMusic(void)                      { sys_stop_music(); }
/* Compiled .vsfx over the music: the BIOS sequencer merges it on channel C
 * (SYS_PLAY_SFX = 23, see the note above about not confusing it with 26). */
void v_playSFX(const unsigned char *vsfx)   { sys_play_sfx(vsfx); }

#include <stddef.h>
#ifndef UVM2_PICO_RUNTIME
void *memset(void *d, int c, size_t n)  { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *pd = d; const unsigned char *ps = s; while (n--) *pd++ = *ps++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *pd = d; const unsigned char *ps = s;
    if (pd < ps) { while (n--) *pd++ = *ps++; }
    else { pd += n; ps += n; while (n--) *--pd = *--ps; }
    return d;
}
#endif
