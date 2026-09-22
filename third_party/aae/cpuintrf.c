/***************************************************************************

  cpuintrf.c

  Don't you love MS-DOS 8+3 names? That stands for CPU interface.
  Functions needed to interface the CPU emulator with the other parts of
  the emulation.

***************************************************************************/

#include "driver.h"
#include "globals.h"
#include "aae_perf.h"
#include "m6809/m6809_1.h"
#include "m6809/m6809_2.h"

#ifndef NO_PI
#include <vectrex/vectrexInterface.h>
#endif


int activecpu,totalcpu;
//static int activecpu,totalcpu;
static int iloops,iperiod_1,iperiod_2;
static int cpurunning[MAX_CPU];
static int totalcycles[MAX_CPU];
static int have_to_reset;


static int lookup_shift; /* LBO 090597 */
static int lookup_entries; /* LBO 090597 */

static const struct MemoryReadAddress *memoryread;
static const struct MemoryWriteAddress *memorywrite;

/* Lookup constants for CPUs using a 16-bit address space */
#define MH_SHIFT_16		8
#define MH_ENTRIES_16	(1<<(16-MH_SHIFT_16))

/*
static int (*memoryreadhandler[MH_ENTRIES_16])(int address);
static int memoryreadoffset[MH_ENTRIES_16];
static void (*memorywritehandler[MH_ENTRIES_16])(int address,int data);
static int memorywriteoffset[MH_ENTRIES_16];
*/

/* Compact per-address memory dispatch. Every address maps to one of a small,
 * fixed set of handlers, so we store a 1-byte INDEX per address into a tiny
 * handler table instead of a full 4/8-byte function pointer. This cuts each
 * per-CPU 64K dispatch table from 256KB (pointers) to 64KB (bytes) — required
 * to fit the rp2350 cartridge RAM (only the 6809 path uses these; the 6502/Z80
 * cores carry their own memory). rd_/wr_handler_tbl are global so the m6809
 * core TUs can index them from the RDMEM/WRMEM macros. */
enum { RDH_ERROR=0, RDH_NOP, RDH_RAM, RDH_READMEM, RDH_COUNT };
enum { WRH_ERROR=0, WRH_NOP, WRH_RAM, WRH_RAMROM, WRH_WRITEMEM, WRH_COUNT };
/* aae_memdispatch.h inlines the plain-memory case and has to name these two slots as
 * constants. If anyone reorders the enums, fail the build here rather than silently
 * send every RAM read to the wrong handler. */
_Static_assert(RDH_RAM == AAE_RDH_RAM, "AAE_RDH_RAM out of step with RDH_RAM");
_Static_assert(WRH_RAM == AAE_WRH_RAM, "AAE_WRH_RAM out of step with WRH_RAM");
/* One byte per address and per CPU: a slot index into rd_/wr_handler_tbl, see
 * aae_memdispatch.h. Slots above the fixed ones are allocated per distinct
 * (handler, start) pair found in the memory maps, shared by all CPUs. */
#include "aae_memdispatch.h"
/* THE DISPATCH TABLES IN THE CART'S OWN SRAM (2026-09-16). On that cart the game runs
 * from PSRAM (QSPI behind a 16 KB XIP cache) and every 6809/6502 access goes through
 * here; with the tables also in PSRAM the dispatch was 18% of core 1's instructions.
 * The 220 KB window (0x20040000..0x20077000, rp2350_game_ram.ld) does not hold both
 * CPUs' read tables plus the CPU cores' code, so: CPU 0's read table in SRAM (every
 * opcode fetch of the main CPU goes through it), CPU 1's and the write tables in
 * ordinary RAM. In the .um2 the game already lives in SRAM and none of this applies. */
#include "aae_fast.h"
/* ONE ENTRY PER 256-BYTE PAGE, NOT PER ADDRESS.
 *
 * These were 65536 bytes each -- a byte for every address of every CPU -- which is 128 KB
 * of RAM for two 6809s and, on a board where memory is the binding constraint, the single
 * largest thing in the image after the ROMs themselves. The WRITE side has always been
 * per page (`wrhandler` below) with a linear scan for pages a range only partly covers;
 * this is the same thing, and there was no reason for the two sides to differ.
 *
 * WHAT IT COSTS, counted rather than assumed. Star Wars' read map has 19 ranges for the
 * main CPU of which 10 are not page-aligned, and they fall in THREE pages (0x43xx, 0x44xx,
 * 0x47xx): input ports, DIP switches, the ADC, the mathbox and the PRNG. The sound CPU has
 * two such ranges in ONE page. Everything on the hot path -- instruction fetch, ROM, RAM --
 * covers whole pages and pays nothing. What does pay is an I/O read, and the one to watch
 * is the sound CPU's 6532 scratchpad at 0x1000-0x107f, which it uses constantly; writes
 * there already took the scan.
 *
 * 128 KB -> 512 bytes. */
