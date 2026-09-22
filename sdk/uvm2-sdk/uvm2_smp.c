/* uvm2_smp.c — the .vsmp sample sequencer. See uvm2_smp.h for the whole model
 * (why the injection goes in the list, and why the clock is the bus); this file
 * is only the voice state and the arithmetic.
 *
 * WHAT IS NOT HERE IS A SINGLE BUS WRITE, deliberately: packing commands belongs
 * to uvm2_draw.c, which owns the list, the port caches and the cycle counter.
 * This file answers two questions — is a sample due, and with what value — and
 * because of that it compiles and is tested on the host as-is, with none of the
 * cartridge in front of it.
 */

#include "uvm2_bus.h"
#include "uvm2_smp.h"
#ifndef UVM2_HOST
#include "uvm2_sd.h"
#endif

/* Bus cycles between two samples. At 8 kHz over a 1.5 MHz bus, 187. */
#define SMP_PERIOD  (UVM2_BUS_HZ / UVM2_SMP_HZ)

/* ── THE VOLUME REGISTER IS LOGARITHMIC, AND THAT WAS THE CRUNCH ────────────
 *
 * Writing a linear PCM value into the PSG's volume register assumes the register is a
 * linear DAC. It is not: it is an ATTENUATOR built from a resistor ladder, and its
 * steps are roughly logarithmic. Derived from the ladder MAME uses for the AY-3-8910
 * (`ay8910_param` in ay8910.cpp, RL=1000), normalised:
 *
 *     level    5      8      10     12     15
 *     output  0.059  0.159  0.356  0.564  1.000
 *     linear  0.333  0.533  0.667  0.800  1.000
 *
 * So the value we called the midpoint actually sits at a sixth of full scale, and a
 * linear ramp comes out squashed at the bottom and stretched at the top — harmonic
 * distortion on every sample, independent of the mixer and of the sample rate. Traced
 * on hardware the DAC was emitting 5..10, i.e. 0.06..0.36: the whole signal living in
 * the bottom third of the curve.
 *
 * This table inverts it: index by the LINEAR value you want, get the level whose real
 * output is nearest. Collisions in the top half are the log curve's doing and are
 * honest -- fewer distinct levels, but at the right amplitudes.
 *
 * IT IS APPLIED AT THE LAST MOMENT, AFTER MIXING, and that is the whole reason it
 * lives here and not in the converter. Mixing has to happen in the linear domain; a
 * file holding pre-distorted levels would make summing two voices a sum of logarithms,
 * which means nothing. */
static const uint8_t s_ay_lin[16] = {  0,  5,  7,  8,  9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 15, 15 };

/* The middle of the 4-bit scale. audio2vsmp maps silence here (0..15 with 8 ≈
 * zero), so it is also the DAC's resting value. */
#define SMP_MID  8

/* WHERE THE BUNDLE LANDS IN PSRAM, and it is a different address per board.
 *
 *   UVM2: the UNCACHED alias (0x15000000), which is how uvm2_romzip.c writes and
 *   reads the romset and the only thing that does not corrupt the data there — see
 *   the PSRAM.md block. Its romset sits at +4 MB and takes tens of KB.
 *
 *   Vectrex Studio cartridge (UVM2_BIOS): the normal window, because the whole
 *   game already lives and executes there. That PSRAM is packed from both ends —
 *   the game from 0x11000000 up, the romset from ROM_WINDOW_TOP (0x117FFFF0) down —
 *   so +6 MB is the middle, clear of both.
 *
 * Either way it is +6 MB with 2 MB ahead of it, which is the real limit on a
 * bundle: UVM2_SMP_BUNDLE_MAX. */
#ifndef UVM2_SMP_PSRAM_BASE
#  ifdef UVM2_BIOS
#    define UVM2_SMP_PSRAM_BASE 0x11600000u
#  else
#    define UVM2_SMP_PSRAM_BASE 0x15600000u
#  endif
#endif
#ifndef UVM2_SMP_BUNDLE_MAX
#define UVM2_SMP_BUNDLE_MAX (1024u * 1024u)
#endif

uint32_t uvm2_smp_injected;

