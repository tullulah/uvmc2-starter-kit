/* ts_jack.h — Tac/Scan's sounds out of the cartridge's 16-bit jack.
 *
 * The console path (uvm2_smp, the .vsm bundle) plays the same sounds through the PSG's
 * 4-bit volume register, injected into the draw list at 12 kHz. This plays them at 16 bits
 * and 32 kHz out of the PT8211 on the cartridge, which touches neither the Vectrex bus nor
 * its sound chip.
 *
 * WHICH ONE IS USED IS THE PLAYER'S CHOICE: the `AUDIO` line in the game's menu
 * (UVM2_SETTING_AUDIO -> uvm2_setting_audio, 0 = jack, 1 = console). samples.c routes it.
 * The SDK deliberately does not: it stores the setting and the game decides what it means.
 */
#ifndef TS_JACK_H
#define TS_JACK_H

/* Brings up the jack and starts loading `TACSCAN.PCM` off the card into PSRAM. Returns 0 if
 * there is no jack on this board or no PSRAM, in which case nothing else here does anything
 * and the console path is the only one. */
int  ts_jack_begin(void);

/* ONE SLICE OF THE LOAD, called once per frame while the title screen says so. 2.64 MB over
 * a bit-banged SPI card is not something to do inside one frame: see the note in ts_jack.c.
 * Returns 1 when there is no more to do (loaded, or failed and given up). */
int  ts_jack_load_step(void);

/* 0..100, for the loading line. */
int  ts_jack_load_percent(void);

/* 1 = the jack is up AND the bundle is in PSRAM, so it can actually play. */
int  ts_jack_ready(void);

/* AAE's sample interface, voice-for-voice. `idx` indexes samples.json exactly as the
 * console path's does, so the two are interchangeable. */
void ts_jack_play(int voice, int idx, int loop);
void ts_jack_stop(int voice);
int  ts_jack_playing(int voice);

/* Mixes and hands samples to the driver. ONCE PER FRAME, and the frame has to stay under
 * ~50 ms or the ring drains and you hear the re-sync. */
void ts_jack_update(void);

#endif /* TS_JACK_H */
