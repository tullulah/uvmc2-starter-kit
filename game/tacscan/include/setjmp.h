#ifndef _AAE_SETJMP_H
#define _AAE_SETJMP_H
/* Minimal setjmp shim. Pulled in only by musashi/m68kcpu.h (the 68000 core),
 * which Asteroids (pure 6502) never executes — these are never actually called.
 * Provided so the freestanding build links; not a working setjmp/longjmp. */
typedef long jmp_buf[16];
static inline int  setjmp(jmp_buf b)          { (void)b; return 0; }
static inline void longjmp(jmp_buf b, int v)   { (void)b; (void)v; for (;;) {} }
#endif