/* ── THE MIXER INSTRUMENTS, OFF BY DEFAULT ───────────────────────────────────
 *
 * Everything below is measurement, not playback: build with -DUVM2_SMP_TELEM=1 to
 * compile it in and read it with tools/smp_stats.py.
 *
 * IT IS GATED RATHER THAN DELETED because every single decision the mixer makes was
 * made with it, and none of them is final — the divide threshold alone was correct
 * at two, then at three, then at four, each time invalidated not by a bug but by a
 * change to the .vsm bundle. The next such change needs these back, and deleting
 * them would mean rebuilding the instrument before being able to ask.
 *
 * OFF IS NOT ONLY ABOUT SPEED. Left on, `sum_hist` alone is 2.8 KB of .bss, and this
 * cartridge has form: leftover per-frame instrumentation is what caused the vector
 * flicker we once spent a session chasing. A measurement that ships is a bug with a
 * good excuse. */
#ifndef UVM2_SMP_TELEM
#define UVM2_SMP_TELEM 0
#endif

#if UVM2_SMP_TELEM
/* ── WHAT ACTUALLY REACHES THE DAC ───────────────────────────────────────────
 *
 * "Crunchy" can be the DATA (the sample we read is already wrong) or the
 * RECONSTRUCTION (the data is good and we emit it at an uneven rate), and the two
 * fixes are opposites. This keeps the last values emitted together with the list
 * cycle they went out at, which is the only thing that separates them: a
 * recognisable wave with uneven gaps is reconstruction; formless jumps from 0 to 15
 * are the data.
 *
 * It also answers the question the ear cannot: HOW MUCH OF THE SCALE is in use.
 * Levels bunched in a narrow band mean amplitude is being thrown away; runs pinned
 * at 0 or 15 mean the mixer is clipping. Both have been read here, and both were
 * invisible from the sofa.
 *
 * Read over SWD, not RTT — which froze the console once already. */
#define SMP_TRACE 192u
uint8_t  uvm2_smp_trace_val[SMP_TRACE];   /* the 4-bit value written           */
uint16_t uvm2_smp_trace_pos[SMP_TRACE];   /* list cycle, for the rate          */
uint32_t uvm2_smp_trace_i;                /* next slot (wraps)                 */

/* ── AND WHAT THE 192-SAMPLE WINDOW CANNOT ANSWER ────────────────────────────
 *
 * The ring above is a photograph, and a mixer question is not about one moment:
 * comparing two traces taken while different sounds happened says nothing, which
 * is exactly how the "does halving at two voices cost amplitude" question came
 * back unanswerable. These accumulate instead, from boot, so one read covers
 * everything that has been played.
 *
 * `dev_min`/`dev_max` are the mixer's own deviation BEFORE the clamp, i.e. the
 * number that has to fit in -8..+7. They say whether the scale is being used or
 * wasted. `clip_lo`/`clip_hi` count the samples the clamp had to rescue, which is
 * the cost side of the same decision. `n_hist` says how many voices were actually
 * being mixed, because a rule about "two or more" is worth nothing if the game
 * almost never plays two. */
int32_t  uvm2_smp_dev_min, uvm2_smp_dev_max;
uint32_t uvm2_smp_clip_lo, uvm2_smp_clip_hi;
uint32_t uvm2_smp_n_hist[UVM2_SMP_VOICES + 1u];

/* ── THE RAW SUM, BEFORE ANY DIVISION, SPLIT BY VOICE COUNT ──────────────────
 *
 * The extremes above say the mix reaches -12..+11 and that 0.13% of samples get
 * clamped, which settles whether the CURRENT threshold is safe. It cannot say what
 * a DIFFERENT one would cost — and guessing that by flashing a threshold, playing,
 * and reading back is one round trip per candidate, with the game never playing
 * quite the same thing twice.
 *
 * This measures the input to the decision instead of its output, so every threshold
 * can be costed from one recording. For a threshold T the emitted value is `sum`
 * when n < T and `sum / 2` when n >= T, and it is clamped when it leaves -8..+7 —
 * all of which is arithmetic on this table once it has been read.
 *
 * SIGNED AND NOT MAGNITUDE, because the range is not symmetric: -8..+7 clips one
 * step sooner on top. Buckets saturate at both ends; a sample there clips under
 * every threshold, so folding them together loses nothing that matters. */
