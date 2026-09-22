/*
 * vpy.h — the VPy builtin runtime, in C.
 *
 * Reimplements the procedural VPy builtins (draw / input / math / text) ON TOP
 * of the minimal PiTrex SDK contract (vectrexInterface.h: v_directDraw32,
 * v_WaitRecal, v_readButtons, v_readJoystick1Analog, v_millis, v_setSoundAY).
 * Because it uses ONLY that contract, the same C code runs identically on real
 * PiTrex hardware and in the Vectrex Studio emulator panel (WASM + host shim).
 *
 * Coordinates use the same logical VPy space as the .vpy language (roughly
 * -127..127, +Y up); vpy scales by x127 to PiTrex units, exactly like the VPy
 * pitrex codegen. Brightness is 0..127.
 */
#ifndef VPY_H
#define VPY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle ---- */
void vpy_init(void);                 /* vectrexinit + v_init + v_setRefresh(60) */
void vpy_frame_begin(void);          /* v_WaitRecal + refresh input snapshot   */
void vpy_run(void (*setup)(void), void (*loop)(void)); /* init; setup(); loop forever */

/* ---- drawing (VPy space, brightness 0..127) ---- */
void vpy_set_intensity(int b);
void vpy_move(int x, int y);
void vpy_draw_line(int x0, int y0, int x1, int y1, int b);
void vpy_draw_circle(int cx, int cy, int r, int b);
void vpy_draw_rect(int x, int y, int w, int h, int b);           /* x,y = lower-left */
void vpy_draw_filled_rect(int x, int y, int w, int h, int b);
void vpy_draw_polygon(const int *xy, int n, int b);              /* n vertices, xy[2n] */
void vpy_draw_ellipse(int cx, int cy, int rx, int ry, int b);    /* 16-segment ellipse */

/* ---- compiled vector sprites (.vec) --------------------------------------
 * `data` is a vector path stream compiled from a .vec by
 *   `vpy_cli compile-asset <file>.vec --format c --out <name>.h`
 * (same path/segment geometry compiler as the ARM/PiTrex backend). The sprite
 * is drawn at VPy position (x,y) using the asset's own per-path intensity.
 * Bezier segments are tessellated into lines (the host contract only exposes
 * v_directDraw32). draw_vector_ex adds horizontal mirror + an optional
 * intensity override (pass intensity<=0 to keep each path's own intensity). */
void vpy_draw_vector(const unsigned char *data, int x, int y);
void vpy_draw_vector_ex(const unsigned char *data, int x, int y, int mirror, int intensity);

/* ---- compiled animations (.vanim) ----------------------------------------
 * `anim` is a position-independent anim descriptor compiled from a .vanim by
 *   `vpy_cli compile-asset <file>.vanim --format c --out <name>.h`
 * and `sprites` its companion frame-sprite pointer table (`{NAME}_anim_sprites`),
 * each entry pointing at a compiled frame `.vec` array. vpy_draw_anim ticks the
 * animation (advancing frames by each frame's duration, looping or freezing per
 * the asset) and draws the current frame at VPy position (x,y). Bit-exact with
 * the VPy pitrex `pitrex_draw_anim`: a SINGLE global cursor is shared by all
 * calls (one animation ticked per frame). */
void vpy_draw_anim(const unsigned char *anim, const unsigned char *const *sprites,
                   int x, int y, int mirror);

/* ---- text ---- */
void vpy_set_text_size(int s);       /* glyph scale (VPy units per grid unit ~ s) */
void vpy_print_text(int x, int y, const char *s);
void vpy_print_number(int x, int y, long n);

/* ---- input ----
 * All read the SDK input globals (currentJoy1X/Y, currentButtonState) directly;
 * they are refreshed each frame by vpy_frame_begin() (vpy_run path) or by the
 * VPy game loop. vpy_update_buttons() forces an immediate re-read mid-frame. */
int vpy_j1_x(void);                  /* -127..127 */
int vpy_j1_y(void);                  /* -127..127 (+ = up) */
int vpy_j1_button(int n);            /* n = 1..4 -> 0/1 (bit n-1 of button state) */
void vpy_update_buttons(void);       /* v_readButtons + v_readJoystick1Analog */

