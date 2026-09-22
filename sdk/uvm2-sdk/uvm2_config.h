/* uvm2_config.h — BEAM CALIBRATION, which belongs to the CONSOLE and not to the game.
 *
 * The same tube, the same drift and the same scale error are suffered by all 44 ports, so
 * the calibration is ONE thing and it is shared. What IS the game's own — VCAP, MIN_T1, the
 * scale of its geometry — stays where it is, in its Makefile.
 *
 * THE FLOW: every game calls `uvm2_config_load()` at startup. If a calibration exists it is
 * applied and the game carries on. If not, the game opens the wizard and saves on the way
 * out.
 *
 * WHERE IT COMES FROM AND WHERE IT IS SAVED, deliberately not the same place:
 *
 *   - READS `config/uvm2.cfg` off the SD card if it is there. It is text, so it can be read
 *     and edited from the PC — which, while we are still tuning this, is worth more than
 *     convenience.
 *   - With no file, it reads the RP2350 FLASH.
 *   - ALWAYS SAVES to FLASH. The FAT reader in `uvm2_sd.c` is READ-ONLY: creating a file
 *     means allocating clusters and rewriting both copies of the FAT, and getting that
 *     wrong corrupts the user's card. Flash also survives swapping cards, which is the
 *     right behaviour for something that describes the CONSOLE.
 *
 * WHAT IS INSIDE, and why these four and not the eighteen drawing knobs. The error between
 * what is asked for and what the beam actually travels has TWO terms:
 *
 *     proportional to the length  -> the scale
 *     fixed per stroke            -> the start of the ramp
 *
 * Long vectors are dominated by the first; text, which is many short strokes, by the second.
 * That is why a closed polygon that OPENS accuses the FIXED term: a scale error would make
 * it smaller but it would still be closed. The other two are the zero and the brightness,
 * which do not correct geometry but do change what you see while adjusting it. */
#ifndef UVM2_CONFIG_H
#define UVM2_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uvm2_config {
    int32_t scale;      /* DRAW_SCALE: the divisor. LARGER = SHORTER strokes. */
    int32_t t1_tail_q8; /* T1_EXTRA_Q8: the REAL length of the ramp, in 1/256 of a count.
                         * The 6522 counts t1 + 1.5 on a one-shot, so the honest value is
                         * not 0; measure it by closing a polygon of short strokes. */
    int32_t zero;       /* uvm2_zero_offset: the value primed into the zero reference. */
    int32_t bright;     /* default Z, 0..127. */
    /* SAMPLING OF THE Y SAMPLE-AND-HOLD, in E cycles: the minimum for a tiny jump and the
     * cap for a full-scale one. It belongs to the CONSOLE — the capacitor is C304 and its
     * Ron is that of whichever 4052 is fitted — and it is the only one of the three channels
     * that changes GEOMETRY: if Y is not sampled long enough, a stroke that only asks for X
     * comes out slanted. PiTrex calibrates four hold times for the same reason.
     * Defaults 4 and 15. */
    int32_t hold_y_min;
    int32_t hold_y_max;
    /* FINE TRIM OF THE NEGATIVE RATES, per axis, in 1/256. The DAC does not deviate the same
     * at +k as at -k, so a diagonal where the two axes ask for OPPOSITE numbers does not come
     * out at 45 degrees and the outbound and return legs separate. Neither the zero (which
     * moves both axes together) nor the scale (symmetric) can reach it. See NEG_RATE_X/Y in
     * vectrex-draw. Default 0. */
    int32_t neg_rate_x;
    int32_t neg_rate_y;
    /* PER-JUMP DRIFT, per axis, in 1/256. The beam does not land where it is told and the
     * error accumulates jump after jump: uncompensated, a column of lines asked for at the
     * SAME x comes out cascading to one side. Measured on the console on 2026-08-12:
     * X -16/256 and Y -112/256, seven times more in Y because Y goes
     * through the sample-and-hold and X goes straight to the DAC. Default 0 = no
     * compensation. */
    int32_t drift_x;
    int32_t drift_y;
    /* TWO GAME SETTINGS, not beam ones. They live in the same file because this is the file
     * that already gets read and written; an old file without these lines simply keeps the
     * stock values. */
    int32_t hz;         /* refresh cap: 50 or 60 (the mains), or 0 = flat out, unlocked. */
    int32_t start_menu; /* 1 = menu on power-up; 0 = straight into the game (button 4 forces
                         * the menu) */
    int32_t rotate;     /* 1 = drawing rotated 90 degrees, for horizontal arcade games. */
};
extern volatile int32_t uvm2_setting_hz, uvm2_setting_menu;