#define SMP_SUM_BUCKETS 64          /* index = sum + 32, clamped to 0..63 */
uint32_t uvm2_smp_sum_hist[UVM2_SMP_VOICES + 1u][SMP_SUM_BUCKETS];
#endif  /* UVM2_SMP_TELEM */

struct voice {
    const uint8_t *pcm;      /* payload, past the 8-byte header      */
    uint32_t       n;        /* total samples                        */
    uint32_t       rate;     /* the file's Hz (NOT the output rate)  */
    uint32_t       cursor;   /* current sample                       */
    uint32_t       rem;      /* pending fraction, in cycle*Hz units  */
    uint8_t        loop;
    uint8_t        active;
};

static struct voice s_voice[UVM2_SMP_VOICES];
/* IT IS NOT STATIC, AND THAT IS THE POINT. `uvm2_smp_active()` is consulted on the hottest
 * path of the draw — `vxs_emit`, on every T1CL: 1,844 times per frame in one of the ports —
 * and it lived here, in another translation unit, i.e. a real call every time just to read
 * an integer. Publishing it lets the .h offer the query as `static inline` and the test is
 * inlined into whoever asks. The name carries the module prefix because it is no longer
 * this file's alone. */
unsigned            uvm2_smp_s_active;
#define s_active uvm2_smp_s_active

/* Cycles elapsed since the last emitted sample. It is a DEBT and not an absolute
 * position: that way the frame boundary needs no signed subtraction, and what is
 * left unemitted at the end of one list is charged at the start of the next. */
static uint32_t s_debt;

/* Where the list stood at the previous query, to know how far it has moved. */
static uint32_t s_seen;

/* 0 = the PSG does NOT have our register latched. It starts that way every frame
 * because other code uses the PSG between lists: uvm2_read_buttons latches
 * register 14 on EVERY frame, so assuming the previous latch survived is betting
 * that nobody read the pads — and somebody always does. */
static uint8_t s_latched;

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── The sample bundle in PSRAM (see the header) ──────────────────────────── */

static const unsigned char *s_bundle;
static uint32_t s_bundle_n;       /* entries; 0 = no bundle loaded */
static uint32_t s_bundle_bytes;

/* What the last load actually got, for telemetry: a missing bundle has several
 * distinct causes — no card, name not found, magic wrong — and "MISSING" alone
 * sends you looking in the wrong place. */
uint32_t uvm2_smp_bundle_read;      /* bytes the reader returned */
uint32_t uvm2_smp_bundle_count(void) { return s_bundle_n; }

/* WHO READS THE CARD IS NOT THE SAME ON BOTH BOARDS, and conflating them is what
 * made this fail: the cartridge's `uvm2_sd_read_from` looks like the UVM2's and is
 * not — it is a shim for `config/uvm2.cfg` that demands a `dir/file` path and reads
 * through a 1 KB stack buffer. A 124 KB bundle in the root returns 0 bytes from it,
 * silently.
 *
 * So the buffer is published and the validation is separate from the reading:
 *   - UVM2: `uvm2_smp_bundle_load` reads it here, with the SDK's real reader.
 *   - Cartridge: the BIOS reads it with the same code that loads a romset
 *     (sd.rs::load_samples) straight into this buffer, then calls `_set`.
 * One definition of the address and of what a valid bundle is, two readers. */
void *uvm2_smp_bundle_buffer(uint32_t *max_bytes)
{
    if (max_bytes) *max_bytes = UVM2_SMP_BUNDLE_MAX;
    return (void *)(uintptr_t)UVM2_SMP_PSRAM_BASE;
}

