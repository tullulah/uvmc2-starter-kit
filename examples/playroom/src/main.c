/* playroom — a small game that walks through what the SDK can do.
 *
 * A wireframe figure walks a hub (vpybone: a 13-bone skeleton, walk and run
 * blended by the stick, feet put on the steps by the two-bone IK, the camera
 * following with vpycam). Four doors lead to four rooms, each built round some
 * of the modules, each with one goal; meet it and its piece appears on a
 * pedestal in the hub. With all four, the middle of the hub opens on the finale.
 *
 *   CRATES  knock eight blocks off a pad: vpyphys (boxes, hull pyramids, a
 *           blast), vpyent, dents and marks, shadows, vpycam shake and hit-stop,
 *           vpyimpact sounds by material
 *   JELLY   blow a jelly into a ring with the wind: vpysoft (cloth, a blob that
 *           keeps its area, a jelly cube)
 *   SHAPES  shoot five shapes to pieces: vpyfx disintegrate and assemble, rings,
 *           a vpy3d ray against the mesh
 *   FLYER   hit a ship flying over hills: vpy3d terrain, fog, levels of detail,
 *           and its engine on the UVMC2's jack with the Doppler shift
 *   FINALE  the trophy assembles from the four pieces: text in the world,
 *           billboards, fireworks
 *
 * Before each room a card says what to do and how; while it plays the line at
 * the top says how far along it is. Button 4 leaves a room (back to the hub).
 *
 * THE READOUT at the bottom, on every screen: STK strokes this frame, DROP must
 * be 0.
 */
#include <string.h>
#ifdef PLAYROOM_HOST
#include <stdio.h>
#include <stdlib.h>
#endif
#include "playroom.h"
#include <vpyease.h>
#include <vpyimpact.h>
#ifndef VPY_DUAL_CORE
#include <uvm2_bus.h>   /* the .um2: the SDK is in the image */
#include <uvm2_jack.h>  /* the UVMC2's stereo DAC, where there is one */
#endif

/* ── where the game is ─────────────────────────────────────────────────── */
enum { S_TITLE, S_HUB, S_CARD, S_PLAY, S_CLEARED, S_EARNED, S_FINALE };
static int s_state = S_TITLE, s_room = -1, s_t;
static uint8_t s_done[N_ROOMS];
static uint8_t s_held[5], s_edge[5];

#define CLEARED_FRAMES  90    /* the room keeps going under "CLEARED!" this long */
#define TITLE_IN        24    /* frames for a card's title to drop in */
#define LINE_EVERY       5    /* and one more line of it every this many */

/* ── what every room gets ─────────────────────────────────────────────── */
/* EDGES ARE TAKEN ONCE A FRAME, for every button, whoever asks: a button only
 * looked at on some screens would otherwise keep the state it had when it was
 * last looked at, and the next press there would not count. An edge is used up
 * by the first one to ask for it. */
static void read_buttons(void)
{
    for (int n = 1; n <= 4; n++) {
        const int b = vpy_j1_button(n);
        s_edge[n] = (uint8_t)(b && !s_held[n]);
        s_held[n] = (uint8_t)b;
    }
}
int pr_pressed(int n)
{
    const int e = s_edge[n];
    s_edge[n] = 0;
    return e;
}
int pr_done(int room) { return room >= 0 && room < N_ROOMS && s_done[room]; }
int pr_done_count(void)
{
    int n = 0;
    for (int i = 0; i < N_ROOMS; i++) n += s_done[i];
    return n;
}

/* The font's advance is a hair over `size` units a character (measured with
 * vpy_print_text on the host: 10 chars at size 8 span 81.6 units). */
static int text_width(const char *s, int size) { return (int)strlen(s) * size * 102 / 100; }

void pr_text_centre(int y, const char *s, int size, int br)
{
    vpy_set_text_size(size);
    vpy_set_intensity(br);
    vpy_print_text(-text_width(s, size) / 2, y, s);
}

static char *put_num(char *p, int n)
{
    char d[8]; int k = 0;
    if (n < 0) n = 0;
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 7);
    while (k) *p++ = d[--k];
    *p = 0;
    return p;
}

void pr_objective(const char *what, int have, int need)
{
    char line[48];
    size_t n = strlen(what);
    if (n > 36) n = 36;
    memcpy(line, what, n);
    char *p = line + n;
    if (need > 0) { *p++ = ' '; p = put_num(p, have); memcpy(p, " OF ", 4); p += 4; p = put_num(p, need); }
    *p = 0;
    pr_text_centre(118, line, 6, BR_TEXT);
}

