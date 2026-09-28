/* uvm2_wizard.c — THE CALIBRATION SCREEN.
 *
 * See uvm2_config.h for the reasoning behind the four parameters. This is only the interface.
 *
 * THE PATTERN MEASURES, IT DOES NOT DECORATE. TWO squares of the SAME size are drawn side by
 * side: the left one with 4 long strokes, the right one with 40 short ones. Since they measure
 * the same, any difference between them comes from the NUMBER of strokes and not from their
 * length, and that separates the error's two terms:
 *
 *     the 40 one opens and the 4 one does not  -> it is the FIXED per-stroke term (t1_tail_q8)
 *     both open in proportion                  -> it is the SCALE                 (scale)
 *
 * A closed polygon that opens accumulates the fixed loss N times; a scale error would make it
 * smaller but it would still be closed. That is why the reference cartridge has one screen for
 * vectors and another for text: they are these same two terms.
 *
 * WITH ZERO SELECTED THE PATTERN IS TEXT INSTEAD: see zero_pattern.
 *
 * CONTROLLER: up/down picks a parameter, left/right moves it, button 4 saves and exits.
 * HOW TO GET HERE: hold buttons 1 and 4 while the game starts (uvm2_config_boot_combo).
 */
#include "uvm2_config.h"
#include "uvm2_draw.h"
#include "uvm2_text.h"
#include "uvm2_bus.h"

void uvm2_core1_start(void);
void uvm2_core1_stop(void);

/* INPUT IS READ FROM THE CACHE, NOT FROM THE BUS.
 *
 * `uvm2_read_buttons()` and `uvm2_read_axes()` talk to the PSG over the Vectrex bus — and CORE
 * 1 is using that bus to replay the list. Calling them from core 0, as this did when it was
 * written, puts both cores on the same bus: measured on the console, the wizard ran at 1 fps
 * with `us_exec = 1,125,454 us` (1.1 seconds executing a list of 17,111 cycles, i.e. 11 ms of
 * work). The emulator does NOT reproduce it: there it ran at 50 Hz, because it does not model
 * bus contention.
 *
 * Core 1 already reads them once per frame and leaves them here; games read from here. The
 * buttons arrive RAW, active low (0 = pressed). */
extern volatile uint8_t  uvm2_cached_buttons;
extern volatile uint32_t uvm2_cached_axes;

#define SIDE      44      /* the square's side, in device units */
#define SEP       56      /* distance between the two squares' centres */
/* uvm2_print_text's `scale` is in HALF units and its stock value is 3 (x1.5). I once used 1
 * — a third of normal — and on the console it was unreadable. 4 is x2. */
#define TEXT      3      /* the wizard's letter size */
#define STEP      19     /* distance between lines */
#define VISIBLE   5      /* how many fit at once in the screen's +-127 */

/* A CALIBRATION FIGURE taken from an open-source Vectrex game (`displaySwarmCalibration` in
 * Vectorblade's objectEnemySwarm.asm). Eight long, crooked segments at scale 6 — not a square:
 * what makes it useful is that it mixes strokes of very different lengths and slopes, which is
 * where the zero reference's offset shows.
 *
 * Its macros carry the deltas as (dy, dx) packed into a word; here they are written out,
 * already unpacked and with the sums it writes inline (`$00-40`, `-$1B-10`, ...). This is
 * placeholder geometry — replace it with your own .vec. */
static const signed char VB_SWARM[][2] = {   /* {dx, dy} */
    {  -40,  127 }, {  -50,  -37 }, {    6,   40 }, {  -52,    0 },
    {   36,  -40 }, {  -50,   47 }, {   40, -127 }, {   60,   25 },
};

static void calibration_figure(int cx, int cy)
{
    /* Its INIT_DRAW_6_MOVE_END moves (dx, dy) = (60, -18) from the centre before drawing. */
    uvm2_draw_move_abs(cx + 60 / 3, cy - 18 / 3);
    for (unsigned i = 0; i < sizeof VB_SWARM / sizeof VB_SWARM[0]; i++) {
        /* /3 because its scale of 6 runs off a +-127 screen; the SHAPE is what matters, and
         * dividing everything equally preserves it. */
        uvm2_draw_delta(VB_SWARM[i][0] / 3, VB_SWARM[i][1] / 3);
    }
}

/* THE REFERENCE LINE, also theirs: each of that game's calibration screens draws a line of
 * FIXED length next to whatever is being calibrated (`ldd #$0080  jsr DrawLined`, i.e. 128 on
 * one axis) and you adjust until they MATCH. Comparing against a reference is measuring;
 * looking at one figure and deciding whether it "looks right" is not. */