int uvm2_smp_bundle_set(const void *base, uint32_t bytes)
{
    const unsigned char *p = (const unsigned char *)base;

    s_bundle_n = 0;
    uvm2_smp_bundle_read = bytes;
    if (!p || bytes < 8u) return 0;

    /* THE MAGIC IS CHECKED, and not out of formality: whatever is on the card under
     * that name arrives here. Without this filter, any file would be read as an
     * offset table and uvm2_smp_play would be handed pointers to anywhere in PSRAM. */
    if (p[0] != 'V' || p[1] != 'S' || p[2] != 'M' || p[3] != 'B') return 0;

    s_bundle       = p;
    s_bundle_bytes = bytes;
    s_bundle_n     = rd_le32(p + 4);
    /* The offset table has to fit inside what was read, or an index points past the
     * end before we even look at a sound. */
    if (s_bundle_n == 0u || 8u + s_bundle_n * 4u > bytes) { s_bundle_n = 0; return 0; }
    return 1;
}

int uvm2_smp_bundle_load(const char *path)
{
    s_bundle_n = 0;
    if (!path) return 0;
#ifdef UVM2_HOST
    return 0;                     /* no card in the host harness */
#else
    {
        uint32_t max = 0;
        void *buf = uvm2_smp_bundle_buffer(&max);
        uint32_t got = uvm2_sd_read_from(path, (unsigned char *)buf, max, 0);
        return uvm2_smp_bundle_set(buf, got);
    }
#endif
}

const void *uvm2_smp_bundle_entry(unsigned idx)
{
    uint32_t off;

    if (s_bundle_n == 0u || idx >= s_bundle_n) return 0;
    off = rd_le32(s_bundle + 8u + idx * 4u);
    /* 0 = that entry is not in the bundle. And an offset leaving no room for the
     * .vsmp's own 8-byte header means a corrupt bundle: better silent than playing
     * whatever PSRAM follows. */
    if (off == 0u || off + 8u > s_bundle_bytes) return 0;
    return s_bundle + off;
}

/* ── Voices ───────────────────────────────────────────────────────────────── */

/* Sample `i` of a voice: 2 per byte, low nibble = even sample (the packing done
 * by tools/audio2vsmp/audio2vsmp.py and by compile_vsmp in the ARM codegen). */
static uint8_t sample_at(const struct voice *v, uint32_t i)
{
    const uint8_t b = v->pcm[i >> 1];
    return (uint8_t)((i & 1u) ? (b >> 4) & 0x0Fu : b & 0x0Fu);
}

void uvm2_smp_play(const void *data, unsigned voice, int loop)
{
    const uint8_t *p = (const uint8_t *)data;
    struct voice *v;
    uint32_t rate, n;

    if (!p || voice >= UVM2_SMP_VOICES) return;

    rate = rd_le32(p);
    n    = rd_le32(p + 4);
    /* A HEADER THAT DOES NOT ADD UP IS NOT TOUCHED. The pointer is chosen by the
     * game (in AAE, an index into its own sound table), so whatever arrives,
     * arrives: without this filter an out-of-range index plays as full-volume
     * noise for as long as a garbage `n` says. It is the same fault that hung the
     * core when a text pointer came in through SYS_PLAY_SFX. */
    if (rate == 0u || n == 0u || rate > 48000u) return;

    v = &s_voice[voice];
    if (!v->active) s_active++;
    v->pcm    = p + 8;
    v->n      = n;
    v->rate   = rate;
    v->cursor = 0;
    v->rem    = 0;
    v->loop   = (uint8_t)(loop ? 1 : 0);
    v->active = 1;
}

void uvm2_smp_stop(unsigned voice)
{
    unsigned i;
    if (voice != UVM2_SMP_ALL && voice >= UVM2_SMP_VOICES) return;
    for (i = 0; i < UVM2_SMP_VOICES; i++) {
        if (voice != UVM2_SMP_ALL && i != voice) continue;
        if (s_voice[i].active) { s_voice[i].active = 0; s_active--; }
    }
}

int uvm2_smp_playing(unsigned voice)
{
    if (voice == UVM2_SMP_ALL) return s_active != 0u;
    return (voice < UVM2_SMP_VOICES) ? s_voice[voice].active : 0;
}

/* `uvm2_smp_active()` is now `static inline` in uvm2_smp.h over the counter above; there
 * is no out-of-line definition here to contradict it. */

