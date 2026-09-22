/* HOW MANY COMMANDS A REAL FRAME ASKS FOR, AND WHERE THEY GO.
 *
 * UVM2_CMD_CAPACITY used to be chosen by analogy ("12288 was enough for Star Wars"), and a
 * cap that is exceeded does NOT look like an error: it looks like an incomplete drawing.
 * This answers the question before anything is built — it links the REAL emitter
 * (uvm2_draw.c) against a simulated bus and feeds it the geometry a game's host harness
 * dumped from one concrete frame.
 *
 * And it answers all three halves:
 *   - how many COMMANDS (the cap, UVM2_CMD_CAPACITY),
 *   - how many bus WORDS (the stream's LIST_MAX: one per command, TWO if it carries a
 *     delay — see uvm2_exec in uvm2_bus.c),
 *   - how many bus CYCLES (the frame's duration, which fixes the fps and depends on no
 *     tuning at all: 667 ns per cycle).
 *
 * THE BREAKDOWN IS WHAT EXPLAINS THE DIFFERENCES BETWEEN GAMES. "Commands per segment" is
 * not an SDK constant: it comes out of the GEOMETRY. A long stroke is split into more ramp
 * steps, a stroke that does not start where the previous one ended pays a whole jump, and a
 * brightness change pays its own write. That is why the per-VIA-register breakdown and the
 * geometry statistics are worth reading together, not the average on its own.
 *
 *   cd <game> && make host && ./build_wasm/<game>_host 60 2>/tmp/frame.txt
 *   (cd <vectrex-draw>/cabi && cargo build --release)      # the .a for the host
 *   cc -O2 -DUVM2_HOST -DUVM2_BENCH_NO_CORE1 -DUVM2_SUBUNITS -DUVM2_CMD_CAPACITY=65536u \
 *      -I<sdk> -o /tmp/count tools/uvm2_list_count.c <sdk>/uvm2_draw.c \
 *      <vectrex-draw>/cabi/target/release/libvectrex_draw_cabi.a
 *   /tmp/count /tmp/frame.txt
 *
 * The cap is set LARGE when compiling this (65536): what we want to know is how much the
 * frame asks for, not whether it fits the cap we already had.
 *
 * The input is lines of "x0 y0 x1 y1 z" in the units the game passes to v_directDraw32
 * (each port's host harness already dumps them in that format).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"

uvm2_stats_t uvm2_stats;

static uint32_t g_cmds, g_cycles, g_words;
static uint32_t g_reg_cmds[16], g_reg_cycles[16];
static uint32_t g_sr_total, g_sr_zero, g_sr_repeat, g_sr_prev;

/* A command is 3 bytes: delay 12, register 4, data 8 (see tools/uvm2_anatomy.c), and the
 * executor spends 1 + delay bus cycles on each one. */
uint32_t uvm2_exec(const uint8_t *c, uint32_t n)
{
    g_cmds = n; g_cycles = 0; g_words = 0;
    for (uint32_t i = 0; i < 16; i++) { g_reg_cmds[i] = 0; g_reg_cycles[i] = 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)c[i*3] | ((uint32_t)c[i*3+1] << 8) | ((uint32_t)c[i*3+2] << 16);
        uint32_t reg = (v >> 8) & 0xF, d = UVM2_CMD_DELAY(v);
        g_cycles    += 1 + d;
        g_words     += d ? 2u : 1u;
        g_reg_cmds[reg]++;
        g_reg_cycles[reg] += 1 + d;
        /* THE SR IS NOT "BRIGHTNESS CHANGE". A game that never changes intensity writes the
         * SR hundreds of times per frame, and it is worth knowing WHAT it writes: if it
         * alternates between zero and one value, those are the beam being blanked and
         * unblanked around each jump, not brightness — and then "do not write if it did not
         * change" does not apply. */
        if (reg == UVM2_VIA_SR) {
            uint32_t data = v & 0xFFu;
            g_sr_total++;
            if (data == 0) g_sr_zero++;
            if (g_sr_total > 1 && data == g_sr_prev) g_sr_repeat++;
            g_sr_prev = data;
        }
    }
    return g_cycles;
}
void     uvm2_bus_delay(uint32_t c) { (void)c; }
void     uvm2_via_write(uint32_t r, uint32_t d) { (void)r; (void)d; }
uint8_t  uvm2_via_read(uint32_t r) { (void)r; return 0; }

