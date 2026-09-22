/* aae_machine.c - the Tac/Scan "machine": the AAE global state that normally
 * lives in aaemain.c, trimmed to the single game we run (mirrors the asteroids
 * port's aae_machine.c, adapted for Sega G80 / Z80).
 *
 * KEY DIFFERENCE vs asteroids: SegaG80.c's init_segag80() does `switch(gamenum)`
 * expecting the AAE enum TACSCAN, and run_segag80() checks `gamenum==TACSCAN`.
 * So gamenum MUST equal TACSCAN (not 0), and driver[] is sized to cover that
 * index (a designated initializer fills only the TACSCAN row; the rest are
 * zero-init .bss and never read because cpu_control reads only driver[gamenum]).
 */
#include <stdint.h>
#include <string.h>
#include "globals.h"       /* struct AAEDriver, GI, MAX_ACPU, CONTEXT*, GameDef enum */
#include "cpu_control.h"   /* CPU_MZ80, CPU_NONE, INT_TYPE_INT */
#include "tacscan_romtable.h"  /* the load table; the ROM lives on the SD card */

/* SegaG80 entry points (SegaG80.c). */
extern int  init_segag80(void);
extern void run_segag80(void);
extern void end_segag80(void);

/* ---- machine globals (subset of aaemain.c's, referenced by our path) ---- */
unsigned char   *GI[5];                 /* Z80 game image / memory pointers */
CONTEXTM6502    *c6502[MAX_ACPU];       /* 6502 contexts (unused; referenced by dispatch) */
CONTEXTMZ80      cMZ80[MAX_ACPU];       /* Z80 CPU contexts (used) */
int              gamenum = TACSCAN;     /* SegaG80 switches on this enum */
int              WATCHDOG;
int              total_length;          /* vector-list length accumulator */
int              testsw;
int              paused;
colors           vec_colors[1024];      /* per-vector colour table */
aae_settings     config;                /* global config (zeroed) */

/* ---- the one game: Tac/Scan, copied from aaemain.c (driver[TACSCAN] row) ---- */
struct AAEDriver driver[TACSCAN + 1] =
{
    [TACSCAN] =
    { "tacscan", "Tac/Scan", 0,
      &init_segag80, 0, &run_segag80, &end_segag80,
      0, 0,                             /* game_dips, game_keys (input via getport) */
      0, 0,                             /* game_samples, artwork */
      {CPU_MZ80, CPU_NONE, CPU_NONE, CPU_NONE},
      {3000000, 0, 0, 0},               /* cpu_freq: 3 MHz Z80 */
      {1, 0, 0, 0},                     /* cpu_divisions */
      {1, 0, 0, 0},                     /* cpu_intpass_per_frame */
      {INT_TYPE_INT, 0, 0, 0},          /* cpu_int_type: maskable IRQ (not NMI) */
      {0, 0, 0, 0},                     /* int_cpu */
      40, VEC_COLOR, 0,                 /* fps 40, colour vectors, no rotation flag */
      {0, 1024, 0, 1024}                /* gamerect */
    }
};

/* ---- input: the Z80 reads Tac/Scan's controls through getport(0..6), called
 * from sega_ports_r2 (SegaG80.c) for I/O ports $F8-$FC. Sega switches are
 * Exact bit map from AAE's tacscan_keys (gamekeys.h) — the previous mapping was
 * wrong (fire/start on the low nibble, which sega_fix_dips masks off with
 * `val & 0xF0`), so the controls did nothing. Correct layout:
 *   getport(0) $F8: Coin1=0x20, Coin2=0x40  — ACTIVE-LOW, default 0xe0
 *   getport(4) $FC (button mode, ioSwitch&1): Start1=0x01, Fire=0x04, Grab=0x08 — ACTIVE-HIGH
 *   getport(6) $FC (spinner mode): per-read signed steering DELTA (accumulated
 *              by sega_ports_r2 into spinner_count/sign)
 *   getport(1..3) $F9/$FA/$FB: default 0xf0 (no player inputs there)
 * Vectrex mapping: stick X -> steer;  btn1 -> Fire;  btn2 -> Grab;
 *                  btn3 -> insert Coin;  btn4 -> Start.
 */
