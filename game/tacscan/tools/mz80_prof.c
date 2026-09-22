/* mz80_prof.c — which Z80 opcode is eating the frame, and is its cycle count right.
 *
 * WHY THIS EXISTS. On hardware Tac/Scan's frame is 41 ms of which only 23 are the
 * beam: the executor sits idle 16.5 ms per frame waiting for the builder, and the
 * builder is the Z80 plus the vector generator. So the question is not "is the
 * drawing too slow" (measured: the bus runs at 97% of nominal) but "what is the
 * emulation spending itself on".
 *
 * TWO DIFFERENT BUGS LOOK THE SAME FROM OUTSIDE, and this separates them:
 *
 *   1. A WRONG CYCLE COUNT. mz80 subtracts a literal per opcode. If one of them
 *      does not match the real Z80, the game's own timing loops run for a different
 *      number of emulated cycles than they should — the machine appears to do more
 *      work per frame than the real board did. This is the Major Havoc case from
 *      2026-09-17, where a Musashi opcode waited too long. It shows up here as
 *      cycles/execution that does not match the Z80 table, and NO timing is needed
 *      to find it: the count comes from the emulator's own counter.
 *
 *   2. A SLOW HANDLER. The opcode costs the right number of EMULATED cycles but too
 *      much REAL time on the M33. mz80's memory access is a linear scan of the
 *      handler table on every read and write, so an instruction that touches memory
 *      pays for the length of that table. That shows up as executions concentrated
 *      in the memory opcodes rather than as a bad cycles/execution.
 *
 * The PC histogram answers a third question the opcode counts cannot: WHERE. A hot
 * opcode spread over the whole ROM is the game working; a hot opcode in one 256-byte
 * bucket is a loop, and then the thing to read is that loop.
 *
 * Time is deliberately NOT measured per opcode: a clock read costs more than the
 * instructions being measured and would report its own overhead. Executions and
 * emulated cycles are exact and cost two increments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PC_BUCKETS 256          /* 64 KB of Z80 space, 256 bytes each */

static unsigned long long g_n[256];      /* executions per opcode        */
static unsigned long long g_cyc[256];    /* emulated cycles per opcode   */
static unsigned long long g_pc[PC_BUCKETS];
static unsigned long long g_pcb[65536];   /* byte resolution: WHICH instruction */
static int      g_prev_op = -1;
static int      g_prev_rem;

static const unsigned char *g_base;
static unsigned g_last_pc;   /* the instruction currently executing */

void mz80_prof_base(const unsigned char *base) { g_base = base; }

void mz80_prof(unsigned char op, int rem, const unsigned char *pcp)
{
    unsigned pc = g_base ? (unsigned)(pcp - g_base) : 0u;
    g_last_pc = pc;
    if (g_prev_op >= 0) {
        /* The counter only ever goes down inside a run; a rise means mz80exec was
         * re-entered (a new slice), and that instruction's cost belongs to neither
         * slice. Dropping it is worth a handful of samples out of millions. */
        int d = g_prev_rem - rem;
        if (d > 0 && d < 1000) g_cyc[g_prev_op] += (unsigned)d;
    }
    g_n[op]++;
    g_pc[(pc >> 8) & (PC_BUCKETS - 1)]++;
    g_pcb[pc & 0xFFFF]++;
    g_prev_op  = op;
    g_prev_rem = rem;
}

/* ── WATCHPOINT ─────────────────────────────────────────────────────────────
 * Which PCs write the address the idle loop waits on. Fed from Vector_Write, the
 * catch-all handler every Z80 write goes through. */
#define WATCH_ADDR 0xC80A
static struct { unsigned pc; unsigned long long n; } g_w[16];
static int g_wn;
extern const unsigned char *mz80_prof_pc_now(void);

void mz80_prof_watch(unsigned addr, unsigned data)
{
    unsigned pc;
    int i;
    (void)data;
    if (addr != WATCH_ADDR) return;
    pc = g_last_pc;
    for (i = 0; i < g_wn; i++) if (g_w[i].pc == pc) { g_w[i].n++; return; }
    if (g_wn < 16) { g_w[g_wn].pc = pc; g_w[g_wn].n = 1; g_wn++; }
}

/* The real Z80's cycle count for the un-prefixed opcodes, to compare against what
 * mz80 subtracts. 0 = a prefix or a conditional whose cost depends on the branch,
 * which cannot be checked against a single number and is skipped. */
static const unsigned char z80_cyc[256] = {
     4,10, 7, 6, 4, 4, 7, 4,  4,11, 7, 6, 4, 4, 7, 4,
     0,10, 7, 6, 4, 4, 7, 4, 12,11, 7, 6, 4, 4, 7, 4,
     0,10,16, 6, 4, 4, 7, 4,  0,11,16, 6, 4, 4, 7, 4,
     0,10,13, 6,11,11,10, 4,  0,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4,  4, 4, 4, 4, 4, 4, 7, 4,
     0,10, 0,10, 0,11, 7,11,  0,10, 0, 0, 0,17, 7,11,
     0,10, 0,11, 0,11, 7,11,  0, 4, 0,11, 0, 0, 7,11,
     0,10, 0,19, 0,11, 7,11,  0, 4, 0, 4, 0, 0, 7,11,
     0,10, 0, 4, 0,11, 7,11,  0, 6, 0, 4, 0, 0, 7,11,
};

