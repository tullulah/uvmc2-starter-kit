/* Real sample playback for Tac/Scan — replaces AAE's stub samples.c. Routes the
 * AAE sample interface (called by SegaG80snd.c's TacScan_sh_w / tacscan_sh_update)
 * to the SDK's v_playSample path: in the WASM sim the JS side (PitrexSimView)
 * decodes the .wav set and mixes voices in Web Audio; on rp2350 it's silent for
 * now (HW needs a software mixer → PSG DAC). The sample INDEX is the position in
 * gamesamp.h's tacscan_samples[] minus the leading ".zip" entry, i.e. 0="01.wav",
 * 3="plaser.wav", … — exactly what TacScan_sh_w passes as `sound`.
 */
extern void v_playSample(int idx, int voice, int loop);
extern void v_stopSample(int voice);
extern int  v_samplePlaying(int voice);

/* ── THE FORK: THE CONSOLE'S CHIP OR THE CARTRIDGE'S JACK ──────────────────────
 *
 * The same 22 sounds and the same indices, two outputs. `uvm2_setting_audio` is the player's
 * choice from the AUDIO line of the menu (0 = jack, 1 = console), and it is read HERE, per
 * sound, rather than being latched at startup — so switching in the menu takes effect on the
 * next sound instead of on the next power-up.
 *
 * `ts_jack_ready()` is the other half of the condition and it is not redundant: the jack does
 * not exist on the debug cartridge and the bundle may not be on the card. When it is not
 * ready the console path is used whatever the setting says, because a setting the hardware
 * cannot honour must not mean silence.
 *
 * Only on the .um2 runtime. The host harness and the WASM sim have neither jack nor setting,
 * and there the sounds go where they always went. */
#ifdef UVM2_PICO_RUNTIME
#include <stdint.h>
#include "ts_jack.h"
extern volatile int32_t uvm2_setting_audio;
static int to_jack(void) { return uvm2_setting_audio == 0 && ts_jack_ready(); }
#else
static int to_jack(void) { return 0; }
#define ts_jack_play(v, i, l) ((void)0)
#define ts_jack_stop(v)       ((void)0)
#define ts_jack_playing(v)    0
#endif

void voice_init(int num) { (void)num; }

void sample_start(int channel, int samplenum, int loop)
{
    if (to_jack()) ts_jack_play(channel, samplenum, loop);
    else           v_playSample(samplenum, channel, loop);
}

void sample_set_freq(int channel, int freq)     { (void)channel; (void)freq; }
void sample_set_volume(int channel, int volume) { (void)channel; (void)volume; }
void sample_adjust(int channel, int mode)       { (void)channel; (void)mode; }

/* BOTH PATHS ARE STOPPED, not just the current one. The setting can change between the
 * start of a sound and its stop — that is exactly what the menu does to a looping voice — and
 * a stop that only reached the path in favour at that instant would leave the tunnel humming
 * on the other one with nothing able to silence it. */
void sample_stop(int channel) { v_stopSample(channel); ts_jack_stop(channel); }
void sample_end(int channel)  { v_stopSample(channel); ts_jack_stop(channel); }

int  sample_playing(int channel)
{
    return to_jack() ? ts_jack_playing(channel) : v_samplePlaying(channel);
}

void free_samples(void)  {}
void mute_sound(void)    {}
void restore_sound(void) {}

void aae_play_streamed_sample(int channel, unsigned char *data, int len, int freq, int volume)
{ (void)channel; (void)data; (void)len; (void)freq; (void)volume; }
