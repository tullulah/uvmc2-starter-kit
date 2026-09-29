/* ts_jack.c — the 16-bit mixer behind ts_jack.h. See that header for what it is for.
 *
 * TEN VOICES, AND THE NUMBER COMES FROM THE GAME. AAE addresses voices by a fixed id and
 * Tac/Scan's go up to 9 (SegaG80.h: kVoiceShip 1, kVoiceTunnel 2, kVoiceStinger 3,
 * kVoiceEnemy 4, kVoiceShipRoar 5, kVoiceForm 7, kVoiceExtra 8, kVoiceExtralife 9). The
 * console path was written with 4 once, and the result on hardware was that the shots
 * sounded and the engines did not — everything from voice 4 up was dropped by a range
 * check, silently. The ceiling fits the caller or it is a bug waiting to be reported as
 * "half the sounds are missing".
 */
#ifdef UVM2_PICO_RUNTIME

#include <stdint.h>
#include "ts_jack.h"
#include "uvm2_jack.h"
#include "uvm2_sd.h"

/* The runtime brought the PSRAM up before main; 1 = it answered. */
extern int uvm2_psram_ready;

#define TS_VOICES 10
#define TS_BUNDLE "tacscan.pcm"

/* WHERE THE BUNDLE LIVES. PSRAM offset 0, which is free: the romset buffer sits at the
 * 4 MB mark (UVM2_ROMZIP_PSRAM_BASE 0x15400000) and this game declares no ROM stage. 2.64 MB
 * ends well short of it.
 *
 * TWO WINDOWS ONTO THE SAME CHIP, AND THE DIFFERENCE MATTERS BOTH WAYS:
 *   0x15000000  uncached — WRITE through this one. Writing through the cached window
 *               corrupts the data; measured in uvm2_romzip.c, 2512 bad words of 4096.
 *   0x11000000  cached   — READ through this one. The mixer walks each voice forwards, so
 *               the XIP cache's prefetch is exactly what we want; reading the uncached
 *               alias instead would put a QSPI round trip on every sample.
 *
 * Both are overridable so the mixer can be built and LISTENED TO on a desktop against the
 * real bundle (tools/jack_probe.c). A mixer that is only ever exercised by flashing a card
 * is one whose headroom and looping nobody has actually checked. */
#ifndef TS_PSRAM_WRITE
#define TS_PSRAM_WRITE ((unsigned char *)(uintptr_t)0x15000000u)
#endif
#ifndef TS_PSRAM_READ
#define TS_PSRAM_READ  ((const unsigned char *)(uintptr_t)0x11000000u)
#endif

/* HOW MUCH PER FRAME. The bundle comes in WHILE THE GAME RUNS, so a slice has to fit inside
 * a frame with room to spare: 8 KB per 25 ms asks 320 KB/s of the card, which is modest.
 * 32 KB would have asked 1.3 MB/s and blown the frame on a slower one.
 *
 * At 8 KB the 2.64 MB takes 322 frames, about 8 seconds at 40 Hz, and the first sound the
 * game uses is in after 45 of them. Nobody waits for any of it: see the note in main.c. */
#define TS_SLICE 8192u

static uvm2_sd_file  s_file;
static uint32_t      s_got;        /* bytes written into PSRAM so far */
static uint32_t      s_size;       /* the file's size, 0 until it is opened */
static int           s_loading;
static int           s_hdr;        /* the header is in and says KSFX */
static int           s_jack;

/* For SWD, and because "no sound" has four different causes that look identical from the
 * sofa: no jack on the board, no PSRAM, no file on the card, or a file that is not a
 * bundle. */
uint32_t ts_jack_err;            /* 0 = fine; see TS_JACK_E* below */
uint32_t ts_jack_bytes;          /* what the load actually read */
#define TS_JACK_E_NOJACK  1u
#define TS_JACK_E_NOPSRAM 2u
#define TS_JACK_E_NOFILE  3u
#define TS_JACK_E_BADFILE 4u
#define TS_JACK_E_BADFMT  5u   /* a format this reader has no decoder for (ADPCM) */