unsigned uvm2_smp_pos(unsigned voice, unsigned fps)
{
    const struct voice *v;
    if (voice >= UVM2_SMP_VOICES) return 0;
    v = &s_voice[voice];
    if (!v->active || v->rate == 0u) return 0;
    /* cursor / rate = seconds; times fps = frame. In 64 bits because cursor * fps
     * overflows 32 on a long sample. It is one division per frame, not per
     * sample. */
    return (unsigned)(((uint64_t)v->cursor * fps) / v->rate);
}

/* Advance every voice by `cycles` bus cycles.
 *
 * THE ARITHMETIC IS INTEGER AND EXACT on purpose: `rem` accumulates cycle*Hz and
 * every time it passes UVM2_BUS_HZ the cursor moves one sample. A fixed-point
 * step would leave a permanent pitch error (at 8 kHz a Q16 step comes to 349.5 ->
 * 349, i.e. 0.15% flat, for ever); this way there is none. The numbers fit in 32
 * bits with room to spare: the product overflows only past 357000 cycles, and the
 * largest `cycles` this is ever called with is one frame (~39000 at 40 Hz); the
 * measured gap between injections is 132 and its worst case 373. The remainder
 * always stays below UVM2_BUS_HZ. */
static void advance(uint32_t cycles)
{
    unsigned i;
    for (i = 0; i < UVM2_SMP_VOICES; i++) {
        struct voice *v = &s_voice[i];
        if (!v->active) continue;
        v->rem += cycles * v->rate;
        while (v->rem >= UVM2_BUS_HZ) {
            v->rem -= UVM2_BUS_HZ;
            v->cursor++;
        }
        if (v->cursor >= v->n) {
            if (v->loop) {
                /* Modulo, not zero: a short looping sample would advance less
                 * than it should on each pass and drift out of tune. */
                v->cursor %= v->n;
            } else {
                v->active = 0;
                s_active--;
            }
        }
    }
}

/* The 4 bits to write now: the sum of each active voice's deviation from the
 * midpoint, clamped.
 *
 * THE TWO WRONG ANSWERS, BOTH MEASURED ON THE CONSOLE, because this is not obvious
 * and the reasoning that picks either one sounds fine until you look at a trace:
 *
 *   SUM AND CLAMP was called "permanent hard clipping" on the argument that four
 *   full-scale voices give four times the range. Audio does not do that: the voices
 *   are at unrelated points of their waveforms, so they partly cancel. Traced: the
 *   sum sits around ±8 with peaks beyond, i.e. clipping on transients only.
 *
 *   AVERAGING was the reaction to that, and it was far worse. Traced on hardware with
 *   four voices playing, the DAC only ever emitted 5..10 — **±2 of ±7**, six distinct
 *   values out of sixteen. Two usable bits. Daniel heard it exactly: still crunchy,
 *   and the ship roar still inaudible — not clipped away this time, buried under the
 *   quantisation noise of its own tiny amplitude.
 *
 * With four bits the scarce thing is amplitude, not headroom. Summing keeps the
 * signal at full scale and pays for it with occasional transient clipping, which is
 * the cheaper of the two distortions by a wide margin. */