static void reference_line(int cx, int cy)
{
    uvm2_draw_move_abs(cx, cy);
    uvm2_draw_delta(0, 128 / 3);
}

/* THE ZERO PATTERN: SEVERAL LINES OF TEXT, DRAWN THE WAY A GAME DRAWS THEM.
 *
 * Shown while ZERO is the selected field. A zero reference that is wrong for this console adds
 * the same velocity to every ramp, and nothing makes that more obvious than rows of short
 * strokes run without a re-zero: each row leans into a diagonal and the glyphs slant. That is
 * what a tester's console showed with the AAE ports on 2026-09-28 (and the VecFever writes 0x07
 * where the SDK's default is 0x23 — see UVM2_ZERO_OFFSET).
 *
 * The two long strokes are the reference: one ramp each, so they barely carry the offset.
 * Adjust ZERO until every row runs parallel to the top line and every column stands parallel
 * to the left one. */
static const char *const ZERO_ROWS[] = {
    "HIGH SCORES",
    "1 DBC 0025350",
    "2 WAN 0019420",
    "3 HAN 0017880",
    "4 GAR 0012750",
    "5 MLH 0010030",
};
#define ZERO_LEFT   (-112)   /* the rows' left edge */
#define ZERO_TOP    118      /* the first row's top */
#define ZERO_ROW    15       /* distance between rows */

static void zero_pattern(int bright)
{
    const int rows = (int)(sizeof ZERO_ROWS / sizeof ZERO_ROWS[0]);
    const int bottom = ZERO_TOP - rows * ZERO_ROW;
    uvm2_draw_move_abs(ZERO_LEFT - 6, ZERO_TOP + 4);           /* the top line */
    uvm2_draw_delta(160, 0);
    uvm2_draw_move_abs(ZERO_LEFT - 6, ZERO_TOP + 4);           /* the left line */
    uvm2_draw_delta(0, bottom - ZERO_TOP - 4);
    for (int r = 0; r < rows; r++)
        uvm2_print_text_chained(ZERO_LEFT, ZERO_TOP - r * ZERO_ROW, ZERO_ROWS[r], TEXT, bright);
}

/* A square drawn with `n` strokes per side, centred on (cx, cy). With n = 1 it is the 4 long
 * strokes; with n = 10, the 40 short ones. The total travel is THE SAME. */
static void square(int cx, int cy, int n)
{
    static const int dx[4] = { 1, 0, -1, 0 };
    static const int dy[4] = { 0, 1,  0, -1 };
    const int side = SIDE;
    uvm2_draw_move_abs(cx - side / 2, cy - side / 2);
    for (int l = 0; l < 4; l++) {
        /* The exact split, so n strokes measure the same as one: the ideal position is
         * accumulated and the difference emitted, instead of repeating side/n and losing the
         * remainder n times. */
        int done = 0;
        for (int i = 1; i <= n; i++) {
            int ideal = side * i / n;
            uvm2_draw_delta(dx[l] * (ideal - done), dy[l] * (ideal - done));
            done = ideal;
        }
    }
}

struct field { const char *name; int32_t *value; int32_t min, max, step; };

/* THE GAME CAN SUPPLY THE FIGURE.
 *
 * The SDK's pattern (the reference figure and the two squares) separates the error's two terms
 * well, but calibrating against it is not the same as calibrating against WHAT LOOKS WRONG.
 * The arbiter is the drawing that bothers you, not a laboratory figure.
 *
 * With `figure` null the usual pattern is drawn. With a function, that one is drawn IN ITS
 * PLACE and the rest of the screen — the values, the controller, the saving — is identical.
 * The game passes it already centred and at its own scale: the wizard knows nothing about
 * it. */