/* `pos` and `step` are Q16 so a bundle that is not at the jack's own rate still plays at the
 * right pitch: step = src_hz / UVM2_JACK_RATE. At the jack's rate step is exactly 1.0 and the
 * walk is pos += 65536, which is what the generator produces by default — the fraction is
 * there so `--rate 22050` (a smaller bundle, a shorter load) is a real option and not one
 * that plays everything sharp. */
typedef struct {
    const int16_t *pcm;
    uint32_t       len;      /* in samples */
    uint32_t       pos;      /* Q16 */
    uint32_t       step;     /* Q16 */
    int            live, loop;
} voice_t;

static voice_t s_v[TS_VOICES];

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

int ts_jack_begin(void)
{
    s_jack = uvm2_jack_init();
    if (!s_jack)            { ts_jack_err = TS_JACK_E_NOJACK;  return 0; }
    if (uvm2_psram_ready != 1) { ts_jack_err = TS_JACK_E_NOPSRAM; return 0; }
    if (!uvm2_sd_open(TS_BUNDLE, &s_file)) { ts_jack_err = TS_JACK_E_NOFILE; return 0; }
    s_size   = s_file.len;
    s_got    = 0;
    s_loading = 1;
    return 1;
}

int ts_jack_load_step(void)
{
    if (!s_loading) return 1;
    uint32_t n = uvm2_sd_next(&s_file, TS_PSRAM_WRITE + s_got, TS_SLICE);
    s_got += n;
    /* THE MAGIC IS CHECKED AS SOON AS IT IS IN, not at the end. A bundle or nothing: a
     * truncated read or the wrong file would otherwise be played as whatever those bytes
     * happen to be, and on a 16-bit DAC that is full-scale noise — loud, through a line
     * output, into somebody's amplifier. Checking it early also means routing can start
     * after the first slice instead of after the last. */
    if (!s_hdr && s_got >= 8u) {
        if (TS_PSRAM_READ[0] == 'K' && TS_PSRAM_READ[1] == 'S' &&
            TS_PSRAM_READ[2] == 'F' && TS_PSRAM_READ[3] == 'X') {
            s_hdr = 1;
        } else {
            ts_jack_err = TS_JACK_E_BADFILE;
            uvm2_sd_close(&s_file);
            s_loading = 0;
            return 1;
        }
    }
    if (n == 0 || s_got >= s_size) {
        uvm2_sd_close(&s_file);
        s_loading = 0;
        ts_jack_bytes = s_got;
        if (s_got < s_size) ts_jack_err = TS_JACK_E_BADFILE;   /* the card stopped short */
        return 1;
    }
    return 0;
}

int ts_jack_load_percent(void)
{
    if (!s_size) return 0;
    if (s_got >= s_size) return 100;
    return (int)((uint64_t)s_got * 100u / s_size);
}

/* Ready means "the table can be read", not "everything is in": the game is already running
 * and each sound waits for its own bytes. See ts_jack_avail. */
int ts_jack_ready(void) { return s_hdr; }


/* ── THE KSFX HEADER ───────────────────────────────────────────────────────────
 *
 *   "KSFX" | u16 n | u16 - | n x (u32 offset, u32 samples, u32 hz, u16 format, u16 -) | data
 *
 * The container is shared with the other game on this cartridge that drives the jack, and
 * `format` is what makes that possible: 0 is 16-bit linear, 1 is IMA ADPCM at four bits.
 * Tac/Scan ships format 0 — the point of the jack is that nothing is quantised.
 *
 * A FORMAT THIS READER CANNOT DECODE IS REFUSED, not played. There is no ADPCM decoder here,
 * and ADPCM nibbles read as 16-bit samples are full-scale noise: loud, on a line output, into
 * somebody's amplifier. Silence is the right answer to a bundle we do not understand. */
