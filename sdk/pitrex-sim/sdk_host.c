/*
 * PiTrex SDK host implementation for the IDE simulator.
 *
 * Implements the contract in include/vectrex/vectrexInterface.h and the FatFs
 * surface in include/baremetal/ff.h ONCE, for any pitrex program. Each call
 * bridges to a JS hook on `Module.pitrex` that the harness / IDE panel provides
 * (draw to a canvas, read host input, present a frame). There is deliberately
 * ZERO game-specific code here: the simulator is agnostic to what it runs.
 *
 * Frame model: the game loops calling v_WaitRecal() once per frame. We present
 * the accumulated vectors and emscripten_sleep(0) — Asyncify suspends the WASM
 * and resumes it on the host's next animation frame. (Same model the existing
 * PitrexCore asm interpreter uses: run until v_WaitRecal.)
 */
#include <stdint.h>
#include <stdio.h>
#include <emscripten.h>

#include "vectrex/vectrexInterface.h"
#include "baremetal/ff.h"

/* ---- Contract globals ---- */
uint8_t currentButtonState = 0;
int8_t  currentJoy1X = 0;
int8_t  currentJoy1Y = 0;

/* ---- JS bridges: the host supplies Module.pitrex.* (all optional) ---- */
EM_JS(void, js_draw_line, (int x0, int y0, int x1, int y1, int b, int rgb), {
    if (Module.pitrex && Module.pitrex.drawLine) Module.pitrex.drawLine(x0, y0, x1, y1, b, rgb);
});
EM_JS(void, js_present, (void), {
    if (Module.pitrex && Module.pitrex.present) Module.pitrex.present();
});
EM_JS(int, js_read_buttons, (void), {
    return (Module.pitrex && Module.pitrex.readButtons) ? (Module.pitrex.readButtons() | 0) : 0;
});
EM_JS(int, js_joy_x, (void), {
    return (Module.pitrex && Module.pitrex.joyX) ? (Module.pitrex.joyX() | 0) : 0;
});
EM_JS(int, js_joy_y, (void), {
    return (Module.pitrex && Module.pitrex.joyY) ? (Module.pitrex.joyY() | 0) : 0;
});
EM_JS(int, js_millis, (void), {
    return (Module.pitrex && Module.pitrex.millis) ? (Module.pitrex.millis() | 0)
                                                   : (Date.now() | 0);
});
EM_JS(void, js_sound_ay, (int reg, int val), {
    if (Module.pitrex && Module.pitrex.soundAY) Module.pitrex.soundAY(reg, val);
});
EM_JS(void, js_sim_save_data, (const char *name, const void *buf, unsigned len), {
    if (Module.pitrex && Module.pitrex.saveData)
        Module.pitrex.saveData(UTF8ToString(name), HEAPU8.slice(buf, buf + len));
});

/* Persist a blob into the running project's folder (the host decides where —
 * the IDE writes <project>/dumps/<session>/<basename>). Generic sim-only debug
 * channel: frame harvests, trace recordings, anything a game wants to keep
 * from a play session without a console-log round-trip. No-op when the host
 * doesn't implement it. Each name should be written once — the host appends. */
void v_simSaveData(const char *name, const void *buf, unsigned len)
{
    js_sim_save_data(name, buf, len);
}

/* ---- vectrexInterface ---- */
static int s_refresh_hz = 50;   /* Vectrex frame rate; the game may change it */

void vectrexinit(int mode) { (void)mode; }
void v_init(void) {}
void v_setRefresh(int hz) { if (hz > 0) s_refresh_hz = hz; }

/* Pace the game loop to the declared refresh: return how many ms v_WaitRecal
 * should sleep so frames advance at ~hz, instead of running flat-out. Keeps a
 * running target on the JS side and re-syncs if it falls behind. */
EM_JS(int, js_frame_delay, (int hz), {
    var now = (typeof performance !== 'undefined') ? performance.now() : Date.now();
    var period = 1000 / (hz > 0 ? hz : 50);
    if (Module._simNextFrame === undefined || Module._simNextFrame < now - 4 * period)
        Module._simNextFrame = now;
    Module._simNextFrame += period;
    var d = Module._simNextFrame - now;
    return (d < 0) ? 0 : (d | 0);
});

/* COLOUR. The Vectrex is monochrome and so is this by default: s_colour 0 means "use the
 * display's own look", which is what every existing game gets without changing a line.
 *
 * It is here because colour vector hardware is coming (the Masteroids board drives colour
 * arcade monitors), and because a port that is still half raster needs its untraced art
 * TELLABLE APART — a colour game's raster is one flat white blob otherwise. So: vectors
 * stay monochrome by default, the raster fallback colours itself from the game's palette.
 *
 * v_setColour(0) restores the default. Backends without colour ignore it. */
