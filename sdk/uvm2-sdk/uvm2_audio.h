/*
 * uvm2_audio.h — compiled .vmus / .vsfx playback on the halted bus.
 * Ticked once per frame from SYS_WAIT_RECAL; see uvm2_audio.c.
 */
#ifndef UVM2_AUDIO_H
#define UVM2_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SYS_PLAY_MUSIC — start a compiled .vmus track. Re-issuing the track that is
 * already playing is a no-op, so a game may call this every frame. */
void uvm2_play_music(const uint8_t *data);

/* SYS_STOP_MUSIC — stop and silence all three channels. */
void uvm2_stop_music(void);

/* SYS_PLAY_SFX — start a compiled .vsfx on channel C, over the music. */
void uvm2_play_sfx(const uint8_t *data);

/* Advance both sequencers by one frame. Must run between frames, while /ZERO
 * clamps the beam: PSG writes drive Port B and would disturb a draw. */
void uvm2_audio_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* UVM2_AUDIO_H */