static unsigned char rdhandler0[256] AAE_SRAM_FAST;
static unsigned char rdhandler1[256];
static unsigned char *const rdhandler[2] = { rdhandler0, rdhandler1 };
static unsigned char wrhandler[2][256];       /* per 256-byte page, see aae_memdispatch.h */
static int memoryreadoffset[4][MH_ENTRIES_16];
static int memorywriteoffset[4][MH_ENTRIES_16];
int  (*rd_handler_tbl[AAE_HANDLER_SLOTS])(int) AAE_SRAM_FAST;
int  rd_handler_start[AAE_HANDLER_SLOTS] AAE_SRAM_FAST;
#ifdef AAE_RD_BASE
/* The slot's base when its handler is a banked flat array; see the long note in
 * aae_memdispatch.h. It lives in .sram_fast, which is NOLOAD: the init below zeroes it, NOT
 * the startup code. Without that zeroing, a garbage pointer would be read as a valid base
 * and the game would execute arbitrary memory. */
const unsigned char *rd_base_tbl[AAE_HANDLER_SLOTS] AAE_SRAM_FAST;
#endif
void (*wr_handler_tbl[AAE_HANDLER_SLOTS])(int,int) AAE_SRAM_FAST;
int  wr_handler_start[AAE_HANDLER_SLOTS] AAE_SRAM_FAST;
static int rd_slots_used = RDH_COUNT, wr_slots_used = WRH_COUNT;

/* Slot for a (handler, start) pair: reuse an existing one, allocate a new one, or
 * fall back to the linear-scan slot when the table is full (256 is far beyond any map). */
static int rd_slot_for(int (*h)(int), int start)
{
    int i;
    for (i = RDH_COUNT; i < rd_slots_used; i++)
        if (rd_handler_tbl[i] == h && rd_handler_start[i] == start) return i;
    if (rd_slots_used >= AAE_HANDLER_SLOTS) return RDH_READMEM;
    rd_handler_tbl[rd_slots_used] = h; rd_handler_start[rd_slots_used] = start;
    return rd_slots_used++;
}
/* For the game: records the base of the slot already serving (handler, start). It is called
 * AFTER cpu_init, from the same place that flips the bank latch. If the pair was not in the
 * map, rd_slot_for would register it and the base would end up in a slot no page points at
 * — harmless, but a sign that the game named the wrong pair. */
#ifdef AAE_RD_BASE
void aae_rd_base(int (*h)(int), int start, const unsigned char *base)
{
    int s = rd_slot_for(h, start);
    if (s >= RDH_COUNT) rd_base_tbl[s] = base;
}
#endif

static int wr_slot_for(void (*h)(int,int), int start)
{
    int i;
    for (i = WRH_COUNT; i < wr_slots_used; i++)
        if (wr_handler_tbl[i] == h && wr_handler_start[i] == start) return i;
    if (wr_slots_used >= AAE_HANDLER_SLOTS) return WRH_WRITEMEM;
    wr_handler_tbl[wr_slots_used] = h; wr_handler_start[wr_slots_used] = start;
    return wr_slots_used++;
}

/* TODO: this should be static, but currently the Qix driver needs it */
static unsigned char cpucontext[MAX_CPU][100];	/* enough to accomodate the cpu status */
static unsigned char *ramptr[MAX_CPU],*romptr[MAX_CPU];


int yield_cpu;
int saved_icount;

struct m6809context
{
	m6809_Regs	regs;
	int	icount;
	int iperiod;
	int	irq;
};



/***************************************************************************

  Memory handling

***************************************************************************/
AAE_SRAM_TEXT int mrh_error(int address)
{
//	if (errorlog) fprintf(errorlog,"CPU #%d PC %04x: warning - read unmapped memory address %04x\n",activecpu,cpu_getpc(),address);
//	log_it ("CPU #%d PC %04x: warning - read unmapped memory address %04x\n",activecpu,cpu_getpc(),address);
	return RAM[address];
}
AAE_SRAM_TEXT int mrh_ram(int address)
{
//		log_it("RAM %i: %i,",address,RAM[address]);
	return RAM[address];
}
AAE_SRAM_TEXT int mrh_nop(int address)
{
	return 0;
}
struct MemoryReadAddress *mra;
AAE_SRAM_TEXT int mrh_readmem(int address)
{
    mra = (struct MemoryReadAddress *)memoryread;
    while (mra->start != -1)
    {
        if (address >= mra->start && address <= mra->end)
        {
            if (mra->handler == MRA_RAM || mra->handler == MRA_ROM) return RAM[address];
            if (mra->handler == MRA_NOP) return 0;
            return mra->handler(address - mra->start);
        }
        mra++;
    }

    log_it("CPU #%d PC %04x: warning - read unmapped memory address %04x\n",activecpu,cpu_getpc(),address);
    return RAM[address];
}


