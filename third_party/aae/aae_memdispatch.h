/* PER-PAGE MEMORY DISPATCH, reads and writes alike.
 *
 * Each emulated CPU carries a 256-byte table, one slot index per 256-byte page. Slots
 * 0..3 are the fixed handlers (error / nop / plain array / linear-scan fallback); the
 * rest are allocated at init, one per distinct (handler, start) pair of the memory maps.
 * A page with a single handler resolves in one table lookup and one call; a page a range
 * only partly covers is MIXED and takes the linear-scan fallback (mrh_readmem /
 * mwh_writemem), which resolves the address against the map itself.
 *
 * Before this, every custom-handler region went through that linear scan: in ESB that
 * was ALL the code (banked ROM) and the main 6809 cost ~100 M33 cycles per emulated
 * cycle, three times the sound 6809 whose ROM is a plain array. The table is what fixed
 * that, and the hot path -- instruction fetch, ROM, RAM -- is whole pages.
 *
 * READS WERE PER ADDRESS UNTIL 2026-09-21, which meant 65536 bytes per CPU: 128 KB for a
 * two-6809 game, and the largest thing in the image after the ROMs. On the .um2 target,
 * where the whole game lives in SRAM, that alone was more than the deficit that kept Star
 * Wars from fitting. Counted before changing it: of Star Wars' 19 main-CPU read ranges,
 * 10 are not page-aligned and they fall in THREE pages (I/O, DIPs, ADC, mathbox, PRNG);
 * the sound CPU has two in ONE page. So the scan is paid by I/O reads and by the sound
 * CPU's 6532 scratchpad, whose writes already paid it -- and never by a fetch.
 *
 * 128 KB -> 512 bytes, and the two sides now work the same way. */
#ifndef AAE_MEMDISPATCH_H
#define AAE_MEMDISPATCH_H
/* ONE SLOT PER DISTINCT (handler, start) PAIR, and 256 is room for far more than any of
 * these games has -- Star Wars uses about 25 across both CPUs. Each slot costs 8 bytes
 * per direction (a function pointer and a start address), so 256 is 4 KB of tables for a
 * few dozen entries. Running OUT is not a failure: `rd_slot_for` hands back the
 * linear-scan fallback, which is what every custom handler used before these tables
 * existed -- slower, never wrong. So a memory-tight target can lower it. */
#ifndef AAE_HANDLER_SLOTS
#define AAE_HANDLER_SLOTS 256
#endif
extern int  (*rd_handler_tbl[AAE_HANDLER_SLOTS])(int);
extern int  rd_handler_start[AAE_HANDLER_SLOTS];
extern void (*wr_handler_tbl[AAE_HANDLER_SLOTS])(int,int);
extern int  wr_handler_start[AAE_HANDLER_SLOTS];

/* THE PLAIN-MEMORY CASE DOES NOT NEED A CALL (2026-09-19) -- MAME's opbase idea, which
 * the note in cpuintrf.c said was still missing.
 *
 * Almost every byte an emulated CPU touches is plain RAM or ROM, and the handler those
 * addresses point at is literally `return RAM[address]` (mrh_ram) / `RAM[address] = data`
 * (mwh_ram). Reaching it cost two table loads, an indirect call and its prologue on top
 * of the load that actually does the work -- measured at ~170 M33 cycles per emulated
 * 6809 instruction, which at 8,700 instructions a frame is 60% of starwars' gameplay
 * frame. Testing the slot index first turns that into a compare and a load.
 *
 * `RAM` is the same global the handlers read, and cpu_run repoints it per CPU, so this
 * is the identical value by construction -- not an equivalent one. The fixed slot
 * numbers come from cpuintrf.c's enums and are asserted there against these. */
#define AAE_RDH_RAM 2u    /* RDH_RAM  in cpuintrf.c: mrh_ram, start 0 */
#define AAE_WRH_RAM 2u    /* WRH_RAM  in cpuintrf.c: mwh_ram, start 0 */
extern unsigned char *RAM;

/* ── BANKED ROM DOES NOT NEED A CALL EITHER ─────────────────────────────────────────
 *
 * The shortcut above rescues plain RAM, but a game whose CODE is banked never touches it:
 * its pages point at a handler, and every byte it executes pays two loads, an indirect call
 * and the callee's prologue, only to end up doing an `ldrb`.
 *
 * MEASURED on one of the ports (sampling profile of the host harness): two bank-read
 * handlers took 8.1% and 5.6% of emulation time — 13.7% in two functions that are literally
 * `return ROM[base + offset]`. That is 19,837 calls per frame out of the main 6809's 26,396
 * reads: 75% of its traffic.
 *
 * THE IDEA IS MAME's (opbase), named above as still missing: if a slot's handler is a flat
 * array with a base that only changes when the game flips a latch, then whoever flips the
 * latch can WRITE THE BASE DOWN and the read becomes `base[a]`. The base is stored already
 * biased (array + region_start - map_start), so `base[a]` is exactly what `handler(a -
 * start)` would return — not an equivalent, the same number.
 *
 * ONLY FOR SIDE-EFFECT-FREE HANDLERS. A plain `return ROM[...]` qualifies; a handler that
 * also POKES hardware on every access does not (and at a few dozen accesses per frame there
 * is nothing to gain by inlining it anyway). Giving a base to a handler with side effects
 * would stop them running SILENTLY, which is the worst way to break this.
 *
 * IT IS PER GAME, WHICH IS WHY IT SITS BEHIND A FLAG. It is not free for those who do not
 * use it: the table is 1 KB (256 slots x 4) and the test is inlined at EVERY access site.
 * Enabled by default, one of the ports stopped fitting in the UVM2 by 6,824 bytes — and a
 * port that does not bank its code gains absolutely nothing in return. Turn it on with
 * -DAAE_RD_BASE in the Makefile of the game that does need it. */
