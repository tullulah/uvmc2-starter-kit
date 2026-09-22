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

void voice_init(int num) { (void)num; }

void sample_start(int channel, int samplenum, int loop) { v_playSample(samplenum, channel, loop); }

void sample_set_freq(int channel, int freq)     { (void)channel; (void)freq; }
void sample_set_volume(int channel, int volume) { (void)channel; (void)volume; }
void sample_adjust(int channel, int mode)       { (void)channel; (void)mode; }

void sample_stop(int channel) { v_stopSample(channel); }
void sample_end(int channel)  { v_stopSample(channel); }  /* stop the loop (thrust/roar) when the game ends it */

int  sample_playing(int channel) { return v_samplePlaying(channel); }

void free_samples(void)  {}
void mute_sound(void)    {}
void restore_sound(void) {}

void aae_play_streamed_sample(int channel, unsigned char *data, int len, int freq, int volume)
{ (void)channel; (void)data; (void)len; (void)freq; (void)volume; }