AAE_SRAM_TEXT void mwh_error(int address,int data)
{
	if (errorlog) fprintf(errorlog,"CPU #%d PC %04x: warning - write %02x to unmapped memory address %04x\n",activecpu,cpu_getpc(),data,address);
	log_it ("CPU #%d PC %04x: warning - write %02x to unmapped memory address %04x\n",activecpu,cpu_getpc(),data,address);
	RAM[address] = data;
}
AAE_SRAM_TEXT void mwh_ramrom(int address,int data)
{
	RAM[address] = ROM[address] = data;
}
AAE_SRAM_TEXT void mwh_rom(int address,int data)
{
	ROM[address] = data;
}
AAE_SRAM_TEXT void mwh_ram(int address,int data)
{
	ROM[address] = data;
}
AAE_SRAM_TEXT void mwh_nop(int address,int data)
{
}

struct MemoryWriteAddress *mwa;
AAE_SRAM_TEXT void mwh_writemem(int address,int data)
{
    mwa = (struct MemoryWriteAddress *)memorywrite;
    while (mwa->start != -1)
    {
        if (address >= mwa->start && address <= mwa->end)
        {
            if (mwa->handler == MWA_RAM) { RAM[address] = data; return; }
            if (mwa->handler == MWA_ROM || mwa->handler == MWA_NOP) return;
            if (mwa->handler == MWA_RAMROM) { RAM[address] = data; ROM[address] = data; return; }
            mwa->handler(address - mwa->start,data);
            return;
        }
        mwa++;
    }
    log_it("CPU #%d PC %04x: warning - write %02x to unmapped memory address %04x\n",activecpu,cpu_getpc(),data,address);
}


static void _2initmemoryhandlers(int acpu)
{
    int i,s,e;
    const struct MemoryReadAddress *mra;
    const struct MemoryWriteAddress *mwa;


    memoryread = Machine->drv->cpu[acpu].memory_read;
    memorywrite = Machine->drv->cpu[acpu].memory_write;

    /* populate the small handler tables the 1-byte indices point into (idempotent) */
    rd_handler_tbl[RDH_ERROR]=mrh_error; rd_handler_tbl[RDH_NOP]=mrh_nop;
    rd_handler_tbl[RDH_RAM]=mrh_ram;     rd_handler_tbl[RDH_READMEM]=mrh_readmem;
    wr_handler_tbl[WRH_ERROR]=mwh_error; wr_handler_tbl[WRH_NOP]=mwh_nop;
    wr_handler_tbl[WRH_RAM]=mwh_ram;     wr_handler_tbl[WRH_RAMROM]=mwh_ramrom;
    wr_handler_tbl[WRH_WRITEMEM]=mwh_writemem;

    /* .sram_fast is NOLOAD: at reset it is garbage, and garbage read as a valid base makes
     * the emulated CPU execute arbitrary memory. This runs once PER CPU inside cpu_init,
     * i.e. it wipes ALL the bases on every pass — which is why `aae_rd_base` is called
     * AFTER cpu_init and not before. */
#ifdef AAE_RD_BASE
    for (i = 0;i < AAE_HANDLER_SLOTS;i++) rd_base_tbl[i] = 0;
#endif
    for (i = 0;i < RDH_COUNT;i++) rd_handler_start[i] = 0;
    for (i = 0;i < WRH_COUNT;i++) wr_handler_start[i] = 0;
    for (i = 0;i < 256;i++) rdhandler[acpu][i] = RDH_ERROR;
    for (i = 0;i < 256;i++) wrhandler[acpu][i] = WRH_ERROR;

    mra = memoryread;
    while (mra->start != -1) mra++;
    mra--;

    /* go backwards because entries up in the memory array have greater priority than */
    /* the following ones. If an entry is duplicated, going backwards we overwrite */
    /* the handler set by the lower priority one. */
    while (mra >= memoryread)
    {
        int (*handler)() = mra->handler;
        unsigned char rd;
        if (handler == MRA_NOP)
            rd = RDH_NOP;
        else if (handler == MRA_RAM || handler == MRA_ROM)
            rd = RDH_RAM;                      /* just read the array */
        else
            rd = (unsigned char)rd_slot_for(handler, mra->start);

        s = mra->start >> 8;
        e = mra->end >> 8;
        for (i = s;i <= e;i++)
        {
            /* A page this entry only partly covers is MIXED, and no single handler can
             * answer for it: mark it for the linear scan, which resolves the address
             * against the map the way the byte-granular table used to. Same rule, and
             * the same words, as the write side below. */
            int entera = (i > s || (mra->start & 0xff) == 0)
                      && (i < e || (mra->end & 0xff) == 0xff);
            if (!entera)
                rdhandler[acpu][i] = RDH_READMEM;
            else
                rdhandler[acpu][i] = rd;
        }
        mra--;
    }


    mwa = memorywrite;
    while (mwa->start != -1) mwa++;
    mwa--;

    /* go backwards because entries up in the memory array have greater priority than */
    /* the following ones. If an entry is duplicated, going backwards we overwrite */
    /* the handler set by the lower priority one. */
    while (mwa >= memorywrite)
    {
        void (*handler)() = mwa->handler;
        unsigned char wr = 0xff;
        int p;
        if (handler == MWA_NOP)
            wr = WRH_NOP;
        else if (handler == MWA_RAM)
            wr = WRH_RAM;
        else if (handler == MWA_RAMROM)
            wr = WRH_RAMROM;
        else if (handler != MWA_ROM)
            wr = (unsigned char)wr_slot_for(handler, mwa->start);
        s = mwa->start >> 8;
        e = mwa->end >> 8;
        for (p = s; p <= e; p++)
        {
            /* a page this entry only partly covers is mixed: the linear scan sorts it out */
            int entera = (p > s || (mwa->start & 0xff) == 0) && (p < e || (mwa->end & 0xff) == 0xff);
            if (!entera)
                wrhandler[acpu][p] = WRH_WRITEMEM;
            else if (wr != 0xff)
                wrhandler[acpu][p] = wr;
        }
        mwa--;
    }
//log_it("Mem Handler init completed");
}


