/*** m6809: Portable 6809 emulator ******************************************/

#ifndef _M6809_1_H
#define _M6809_1_H

#include "m6809.h"


/* PUBLIC FUNCTIONS */
void m6809_1_SetRegs(m6809_Regs *Regs);
void m6809_1_GetRegs(m6809_Regs *Regs);
unsigned m6809_1_GetPC(void);
void m6809_1_reset(void);
void m6809_1_execute(void);

/* PUBLIC GLOBALS */
extern int	m6809_1_IPeriod;
extern int	m6809_1_ICount;
extern int	m6809_1_IRequest;


/****************************************************************************/
/* Read a byte from given memory location                                   */
/****************************************************************************/
//int cpu_readmem_1(int address);
//#define M6809_1_RDMEM(A) ((unsigned)cpu_readmem_1(A))
/* compact dispatch: readHandler is now a 1-byte-per-address index into the
 * shared rd_handler_tbl (see cpuintrf.c) instead of a full pointer table. */
extern unsigned char *m6809_1_readHandler;
#include "../aae_memdispatch.h"
#include "../aae_access_count.h"
#define M6809_1_RDMEM(A) (AAE_CNT(aae_acc_rd, 0, (A)), aae_rd_byte(m6809_1_readHandler, (A)))



/****************************************************************************/
/* Write a byte to given memory location                                    */
/****************************************************************************/
//void cpu_writemem_1(int address, int data);
//#define M6809_1_WRMEM(A,V) (cpu_writemem_1(A,V))

extern unsigned char *m6809_1_writeHandler;
#define M6809_1_WRMEM(A,V) (AAE_CNT(aae_acc_wr, 0, (A)), aae_wr_byte(m6809_1_writeHandler, (A), (V)))


/****************************************************************************/
/* Z80_RDOP() is identical to Z80_RDMEM() except it is used for reading     */
/* opcodes. In case of system with memory mapped I/O, this function can be  */
/* used to greatly speed up emulation                                       */
/****************************************************************************/
#define M6809_1_RDOP(A) (AAE_CNT(aae_acc_op, 0, (A)), ROM[A])

/****************************************************************************/
/* Z80_RDOP_ARG() is identical to Z80_RDOP() except it is used for reading  */
/* opcode arguments. This difference can be used to support systems that    */
/* use different encoding mechanisms for opcodes and opcode arguments       */
/****************************************************************************/
/*#define Z80_RDOP_ARG(A)		Z80_RDOP(A)*/
#define M6809_1_RDOP_ARG(A) (AAE_CNT(aae_acc_ar, 0, (A)), RAM[A])

/****************************************************************************/
/* Flags for optimizing memory access. Game drivers should set m6809_Flags  */
/* to a combination of these flags depending on what can be safely          */
/* optimized. For example, if M6809_FAST_OP is set, opcodes are fetched     */
/* directly from the ROM array, and cpu_readmem() is not called.            */
/* The flags affect reads and writes.                                       */
/****************************************************************************/
extern int m6809_1_Flags;

#endif /* _M6809_1_H */