/* ── sound: the UVMC2's jack, where there is one ──────────────────────── */
#ifndef VPY_DUAL_CORE
static int s_jack;
static void jack_sink(const int16_t *l, const int16_t *r, int n) { uvm2_jack_write_lr(l, r, n); }
#endif
void pr_sound_frame(void)
{
#ifndef VPY_DUAL_CORE
    if (s_jack) vpyimpact_pcm(uvm2_jack_space());   /* what the DAC can take, every frame */
#endif
}

/* ── the cards ─────────────────────────────────────────────────────────── */
typedef struct { const char *title; const char *lines[7]; } card_t;

/* THE FONT HAS LETTERS, DIGITS AND . ! < > ONLY: a hyphen, a slash, a comma, a
 * colon or a question mark draws as a space (counted on the host: 0 strokes). So
 * the cards point with ">" and count with "OF". */
static const card_t TITLE = { "VECTREX PLAYROOM", {
    "FOUR ROOMS. FOUR PIECES.",
    "WALK INTO A DOOR TO PLAY.",
    "",
    "STICK UP > WALK. ALL UP > RUN",
    "STICK LEFT OR RIGHT > TURN",
    "BUTTON 2 > WAVE",
    0 } };

static const card_t CARDS[N_ROOMS] = {
    { "CRATES", {
        "KNOCK ALL 8 BLOCKS",
        "OFF THE PAD.",
        "2 SHOTS BREAK A CRATE",
        "AND ITS BLAST PUSHES.",
        "",
        "STICK > AIM   1 > SHOOT",
        "2 > DROP A HEAVY BALL" } },
    { "JELLY", {
        "BLOW THE JELLY INTO",
        "THE RING AND KEEP IT",
        "THERE FOR 2 SECONDS.",
        "",
        "STICK > WIND   1 > HOP",
        "2 > DROP THE JELLY CUBE",
        0 } },
    { "SHAPES", {
        "SHOOT 5 SHAPES TO PIECES.",
        "THEY BREAK FROM WHERE",
        "YOU HIT THEM AND THE",
        "NEXT ONE BUILDS ITSELF.",
        "",
        "STICK > AIM   1 > SHOOT",
        0 } },
    { "FLYER", {
        "HIT THE FLYER 3 TIMES.",
        "IT FADES WITH DISTANCE.",
        "WITH HEADPHONES ON",
        "YOU HEAR IT GO BY.",
        "",
        "STICK > AIM   1 > SHOOT",
        0 } },
};

/* A card: the title drops in and settles (ease out-back), then its lines appear
 * one after another; the last line says what the buttons do. */
static void draw_card(const card_t *c, int t, const char *foot)
{
    const int y = vpy_tween(150, 78, t, TITLE_IN, vpy_ease_out_back);
    pr_text_centre(y, c->title, 12, 127);
    int row = 0;
    for (int i = 0; i < 7 && c->lines[i]; i++, row++)
        if (t >= TITLE_IN + i * LINE_EVERY && c->lines[i][0])
            pr_text_centre(46 - row * 15, c->lines[i], 7, BR_TEXT);
    if ((t / 25) & 1 || t < TITLE_IN) return;     /* the prompt blinks */
    pr_text_centre(-92, foot, 6, BR_HINT);
}

static void draw_readout(void)
{
    const vpy_draw_stats_t *ds = vpy_draw_stats();
    vpy_set_text_size(5);
    vpy_set_intensity(60);
    vpy_print_text(-120, -114, "STK"); vpy_print_number(-100, -114, (long)ds->strokes);
    vpy_print_text(  60, -114, "DROP"); vpy_print_number(86, -114, (long)(ds->dropped
#ifndef VPY_DUAL_CORE
                                                                         + uvm2_stats.dropped
#endif
                                                                         ));
#if defined(PLAYROOM_RESET_DIAG) && !defined(VPY_DUAL_CORE)
    /* the reset button's watch (uvm2_core1.c): presses it saw, the longest in tenths of a
     * second, the T1 high byte its two reads agreed on, polls made. A diagnostic build
     * (make uvm2 EXTRA_DEFS="-DUVM2_RESET_WATCH_ONLY -DPLAYROOM_RESET_DIAG"): it counts
     * and never resets. */
    vpy_print_text(-120, -102, "RST"); vpy_print_number(-100, -102, (long)(uvm2_reset_seen % 10000u));
    vpy_print_text( -60, -102, "MAX"); vpy_print_number(-40, -102, (long)(uvm2_reset_held_max_us / 100000u));
    vpy_print_text(   0, -102, "T1");  vpy_print_number(16, -102, (long)uvm2_reset_last_t1);
    vpy_print_text(  60, -102, "POLL"); vpy_print_number(86, -102, (long)(uvm2_reset_polls % 10000u));
#endif
}

