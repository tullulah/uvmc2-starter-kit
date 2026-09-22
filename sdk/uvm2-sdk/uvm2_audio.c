/*
 * uvm2_audio.c — compiled .vmus / .vsfx playback (svc #21, #22, #23).
 *
 * The sequencer is a port of the software one in libvpy (vpy-c/vpy.c), which is
 * the reference implementation for the PiTrex and WASM paths: same event
 * format, same one-tick-per-frame model, same channel-C mixer merge.  Keeping
 * it identical means a track sounds the same in the IDE simulator as it does on
 * the cartridge.
 *
 * Where our own RP2350 cartridge runs this on core 1 — clocked by real time, so
 * the tempo survives a heavy draw frame — UVM2 ticks it once per frame from
 * SYS_WAIT_RECAL.  That is exactly the 50 Hz the assets are compiled at, and it
 * keeps the whole target single-core for now.  The cost is that a frame which
 * overruns its budget also stretches the music; `uvm2_stats.overrun` is what
 * says whether that is happening.
 *
 * PSG writes go out as direct bus accesses in the same between-frames window as
 * input, never recorded into the beam command stream: they drive Port B, which
 * would disturb a draw in progress.  A frame's worth of them is a few dozen bus
 * cycles out of 30000.
 */

#include "uvm2_bus.h"
#include "uvm2_input.h"
#include "uvm2_audio.h"
#ifdef UVM2_DUAL_CORE
void uvm2_psg_queue(uint32_t reg, uint32_t value);   /* uvm2_core1.c */
#endif

/* Event streams (little-endian):
 *   MUSIC: [0..4] num_events, [4..8] loop event byte offset, events at base+8
 *   SFX:   [0..4] num_events, events at base+4
 *   event: [delay, num_writes, (reg,val) * num_writes]
 *          num_writes == 0xFF -> loop (music only), == 0 -> end
 * The first event fires immediately; each event's delay byte is the wait
 * BEFORE the next one, read after the current event has fired. */

static const uint8_t *s_mus_base;
static const uint8_t *s_mus_ptr;
static int            s_mus_playing;
static int            s_mus_delay;
static int            s_mus_primed;

static const uint8_t *s_sfx_ptr;
static int            s_sfx_active;
static int            s_sfx_delay;

/* Shadow of PSG mixer register 7, so an SFX on channel C can be merged in
 * without silencing the music on A and B. */
static uint8_t s_psg_mixer = 0x3F;

/* ...and the channel-C mixer bits the RUNNING EFFECT asked for, so the merge
 * works in BOTH directions.
 *
 * It used to work in one: an effect merged itself over the music's A/B bits,
 * but a music event that wrote register 7 afterwards wrote it raw and took
 * channel C straight back — the effect fell silent mid-flight and stayed that
 * way until its next mixer write, which for a diff-encoded stream may never
 * come. Any track with noise percussion writes register 7 on every drum hit,
 * so on those the effects were being cut within a couple of frames. */
static uint8_t s_sfx_cbits = 0x24;      /* 0x24 = tone C and noise C disabled */

/* What the sample player needs to know to turn its channel into a DC level without
 * silencing the music on A and B (uvm2_smp.c). Strong here, weak there. */
uint8_t uvm2_audio_mixer(void) { return s_psg_mixer; }

/* A GAME THAT DRIVES THE PSG ITSELF still owns the mixer, and the sample injector
 * composes its DAC channel over whatever this says. Without this the shadow only ever
 * moved when the .vmus sequencer wrote it, so a game with no track kept the initial 0x3F
 * -- everything silenced -- and the injector re-latched that over the game's own mixer
 * once a frame. Measured on Star Wars, which translates the arcade's POKEYs live: all
 * audio vanished the moment the speech stream started. Weak twin in uvm2_smp.c. */
void uvm2_audio_note_mixer(uint8_t val) { s_psg_mixer = (uint8_t)(val & 0xBFu); }