#if 0 /* dead: shift-based dispatch, never called. Incompatible with the compact
       * 1-byte index tables (it stored raw function pointers). Kept for reference. */
static void _initmemoryhandlers(int acpu)
{
    int i,s,e,a,b;
    const struct MemoryReadAddress *mra;
    const struct MemoryWriteAddress *mwa;


    memoryread = Machine->drv->cpu[acpu].memory_read;
    memorywrite = Machine->drv->cpu[acpu].memory_write;


    lookup_entries = MH_ENTRIES_16;
    lookup_shift = MH_SHIFT_16;
    

    for (i = 0;i < lookup_entries;i++)
    {
        memoryreadhandler[acpu][i] = mrh_error;
        memoryreadoffset[acpu][i] = 0;

        memorywritehandler[acpu][i] = mwh_error;
        memorywriteoffset[acpu][i] = 0;
    }

    mra = memoryread;
    while (mra->start != -1) mra++;
    mra--;

    /* go backwards because entries up in the memory array have greater priority than */
    /* the following ones. If an entry is duplicated, going backwards we overwrite */
    /* the handler set by the lower priority one. */
    while (mra >= memoryread)
    {
        s = mra->start >> lookup_shift;
        a = mra->start ? ((mra->start-1) >> lookup_shift) + 1 : 0;
        b = ((mra->end+1) >> lookup_shift) - 1;
        e = mra->end >> lookup_shift;

        /* first of all make all the entries point to the general purpose handler... */
        for (i = s;i <= e;i++)
        {
            memoryreadhandler[acpu][i] = mrh_readmem;
            memoryreadoffset[acpu][i] = 0;
        }
        /* ... and now make the ones containing only one handler point directly to the handler */
        for (i = a;i <= b;i++)
        {
            int (*handler)() = mra->handler;


            if (handler == MRA_NOP)
            {
                memoryreadhandler[acpu][i] = mrh_nop;
                memoryreadoffset[acpu][i] = 0;
            }
            else if (handler == MRA_RAM || handler == MRA_ROM)
            {
                memoryreadhandler[acpu][i] = 0;   /* special case handled by cpu_readmem() */
                memoryreadoffset[acpu][i] = 0;
            }
            else
            {
                memoryreadhandler[acpu][i] = mra->handler;
                memoryreadoffset[acpu][i] = mra->start;
            }
        }

        mra--;
    }


    mwa = memorywrite;
    while (mwa->start != -1) mwa++;
    mwa--;

    /* go backwards because entries up in the memory array have greater priority than */
    /* the following ones. If an entry is duplicated, going backwards we overwrite */
    /* the handler set by the lower priority one. */
    while (mwa >= memorywrite)
    {
        s = mwa->start >> lookup_shift;
        a = mwa->start ? ((mwa->start-1) >> lookup_shift) + 1 : 0;
        b = ((mwa->end+1) >> lookup_shift) - 1;
        e = mwa->end >> lookup_shift;

        /* first of all make all the entries point to the general purpose handler... */
        for (i = s;i <= e;i++)
        {
            memorywritehandler[acpu][i] = mwh_writemem;
            memorywriteoffset[acpu][i] = 0;
        }
        /* ... and now make the ones containing only one handler point directly to the handler */
        for (i = a;i <= b;i++)
        {
            void (*handler)() = mwa->handler;


            if (handler == MWA_NOP)
            {
                memorywritehandler[acpu][i] = mwh_nop;
                memorywriteoffset[acpu][i] = 0;
            }
            else if (handler == MWA_RAM)
            {
                memorywritehandler[acpu][i] = 0;  /* special case handled by cpu_writemem() */
                memorywriteoffset[acpu][i] = 0;
            }
            else if (handler == MWA_RAMROM)
            {
                memorywritehandler[acpu][i] = mwh_ramrom;
                memorywriteoffset[acpu][i] = 0;
            }
            else if (handler != MWA_ROM)
            {
                memorywritehandler[acpu][i] = mwa->handler;
                memorywriteoffset[acpu][i] = mwa->start;
            }
        }

        mwa--;
    }
//log_it("Mem Handler init completed");
}
#endif /* dead _initmemoryhandlers */