/* ── the rooms, by number ──────────────────────────────────────────────── */
static void room_enter(int r)
{
    switch (r) {
    case R_CRATES: crates_enter(); break;
    case R_JELLY:  jelly_enter();  break;
    case R_SHAPES: shapes_enter(); break;
    case R_FLYER:  flyer_enter();  break;
    default:       finale_enter(); break;
    }
}
static int room_frame(int r)
{
    switch (r) {
    case R_CRATES: return crates_frame();
    case R_JELLY:  return jelly_frame();
    case R_SHAPES: return shapes_frame();
    case R_FLYER:  return flyer_frame();
    default:       return finale_frame();
    }
}

static void go(int state) { s_state = state; s_t = 0; }

/* button 4 leaves; held with 1 it is the SDK's HUD instead */
static int leave(void) { return pr_pressed(4) && !vpy_j1_button(1); }

static void setup(void)
{
    hub_build();
    crates_build();
    jelly_build();
    shapes_build();
    flyer_build();
    finale_build();
    vpyimpact_reset();
#ifndef VPY_DUAL_CORE
    s_jack = uvm2_jack_init();
    if (s_jack) vpyimpact_set_pcm(jack_sink, UVM2_JACK_RATE, 1);
#endif
    hub_enter(-1);
    go(S_TITLE);
#ifdef PLAYROOM_HOST
    /* tools/host_render.c only: ROOM=n starts at room n's card, DONE=n with n pieces */
    if (getenv("POOLS")) {
        const vpy3d_stats_t *v = vpy3d_stats();
        printf("vpy3d pools after building every mesh: verts %u faces %u face_idx %u edges %u, overflow %u\n",
               v->verts, v->faces, v->face_idx, v->edges, v->overflow);
    }
    if (getenv("DONE")) for (int i = 0; i < atoi(getenv("DONE")) && i < N_ROOMS; i++) s_done[i] = 1;
    if (getenv("ROOM")) {
        s_room = atoi(getenv("ROOM"));
        if (s_room == R_FINALE) { room_enter(R_FINALE); go(S_FINALE); } else go(S_CARD);
    }
#endif
}

static void loop(void)
{
    s_t++;
    read_buttons();
    switch (s_state) {
    case S_TITLE:
        draw_card(&TITLE, s_t, "BUTTON 1 > START");
        if (s_t > TITLE_IN && pr_pressed(1)) go(S_HUB);
        break;
    case S_HUB: {
        const int r = hub_frame(1);
        if (r >= 0 && r < N_ROOMS) { s_room = r; go(S_CARD); }
        else if (r == R_FINALE) { s_room = R_FINALE; room_enter(R_FINALE); go(S_FINALE); }
        break;
    }
    case S_CARD:
        draw_card(&CARDS[s_room], s_t, "BUTTON 1 > GO   4 > BACK");
        if (s_t > TITLE_IN && pr_pressed(1)) { room_enter(s_room); go(S_PLAY); }
        else if (leave()) { hub_enter(s_room); go(S_HUB); }
        break;
    case S_PLAY:
        if (room_frame(s_room)) { s_done[s_room] = 1; go(S_CLEARED); }
        else if (leave()) { hub_enter(s_room); go(S_HUB); }
        break;
    case S_CLEARED:
        room_frame(s_room);                 /* the room carries on under the banner */
        if ((s_t / 8) & 1) pr_text_centre(20, "CLEARED!", 14, 127);
        if (s_t >= CLEARED_FRAMES) go(S_EARNED);
        break;
    case S_EARNED: {
        static const card_t EARNED = { "PIECE EARNED", { "", "IT IS WAITING FOR YOU", "ON ITS PEDESTAL.", 0 } };
        static const card_t LAST   = { "ALL 4 PIECES", { "", "THE MIDDLE OF THE HUB", "IS OPEN. GO UP THE STEPS.", 0 } };
        draw_card(pr_done_count() == N_ROOMS ? &LAST : &EARNED, s_t, "BUTTON 1 > BACK TO THE HUB");
        if (s_t > TITLE_IN && pr_pressed(1)) { hub_enter(s_room); go(S_HUB); }
        break;
    }
    case S_FINALE:
        finale_frame();
        if (leave()) { hub_enter(-1); go(S_HUB); }
        break;
    }
    pr_sound_frame();
    draw_readout();
}

int main(void)
{
    vpy_run(setup, loop);   /* never returns */
    return 0;
}