int uvm2_config_wizard_with(void (*figure)(void))
{
    struct uvm2_config c;
    uvm2_config_current(&c);

    /* THE PARAMETERS, TAKEN FROM A REFERENCE GAME AND NOT DEDUCED BY ME.
     *
     * Vectorblade's `calibration.asm` has THREE screens and all three adjust ONE thing: a byte
     * primed into the ZERO REFERENCE (`calibrateString` does `ORB=$82` — mux on channel 1 — and
     * `ORA = calibrationValueString`), which is exactly what our zero block emits with
     * `uvm2_zero_offset`.
     *
     * Its factory values: `calibrationValue16 = $23` (35) and `calibrationValue50 = $56` (86).
     * **Ours was at 7**, thirty units below where it starts to matter — which is why moving it
     * from 7 to 5 on the console did nothing.
     *
     * It has THREE because the value depends on the SCALE of what is drawn: one for the boss
     * (long strokes), another for text. It is the same division others describe ("separate ones
     * for vectors and text"), and not two terms of an error model as I had assumed.
     *
     * `scale` and `t1_tail_q8` stay because they are REAL drawing knobs, but at the end: they
     * are not what calibrates a console.
     *
     * THE CONSOLE'S ALWAYS; THE GAME'S, ONLY THOSE THE GAME DECLARES AS ITS OWN
     * (uvm2_config_game). A switch that means nothing in this game is not shown: Donkey Kong is
     * vertical and has no business seeing a ROTATE, and the menu is hidden per game and not for
     * everyone. */
    struct field fields[8];
    int n = 0;
    fields[n++] = (struct field){ "ZERO",   &c.zero,       0, 255, 1 };
    fields[n++] = (struct field){ "BRIGHT", &c.bright,     0, 127, 1 };
    fields[n++] = (struct field){ "SCALE",  &c.scale,   80, 400, 1 };
    fields[n++] = (struct field){ "TAIL",   &c.t1_tail_q8, -512, 512, 8 };
    {
        const unsigned mine = uvm2_config_game_settings();
        /* ROTATE: the screen is vertical and quite a few arcade machines are horizontal. It
         * shows instantly on the figure itself, which is exactly what a setting like this
         * needs. */
        if (mine & UVM2_SETTING_ROTATE) fields[n++] = (struct field){ "ROTATE", &c.rotate,     0, 1, 1 };
        if (mine & UVM2_SETTING_MENU)   fields[n++] = (struct field){ "MENU",   &c.start_menu, 0, 1, 1 };
        if (mine & UVM2_SETTING_HZ)     fields[n++] = (struct field){ "HZ",     &c.hz,         0, 60, 10 };
    }
    int sel = 0, saved = 0;
    /* THE BUTTONS ARE ACTIVE LOW (PSG reg 14 raw: 0 = pressed), so they are inverted here ONCE
     * and the rest of the code reasons with 1 = pressed. Without inverting, the edge detects
     * the RELEASE and at rest every bit is 1. */
    uint8_t before = (uint8_t)~uvm2_cached_buttons;

    for (;;) {
        uvm2_config_apply(&c);          /* the effect is visible WHILE it is moved */
        uvm2_frame_begin();
        uvm2_draw_intensity(c.bright);

        if (fields[sel].value == &c.zero) {
            zero_pattern(c.bright);       /* the zero's own pattern, whatever the game passed */
        } else if (figure) {
            figure();                     /* the game's, see above */
        } else {
            /* The reference figure with its reference line beside it, and below the two
             * squares — 4 strokes against 40 — which still show the fixed term. */
            reference_line(-100, 20);
            calibration_figure(-40, 38);
            square( 70, 60,  1);
            square( 70, 10, 10);
        }

        /* A WINDOW, NOT THE WHOLE LIST, like dkong's menu: with the game settings it is up to
         * seven lines, and at 26 units per step from -18 the list ran off the bottom of the
         * screen — you saw four and a half. VISIBLE of them are shown with the selected one
         * centred, and the letter size drops to something that fits. */
        int first = sel - VISIBLE / 2;
        if (first > n - VISIBLE) first = n - VISIBLE;
        if (first < 0) first = 0;
        for (int w = 0; w < VISIBLE && first + w < n; w++) {
            const int i = first + w;
            char line[24];
            int p = 0;
            line[p++] = (i == sel) ? '>' : ' ';
            for (const char *s = fields[i].name; *s; s++) line[p++] = *s;
            line[p++] = ' ';
            /* The value, by hand: the SDK has no printf and pulling it in for this would cost
             * 20 KB of flash for four numbers. */
            int32_t v = *fields[i].value;
            if (v < 0) { line[p++] = '-'; v = -v; }
            char d[8]; int k = 0;
            do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 7);
            while (k) line[p++] = d[--k];
            line[p] = 0;
            uvm2_print_text(-112, -22 - w * STEP, line, TEXT, c.bright);
        }
        uvm2_frame_end();

        /* THE CONTROLLER, ON EDGES. Without this one tap moves the value thirty times: the
         * loop runs at 50 Hz and a finger takes longer. */
        uint8_t b = (uint8_t)~uvm2_cached_buttons;
        uint8_t pressed = (uint8_t)(b & ~before);
        before = b;
        /* (J1X << 24) | (J1Y << 16) | (J2X << 8) | J2Y — controller 1 is in the HIGH bytes.
         * Reading the low ones reads controller 2, which is what this did when it was
         * written. */
        uint32_t axes = uvm2_cached_axes;
        int jx = (int8_t)(axes >> 24), jy = (int8_t)(axes >> 16);

        /* UP/DOWN SELECTS: ONE STEP PER EXCURSION, WITH HYSTERESIS AND SETTLING.
         *
         * Reported on this screen: "push down and bring the stick back to centre, and it goes
         * back up". The stick is a SPRING: on release it crosses the centre and overshoots to
         * the other side for a few frames. What used to be here re-armed as soon as the axis
         * entered +-40, so that bounce counted as a second excursion — in the opposite
         * direction.
         *
         * The rule is the one already validated in dkong's menu, with the SAME complaint and
         * the same words: it fires on crossing +-60 and does not re-arm until the axis has
         * spent three consecutive frames within +-25. The spring's bounce lasts less than that
         * and falls entirely inside the unarmed period. */
        {
            static int armed = 1, settled = 0;
            int step = 0;
            if (jy > -25 && jy < 25) {
                if (settled < 3) settled++;
                if (settled >= 3) armed = 1;
            } else {
                settled = 0;
                if (armed) {
                    if (jy >= 60)      { armed = 0; step = -1; }   /* up */
                    else if (jy <= -60){ armed = 0; step = +1; }   /* down */
                }
            }
            if (step) sel = (sel + n + step) % n;
        }

        /* LEFT/RIGHT ADJUSTS, CONTINUOUSLY WHILE HELD — like the reference game, which moves
         * +-1 every two frames (`Vec_Loop_Count+1 & 1`). On edges it was useless: the zero
         * reference's useful range runs from 0 to 255, and at one step per press it takes a
         * hundred taps to reach where it starts to show. */
        static uint32_t tick = 0;
        tick++;
        if ((jx > 40 || jx < -40) && (tick & 1u) == 0) {
            int32_t *v = fields[sel].value;
            *v += (jx > 0 ? fields[sel].step : -fields[sel].step);
            if (*v < fields[sel].min) *v = fields[sel].min;
            if (*v > fields[sel].max) *v = fields[sel].max;
        }

        if (pressed & 0x08) {           /* button 4: save and exit */
            /* CORE 1 IS NOT STOPPED HERE, AND THIS COMMENT USED TO LIE.
             *
             * It said "stop core 1 before touching the flash", and that was true when
             * `uvm2_config_save` wrote to flash. Not any more: that path is disabled
             * (`save_to_flash_DO_NOT_USE`, and the reason is there) and it now writes to the
             * SD. The comment stayed and so did the call.
             *
             * And it is not harmless: `uvm2_frame_end` has core 0's ONLY wait —
             * `while (uvm2_frame_done - (s_frame_no-1) < 0)` — and the one that advances that
             * counter is core 1. Reset it and core 0 stays there for ever. It hung dkong on the
             * console on 2026-09-14; over SWD, pc pinned in uvm2_draw.c in three samples, and
             * the screen NOT black because core 1 kept repeating the last list. */
            saved = uvm2_config_save();
            break;
        }
    }
    return saved;
}