extern unsigned char currentButtonState;   /* P1 buttons in bits 0-3 (btn N = bit N-1) */
extern signed char   currentJoy1X;         /* -127..127 */
extern signed char   currentJoy1Y;         /* -127..127, + = up */

int getport(int port)
{
    int b = currentButtonState;
    int jx = currentJoy1X;
    switch (port) {
    case 0:                       /* $F8 coins — active LOW, default 0xe0 */
        return (b & 0x04) ? (0xe0 & ~0x20) : 0xe0;   /* button 3 -> Coin 1 */
    case 4: {                     /* $FC button mode — active HIGH */
        int v = 0;
        if (b & 0x08) v |= 0x01;   /* button 4 -> Start 1 */
        if (b & 0x01) v |= 0x04;   /* button 1 -> Fire    */
        if (b & 0x02) v |= 0x08;   /* button 2 -> Grab    */
        return v;
    }
    case 6:                       /* $FC spinner — steering delta from the stick */
        if (jx >  20) return (jx >  80) ?  5 :  3;   /* right */
        if (jx < -20) return (jx < -80) ? -5 : -3;   /* left  */
        return 0;
    case 1: case 2: case 3:       /* $F9/$FA/$FB — no player inputs */
        return 0xf0;
    default:
        return 0xff;
    }
}

/* ---- ROM loader: build the Z80 64 KB memory image (GI[0]) with the 22 program
 * ROMs at $0000-$AFFF, and region 1 (GI[1], used as `sintable` by the Sega
 * vector generator) with the s-c.xyt spatial-transform PROM at offset 0. Must
 * run BEFORE init_segag80(): cpu_control's initz80N points cMZ80.z80Base at
 * GI[0]. Static buffers (zero-init .bss), not malloc'd, to keep them off the
 * libc-stub arena (end_segag80's free(GI[1]) is never reached on the cart). ---- */
static unsigned char gi0_mem[0x10000];   /* Z80 address space (full 64 KB) */
/* Region 1 is used ONLY as `sintable` by the Sega vector generator, indexed
 * `sintable[((...) & 0x1ff) << 1]` → max 1022. The s-c.xyt PROM is exactly
 * 1024 bytes, so 0x400 suffices (AAE malloc'd 64 KB but never used past 1 KB).
 * Keeping it 1 KB instead of 64 KB is what makes the game fit GAME_RAM. */
static unsigned char gi1_mem[0x400];     /* region 1: xyt PROM / sin table */

/* THE IDLE SPIN, SKIPPED. See the long note on mz80_set_idle_pc.
 *
 * Tac/Scan waits for its once-a-frame interrupt by spinning at $7A84:
 *
 *     7A84  PUSH BC
 *     7A85  CALL $3A12      -> $45FF: LD HL,$CACB / LD A,(HL) / OR A / RET Z
 *     7A88  POP BC
 *     7A89  LD A,($C80A)
 *     7A8C  CP B
 *     7A8D  JR Z,$7A84
 *
 * Measured with `make host-prof`: 663 turns a frame, 94.8% of all the Z80's cycles,
 * and each turn passes through Vector_Write four times (PUSH + CALL). It is safe to
 * cut because $C80A has exactly one writer — PC $007C, the IRQ handler, once per
 * frame — so nothing can end the wait before the next slice anyway.
 *
 * The address belongs here and not in mz80: the core provides the mechanism, the
 * game provides the fact about itself. */
void mz80_set_idle_pc(unsigned int pc);
#define TACSCAN_IDLE_PC 0x7A84

void aae_load_tacscan_roms(void)
{
    mz80_set_idle_pc(TACSCAN_IDLE_PC);
    GI[CPU0] = gi0_mem;
    GI[1]    = gi1_mem;
    /* The ROM no longer travels inside: it is read from roms/<game>.zip on the
     * card and decompressed DIRECTLY onto GI[], with no intermediate array. */
    aae_rom_load(ROM_OPS, ROM_OPS_N);
}