static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void psg(uint8_t reg, uint8_t val)
{
    if (reg == 7) {
        /* Bit 6 stays at zero: this is an INVARIANT, not a workaround.
         *
         * On the AY-3-8912 that bit is port A's direction, and on the Vectrex that port is
         * how the buttons are read. Setting it to one makes it an output and the
         * controllers can no longer be read. There is no legitimate case in which a Vectrex
         * game wants that, and EVERY write to register 7 passes through here, so this is
         * the place.
         *
         * NOTE: this was NOT the cause of the constant 0x3F seen on the buttons on
         * 2026-08-12, although it was added in that belief. That one was uvm2_read_buttons
         * not forcing DDRA, so the register number never reached the bus and the PSG stayed
         * with 7 latched — and 0x3F is exactly the contents of that register. */
        val = (uint8_t)(val & ~0x40u);
        /* While an effect is running it owns channel C; a music write may set
         * everything else but must leave those two bits as the effect left
         * them. (sfx_tick merges the other way for the same reason.) */
        if (s_sfx_active)
            val = (uint8_t)((val & 0xDBu) | (s_sfx_cbits & 0x24u));
        s_psg_mixer = val;
    }

#ifdef UVM2_DUAL_CORE
    /* UNDER DUAL CORE THE BUS BELONGS TO CORE 1. Anyone who is not core 1 queues.
     *
     * The routing is by the CORE THAT IS EXECUTING, not by the calling function: that way
     * it stays correct when somebody adds a new call without remembering this rule. Today
     * the only one arriving here from core 0 was uvm2_stop_music() through its syscall;
     * uvm2_audio_tick() already runs on core 1 and still writes directly, without going
     * through the queue or losing a frame.
     *
     * Two unarbitrated writers on the VIA is the bug another cartridge shipped with when 40
     * games claimed dual-core and were compiled single. It had shown no symptom here, and
     * that is exactly the kind of thing that comes back a month later as an
     * irreproducible glitch. */
    if (UVM2_CPUID != 1u) {
        uvm2_psg_queue(reg, val);
        return;
    }
#endif
    uvm2_psg_write(reg, val);
}

void uvm2_play_music(const uint8_t *data)
{
    if (!data) return;
    /* Re-issuing the same track must not restart it: VPy games call
     * PLAY_MUSIC unconditionally inside the frame loop. */
    if (s_mus_playing && s_mus_base == data) return;

    s_mus_base    = data;
    s_mus_ptr     = data + 8;      /* events follow the 8-byte header */
    s_mus_playing = 1;
    s_mus_delay   = 0;             /* first event fires immediately */
}

void uvm2_stop_music(void)
{
    s_mus_playing = 0;
    s_mus_ptr     = 0;
    psg(8,  0);                    /* channel volumes A, B, C */
    psg(9,  0);
    psg(10, 0);
    psg(7,  0x3F);                 /* mixer: everything disabled */
}

void uvm2_play_sfx(const uint8_t *data)
{
    if (!data) return;
    s_sfx_ptr    = data + 4;       /* events follow the 4-byte header */
    s_sfx_active = 1;
    s_sfx_delay  = 0;
}

static void music_tick(void)
{
    const uint8_t *p, *w;
    uint8_t num_writes;

    if (!s_mus_playing || !s_mus_ptr) return;

    /* One priming frame before the first event, matching libvpy: its inline
     * PiTrex sequencer spends its first update capturing a timer baseline and
     * fires nothing.  Without this the whole timeline runs one frame early
     * relative to every other target. */
    if (!s_mus_primed) { s_mus_primed = 1; return; }

    if (s_mus_delay > 0) { s_mus_delay--; return; }

    p = s_mus_ptr;
    num_writes = p[1];

    if (num_writes == 0x00) { uvm2_stop_music(); return; }

    if (num_writes == 0xFF) {                       /* loop back */
        s_mus_ptr   = s_mus_base + rd_le32(s_mus_base + 4);
        s_mus_delay = s_mus_ptr[0];
        return;
    }

    w = p + 2;
    for (uint8_t i = 0; i < num_writes; i++) { psg(w[0], w[1]); w += 2; }

    s_mus_ptr   = w;                                /* next event... */
    s_mus_delay = w[0];                             /* ...and its wait */
}

static void sfx_tick(void)
{
    const uint8_t *p, *w;
    uint8_t num_writes;

    if (!s_sfx_active || !s_sfx_ptr) return;
    if (s_sfx_delay > 0) { s_sfx_delay--; return; }

    p = s_sfx_ptr;
    num_writes = p[1];

    if (num_writes == 0x00) {                       /* end of effect */
        psg(10, 0);                                 /* mute channel C */
        s_sfx_active = 0;                           /* before the mixer write: */
        s_sfx_cbits  = 0x24;                        /* C is the music's again  */
        psg(7, (uint8_t)(s_psg_mixer | 0x24u));     /* positively disable C    */
        return;
    }

    w = p + 2;
    for (uint8_t i = 0; i < num_writes; i++) {
        uint8_t reg = w[0], val = w[1];
        if (reg == 7) {
            /* Take only the channel-C bits from the effect and keep the
             * music's A/B bits, or an SFX would cut the track. */
            s_sfx_cbits = (uint8_t)(val & 0x24);
            val = (uint8_t)((s_psg_mixer & 0xDB) | (val & 0x24));
        }
        psg(reg, val);
        w += 2;
    }

    s_sfx_ptr   = w;
    s_sfx_delay = w[0];
}

void uvm2_audio_tick(void)
{
    music_tick();
    sfx_tick();
}
