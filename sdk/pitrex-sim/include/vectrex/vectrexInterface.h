/*
 * PiTrex SDK host contract — DECLARATIONS ONLY.
 *
 * This header is the ABI the IDE simulator implements. ANY pitrex program
 * (DOOM, a hand-written C game, VPy-pitrex output) links against exactly this
 * surface — the simulator is agnostic to which program it runs. The host
 * implementation lives in sdk_host.c and bridges each call to JS hooks on
 * `Module.pitrex` provided by the harness / IDE panel.
 *
 * Deliberately free of the bare-metal SDK's transitive includes (uspi.h,
 * pi_support.h, osWrapper.h, ...): those pull in hardware code that cannot
 * compile/run under WASM. A sim build puts this include dir AHEAD of the real
 * SDK so this shim wins.
 */
#ifndef PITREX_SIM_VECTREXINTERFACE_H
#define PITREX_SIM_VECTREXINTERFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input state — written by v_readButtons() / v_readJoystick1Analog(),
 * read directly by the game afterwards. */
extern uint8_t currentButtonState;
extern int8_t  currentJoy1X;
extern int8_t  currentJoy1Y;

/* Lifecycle */
void     vectrexinit(int mode);        /* pre-SDK bring-up (SD mount etc.) */
void     v_init(void);
void     v_setRefresh(int hz);

/* Frame boundary — presents the accumulated vectors and yields to the host. */
void     v_WaitRecal(void);

/* The one drawing primitive: a vector line in PiTrex space
 * (roughly x:[-18000,18000], y:[-24000,24000]), brightness 0-127. */
void     v_directDraw32(int32_t xStart, int32_t yStart,
                        int32_t xEnd,  int32_t yEnd, uint8_t brightness);

/* Colour for subsequent draws, 0x00RRGGBB. 0 = the display's own default, which is what
 * a monochrome Vectrex always uses and what every existing game gets for free.
 *
 * Kept because colour vector hardware is coming (Masteroids drives colour arcade
 * monitors) and because a half-finished port needs its untraced RASTER art tellable
 * apart. Vectors stay monochrome unless a port asks otherwise. Backends without colour
 * ignore this. */
void     v_setColour(uint32_t rgb);

/* Input */
uint8_t  v_readButtons(void);          /* also stores into currentButtonState */
void     v_readJoystick1Analog(void);  /* stores into currentJoy1X / currentJoy1Y */

/* Time */
uint32_t v_millis(void);

/* Audio — one AY-3-8910 register write. */
void     v_setSoundAY(uint8_t reg, uint8_t val);   /* legacy name (some games use it) */
void     v_writePSG(uint8_t reg, uint8_t val);     /* real PiTrex SDK name — libvpy uses this */

/* Digitised-sample audio (e.g. AAE Sega-G80 arcade sound). A game with sampled
 * SFX plays voice `voice` (a mixing channel) with sample bank index `idx`,
 * `loop`=1 to repeat. The sim mixes voices in Web Audio; on RP2350 these are
 * no-ops for now (HW needs a software mixer → PSG DAC stream). */
void     v_playSample(int idx, int voice, int loop);
void     v_stopSample(int voice);
int      v_samplePlaying(int voice);

#ifdef __cplusplus
}
#endif

#endif /* PITREX_SIM_VECTREXINTERFACE_H */