int uvm2_config_wizard(void) { return uvm2_config_wizard_with(0); }

/* HOLD BUTTONS 1 AND 4 WHILE THE GAME STARTS, AND THE WIZARD OPENS FIRST.
 *
 * Until this existed no game opened the wizard, so a console whose zero differs from the
 * default had no way to fix it short of editing config/uvm2.cfg on a PC.
 *
 * Core 1 refreshes the button cache every frame period even with no list published, so this
 * waits until it has done so a few times — a cache still at its power-on value would read as
 * "not held" and the check would be one that cannot fire. `uvm2_boot_combo` records the
 * outcome for SWD: -1 never checked, 0 not held, 1 held and the wizard ran, 2 core 1 never
 * refreshed the cache (the check could not be made). */
volatile int32_t uvm2_boot_combo = -1;

void uvm2_config_boot_combo(void)
{
    /* uvm2_stats is not volatile, and core 1 is the one advancing this: read it as volatile
     * or the loop may never see it move. */
    volatile const uint32_t *idle = &uvm2_stats.idle_frames;
    const uint32_t start = *idle;
    /* Bounded by a spin count, not a clock, so it cannot hang if core 1 never runs. At the
     * 20 ms idle period, 3 refreshes are ~60 ms. */
    for (uint32_t spin = 0; *idle - start < 3u; spin++) {
        if (spin > 50000000u) { uvm2_boot_combo = 2; return; }
    }
    const uint8_t held = (uint8_t)~uvm2_cached_buttons;       /* active low: 1 = pressed */
    if ((held & 0x09u) != 0x09u) { uvm2_boot_combo = 0; return; }   /* buttons 1 (bit 0) and 4 (bit 3) */
    uvm2_boot_combo = 1;
    uvm2_config_wizard();
}
