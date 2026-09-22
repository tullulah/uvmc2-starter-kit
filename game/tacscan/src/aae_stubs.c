/* aae_stubs.c — no-op stubs for AAE subsystems Tac/Scan never exercises, plus
 * shim-side globals.
 *
 * Tac/Scan runs a single CPU_MZ80 (Z80) — see the driver row in aae_machine.c.
 * The REAL mz80 core (mz80/mz80.c) is compiled in, so unlike the asteroids port
 * we do NOT stub mz80*. The 68000 (musashi) core is referenced by AAE's generic
 * cpu dispatch but never entered. Sound (SegaG80snd: AY8910 + speech) is stubbed
 * here rather than pulling in the whole sound file. The linker matches by name,
 * so plain signatures are fine.
 */

/* ---- Player-2 stick (SegaG80 input macros reference it; shims are P1-only) ---- */
signed char currentJoy2X = 0;
signed char currentJoy2Y = 0;

/* ---- 68000 core / musashi (unused by Tac/Scan) ---- */
void m68k_pulse_reset(void)              { }
int  m68k_execute(int n)                 { (void)n; return 0; }
int  m68k_cycles_run(void)               { return 0; }
void m68k_end_timeslice(void)            { }
void m68k_set_irq(unsigned int level)    { (void)level; }

/* ---- 6502 core (unused by Tac/Scan — Z80 game — but cpu_control's generic
 * dispatch references every core; gamenum=TACSCAN means these never run) ---- */
unsigned m6502exec(unsigned n)             { (void)n; return 0; }
unsigned m6502GetElapsedTicks(unsigned n)  { (void)n; return 0; }
void     m6502GetContext(void *p)          { (void)p; }
void     m6502SetContext(void *p)          { (void)p; }
void     m6502reset(void)                  { }
unsigned m6502int(unsigned v)              { (void)v; return 0; }
unsigned m6502nmi(void)                    { return 0; }
unsigned m6502zpexec(unsigned n)           { (void)n; return 0; }
unsigned m6502zpGetElapsedTicks(unsigned n){ (void)n; return 0; }
void     m6502zpGetContext(void *p)        { (void)p; }
void     m6502zpSetContext(void *p)        { (void)p; }
void     m6502zpreset(void)                { }
unsigned m6502zpint(unsigned v)            { (void)v; return 0; }
unsigned m6502zpnmi(void)                  { return 0; }

/* SegaG80 sound (sega_sh_*, tacscan_sh_update, *_sh_w) is now REAL: SegaG80snd.c
 * is compiled in and drives the sample system (src/samples.c → v_playSample).
 * `save_dips` lives in dips.c (not compiled) so it stays a stub here. */
void save_dips(void)                     { }

/* ---- Sega DIP-switch shadow registers (init_segag80 sets z80dip2) ---- */
int z80dip1 = 0;
int z80dip2 = 0;

/* ---- logging — no-op on the cart ---- */
int log_it(char *fmt, ...)               { (void)fmt; return 0; }

/* ---- high-score EEROM save — no persistent storage on the cart ---- */
int save_hi_aae(int start, int size, int image) { (void)start; (void)size; (void)image; return 0; }