void cpu_init(void)
{
	/* count how many CPUs we have to emulate */
	totalcpu = 0;

//	have_24bit_address_space = 0; /* LBO 090597 */

	while (totalcpu < MAX_CPU)
	{
		const struct MemoryReadAddress *mra;
		const struct MemoryWriteAddress *mwa;


		if (Machine->drv->cpu[totalcpu].cpu_type == 0) break;

		
		ramptr[totalcpu] = GI[totalcpu];//Machine->memory_region[Machine->drv->cpu[totalcpu].memory_region];

		/* opcode decryption is currently supported only for the first memory region */
		if (totalcpu == 0) romptr[totalcpu] = ROM;
		else romptr[totalcpu] = ramptr[totalcpu];

		/* initialize the memory base pointers for memory hooks */
		mra = Machine->drv->cpu[totalcpu].memory_read;
		while (mra->start != -1)
		{
			if (mra->base) *mra->base = &ramptr[totalcpu][mra->start];
			if (mra->size) *mra->size = mra->end - mra->start + 1;
			mra++;
		}
		mwa = Machine->drv->cpu[totalcpu].memory_write;
		while (mwa->start != -1)
		{
			if (mwa->base) *mwa->base = &ramptr[totalcpu][mwa->start];
			if (mwa->size) *mwa->size = mwa->end - mwa->start + 1;
			mwa++;
		}


      totalcpu++;
	}
	
	
log_it("CPU Init complete.");
}



int cpu_interrupt(void)
{
	return (*Machine->drv->cpu[activecpu].interrupt)();
}
AAE_SRAM_TEXT int cpu_interrupt_1(void)
{
	return (*Machine->drv->cpu[0].interrupt)();
}
AAE_SRAM_TEXT int cpu_interrupt_2(void)
{
	return (*Machine->drv->cpu[1].interrupt)();
}

void cpu_start(void)
{

	have_to_reset = 0;

	for (activecpu = 0;activecpu < totalcpu;activecpu++)
	{
		/* if sound is disabled, don't emulate the audio CPU */
		//if (play_sound == 0 && (Machine->drv->cpu[activecpu].cpu_type & CPU_AUDIO_CPU))
		//	cpurunning[activecpu] = 0;
		//else
			cpurunning[activecpu] = 1;

		   totalcycles[activecpu] = 0;
	}

	/* do this AFTER the above so init_machine() can use cpu_halt() to hold the */
	/* execution of some CPUs */
	if (Machine->drv->init_machine) (*Machine->drv->init_machine)();

	for (activecpu = 0;activecpu < totalcpu;activecpu++)
	{
		int cycles;


		cycles = Machine->drv->cpu[activecpu].cpu_clock /
				(Machine->drv->frames_per_second * Machine->drv->cpu[activecpu].interrupts_per_frame);

		RAM = ramptr[activecpu];
		ROM = romptr[activecpu];
	//	initmemoryhandlers();
_2initmemoryhandlers(activecpu);

if (activecpu==0)
{
	m6809_1_readHandler = rdhandler[0];
	m6809_1_writeHandler= wrhandler[0];
}
else
{
	m6809_2_readHandler = rdhandler[1];
	m6809_2_writeHandler= wrhandler[1];
}


      switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
		{
					
			 case CPU_M6809:
				{
					struct m6809context *ctxt;

					ctxt = (struct m6809context *)cpucontext[activecpu];
				//	m6809_Flags |= M6809_FAST_OP |M6809_FAST_U|M6809_FAST_S;
				//	m6809_Flags |= M6809_FAST_U|M6809_FAST_S;

					if (activecpu==0)
					{
						m6809_1_IPeriod = cycles;
						m6809_1_reset();
						m6809_1_GetRegs(&ctxt->regs);
			      m6809_1_ICount = cycles;
			      iperiod_1 = m6809_1_IPeriod = cycles;
			      m6809_1_IRequest = INT_NONE;

					}
					if (activecpu==1)
					{
						m6809_2_IPeriod = cycles;
						m6809_2_reset();
						m6809_2_GetRegs(&ctxt->regs);
			      m6809_2_ICount = cycles;
			      iperiod_2 = m6809_2_IPeriod = cycles;
			      m6809_2_IRequest = INT_NONE;
					}
//					m6809_reset();
//					m6809_GetRegs(&ctxt->regs);
					ctxt->icount = cycles;
					ctxt->iperiod = cycles;
					ctxt->irq = INT_NONE;
					printf("Cpu Reset\n");
				}
				break;
			
		}
	}

}