/* The SDK neighbours uvm2_draw.c calls and that mean nothing here. */
void uvm2_config_load(void) {}
volatile int uvm2_have_calibration = 0;
unsigned uvm2_smp_s_active = 0;   /* uvm2_smp_active() is inline in uvm2_smp.h */
int  uvm2_smp_due(uint32_t c, uint8_t *v) { (void)c; (void)v; return 0; }
uint8_t uvm2_smp_mixer(void) { return 0; }
int  uvm2_smp_needs_latch(void) { return 0; }
void uvm2_smp_frame(uint32_t c) { (void)c; }
uint32_t uvm2_smp_injected = 0;
void rust_eh_personality(void) {}

/* The same arithmetic as sdk_rp2350.c. CAREFUL: 127, not 100 — these are VPy's logical
 * units (a +-127 screen). With 100 the ramps come out 1.27x longer and everything counted
 * here belongs to a different game. */
#ifndef VPY_SCALE
#define VPY_SCALE 127
#endif
#define VS_Q4(v) ((int)(((v) < 0 ? (long)(v) * 16 - VPY_SCALE / 2 \
                                 : (long)(v) * 16 + VPY_SCALE / 2) / VPY_SCALE))

static const char *name(uint32_t reg)
{
    switch (reg) {
    case UVM2_VIA_PORTA: return "PORT_A  DAC: X or Y rate";
    case UVM2_VIA_PORTB: return "PORT_B  mux / sample Y";
    case UVM2_VIA_T1CL:  return "T1CL    the ramp's scale";
    case UVM2_VIA_T1CH:  return "T1CH    STARTS the ramp";
    case UVM2_VIA_T1LL:  return "T1LL    WAITS for it to finish";
    case UVM2_VIA_PCR:   return "CNTL    unblanks/blanks the beam";
    case UVM2_VIA_SR:    return "SR      brightness (shift register)";
    case UVM2_VIA_ACR:   return "ACR";
    case UVM2_VIA_DDRA:  return "DDRA";
    case UVM2_VIA_DDRB:  return "DDRB";
    default:             return "other";
    }
}

#define MAXSEG 200000
static int sx0[MAXSEG], sy0[MAXSEG], sx1[MAXSEG], sy1[MAXSEG], sz[MAXSEG];

