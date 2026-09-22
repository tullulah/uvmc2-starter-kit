#ifndef VECTOR_H
#define VECTOR_H

#ifdef NO_PI
/* Host harness: no PiTrex/SDK header, so declare the draw sink used inline below. */
#include <stdint.h>
void v_directDraw32(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t z);
#endif

#ifndef NO_PI
#define MUL_FAC 32
#define MUL_SHIFT 5
#define X_OFFSET_BASE -14000
#define Y_OFFSET_BASE -12000
#else
#define MUL_FAC 1
#define MUL_SHIFT 0
#define X_OFFSET_BASE 0
#define Y_OFFSET_BASE 0
#endif

//void draw_points(void);
void draw_lines(void);
void draw_texs(void);
void draw_color_vectors();
void add_color_line(float sx, float sy, float ex, float ey, int r, int g, int b);
void add_color_point(float sx, float sy, int r, int g, int b);
void add_line(float sx, float sy, float ex, float ey, int zvalue, float gc, float mod);
void add_line_yr(float sx, float sy, float ex, float ey, int zvalue, float gc, float mod);


void add_point(float sx, float sy ,int zvalue, float gc, float mod , float adj);
void add_point_yr(float sx, float sy ,int zvalue, float gc, float mod , float adj);


void cache_line(float startx, float starty, float endx, float endy, int zvalue, float gc, float mod);
void cache_point(float pointx, float pointy ,int zvalue, float gc, float mod , float adj);
void cache_txt(float pointx, float pointy ,int size, int color);

void cache_end(void);
void cache_clear(void);


/* THE FLOATS HERE NEVER HELD A FRACTION. Every caller passes an int cast to float and
 * the body casts it straight back, so on a soft-float build (the RP2350 cart is
 * -mfloat-abi=soft) each segment paid eight __aeabi conversions for nothing -- and
 * those helpers live in the game's PSRAM text, off the hot path's SRAM. Measured on
 * starwars: ~1.2 us per segment of the 22.7 the draw hand-off costs.
 *
 * The integer entry point is the real one; the float signature stays for the callers
 * that still use it. Identical output is not an argument, it is checked: the host
 * harness hashes every coordinate it draws (aae_starwars/tools/host_test.c), and the
 * hash is unchanged by this. */
/* COLOUR IS BRIGHTNESS HERE, AND THE AVERAGE WAS THE WRONG WAY TO SPELL IT.
 *
 * These games drove a colour vector monitor; a Vectrex tube has one phosphor, so every
 * colour has to become an intensity. The obvious reading -- average the three components
 * -- punishes SATURATION, and that is not what a colour monitor did: a fully red stroke
 * was a fully lit stroke, not a third-lit one.
 *
 * MEASURED over 880,024 segments of a real Star Wars game:
 *
 *     one component lit (a pure red/green/blue)   747,964   85%
 *     two components                              119,692   14%
 *     all three (white)                            12,368    1.4%
 *
 * swavg.c sets r, g and b to the frame's own z on whichever components the colour has
 * on, so `(r+g+b)/3` hands 85% of the picture a THIRD of white's brightness for the same
 * z. That is exactly what the console showed: a dark game you had to turn the tube up
 * for, with the 1.4% of white strokes glaring. With the old mapping plus a floor, 748,204
 * of those segments landed clamped on the floor itself and 10,079 sat at 112 and above.
 *
 * `max(r, g, b)` instead returns the game's OWN z untouched, so what varies is what the
 * game meant to vary and colour stops being an accidental attenuator. The /2 is the
 * range: AAE works in 0..255 and the SDK's intensity is 0..127.
 *
 * THE CUT IS PITREX'S NUMBER, from their own Star Wars port (starwars/vector.h): below
 * 20 they do not draw the stroke at all rather than draw one the tube cannot light. It
 * is kept because it is the honest way to spend nothing on an invisible stroke -- but it
 * is not a performance lever here and should not be sold as one: with max() instead of
 * the average it drops 60 segments out of 880,024.
 *
 * It replaces a floor that raised everything dim to 35. That floor was compensating for
 * the averaging, and it flattened gradients to do it -- the Star Wars logo's depth
 * shading became one flat level. With max() the same logo comes out at 21..126 and keeps
 * its shading, which is the better answer to the same problem. */
#define AAE_Z_CUT 20

/* FILLING THE TUBE, AND CENTRING ON WHAT THE GAME ACTUALLY DRAWS.
 *
 * `(v << MUL_SHIFT) + X_OFFSET_BASE` is PiTrex's transform, copied along with the port,
 * and on a Vectrex it leaves the picture small and off to one side. Those constants are
 * also NOT a window anyone measured: the AVG does not clip, so the game happily emits
 * objects far outside its own screen -- over 3000 frames Star Wars reaches x -600..1228
 * and y -3721..2127, which the arcade monitor simply did not show. A min/max over that
 * describes the off-screen traffic, not the playfield.
 *
 * MEASURED BY PERCENTILE instead, over attract and gameplay (1% to 99% of all endpoints):
 *
 *     x  -30 .. 773   (803 wide, centred on 371)
 *     y  -56 .. 832   (888 tall, centred on 388)
 *
 * Through the old transform that landed centred at (-16.8, +3.3) of a +-127 screen and
 * filling 202 x 224 of the 254 available -- 80% of the width, and visibly left of centre.
 *
 * So: one multiply instead of a shift, because the factor that fits is not a power of
 * two, and offsets that put the MEASURED centre at zero rather than a number inherited
 * from another machine. At 36 the 888-unit axis spans 252 of 254 and the other 228, with
 * every endpoint inside the screen:
 *
 *     x  -113.7 .. +114.0        y  -125.9 .. +125.9
 *
 * This is Star Wars' and Empire Strikes Back's space only -- they are the only ports that
 * compile swavg.c, and they share the hardware. Anything that needs its own numbers
 * defines them before including this. */
#ifndef AAE_SCREEN_MUL
#ifndef NO_PI
#define AAE_SCREEN_MUL 36
#define AAE_SCREEN_OX  (-13356)      /* -371 * 36 */
#define AAE_SCREEN_OY  (-13968)      /* -388 * 36 */
#else
/* The host harness works in the game's own units, so the transform is the identity and
 * a dump can be read against the game's coordinates. */
#define AAE_SCREEN_MUL 1
#define AAE_SCREEN_OX  0
#define AAE_SCREEN_OY  0
#endif
#endif

#define AAE_SX(v) ((int)(v) * AAE_SCREEN_MUL + AAE_SCREEN_OX)
#define AAE_SY(v) ((int)(v) * AAE_SCREEN_MUL + AAE_SCREEN_OY)

#ifdef AAE_COLOR_LOG
extern void aae_color_seen(int r, int g, int b);
extern void aae_range_seen(int x, int y);
#endif

static inline void add_color_line2i(int sx, int sy, int ex, int ey, int r, int g, int b)
{
   int m = r > g ? r : g;
   int z;
   if (b > m) m = b;
   z = m / 2;
#ifdef AAE_COLOR_LOG
   aae_color_seen(r, g, b);
   aae_range_seen(sx, sy); aae_range_seen(ex, ey);
#endif
   if (z < AAE_Z_CUT) z = 0;
   v_directDraw32(AAE_SX(sx), AAE_SY(sy), AAE_SX(ex), AAE_SY(ey), z);
}

static inline void add_color_line2(float sx, float sy, float ex, float ey, int r, int g, int b)
{
   add_color_line2i((int)sx, (int)sy, (int)ex, (int)ey, r, g, b);
}

#endif