static uint8_t mix(void)
{
    int acc = 0;
    unsigned i, n = 0;
    for (i = 0; i < UVM2_SMP_VOICES; i++)
        if (s_voice[i].active) {
            acc += (int)sample_at(&s_voice[i], s_voice[i].cursor) - SMP_MID;
            n++;
        }
    /* ONE BIT OF HEADROOM, AND THE THRESHOLD IS THREE VOICES. The divisor is 2 — not
     * the number of voices, and not nothing. Both extremes were tried on hardware and
     * both are in the traces:
     *
     *   /1 (plain sum): the DAC sat at 15 for runs of ~25 samples and at 0 for ~21,
     *      i.e. a square wave, not a mix. 11% of samples pinned to a rail.
     *   /n (average):   the DAC only ever emitted 5..10 of 0..15 — two usable bits,
     *      and the ship roar inaudible under its own quantisation noise.
     *
     * WHERE THE THRESHOLD GOES IS DECIDED WITH THE BUNDLE, NOT ALONE. It and the
     * source compression pull on the same 4 bits, so a reading is only valid for the
     * .vsm it was taken with. tools/smp_stats.py reads the raw-sum histogram recorded
     * just below and costs every candidate from one recording; both columns moved
     * hard when wav_to_vsmp.py started compressing (SHAPE 0.5), because louder bodies
     * make bigger sums:
     *
     *     threshold     uncompressed bundle      compressed bundle
     *                   clipped  at full scale   clipped  at full scale
     *     n > 2          0.077%       33.7%       1.134%       42.4%
     *     n > 3          0.781%       55.1%       5.277%       74.4%   <- here
     *     never          3.767%      100.0%      10.638%      100.0%
     *
     * AND THREE WAS CHOSEN BY EAR, WITH THE TABLE ONLY NARROWING THE QUESTION. The
     * arithmetic says the two candidates differ in exactly one place — a three-voice
     * mix, 32% of playing time, emitted with a median deviation of 4 at n > 3 and 2
     * at n > 2; four voices and up are identical either way. So the A/B was worth
     * doing and worth only one listen: at n > 2 Daniel heard it as "less full", which
     * is that halving and nothing else.
     *
     * The clipping it costs is real and was listened for specifically: 3.6% measured
     * on one session, 5.3% predicted on a denser one, and inaudible in both. That is
     * the answer to a question this file got wrong twice in the other direction —
     * transient clipping at a few percent is cheap, and amplitude is not. What made
     * it safe to spend is that the sources are compressed now (SHAPE in
     * wav_to_vsmp.py), so the clipped samples are peaks over a loud body rather than
     * the whole signal hitting a rail, which is what 11% sounded like.
     *
     * WHY THE DIVIDE STILL EXISTS AT ALL, rather than summing and letting the clamp
     * work: with a compressed bundle it stops being a close call. Never dividing costs
     * 13.2% of samples clipped — the plain sum that this file's first trace already
     * rejected at 11%. Four bits cannot hold five compressed voices at full body, and
     * pretending otherwise just moves the distortion somewhere louder.
     *
     * THE HISTORY IS KEPT BECAUSE EACH STEP WAS RIGHT ON WHAT IT COULD SEE, and the
     * shape of the mistake repeats. Two was first chosen when every sound reached the
     * DAC at full scale. Attenuating the beds in the bundle (the MIX table in
     * wav_to_vsmp.py) made that obsolete and three became correct — measured. Then
     * compressing the sources made THAT obsolete within the hour. The lesson is in the
     * first paragraph: this constant is not independent, and a number here that was
     * measured against a different .vsm is not evidence, however carefully it was
     * taken. Re-run smp_stats.py after any change to the bundle. */

#if UVM2_SMP_TELEM
    /* The raw sum, before the division decides anything — see the declarations. */
    {
        int b = acc + 32;
        if (b < 0) b = 0;
        if (b > SMP_SUM_BUCKETS - 1) b = SMP_SUM_BUCKETS - 1;
        uvm2_smp_sum_hist[n][b]++;
    }
#endif

    if (n > 3) acc /= 2;

#if UVM2_SMP_TELEM
    if (acc < uvm2_smp_dev_min) uvm2_smp_dev_min = acc;
    if (acc > uvm2_smp_dev_max) uvm2_smp_dev_max = acc;
    uvm2_smp_n_hist[n]++;
#endif

    acc += SMP_MID;
#if UVM2_SMP_TELEM
    if (acc < 0)  { acc = 0;  uvm2_smp_clip_lo++; }
    if (acc > 15) { acc = 15; uvm2_smp_clip_hi++; }
#else
    if (acc < 0)  acc = 0;
    if (acc > 15) acc = 15;
#endif
    return s_ay_lin[acc];          /* linear -> the level that really sounds like it */
}