/* ---- math ---- */
int vpy_abs(int v);
int vpy_min(int a, int b);
int vpy_max(int a, int b);
int vpy_clamp(int v, int lo, int hi);
int vpy_sin(int a);                  /* a: 0..127 = full circle; returns -127..127 */
int vpy_cos(int a);
int vpy_sqrt(int v);
int vpy_atan2(int y, int x);         /* returns 0..127 (full circle) */
int vpy_rand(void);
int vpy_rand_range(int lo, int hi);
void vpy_seed(unsigned s);

/* ---- sound (basic tone on PSG channel A) ---- */
void vpy_beep(int on);
void vpy_tone(int period, int volume); /* period 12-bit, volume 0..15; 0 vol = off */

/* ---- compiled music / SFX playback ---------------------------------------
 * `data` is a PSG event stream compiled from a .vmus/.vsfx by
 *   `vpy_cli compile-asset <file> --format c --out <name>.h`
 * (same notes->PSG compiler as the ARM/PiTrex backend). A simple 50 Hz
 * sequencer walks the stream, writing PSG registers via v_setSoundAY each frame.
 * vpy_run() drives vpy_music_update()/vpy_sfx_update() automatically. */
void vpy_play_music(const unsigned char *data);  /* start/replace music playback */
void vpy_music_update(void);                      /* advance one frame (auto-called) */
void vpy_stop_music(void);                         /* mute channels + stop */
void vpy_play_sfx(const unsigned char *data);     /* trigger a one-shot SFX (channel C) */
void vpy_sfx_update(void);                         /* advance one frame (auto-called) */

/* ---- compiled levels (.vplay) --------------------------------------------
 * `level` is a level byte image compiled from a .vplay by
 *   `vpy_cli compile-asset <file>.vplay --format c --out <name>.h`
 * (same header/object layout as the ARM/PiTrex level backend). Objects
 * reference sprites by INDEX; `sprites` is the companion pointer table the
 * generated header emits alongside the image (`{NAME}_level_sprites`), each
 * entry pointing at a compiled `.vec` array. Both are passed to LOAD_LEVEL:
 *   LOAD_LEVEL(WORLD_level, WORLD_level_sprites);
 * SHOW_LEVEL() draws every visible object (BG + GP + FG) offset by the camera
 * and culled to the screen, calling vpy_draw_vector_ex per object — mirroring
 * pitrex_show_level. UPDATE_LEVEL() advances gameplay-object physics (velocity
 * + gravity) exactly like pitrex_update_level. Camera is in VPy units. */
void vpy_load_level(const unsigned char *level, const unsigned char *const *sprites);
void vpy_show_level(void);                          /* draw all layers with camera offset */
void vpy_update_level(void);                        /* advance GP object physics one frame */
void vpy_set_camera_x(int x);
void vpy_set_camera_y(int y);
int  vpy_get_camera_x(void);
int  vpy_get_camera_y(void);
/* Level scalar accessors (bridged: the VPy pitrex codegen calls these). */
int  vpy_get_scroll_limit_left(void);
int  vpy_get_scroll_limit_right(void);
int  vpy_get_scroll_limit_top(void);
int  vpy_get_scroll_limit_bottom(void);
int  vpy_get_level_floor_y(void);
int  vpy_level_collision_y(int px, int py, int hh);
int  vpy_level_collision_x(int px, int py, int hw, int hy);

/* Enemy runtime (bridged: the VPy pitrex codegen calls these). `img` is a
 * position-independent `_NAME_ENEMIES_C` image; `sprites[i]` a `_{SPRITE}_VEC`
 * image. Full AI (patrol/area/wander) + static & anim draw, all render-verified
 * bit-exact vs the inline pitrex enemy runtime. */
void vpy_spawn_enemies(const unsigned char *img, const unsigned char *const *sprites);
void vpy_update_enemies(void);
void vpy_draw_enemies(void);
void vpy_kill_enemy(int idx);
/* Enemy pool accessors (read/write the same s_enemies pool). */
int  vpy_get_enemy_active(int idx);
int  vpy_get_enemy_x(int idx);
int  vpy_get_enemy_y(int idx);
int  vpy_get_enemy_state(int idx);
void vpy_set_enemy_x(int idx, int x);
void vpy_set_enemy_y(int idx, int y);
void vpy_set_enemy_state(int idx, int st);
void vpy_set_enemy_dir(int idx, int dir);
void vpy_enemy_fire_event(int idx, int hash);
/* Runtime-hashing wrapper matching the VPy `ENEMY_FIRE_EVENT(idx, "name")`
 * builtin: computes the SAME FNV-1a u8 hash the codegen bakes in and dispatches
 * it through the enemy's state machine. */