extern uint32_t cpu1Start;
extern uint32_t cpu1End;
extern uint32_t cpu2Start;
extern uint32_t cpu2End;

void cpu_run(void)
{
  unsigned int t_run_ = AAE_T();
  for (activecpu = 0;activecpu < totalcpu;activecpu++)
  {
    //if (have_to_reset) cpu_start();	/* machine_reset() was called, have to reset */

////printf("CPU %i\n\r", activecpu);    
    if (cpurunning[activecpu])
    {
      RAM = ramptr[activecpu];
      ROM = romptr[activecpu];
//      initmemoryhandlers();
//_initmemoryhandlers(activecpu);
    memoryread = Machine->drv->cpu[activecpu].memory_read;
    memorywrite = Machine->drv->cpu[activecpu].memory_write;


      switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
      {
				      
	      case CPU_M6809:
	      {
			  if (activecpu == 0)
			  {
  CCNT0(cpu1Start);
  { unsigned int t0_ = AAE_T();
				  for (iloops = Machine->drv->cpu[activecpu].interrupts_per_frame - 1; iloops >= 0;iloops--)
				  {
	//printf("iLoop:\n\r");    
					  m6809_1_execute();
					  totalcycles[activecpu] += iperiod_1;
				  }
  AAE_ACUM(8, t0_); }
  CCNT0(cpu1End);
			  }
			  else if (activecpu == 1)
			  {
  CCNT0(cpu2Start);
  { unsigned int t0_ = AAE_T();
				  for (iloops = Machine->drv->cpu[activecpu].interrupts_per_frame - 1; iloops >= 0;iloops--)
				  {
	//printf("iLoop:\n\r");    
					  m6809_2_execute();
					  totalcycles[activecpu] += iperiod_2;
				  }
  AAE_ACUM(9, t0_); }
  CCNT0(cpu2End);
			  }

			  
/*			  
			  struct m6809context *ctxt;


		      ctxt = (struct m6809context *)cpucontext[activecpu];
		      m6809_SetRegs(&ctxt->regs);
		      m6809_ICount = ctxt->icount;
		      iperiod = m6809_IPeriod = ctxt->iperiod;
		      m6809_IRequest = ctxt->irq;
		      for (iloops = Machine->drv->cpu[activecpu].interrupts_per_frame - 1; iloops >= 0;iloops--)
		      {
			      m6809_execute();
			      totalcycles[activecpu] += iperiod;
		      }

		      m6809_GetRegs(&ctxt->regs);
		      ctxt->icount = m6809_ICount;
		      ctxt->iperiod = m6809_IPeriod;
		      ctxt->irq = m6809_IRequest;
*/
	      }
	      break;
	      
      }
      /* keep track of changes to RAM and ROM pointers (bank switching) */
      ramptr[activecpu] = RAM;
      romptr[activecpu] = ROM;
    }
  }

  if (yield_cpu)
  {
	  yield_cpu = FALSE;
  }
  else
  {
	  ;//usres = updatescreen();
  }
	
  AAE_ACUM(11, t_run_);
}



/***************************************************************************

  This function resets the machine (the reset will not take place
  immediately, it will be performed at the end of the active CPU's time
  slice)

***************************************************************************/
void machine_reset(void)
{
	

	have_to_reset = 1;
}



/***************************************************************************

  Use this function to stop and restart CPUs

***************************************************************************/
void cpu_halt(int cpunum,int running)
{
	if (cpunum >= MAX_CPU) return;

	cpurunning[cpunum] = running;
}



/***************************************************************************

  This function returns CPUNUM current status  (running or halted)

***************************************************************************/
int cpu_getstatus(int cpunum)
{
	if (cpunum >= MAX_CPU) return 0;

	return cpurunning[cpunum];
}