static int entry_of(int idx, const int16_t **pcm, uint32_t *n, uint32_t *hz)
{
    if (!s_hdr || idx < 0) return 0;
    const uint32_t count = rd16(TS_PSRAM_READ + 4);
    if ((uint32_t)idx >= count) return 0;
    const unsigned char *r = TS_PSRAM_READ + 8 + 16u * (uint32_t)idx;
    const uint32_t off = rd32(r), ns = rd32(r + 4), rate = rd32(r + 8);
    const uint32_t fmt = rd16(r + 12);
    if (off == 0 || ns == 0 || rate == 0) return 0;       /* a hole */
    if (fmt != 0) { ts_jack_err = TS_JACK_E_BADFMT; return 0; }
    /* AGAINST WHAT HAS ARRIVED, not against the file's size. The bundle is still coming in
     * while the game plays, so a sound whose bytes are not all here yet is not playable YET —
     * and saying so is what lets samples.c send it out of the console's path meanwhile
     * instead of playing silence or walking off the end of what was loaded. */
    if (off + ns * 2u > s_got) return 0;
    *pcm = (const int16_t *)(const void *)(TS_PSRAM_READ + off);
    *n = ns;
    *hz = rate;
    return 1;
}

int ts_jack_avail(int idx)
{
    const int16_t *pcm; uint32_t n, hz;
    return entry_of(idx, &pcm, &n, &hz);
}

void ts_jack_play(int voice, int idx, int loop)
{
    if (!s_hdr || voice < 0 || voice >= TS_VOICES) return;
    const int16_t *pcm; uint32_t n, hz;
    if (!entry_of(idx, &pcm, &n, &hz)) { s_v[voice].live = 0; return; }
    /* Re-triggering a voice restarts it, which is what AAE expects. */
    s_v[voice].pcm  = pcm;
    s_v[voice].len  = n;
    s_v[voice].pos  = 0;
    s_v[voice].step = (uint32_t)(((uint64_t)hz << 16) / (uint32_t)UVM2_JACK_RATE);
    s_v[voice].loop = loop ? 1 : 0;
    s_v[voice].live = 1;
}

void ts_jack_stop(int voice)
{
    if (voice < 0 || voice >= TS_VOICES) { for (int i = 0; i < TS_VOICES; i++) s_v[i].live = 0; return; }
    s_v[voice].live = 0;
}

int ts_jack_playing(int voice)
{
    if (voice < 0 || voice >= TS_VOICES) return 0;
    return s_v[voice].live;
}

/* HEADROOM: ONE SHIFT, AND THE NUMBER COMES FROM THE CONTENT.
 *
 * Ten voices summed at full scale would need a shift of 4 and everything would be a
 * sixteenth as loud as it could be. But full scale is not what these sounds spend their time
 * at: the generator prints the median |sample| of each one and they run 3573..8736 out of
 * 32767 — 11% to 27%. Three voices at their medians sum to about 0.5 of full scale with a
 * shift of ONE, so nothing clamps in normal play and a single sound still comes out at half
 * scale, which is loud on a line output.
 *
 * Peaks do clamp, and that is the right trade: a clamp is a flattened transient, a WRAP is a
 * crack. Never let it wrap. */
#define TS_HEADROOM 1

void ts_jack_update(void)
{
    if (!s_hdr) return;
    int n = uvm2_jack_space();
    while (n > 0) {
        int16_t buf[128];
        const int k = (n > 128) ? 128 : n;
        for (int i = 0; i < k; i++) {
            int32_t acc = 0;
            for (int v = 0; v < TS_VOICES; v++) {
                voice_t *p = &s_v[v];
                if (!p->live) continue;
                acc += (int32_t)p->pcm[p->pos >> 16] >> TS_HEADROOM;
                p->pos += p->step;
                if ((p->pos >> 16) >= p->len) {
                    if (p->loop) p->pos = 0;
                    else         { p->live = 0; }
                }
            }
            if (acc >  32767) acc =  32767;
            if (acc < -32768) acc = -32768;
            buf[i] = (int16_t)acc;
        }
        uvm2_jack_write(buf, k);
        n -= k;
    }
}

#else  /* not the .um2 runtime: the host harness and the WASM sim have no jack */
#include "ts_jack.h"
int  ts_jack_begin(void)        { return 0; }
int  ts_jack_load_step(void)    { return 1; }
int  ts_jack_load_percent(void) { return 0; }
int  ts_jack_ready(void)        { return 0; }
int  ts_jack_avail(int i)       { (void)i; return 0; }
void ts_jack_play(int v, int i, int l) { (void)v; (void)i; (void)l; }
void ts_jack_stop(int v)        { (void)v; }
int  ts_jack_playing(int v)     { (void)v; return 0; }
void ts_jack_update(void)       { }
#endif