/* WHICH OF ITS OWN SETTINGS THIS GAME USES. Beam calibration belongs to the CONSOLE and is
 * shared; this is what belongs to the game, and each one declares only what it has — so
 * Donkey Kong, which is vertical, does not show a horizontal/vertical switch, and hiding the
 * menu in one game does not hide it in all of them. */
enum {
    UVM2_SETTING_HZ     = 1u,  /* refresh 50/60/free */
    UVM2_SETTING_MENU   = 2u,  /* menu on power-up */
    UVM2_SETTING_ROTATE = 4u,  /* drawing rotated 90 degrees */
};

/** Declares the game's name (8.3, no extension: "MHAVOC") and which of its own settings it
 *  uses. Call it BEFORE uvm2_config_load. Its settings go in `config/<NAME>.CFG`, layered on
 *  top of `config/uvm2.cfg`, which is the console's. Without calling this you get a single
 *  file and no game settings, which is how this used to behave. */
void uvm2_config_game(const char *name, unsigned settings);
unsigned uvm2_config_game_settings(void);

/** Fills `c` with what the knobs are worth RIGHT NOW. */
void uvm2_config_current(struct uvm2_config *c);

/** Applies `c` to the live knobs. */
void uvm2_config_apply(const struct uvm2_config *c);

/** What `uvm2_config_load()` returned inside `uvm2_draw_init`, so the game does not have to
 *  read the card again just to find out whether to open the wizard. */
extern volatile int uvm2_have_calibration;

/** 1 if there was a saved calibration (and it is now applied); 0 if there is none. */
int  uvm2_config_load(void);

/** THE CALIBRATION SCREEN. Runs its own frame loop until the user presses button 4; returns
 *  1 if the calibration was saved.
 *
 *  The game opens it like this:   if (!uvm2_have_calibration) uvm2_config_wizard();
 *
 *  It draws TWO squares of the same size, one from 4 long strokes and one from 40 short
 *  ones: whichever opens up tells you which of the two error terms to move. See
 *  uvm2_wizard.c. */
int  uvm2_config_wizard(void);

/** The same screen, but drawing THE GAME'S OWN FIGURE instead of the SDK pattern.
 *
 *  Calibrating against the drawing that actually bothers you is worth more than calibrating
 *  against a laboratory pattern: the game passes its own figure, already centred and at its
 *  own scale, and the wizard knows nothing about it. With `figure` null this is exactly
 *  `uvm2_config_wizard()`. */
int  uvm2_config_wizard_with(void (*figure)(void));

/** Saves the current calibration. 1 if it could.
 *
 *  It first tries to write `config/uvm2.cfg` IN PLACE, which touches neither the FAT nor the
 *  directory and is the cheapest thing it can do. If the file does not exist, it CREATES it
 *  — with its folder if need be. */
int  uvm2_config_save(void);

/** Lets (or does not let) `uvm2_config_save` CREATE the file when it does not exist. ON by
 *  default. Turn it off for a card that must not be touched under any circumstances. */
void uvm2_config_allow_create(int enable);

#ifdef __cplusplus
}
#endif
#endif /* UVM2_CONFIG_H */