void vpy_enemy_fire_event_str(int idx, const char *event);

/* Optional VPy-style uppercase aliases so migrated .vpy reads naturally. */
#ifdef VPY_SHORT_NAMES
#define SET_INTENSITY   vpy_set_intensity
#define MOVE            vpy_move
#define DRAW_LINE       vpy_draw_line
#define DRAW_CIRCLE     vpy_draw_circle
#define DRAW_RECT       vpy_draw_rect
#define DRAW_FILLED_RECT vpy_draw_filled_rect
#define DRAW_POLYGON    vpy_draw_polygon
#define DRAW_ELLIPSE    vpy_draw_ellipse
#define DRAW_VECTOR     vpy_draw_vector
#define DRAW_VECTOR_EX  vpy_draw_vector_ex
#define DRAW_ANIM       vpy_draw_anim
#define SET_TEXT_SIZE   vpy_set_text_size
#define PRINT_TEXT      vpy_print_text
#define PRINT_NUMBER    vpy_print_number
#define J1_X            vpy_j1_x
#define J1_Y            vpy_j1_y
#define UPDATE_BUTTONS  vpy_update_buttons
#define PLAY_MUSIC      vpy_play_music
#define STOP_MUSIC      vpy_stop_music
#define PLAY_SFX        vpy_play_sfx
#define LOAD_LEVEL      vpy_load_level
#define SHOW_LEVEL      vpy_show_level
#define UPDATE_LEVEL    vpy_update_level
#define SET_CAMERA_X    vpy_set_camera_x
#define SET_CAMERA_Y    vpy_set_camera_y
#define GET_CAMERA_X    vpy_get_camera_x
#define GET_CAMERA_Y    vpy_get_camera_y
/* Level accessors */
#define GET_LEVEL_FLOOR_Y      vpy_get_level_floor_y
#define GET_SCROLL_LIMIT_LEFT   vpy_get_scroll_limit_left
#define GET_SCROLL_LIMIT_RIGHT  vpy_get_scroll_limit_right
#define GET_SCROLL_LIMIT_TOP    vpy_get_scroll_limit_top
#define GET_SCROLL_LIMIT_BOTTOM vpy_get_scroll_limit_bottom
#define LEVEL_COLLISION_X       vpy_level_collision_x
#define LEVEL_COLLISION_Y       vpy_level_collision_y
/* Enemies. SPAWN_ENEMIES(data, sprites) takes the compiled `_NAME_ENEMIES_C`
 * image + its `_NAME_ENEMY_SPRITES` table (VPy resolves those from the level
 * name; C passes the symbols explicitly). */
#define SPAWN_ENEMIES   vpy_spawn_enemies
#define UPDATE_ENEMIES  vpy_update_enemies
#define DRAW_ENEMIES    vpy_draw_enemies
#define KILL_ENEMY      vpy_kill_enemy
#define GET_ENEMY_ACTIVE vpy_get_enemy_active
#define GET_ENEMY_X     vpy_get_enemy_x
#define GET_ENEMY_Y     vpy_get_enemy_y
#define GET_ENEMY_STATE vpy_get_enemy_state
#define SET_ENEMY_X     vpy_set_enemy_x
#define SET_ENEMY_Y     vpy_set_enemy_y
#define SET_ENEMY_STATE vpy_set_enemy_state
#define SET_ENEMY_DIR   vpy_set_enemy_dir
/* ENEMY_FIRE_EVENT(idx, "name") — runtime-hashed exactly like the VPy builtin. */
#define ENEMY_FIRE_EVENT vpy_enemy_fire_event_str
/* Digital buttons: VPy exposes J1_BUTTON_1..4 (); C maps each to vpy_j1_button(n). */
#define J1_BUTTON_1()   vpy_j1_button(1)
#define J1_BUTTON_2()   vpy_j1_button(2)
#define J1_BUTTON_3()   vpy_j1_button(3)
#define J1_BUTTON_4()   vpy_j1_button(4)
/* Debug print is a no-op on the minimal C runtime (no host console). */
#define DEBUG_PRINT_LABELED(label, val) ((void)(val))
#endif

#ifdef __cplusplus
}
#endif

#endif /* VPY_H */