int uvm2_smp_due(uint32_t cycles, uint8_t *value)
{
    uint32_t d;

    if (!s_active) { s_seen = cycles; return 0; }

    /* A list only grows, so a `cycles` lower than what was seen means another one
     * has started: count from zero instead of subtracting the wrong way round and
     * getting a huge unsigned number. */
    d = (cycles >= s_seen) ? cycles - s_seen : cycles;
    s_seen  = cycles;
    s_debt += d;

    if (s_debt < SMP_PERIOD) return 0;

    advance(s_debt);
    s_debt = 0;
    if (!s_active) {
        /* The last voice ended right here: park the DAC at the midpoint so no
         * step is left sounding until the next sample. */
        *value = s_ay_lin[SMP_MID];   /* rest at the midpoint, through the same curve */
        uvm2_smp_injected++;
        return 1;
    }
    *value = mix();
#if UVM2_SMP_TELEM
    uvm2_smp_trace_val[uvm2_smp_trace_i % SMP_TRACE] = *value;
    uvm2_smp_trace_pos[uvm2_smp_trace_i % SMP_TRACE] = (uint16_t)cycles;
    uvm2_smp_trace_i++;
#endif
    uvm2_smp_injected++;
    return 1;
}

/* The .vmus player's mixer shadow, if that player is in this build. The cartridge
 * BIOS compiles with UVM2_NO_AUDIO and has no shadow, and a build with no music has
 * nothing to preserve either: all channels disabled is the right default for a DAC. */
/* WHAT THE MIXER IS WHEN NOBODY IS PLAYING A .vmus.
 *
 * This fallback used to answer a constant 0x3F -- every channel silenced -- and
 * `uvm2_smp_mixer()` composes the DAC's channel OVER it and re-latches the result once
 * a frame, inside the list. For a game that drives the PSG itself and plays no .vmus,
 * that silently overwrote its own mixer every frame.
 *
 * MEASURED on Star Wars, which writes the PSG directly (its music is a live translation
 * of the arcade's POKEYs) and streams speech through this DAC: the moment the speech
 * started, the music and effects went, and only the voice was left. The cartridge BIOS
 * does not even compile uvm2_audio.c, so this weak version is the one that answers
 * there -- the constant was not a fallback, it WAS the behaviour.
 *
 * So the last mixer the game asked for is remembered and answered instead. */
static uint8_t s_game_mixer = 0x3F;

/* The .vmus player keeps its OWN shadow and its `uvm2_audio_mixer` is the strong symbol
 * wherever it is compiled, so telling only the fallback above is not enough: on the .um2
 * target uvm2_audio.c IS compiled and the fallback never answers. This hook is weak here
 * and strong there, so one call site serves both. */
__attribute__((weak)) void uvm2_audio_note_mixer(uint8_t val) { (void)val; }

void uvm2_smp_note_mixer(uint8_t val)
{
    s_game_mixer = (uint8_t)(val & 0xBFu);
    uvm2_audio_note_mixer(s_game_mixer);
}

__attribute__((weak)) uint8_t uvm2_audio_mixer(void) { return s_game_mixer; }

uint8_t uvm2_smp_mixer(void)
{
    return (uint8_t)(uvm2_audio_mixer() | 0x24u);   /* tone C + noise C off */
}

int uvm2_smp_needs_latch(void)
{
    if (s_latched) return 0;
    s_latched = 1;
    return 1;
}

void uvm2_smp_frame(uint32_t cycles)
{
    /* Whatever debt is left carries into the next frame: `s_seen` goes back to
     * zero because the new list starts at zero, and what was or was not emitted
     * in this one is already in `s_debt`. */
    if (cycles >= s_seen) s_debt += cycles - s_seen;
    s_seen    = 0;
    s_latched = 0;                 /* other code writes the PSG between lists */
    uvm2_smp_injected = 0;

    /* A RUNAWAY DEBT IS NOT CARRIED. If a whole frame goes by with no gap to
     * inject into (a list with no strokes and no fixed refresh), the debt would
     * be worth several periods and the next sample would jump that stretch all at
     * once: a click. It is capped at one period — that frame's audio is lost,
     * which is what actually happened, rather than pretending to recover it. */
    if (s_debt > SMP_PERIOD * 2u) s_debt = SMP_PERIOD;
}
