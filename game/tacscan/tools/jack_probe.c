/* jack_probe.c — run the REAL jack mixer on a desktop and write what it produces.
 *
 * ts_jack.c's body only exists on the cartridge, so on the host it is compiled with
 * UVM2_PICO_RUNTIME defined and with fakes for the three things it talks to: the jack driver,
 * the SD card and the PSRAM window. The bundle is the real build/tacscan.pcm.
 *
 * WHAT IT IS FOR. The headroom shift, the looping, and the offsets the entry table is read
 * with are all things that sound wrong rather than fail, and flashing a card to find that out
 * costs minutes per attempt. This plays a scripted scene and writes out.wav, which can be
 * listened to and measured.
 *
 *   cc -O2 -DUVM2_PICO_RUNTIME -include tools/jack_probe.h \
 *      -I<kit>/sdk/uvm2-sdk -Isrc -o /tmp/jp tools/jack_probe.c src/ts_jack.c
 *   /tmp/jp build/tacscan.pcm /tmp/out.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ts_jack.h"

/* ---- the PSRAM window, as a plain buffer ---- */
#define PSRAM_BYTES (8u * 1024u * 1024u)
unsigned char *probe_psram;
/* Advanced by TS_NOW(); see jack_probe.h. 200 us per chunk read is roughly what the estimate
 * for a bit-banged 1 KB gives, so the deadline divides the load into about the number of
 * passes the cartridge would take. */
unsigned probe_us;

/* ---- the SD card, as one file ---- */
static FILE    *s_fp;
static uint32_t s_len;
int uvm2_psram_ready = 1;

typedef struct { uint32_t cluster, sec, pos, len; int ok; uint32_t magic; uint64_t fil[16]; } uvm2_sd_file;
int uvm2_sd_open(const char *path, uvm2_sd_file *f)
{
    (void)path;                         /* the probe is given the file on the command line */
    memset(f, 0, sizeof *f);
    f->len = s_len; f->ok = 1;
    return s_fp != NULL;
}
uint32_t uvm2_sd_next(uvm2_sd_file *f, unsigned char *dst, uint32_t max)
{
    size_t n = fread(dst, 1, max, s_fp);
    f->pos += (uint32_t)n;
    return (uint32_t)n;
}
void uvm2_sd_close(uvm2_sd_file *f) { (void)f; }

/* ---- the jack driver: collect everything it is handed ---- */
#define CAP (60u * 32000u)
static int16_t  s_out[CAP];
static uint32_t s_n;
static int      s_space;                /* what the driver claims to want this frame */
int  uvm2_jack_init(void)  { return 1; }
int  uvm2_jack_space(void) { int v = s_space; s_space = 0; return v; }
void uvm2_jack_write(const int16_t *s, int n)
{
    for (int i = 0; i < n && s_n < CAP; i++) s_out[s_n++] = s[i];
}

/* ---- a wav, so it can be listened to ---- */
static void wr16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void wr32(FILE *f, uint32_t v) { wr16(f, (uint16_t)(v & 0xffff)); wr16(f, (uint16_t)(v >> 16)); }
static void write_wav(const char *path, const int16_t *pcm, uint32_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + n * 2); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, 1); wr16(f, 1);
    wr32(f, rate); wr32(f, rate * 2); wr16(f, 2); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, n * 2);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

/* One game frame at Tac/Scan's 40 Hz: the driver asks for 800 samples. */
#define FRAME 800
static void frames(int k) { for (int i = 0; i < k; i++) { s_space = FRAME; ts_jack_update(); } }

static void peak_of(const char *what, uint32_t from)
{
    int32_t pk = 0; long clipped = 0;
    for (uint32_t i = from; i < s_n; i++) {
        int32_t v = s_out[i] < 0 ? -s_out[i] : s_out[i];
        if (v > pk) pk = v;
        if (s_out[i] >= 32767 || s_out[i] <= -32768) clipped++;
    }
    printf("  %-34s peak %6d (%3d%% of full scale)  clipped %ld samples\n",
           what, pk, (int)(pk * 100 / 32768), clipped);
}

int main(int argc, char **argv)
{
    const char *bundle = argc > 1 ? argv[1] : "build/tacscan.pcm";
    const char *out    = argc > 2 ? argv[2] : "/tmp/out.wav";

    probe_psram = malloc(PSRAM_BYTES);
    if (!probe_psram) return 1;
    s_fp = fopen(bundle, "rb");
    if (!s_fp) { perror(bundle); return 1; }
    fseek(s_fp, 0, SEEK_END); s_len = (uint32_t)ftell(s_fp); fseek(s_fp, 0, SEEK_SET);
    printf("%s: %u bytes\n", bundle, s_len);

    if (!ts_jack_begin()) { printf("ts_jack_begin failed\n"); return 1; }

    /* THE PARTIAL CASE, which is now the normal one: the bundle arrives while the game runs,
     * so a sound is playable only once ITS OWN bytes are in. One slice, then ask. */
    ts_jack_load_step();
    printf("after 1 slice: ready=%d (the table)  percent=%d\n",
           ts_jack_ready(), ts_jack_load_percent());
    printf("  sound 0 (first in the bundle) available: %s\n", ts_jack_avail(0) ? "yes" : "not yet");
    printf("  sound 21 (last in the bundle) available: %s   <- must be 'not yet'\n",
           ts_jack_avail(21) ? "yes" : "not yet");

    int passes = 1;
    while (!ts_jack_load_step()) passes++;
    printf("loaded in %d slices, ready=%d, percent=%d\n",
           passes, ts_jack_ready(), ts_jack_load_percent());
    printf("  sound 21 available now: %s\n", ts_jack_avail(21) ? "yes" : "NO");
    if (!ts_jack_ready()) return 1;

    /* A scene with the shapes that matter: the continuous bed, a one-shot over it, several
     * at once, and a stop. Voice numbers are the game's own (SegaG80.h). */
    uint32_t m;
    m = s_n; ts_jack_play(5, 0, 1);              /* kVoiceShipRoar, looping   */
    frames(20);  peak_of("roar alone (looping)", m);

    m = s_n; ts_jack_play(1, 3, 0);              /* kVoiceShip: plaser        */
    frames(20);  peak_of("roar + laser", m);

    m = s_n; ts_jack_play(2, 6, 1);              /* kVoiceTunnel, looping     */
              ts_jack_play(4, 11, 0);            /* kVoiceEnemy: eexpl        */
              ts_jack_play(9, 21, 0);            /* kVoiceExtralife: 1up      */
    frames(30);  peak_of("roar + tunnel + expl + 1up", m);

    m = s_n; ts_jack_stop(2); ts_jack_stop(5);
    frames(20);  peak_of("after stopping roar and tunnel", m);

    /* The loop has to come round, not stop: sound 6 (tunnelh) is 0.18 s and this is 2 s. */
    m = s_n; ts_jack_play(2, 6, 1);
    frames(80);
    printf("  looping voice still live after 2 s: %s\n", ts_jack_playing(2) ? "yes" : "NO");
    m = s_n; ts_jack_play(1, 3, 0);
    frames(80);
    printf("  one-shot voice ended by itself:    %s\n", ts_jack_playing(1) ? "NO" : "yes");

    write_wav(out, s_out, s_n, 32000);
    printf("%s: %.2f s\n", out, s_n / 32000.0);
    return 0;
}
