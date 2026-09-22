/* HOW OFTEN THE EMULATED 6809 TOUCHES MEMORY, AND WHERE.
 *
 * A measuring instrument, OPT-IN and host-only: turned on with -DAAE_ACCESS_COUNT, and
 * without it, it does not exist. It exists because on one of the boards the game's `.bss`
 * lives in PSRAM (rp2350_game_ram.ld: GAME_RAM = the XIP window at 0x11000000) and the
 * emulated game's ROM/RAM tables are in there. Every interpreter access is a PSRAM read,
 * except what the XIP cache (16 KB) catches, and "optimising the emulation" without knowing
 * how many accesses there are and which pages they hit is guesswork.
 *
 * Opcode and operand fetches do NOT go through the dispatch in aae_memdispatch.h — they go
 * straight to `ROM[A]` / `RAM[A]` (see the M6809_x_RDOP/RDOP_ARG macros) — so the counting
 * is put in the macros, the one place where all four paths are visible and where the CPU is
 * known.
 *
 * The breakdown is per 4 KB page of the EMULATED CPU's address space (16 buckets per CPU),
 * which is what is needed to answer "which chunk fits in SRAM?".
 */
#ifndef AAE_ACCESS_COUNT_H
#define AAE_ACCESS_COUNT_H

#ifdef AAE_ACCESS_COUNT

/* [cpu][4 KB page] */
extern unsigned long aae_acc_op[2][16];   /* opcode fetch    -> ROM[A] directly  */
extern unsigned long aae_acc_ar[2][16];   /* operand fetch   -> RAM[A] directly  */
extern unsigned long aae_acc_rd[2][16];   /* data read       -> dispatch         */
extern unsigned long aae_acc_wr[2][16];   /* data write      -> dispatch         */

#define AAE_CNT(table, cpu, a) (table[cpu][((unsigned)(a) >> 12) & 15u]++)

/* The definition lives in ONE translation unit, the one that declares this. */
#ifdef AAE_ACCESS_COUNT_DEFINE
unsigned long aae_acc_op[2][16], aae_acc_ar[2][16];
unsigned long aae_acc_rd[2][16], aae_acc_wr[2][16];
#endif

#else
#define AAE_CNT(table, cpu, a) ((void)0)
#endif

#endif /* AAE_ACCESS_COUNT_H */