static uint32_t s_colour = 0;

void v_setColour(uint32_t rgb) { s_colour = rgb & 0xffffff; }

void v_directDraw32(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t b) {
    js_draw_line((int)x0, (int)y0, (int)x1, (int)y1, (int)b, (int)s_colour);
}

static void snd_tick(void);     /* the .vmus/.vsfx sequencer, below */

void v_WaitRecal(void) {
    /* Advance the sequencer by ELAPSED TIME, not once per frame.  The assets
     * are compiled at 50 Hz, and a game that does not reach 50 fps — every
     * arcade port here runs at 30-47 — would otherwise play its music at that
     * fraction of its tempo.  Both cartridges already avoid this: ours
     * sequences on core 1, and UVM2 counts elapsed bus cycles for the same
     * reason (see the note in uvm2_svc.c, where a draw-heavy game played at
     * half speed).  The simulator has a clock, so it can simply use it. */
    static int last_ms;
    static int acc_ms;
    int now = js_millis();
    if (last_ms) {
        int dt = now - last_ms;
        if (dt > 200) dt = 200;     /* a long stall must not fast-forward the track */
        acc_ms += dt;
        while (acc_ms >= 20) { acc_ms -= 20; snd_tick(); }
    }
    last_ms = now;
    js_present();
    emscripten_sleep(js_frame_delay(s_refresh_hz)); /* pace to the game's refresh */
}

uint8_t v_readButtons(void) {
    currentButtonState = (uint8_t)js_read_buttons();
    return currentButtonState;
}
void v_readJoystick1Analog(void) {
    currentJoy1X = (int8_t)js_joy_x();
    currentJoy1Y = (int8_t)js_joy_y();
}
uint32_t v_millis(void) { return (uint32_t)js_millis(); }

void v_setSoundAY(uint8_t reg, uint8_t val) { js_sound_ay((int)reg, (int)val); }
void v_writePSG(uint8_t reg, uint8_t val)   { js_sound_ay((int)reg, (int)val); }

/* ---- compiled .vmus / .vsfx playback -------------------------------------
 *
 * The cartridges get this from the BIOS (uvm2_audio.c / the rp2350 core-1
 * player, reached by svc #21/#22/#23); libvpy carries its own copy for VPy
 * programs. A C import linked against this host shim had neither, so
 * v_playMusic/v_playSFX were simply missing and any game that called them
 * failed to link — which is why the sim was silent. The sequencer belongs
 * HERE, in the SDK contract, not in one game: every C import gets it.
 *
 * Same event format and the same one-tick-per-frame model as the others, so a
 * track sounds the same in the sim as on the console:
 *   MUSIC [u32 n][u32 loop byte offset][events]   SFX [u32 n][events]
 *   event [delay, num_writes, (reg,val)*n]; num_writes 0xFF loop, 0x00 end.
 * A delay byte is the wait BEFORE its own event fires. */
static const uint8_t *s_mus_base, *s_mus_ptr;
static int s_mus_playing, s_mus_delay, s_mus_primed;
static const uint8_t *s_sfx_ptr;
static int s_sfx_active, s_sfx_delay;
static uint8_t s_psg_mixer = 0x3F;
/* The channel-C mixer bits the running effect asked for. The merge has to work
 * BOTH ways: an effect keeps the music's A/B bits, and a music mixer write must
 * keep the effect's C bits — otherwise any track with noise percussion (which
 * writes register 7 on every drum hit) cuts effects off a couple of frames in. */
static uint8_t s_sfx_cbits = 0x24;      /* 0x24 = tone C and noise C disabled */

static void snd_psg(uint8_t reg, uint8_t val) {
    if (reg == 7) {
        if (s_sfx_active) val = (uint8_t)((val & 0xDB) | (s_sfx_cbits & 0x24));
        s_psg_mixer = val;
    }
    js_sound_ay((int)reg, (int)val);
}

void v_playMusic(const unsigned char *vmus) {
    if (!vmus) return;
    if (s_mus_playing && s_mus_base == vmus) return;  /* re-issue = no-op */
    s_mus_base = vmus;
    s_mus_ptr = vmus + 8;
    s_mus_playing = 1;
    s_mus_delay = 0;
    s_mus_primed = 0;
}

void v_stopMusic(void) {
    s_mus_playing = 0;
    s_mus_ptr = 0;
    snd_psg(8, 0); snd_psg(9, 0); snd_psg(10, 0);
    snd_psg(7, 0x3F);
}

void v_playSFX(const unsigned char *vsfx) {
    if (!vsfx) return;
    s_sfx_ptr = vsfx + 4;
    s_sfx_active = 1;
    s_sfx_delay = 0;
}