int cpu_getpc(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
	
		case CPU_M6809:
			if (activecpu == 0)
				return m6809_1_GetPC();
//			if (activecpu == 1)
				return m6809_2_GetPC();
			break;
	
		default:
	if (errorlog) fprintf(errorlog,"cpu_getpc: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}


/***************************************************************************

  This is similar to cpu_getpc(), but instead of returning the current PC,
  it returns the address of the opcode that is doing the read/write. The PC
  has already been incremented by some unknown amount by the time the actual
  read or write is being executed. This helps to figure out what opcode is
  actually doing the reading or writing, and therefore the amount of cycles
  it's taking. The Missile Command driver needs to know this.

***************************************************************************/
int cpu_getpreviouspc(void)  /* -RAY- */
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
		//case CPU_M6502:
			//return ((M6502 *)cpucontext[activecpu])->previousPC.W;
			//break;

		default:
	if (errorlog) fprintf(errorlog,"cpu_getpreviouspc: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}


/***************************************************************************

  This is similar to cpu_getpc(), but instead of returning the current PC,
  it returns the address stored on the top of the stack, which usually is
  the address where execution will resume after the current subroutine.
  Note that the returned value will be wrong if the program has PUSHed
  registers on the stack.

***************************************************************************/
int cpu_getreturnpc(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
		
		default:
	if (errorlog) fprintf(errorlog,"cpu_getreturnpc: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}



/***************************************************************************

  Returns the number of CPU cycles since the last reset of the CPU

  IMPORTANT: this value wraps around in a relatively short time.
  For example, for a 6Mhz CPU, it will wrap around in
  2^32/6000000 = 716 seconds = 12 minutes.
  Make sure you don't do comparisons between values returned by this
  function, but only use the difference (which will be correct regardless
  of wraparound).

***************************************************************************/
int cpu_gettotalcycles(void)
{
  if (activecpu == 0)
	return totalcycles[activecpu] + iperiod_1 - cpu_geticount();
	return totalcycles[activecpu] + iperiod_2 - cpu_geticount();
}



/***************************************************************************

  Returns the number of CPU cycles before the next interrupt handler call

***************************************************************************/
int cpu_geticount(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
		case CPU_M6809:
			if (activecpu == 0)
				return m6809_1_ICount;
//			if (activecpu == 1)
				return m6809_2_ICount;
			break;
	
		default:
	if (errorlog) fprintf(errorlog,"cpu_geticount: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return 0;
			break;
	}
}



/***************************************************************************

  Returns the number of CPU cycles before the end of the current video frame

***************************************************************************/
int cpu_getfcount(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
		
		case CPU_M6809:
			if (activecpu == 0)
				return m6809_1_ICount + iloops * iperiod_1;
//			if (activecpu == 1)
				return m6809_2_ICount + iloops * iperiod_2;
			break;
		
		default:
	if (errorlog) fprintf(errorlog,"cpu_geticycles: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}



/***************************************************************************

  Returns the number of CPU cycles in one video frame

***************************************************************************/
int cpu_getfperiod(void)
{
		return Machine->drv->cpu[activecpu].cpu_clock / Machine->drv->frames_per_second;
}



void cpu_seticount(int cycles)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
				
		case CPU_M6809:
			if (activecpu == 0)
				m6809_1_ICount = cycles;
			if (activecpu == 0)
				m6809_2_ICount = cycles;
			break;
		
		default:
	if (errorlog) fprintf(errorlog,"cpu_seticycles: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			break;
	}
}



/***************************************************************************

  Returns the number of times the interrupt handler will be called before
  the end of the current video frame. This is can be useful to interrupt
  handlers to synchronize their operation. If you call this from outside
  an interrupt handler, add 1 to the result, i.e. if it returns 0, it means
  that the interrupt handler will be called once.

***************************************************************************/
int cpu_getiloops(void)
{
	return iloops;
}



/***************************************************************************

  Interrupt handling

***************************************************************************/

/***************************************************************************

  Use this function to cause an interrupt immediately (don't have to wait
  until the next call to the interrupt handler)

***************************************************************************/
void cpu_cause_interrupt(int cpu,int type)
{


	switch(Machine->drv->cpu[cpu].cpu_type & ~CPU_FLAGS_MASK)
	{
		/*
		case CPU_M6809:
			if (cpu == activecpu)
				m6809_Cause_Interrupt(type);
			else
			{
				m6809_Regs regs;


				m6809_GetRegs(&regs);
				m6809_SetRegs((m6809_Regs *)cpucontext[cpu]);
				m6809_Cause_Interrupt(type);
				m6809_GetRegs((m6809_Regs *)cpucontext[cpu]);
				m6809_SetRegs(&regs);
			}
			break;
		*/
		default:
if (errorlog) fprintf(errorlog,"cpu_cause_interrupt: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			break;
	}
}



void cpu_clear_pending_interrupts(int cpu)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
	
		default:
if (errorlog) fprintf(errorlog,"clear_pending_interrupts: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			break;
	}
}



/* start with interrupts enabled, so the generic routine will work even if */
/* the machine doesn't have an interrupt enable port */
static int interrupt_enable = 1;
static int interrupt_vector = 0xff;

void interrupt_enable_w(int offset,int data)
{
	interrupt_enable = data;

	/* make sure there are no queued interrupts */
	if (data == 0) cpu_clear_pending_interrupts(activecpu);
}



void interrupt_vector_w(int offset,int data)
{
	if (interrupt_vector != data)
	{
		interrupt_vector = data;

		/* make sure there are no queued interrupts */
		cpu_clear_pending_interrupts(activecpu);
	}
}



int interrupt(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
			
		case CPU_M6809:
			if (interrupt_enable == 0) return INT_NONE;
			else return INT_IRQ;
			break;
		
		default:
if (errorlog) fprintf(errorlog,"interrupt: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}



int nmi_interrupt(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
			
		default:
if (errorlog) fprintf(errorlog,"nmi_interrupt: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}



int ignore_interrupt(void)
{
	switch(Machine->drv->cpu[activecpu].cpu_type & ~CPU_FLAGS_MASK)
	{
		
		case CPU_M6809:
			return INT_NONE;
			break;
		
		default:
if (errorlog) fprintf(errorlog,"interrupt: unsupported CPU type %02x\n",Machine->drv->cpu[activecpu].cpu_type);
			return -1;
			break;
	}
}



/***************************************************************************

  Perform a memory read. This function is called by the CPU emulation.

***************************************************************************/
AAE_SRAM_TEXT int cpu_readmem(int address)
{
//	int (*handler)() = memoryreadhandler[activecpu][address];
//	if (handler == 0) return RAM[address];	/* special case */
//	else return handler(address);
		return (int)aae_rd_byte(rdhandler[activecpu], (unsigned)address);
}
AAE_SRAM_TEXT int cpu_readmem_1(int address)
{
//	int (*handler)() = memoryreadhandler[activecpu][address];
//	if (handler == 0) return RAM[address];	/* special case */
//	else return handler(address);
		return (int)aae_rd_byte(rdhandler[activecpu], (unsigned)address);
}
AAE_SRAM_TEXT int cpu_readmem_2(int address)
{
//	int (*handler)() = memoryreadhandler[activecpu][address];
//	if (handler == 0) return RAM[address];	/* special case */
//	else return handler(address);
		return (int)aae_rd_byte(rdhandler[activecpu], (unsigned)address);
}



/***************************************************************************

  Perform a memory write. This function is called by the CPU emulation.

***************************************************************************/
AAE_SRAM_TEXT void cpu_writemem(int address,int data)
{
//	void (*handler)() = memorywritehandler[activecpu][address];
//	if (handler == 0) RAM[address] = data;	/* special case */
//	else handler(address,data);
		
		aae_wr_byte(wrhandler[activecpu], (unsigned)address, data);
}
AAE_SRAM_TEXT void cpu_writemem_1(int address,int data)
{
//	void (*handler)() = memorywritehandler[activecpu][address];
//	if (handler == 0) RAM[address] = data;	/* special case */
//	else handler(address,data);
		
		aae_wr_byte(wrhandler[activecpu], (unsigned)address, data);
}
AAE_SRAM_TEXT void cpu_writemem_2(int address,int data)
{
//	void (*handler)() = memorywritehandler[activecpu][address];
//	if (handler == 0) RAM[address] = data;	/* special case */
//	else handler(address,data);
		
		aae_wr_byte(wrhandler[activecpu], (unsigned)address, data);
}



/***************************************************************************

  Perform an I/O port read. This function is called by the CPU emulation.

***************************************************************************/
int cpu_readport(int Port)
{
	const struct IOReadPort *iorp;


	iorp = Machine->drv->cpu[activecpu].port_read;
	if (iorp)
	{
		while (iorp->start != -1)
		{
			if (Port >= iorp->start && Port <= iorp->end)
			{
				int (*handler)() = iorp->handler;


				if (handler == IORP_NOP) return 0;
				else return (*handler)(Port - iorp->start);
			}

			iorp++;
		}
	}

	if (errorlog) fprintf(errorlog,"CPU #%d PC %04x: warning - read unmapped I/O port %02x\n",activecpu,cpu_getpc(),Port);
	return 0;
}



/***************************************************************************

  Perform an I/O port write. This function is called by the CPU emulation.

***************************************************************************/
void cpu_writeport(int Port,int Value)
{
	const struct IOWritePort *iowp;


	iowp = Machine->drv->cpu[activecpu].port_write;
	if (iowp)
	{
		while (iowp->start != -1)
		{
			if (Port >= iowp->start && Port <= iowp->end)
			{
				void (*handler)() = iowp->handler;


				if (handler == IOWP_NOP) return;
				else (*handler)(Port - iowp->start,Value);

				return;
			}

			iowp++;
		}
	}

	if (errorlog) fprintf(errorlog,"CPU #%d PC %04x: warning - write %02x to unmapped I/O port %02x\n",activecpu,cpu_getpc(),Value,Port);
}



/***************************************************************************

  Interrupt handler. This function is called at regular intervals
  (determined by IPeriod) by the CPU emulation.

***************************************************************************/