static int cmp_d(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    /* WHICH UNITS THE DUMP COMES IN, WHICH IS NOT THE SAME FOR EVERY HARNESS.
     *
     * The AAE ports go through `aae-src/vector.h`, and there the host harness is compiled
     * with -DNO_PI, which sets AAE_SCREEN_MUL = 1: the dump comes out in the GAME's units,
     * whereas on the real target the same function multiplies by 36 and shifts the measured
     * centre to zero. Handing the emitter the game's units measures ramps 36 times too
     * short — and the number that comes out belongs to no game at all. A game that does not
     * go through vector.h already dumps in target units.
     *
     *   MUL=36 OX=-13356 OY=-13968 ./count frame.txt      # AAE ports
     *   ./count frame.txt                                 # identity (hand-written ports, VPy)
     */
    const long mul = getenv("MUL") ? atol(getenv("MUL")) : 1;
    const long ox  = getenv("OX")  ? atol(getenv("OX"))  : 0;
    const long oy  = getenv("OY")  ? atol(getenv("OY"))  : 0;

    long n = 0;
    while (n < MAXSEG &&
           fscanf(f, "%d %d %d %d %d", &sx0[n], &sy0[n], &sx1[n], &sy1[n], &sz[n]) == 5) {
        if (!sz[n]) continue;
        sx0[n] = (int)(sx0[n] * mul + ox); sy0[n] = (int)(sy0[n] * mul + oy);
        sx1[n] = (int)(sx1[n] * mul + ox); sy1[n] = (int)(sy1[n] * mul + oy);
        n++;
    }
    if (!n) { fprintf(stderr, "no segments in the input\n"); return 1; }

    /* ── REORDER TO CHAIN (REORDER=1) ─────────────────────────────────────────────
     *
     * It does NOT change the drawing: every stroke carries both of its absolute endpoints,
     * so reordering them paints exactly the same segments. What changes is how many JUMPS
     * are needed between them, and a jump is a whole ramp — ~6 commands on the bus PLUS a
     * complete ramp resolution in the builder. In one port that is 709 jumps for 1136
     * strokes (62%); another, which draws chained .vec paths, jumps 30% of the time.
     *
     * This is a MEASUREMENT, not a proposed implementation: it says how much there would be
     * to gain before deciding whether reviving a reorder pass in the SDK is worth it (it was
     * retired along with the other whole-frame knobs, though reordering is the only one of
     * that family that cannot move geometry). Greedy nearest neighbour, which is the floor
     * of what a good one would give. */
    if (getenv("REORDER")) {
        /* CHAIN FIRST, THEN ORDER THE CHAINS. Plain nearest neighbour makes it WORSE
         * (measured: 709 -> 882 jumps) because it eats segments that were the continuation
         * of another chain. So the polylines are built first, following exact endpoint
         * coincidences, and only THOSE are then ordered.
         *
         * And this is where the result that matters comes from: the NUMBER of jumps is not
         * decided by the order, it is decided by how many disjoint polylines the game
         * draws. Reordering can only SHORTEN them. */
        char *used = calloc((size_t)n, 1);
        long *next = malloc(sizeof(long) * (size_t)n);
        for (long i = 0; i < n; i++) next[i] = -1;
        /* link: for each stroke, one whose start == its end and that has no parent yet */
        char *has_parent = calloc((size_t)n, 1);
        for (long i = 0; i < n; i++) {
            if (next[i] >= 0) continue;
            for (long j = 0; j < n; j++) {
                if (j == i || has_parent[j]) continue;
                if (sx0[j] == sx1[i] && sy0[j] == sy1[i]) { next[i] = j; has_parent[j] = 1; break; }
            }
        }
        long *order = malloc(sizeof(long) * (size_t)n); long k = 0;
        int px = 0, py = 0;
        long chains = 0;
        for (;;) {
            long best = -1; long long bestd = -1;
            for (long i = 0; i < n; i++) {
                if (used[i] || has_parent[i]) continue;        /* chain heads only */
                long long dx = sx0[i] - px, dy = sy0[i] - py;
                long long d = dx*dx + dy*dy;
                if (best < 0 || d < bestd) { best = i; bestd = d; }
            }
            if (best < 0) break;
            chains++;
            for (long i = best; i >= 0 && !used[i]; i = next[i]) {
                used[i] = 1; order[k++] = i; px = sx1[i]; py = sy1[i];
            }
        }
        for (long i = 0; i < n; i++) if (!used[i]) order[k++] = i;   /* just in case */
        int *a=malloc(sizeof(int)*(size_t)n),*b=malloc(sizeof(int)*(size_t)n),
            *c=malloc(sizeof(int)*(size_t)n),*d=malloc(sizeof(int)*(size_t)n),
            *e=malloc(sizeof(int)*(size_t)n);
        for (long q=0;q<n;q++){ long i=order[q]; a[q]=sx0[i]; b[q]=sy0[i]; c[q]=sx1[i]; d[q]=sy1[i]; e[q]=sz[i]; }
        for (long q=0;q<n;q++){ sx0[q]=a[q]; sy0[q]=b[q]; sx1[q]=c[q]; sy1[q]=d[q]; sz[q]=e[q]; }
        free(used); free(next); free(has_parent); free(order);
        free(a); free(b); free(c); free(d); free(e);
        printf("  [REORDERED: %ld chained polylines, ordered by nearest neighbour]\n", chains);
    }

    uvm2_draw_init();
    /* ZERO_EVERY=N: the safety net by ramp COUNT, which ships turned off. It is set AFTER
     * uvm2_draw_init because that resets it to its compile-time value. */
    { const char *e = getenv("ZERO_EVERY");
      if (e) { extern volatile int32_t uvm2_zero_every; uvm2_zero_every = atoi(e); } }
    uvm2_frame_begin();
    for (long i = 0; i < n; i++) {
        uvm2_draw_intensity(sz[i]);
        uvm2_draw_move_abs_q4(VS_Q4(sx0[i]), VS_Q4(sy0[i]));
        uvm2_draw_delta_q4(VS_Q4(sx1[i]) - VS_Q4(sx0[i]), VS_Q4(sy1[i]) - VS_Q4(sy0[i]));
    }
    uvm2_frame_end();

    printf("%s   (MUL=%ld OX=%ld OY=%ld, VPY_SCALE=%d)\n",
           argc > 1 ? argv[1] : "(stdin)", mul, ox, oy, VPY_SCALE);
    printf("  segments             %ld\n", n);
    printf("  COMMANDS             %u      -> UVM2_CMD_CAPACITY\n", g_cmds);
    printf("  bus words            %u      -> LIST_MAX (one core only)\n", g_words);
    printf("  bus cycles           %u      -> %.1f ms  (%.1f fps)\n",
           g_cycles, g_cycles * 667.0 / 1e6, g_cycles ? 1e9 / (g_cycles * 667.0) : 0.0);
    printf("  dropped              %u\n", uvm2_stats.dropped);
    printf("  COMMANDS PER SEGMENT %.2f\n", (double)g_cmds / (double)n);

    printf("\n  where the commands go (by VIA register):\n");
    printf("    %-32s %8s %8s %8s\n", "register", "cmds", "%", "cycles");
    for (int r = 0; r < 16; r++) {
        if (!g_reg_cmds[r]) continue;
        printf("    %-32s %8u %7.1f%% %8u\n", name((uint32_t)r), g_reg_cmds[r],
               100.0 * g_reg_cmds[r] / g_cmds, g_reg_cycles[r]);
    }

    /* ── THE GEOMETRY, which is what really explains the number ────────────────
     *
     * "Commands per segment" is not fixed by the SDK: it is fixed by what the game draws.
     *   chained      a stroke that starts where the previous one ended pays NO jump.
     *   long         a long ramp is split into more steps (the t1 ladder).
     *   brightness   an intensity change is one more write per stroke. */
    long chained = 0, z_changes = 0;
    double *lens = malloc(sizeof(double) * (size_t)n);
    double *jumps = malloc(sizeof(double) * (size_t)n);
    long n_jumps = 0;
    double sum_l = 0;
    for (long i = 0; i < n; i++) {
        double dx = sx1[i] - sx0[i], dy = sy1[i] - sy0[i];
        lens[i] = sqrt(dx * dx + dy * dy);
        sum_l += lens[i];
        if (i) {
            if (sx0[i] == sx1[i-1] && sy0[i] == sy1[i-1]) chained++;
            else {
                double jx = sx0[i] - sx1[i-1], jy = sy0[i] - sy1[i-1];
                jumps[n_jumps++] = sqrt(jx * jx + jy * jy);
            }
            if (sz[i] != sz[i-1]) z_changes++;
        }
    }
    qsort(lens, (size_t)n, sizeof(double), cmp_d);
    if (n_jumps) qsort(jumps, (size_t)n_jumps, sizeof(double), cmp_d);

    if (g_sr_total)
        printf("\n  the SR (brightness): %u writes — %u to ZERO (%.0f%% beam blanked),"
               " %u repeating the same value\n",
               g_sr_total, g_sr_zero, 100.0*g_sr_zero/g_sr_total, g_sr_repeat);
    printf("\n  the geometry, which is what explains the number:\n");
    printf("    stroke length        median %.1f   p90 %.1f   max %.1f   mean %.1f\n",
           lens[n/2], lens[(n*9)/10], lens[n-1], sum_l / (double)n);
    printf("    chained              %ld of %ld (%.1f%%)  <- these pay no jump\n",
           chained, n, 100.0 * chained / (double)n);
    printf("    jumps                %ld", n_jumps);
    if (n_jumps) printf("   median %.1f   p90 %.1f   max %.1f",
                        jumps[n_jumps/2], jumps[(n_jumps*9)/10], jumps[n_jumps-1]);
    printf("\n");
    /* ── HOW OFTEN THE BEAM IS RE-CENTRED ──────────────────────────────────────────
     *
     * `uvm2_zero_jump` (24 device units by default) forces a re-zero when the transport is
     * long, and `uvm2_zero_every` — the net by COUNT — ships TURNED OFF. In a scene of short
     * chained strokes that leaves enormous runs with no re-centring, and the drift shows up
     * as vectors that jump. This counts it.
     *
     * The threshold is in device units; the input is in the game's. The conversion is the
     * SDK's: device = game * MUL * 16 / VPY_SCALE / 16. */
    {
        /* The coordinates in these arrays ALREADY have MUL applied. VS_Q4 divides by
         * VPY_SCALE and multiplies by 16, and a device unit is q4>>4 — so one device unit is
         * VPY_SCALE of these. The threshold is 24. */
        const long game_threshold = 24L * VPY_SCALE;
        long forcing = 0, run = 0, worst = 0;
        for (long i = 0; i < n; i++) {
            long j = 0;
            if (i) { double jx = sx0[i]-sx1[i-1], jy = sy0[i]-sy1[i-1];
                     j = (long)(fabs(jx) > fabs(jy) ? fabs(jx) : fabs(jy)); }
            if (j >= game_threshold) { forcing++; if (run > worst) worst = run; run = 0; }
            else run++;
        }
        if (run > worst) worst = run;
        printf("\n  the beam re-zero (uvm2_zero_jump = 24 device units):\n");
        printf("    jumps that force one  %ld of %ld  (%.1f%%)\n", forcing, n, 100.0*forcing/n);
        printf("    WORST RUN with no re-centring  %ld consecutive segments\n", worst);
        printf("    (the threshold, in the input's units: %ld)\n", game_threshold);
    }
    printf("    brightness changes   %ld of %ld (%.1f%%)\n",
           z_changes, n, 100.0 * z_changes / (double)n);
    free(lens); free(jumps);
    return 0;
}