#ifdef AAE_RD_BASE
extern const unsigned char *rd_base_tbl[AAE_HANDLER_SLOTS];
/* Writes down the base of the slot serving (handler, start). The game calls it from the
 * same place that flips the latch. NULL withdraws it and the call comes back. */
void aae_rd_base(int (*h)(int), int start, const unsigned char *base);
#else
#define aae_rd_base(h, start, base) ((void)0)
#endif

#ifdef AAE_WATCH_READ
/* WHO READS THIS BYTE. Host-only: set aae_watch_addr and every read of it records the
 * emulated PC, which names the routine. Finding "the code that draws screen X" starts
 * from the string X is made of. */
extern unsigned aae_watch_addr, aae_watch_hits;
extern unsigned short aae_watch_pc[64];
extern unsigned short m6809_1_pc_now(void);
extern unsigned short m6809_1_stack_at(int);
/* WHO CHANGES THIS BYTE. The read watch above names the code that CONSUMES a value;
 * a state variable is more usefully chased from the other end -- what advances it --
 * and only when it actually changes, since a routine that rewrites the same value
 * every frame would bury the transition under thousands of hits. */
extern unsigned aae_watch_waddr, aae_watch_whits;
extern unsigned short aae_watch_wpc[64];
/* WHICH BYTES OF A REGION A SCREEN READS. The watches above chase ONE address; finding
 * which of a game's dozens of message strings a given screen draws is the other
 * question, and it is answered by marking every address in a window that gets read
 * while a chosen phase is on screen. */
extern unsigned aae_range_lo, aae_range_hi, aae_range_on;
extern unsigned char aae_range_hit[0x2000];
#endif

/* SPEED OR SIZE, AND THE ANSWER IS NOT THE SAME ON BOTH BOARDS.
 *
 * Inlining this into every opcode handler is what took the 6809 off a call per memory
 * access, and on our own cartridge -- where the frame was CPU-bound and the game went
 * from 27 to 45 fps -- that is the whole point. It costs about 15 KB of code:
 * `fetch_effective_address` alone grows 2716 bytes, plus a long tail of `*_ix` handlers.
 *
 * On the .um2 target the binding constraint is the other one. The game lives entirely in
 * SRAM, and 15 KB of text is worth more than the cycles. -DAAE_DISPATCH_NOINLINE keeps one copy of each. */
#ifdef AAE_DISPATCH_NOINLINE
/* `static` alone is not enough: at -O3 the compiler inlines it anyway, which is the
 * whole thing being avoided here. */
#define AAE_DISPATCH_INLINE __attribute__((noinline))
#else
#define AAE_DISPATCH_INLINE inline
#endif

static AAE_DISPATCH_INLINE unsigned aae_rd_byte(const unsigned char *idx, unsigned a)
{
    unsigned h = idx[a >> 8];   /* per 256-byte page, like the write side */
#ifdef AAE_WATCH_READ
    if (aae_range_on && a >= aae_range_lo && a < aae_range_hi
        && a - aae_range_lo < sizeof aae_range_hit)
        aae_range_hit[a - aae_range_lo] = 1;
    if (a == aae_watch_addr && aae_watch_hits < 8) {
        unsigned w = aae_watch_hits++;
        int d;
        aae_watch_pc[w * 8] = m6809_1_pc_now();
        for (d = 0; d < 7; d++) aae_watch_pc[w * 8 + 1 + d] = m6809_1_stack_at(d);
    }
#endif
    if (h == AAE_RDH_RAM) return RAM[a];
#ifdef AAE_RD_BASE
    {   /* Banked ROM: the base was written down by whoever flipped the latch. See above. */
        const unsigned char *b = rd_base_tbl[h];
        if (b) return b[a];
    }
#endif
    return (unsigned)rd_handler_tbl[h]((int)a - rd_handler_start[h]);
}
static AAE_DISPATCH_INLINE void aae_wr_byte(const unsigned char *idx, unsigned a, int v)
{
    unsigned h = idx[a >> 8];
#ifdef AAE_MARK_WRITES
    /* Host: marks WHICH addresses are written this frame. */
    { extern unsigned char wr_frame[0x10000]; wr_frame[a & 0xffff] = 1; }
#endif
#ifdef AAE_WATCH_READ
    if (a == aae_watch_waddr && aae_watch_whits < 8
        && RAM[a] != (unsigned char)v) {
        unsigned w = aae_watch_whits++;
        int d;
        aae_watch_wpc[w * 8] = m6809_1_pc_now();
        for (d = 0; d < 7; d++) aae_watch_wpc[w * 8 + 1 + d] = m6809_1_stack_at(d);
    }
#endif
    if (h == AAE_WRH_RAM) { RAM[a] = (unsigned char)v; return; }
    wr_handler_tbl[h]((int)a - wr_handler_start[h], v);
}
#endif