static int by_cyc(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (g_cyc[y] > g_cyc[x]) - (g_cyc[y] < g_cyc[x]);
}

void mz80_prof_dump(unsigned frames)
{
    int ord[256];
    unsigned long long tot_n = 0, tot_c = 0;
    int i;

    for (i = 0; i < 256; i++) { ord[i] = i; tot_n += g_n[i]; tot_c += g_cyc[i]; }
    qsort(ord, 256, sizeof ord[0], by_cyc);

    printf("\n%llu instructions, %llu emulated cycles over %u frames"
           "  (%llu cyc/frame)\n", tot_n, tot_c, frames, frames ? tot_c / frames : 0);
    printf("A real Tac/Scan Z80 runs 3.867 MHz / 60 Hz = 64450 cycles per frame.\n");

    printf("\n  op   executions      cycles   %%cyc  cyc/exec  z80  note\n");
    for (i = 0; i < 18; i++) {
        int op = ord[i];
        if (!g_n[op]) break;
        double per = (double)g_cyc[op] / (double)g_n[op];
        int ref = z80_cyc[op];
        const char *note = "";
        if (ref == 0)                       note = "prefix/conditional, not comparable";
        else if (per > ref + 0.5)           note = "<<< SLOWER THAN A REAL Z80";
        else if (per < ref - 0.5)           note = "faster than a real Z80";
        printf("  %02X %12llu %11llu %5.1f %9.2f %4d  %s\n",
               op, g_n[op], g_cyc[op], 100.0 * g_cyc[op] / (double)(tot_c ? tot_c : 1),
               per, ref, note);
    }

    /* Every opcode whose average disagrees with the table, however rare: the Major
     * Havoc bug was one instruction, and a rare instruction inside a delay loop
     * costs exactly as much as a common one. */
    printf("\n  cycle counts that disagree with the Z80 table:\n");
    {
        int found = 0;
        for (i = 0; i < 256; i++) {
            double per;
            if (!g_n[i] || !z80_cyc[i]) continue;
            per = (double)g_cyc[i] / (double)g_n[i];
            if (per > z80_cyc[i] + 0.5 || per < z80_cyc[i] - 0.5) {
                printf("    %02X  %.2f cycles/exec vs %d  (%llu executions)\n",
                       i, per, z80_cyc[i], g_n[i]);
                found = 1;
            }
        }
        if (!found) printf("    none — every opcode matches\n");
    }

    printf("\n  hottest 256-byte blocks of Z80 address space:\n");
    {
        int b, top[6] = {0};
        for (b = 0; b < PC_BUCKETS; b++) {
            int k;
            for (k = 0; k < 6; k++)
                if (g_pc[b] > g_pc[top[k]]) {
                    int j;
                    for (j = 5; j > k; j--) top[j] = top[j-1];
                    top[k] = b; break;
                }
        }
        for (b = 0; b < 6; b++)
            if (g_pc[top[b]])
                printf("    %04X-%04X  %llu  (%.1f%%)\n", top[b] << 8, (top[b] << 8) + 255,
                       g_pc[top[b]], 100.0 * g_pc[top[b]] / (double)(tot_n ? tot_n : 1));

        /* AND THE INDIVIDUAL ADDRESSES. A 256-byte block says "the loop is around
         * here"; this says which instruction, which is what you need to read the
         * loop and to tell a loop body from the code that calls it. */
        printf("\n  who writes $%04X (the address the idle loop waits on):\n", WATCH_ADDR);
    {
        int k;
        if (!g_wn) printf("    nobody — in 120 frames it was never written\n");
        for (k = 0; k < g_wn; k++)
            printf("    from PC $%04X  %llu writes\n", g_w[k].pc, g_w[k].n);
    }

    printf("\n  hottest individual addresses:\n");
        {
            int k;
            for (k = 0; k < 14; k++) {
                int best = 0, a;
                for (a = 0; a < 65536; a++) if (g_pcb[a] > g_pcb[best]) best = a;
                if (!g_pcb[best]) break;
                printf("    %04X  %llu  (%.1f%% of all instructions)\n",
                       best, g_pcb[best], 100.0 * g_pcb[best] / (double)(tot_n ? tot_n : 1));
                g_pcb[best] = 0;
            }
        }

        /* THE BYTES OF THE HOT BLOCKS, so the loop can actually be read. They are
         * dumped from the Z80's own memory rather than from the zip, because what
         * matters is what it EXECUTES: the romset is several chips and the mapping
         * is the driver's business. Fed to tools/z80dis.py. */
        if (g_base) {
            FILE *f = fopen("build/hot.bin", "wb");
            if (f) {
                for (b = 0; b < 6; b++) {
                    if (!g_pc[top[b]]) break;
                    fprintf(stderr, "dumped %04X-%04X to build/hot.bin\n",
                            top[b] << 8, (top[b] << 8) + 255);
                    fwrite(g_base + (top[b] << 8), 1, 256, f);
                }
                fclose(f);
                f = fopen("build/hot.txt", "w");
                if (f) {
                    for (b = 0; b < 6; b++)
                        if (g_pc[top[b]]) fprintf(f, "%04X\n", top[b] << 8);
                    fclose(f);
                }
            }
        }
    }
}