static void snd_tick(void) {
    if (s_mus_playing && s_mus_ptr) {
        if (!s_mus_primed) {
            s_mus_primed = 1;           /* one priming frame, as in libvpy */
        } else if (s_mus_delay > 0) {
            s_mus_delay--;
        } else {
            const uint8_t *p = s_mus_ptr;
            uint8_t nw = p[1];
            if (nw == 0x00) {
                v_stopMusic();
            } else if (nw == 0xFF) {
                uint32_t off = (uint32_t)s_mus_base[4] | ((uint32_t)s_mus_base[5] << 8)
                             | ((uint32_t)s_mus_base[6] << 16) | ((uint32_t)s_mus_base[7] << 24);
                s_mus_ptr = s_mus_base + off;
                s_mus_delay = s_mus_ptr[0];
            } else {
                const uint8_t *w = p + 2;
                for (uint8_t i = 0; i < nw; i++) { snd_psg(w[0], w[1]); w += 2; }
                s_mus_ptr = w;
                s_mus_delay = w[0];
            }
        }
    }
    if (s_sfx_active && s_sfx_ptr) {
        if (s_sfx_delay > 0) {
            s_sfx_delay--;
        } else {
            const uint8_t *p = s_sfx_ptr;
            uint8_t nw = p[1];
            if (nw == 0x00) {
                snd_psg(10, 0);          /* mute channel C, leave music alone */
                s_sfx_active = 0;        /* before the mixer write: */
                s_sfx_cbits  = 0x24;     /* C belongs to the music again */
                snd_psg(7, (uint8_t)(s_psg_mixer | 0x24));
            } else {
                const uint8_t *w = p + 2;
                for (uint8_t i = 0; i < nw; i++) {
                    uint8_t reg = w[0], val = w[1];
                    /* take only channel-C bits from the effect's mixer, or an
                     * SFX would cut the music on A and B */
                    if (reg == 7) {
                        s_sfx_cbits = (uint8_t)(val & 0x24);
                        val = (uint8_t)((s_psg_mixer & 0xDB) | (val & 0x24));
                    }
                    snd_psg(reg, val);
                    w += 2;
                }
                s_sfx_ptr = w;
                s_sfx_delay = w[0];
            }
        }
    }
}

/* Digitised-sample playback → the JS side (PitrexSimView) mixes via Web Audio. */
EM_JS(void, js_play_sample, (int idx, int voice, int loop), {
    if (Module.pitrex && Module.pitrex.playSample) Module.pitrex.playSample(idx, voice, loop);
});
EM_JS(void, js_stop_sample, (int voice), {
    if (Module.pitrex && Module.pitrex.stopSample) Module.pitrex.stopSample(voice);
});
EM_JS(int, js_sample_playing, (int voice), {
    return (Module.pitrex && Module.pitrex.samplePlaying) ? (Module.pitrex.samplePlaying(voice) | 0) : 0;
});
void v_playSample(int idx, int voice, int loop) { js_play_sample(idx, voice, loop); }
void v_stopSample(int voice)                    { js_stop_sample(voice); }
int  v_samplePlaying(int voice)                 { return js_sample_playing(voice); }

/* ---- FatFs over stdio (emscripten MEMFS) ---- */
FRESULT f_open(FIL* fp, const char* path, BYTE mode) {
    const char* m = (mode & FA_WRITE) ? "wb" : "rb";
    FILE* h = fopen(path, m);
    if (!h) return FR_NO_FILE;
    fp->fp = h;
    return FR_OK;
}
FRESULT f_close(FIL* fp) {
    if (fp && fp->fp) { fclose((FILE*)fp->fp); fp->fp = 0; }
    return FR_OK;
}
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br) {
    size_t n = fread(buff, 1, btr, (FILE*)fp->fp);
    if (br) *br = (UINT)n;
    return FR_OK;
}
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw) {
    size_t n = fwrite(buff, 1, btw, (FILE*)fp->fp);
    if (bw) *bw = (UINT)n;
    return FR_OK;
}
FRESULT f_lseek(FIL* fp, FSIZE_t ofs) {
    return (fseek((FILE*)fp->fp, (long)ofs, SEEK_SET) == 0) ? FR_OK : FR_DISK_ERR;
}
FSIZE_t f_tell(FIL* fp) { return (FSIZE_t)ftell((FILE*)fp->fp); }
FSIZE_t f_size(FIL* fp) {
    FILE* h = (FILE*)fp->fp;
    long cur = ftell(h);
    fseek(h, 0, SEEK_END);
    long end = ftell(h);
    fseek(h, cur, SEEK_SET);
    return (FSIZE_t)end;
}
int f_eof(FIL* fp) { return feof((FILE*)fp->fp) ? 1 : 0; }
