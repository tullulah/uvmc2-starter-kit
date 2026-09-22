/* Native host harness: link the SAME AAE sources + aae_machine against a fake
 * SDK to check the game logic (does the Z80 run, does it draw) off-emulator. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "aae_romload.h"

/* --- fake SDK (what sdk_rp2350.c/sdk_host.c provide) --- */
unsigned char currentButtonState = 0;
signed char   currentJoy1X = 0, currentJoy1Y = 0;
long draw_calls = 0;
void v_init(void){}
void v_WaitRecal(void){}
unsigned char v_readButtons(void){ return 0; }
void v_readJoystick1Analog(void){}
/* A FINGERPRINT OF WHAT IS ACTUALLY DRAWN. `draw_calls` alone proves nothing — a
 * frozen screen keeps its count — so every coordinate goes into an FNV hash. Two
 * runs that differ only in whether the idle-spin skip is on must produce the same
 * hash, or the skip changed the game. */
unsigned long long draw_hash = 1469598103934665603ULL;
static void hashv(int v){ draw_hash ^= (unsigned)v; draw_hash *= 1099511628211ULL; }
/* With TACSCAN_DUMP=<file> one frame's segments are written out as five ints, in the
 * order the SDK receives them, to feed uvm2-sdk/tools/uvm2_ramp_cost.c. Counting
 * VIA writes per ramp only means something against a real game's geometry. */
static FILE *dump_f = 0; static int dump_frame = -1, cur_frame = 0;
void v_directDraw32(int y0,int x0,int y1,int x1,int z){
    hashv(y0); hashv(x0); hashv(y1); hashv(x1); hashv(z); draw_calls++;
    if (dump_f && cur_frame == dump_frame) {
        int32_t r[5] = { y0, x0, y1, x1, z };
        fwrite(r, sizeof r, 1, dump_f);
    } }
/* misc SDK bits SegaG80/vectrexInterface may want */
void v_setBrightness(int b){(void)b;}
void v_zeroBeam(void){}

/* cpuintrf memory-handler error reporting. mrh_error/mwh_error are defined by
 * cpuintrf.c itself in this build, so only the globals belong here. */
int errorlog = 0, have_error = 0;

/* AAE globals that acommon.c writes and that main.c owns on the cartridge. These
 * have to be real definitions and not deferred symbols: a missing DATA symbol fails
 * at load, not at call. (globals.h) */
int done = 0;       /* end-of-emulation flag */
int gamefps = 60;   /* the rate the driver asks for */
int gain = 0;       /* pokey only; dead on Tac/Scan */

/* AAE globals normally in aaemain.c (gc-dropped on the cart). Provide storage
 * big enough for whatever struct the AAE code overlays on _Machine. */
char Machine[8192];
unsigned char *RAM = 0, *ROM = 0;

/* --- AAE entry points --- */
extern int  init_segag80(void);
extern void run_segag80(void);
extern void run_cpus_to_cycles(void);
extern void init_cpu_config(void);
extern void aae_load_tacscan_roms(void);
extern int gamenum;
extern unsigned char *GI[5];

#ifdef MZ80_PROFILE
void mz80_prof_dump(unsigned frames);
#endif

/* ── THE ROMSET, THE WAY THE CARTRIDGE HANDS IT OVER ────────────────────────
 *
 * `aae_rom_load` gets its zip through `romzip_open_cart`, which reads a three-word
 * descriptor ('RMZ1', base, length) that the launcher publishes: on the cartridge
 * the BIOS fills it after reading ROMS/<game>.zip off the card, and on the host
 * nobody ever did — which is why this harness ran a Z80 over zeroed memory and
 * reported 0 draw calls for 120 frames without complaining. So the harness plays
 * launcher.
 *
 * THE DESCRIPTOR HOLDS THE BASE IN 32 BITS (`d[1]`, read back with a cast), so the
 * zip has to live below 4 GB — and a 64-bit host puts malloc far above that. Hence
 * the mmap with a low hint: it is not a trick, it is the descriptor's format. If the
 * mapping cannot be placed low, the harness says so instead of truncating a pointer
 * and crashing somewhere else. */
#include <string.h>
/* romzip.c's host escape: the cartridge descriptor keeps the base in 32 bits and a
 * 64-bit host cannot map below 4 GB, so the zip is published in full-width pointers.
 * Only exists where there is stdio, i.e. host and sim. */
extern const unsigned char *romzip_host_base;
extern size_t romzip_host_len;

static int romset_publish(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *buf;

    if (!f) { printf("no romset at %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    romzip_host_base = buf;
    romzip_host_len  = (size_t)n;
    printf("romset: %s, %ld bytes\n", path, n);
    return 1;
}

int main(void){
    /* The romset that ships with the kit. Override with TACSCAN_ZIP=<path>. */
    const char *rom = getenv("TACSCAN_ZIP");
    if (!romset_publish(rom ? rom : "roms/tacscan.zip"))
        return 1;
    aae_load_tacscan_roms();
    /* NOIDLE=1 turns the idle-spin skip off, which is the control this harness needs:
     * the two runs have to draw the same thing. */
    if (getenv("NOIDLE")) { extern void mz80_set_idle_pc(unsigned int); mz80_set_idle_pc(0);
                            printf("idle-spin skip DISABLED\n"); }
    /* THE GAME CHECKS THIS AND THE HARNESS DID NOT, which is why it happily ran a
     * Z80 over zeroed memory and reported 0 draw calls for 120 frames without a
     * word. src/main.c refuses to emulate without a romset for exactly this reason:
     * a black screen cannot tell "the ROM is missing" from "the game hung". */
    if (aae_rom_last_error) {
        printf("ROMSET FAILED TO LOAD: error %d — nothing below this is meaningful\n",
               aae_rom_last_error);
        return 1;
    }
    init_cpu_config();
    init_segag80();
    printf("gamenum=%d GI0=%p GI1=%p\n", gamenum, (void*)GI[0], (void*)GI[1]);
    /* WHERE THE BUILDER'S TIME GOES. On hardware the frame is 41 ms of which 23 are
     * the beam, so 18 ms are this: the Z80 plus the vector generator. Splitting them
     * is what says whether skipping the Z80's idle spin is worth anything. */
    double t_cpu = 0, t_vec = 0;
    { const char *d = getenv("TACSCAN_DUMP");
      if (d) { dump_f = fopen(d, "wb");
               dump_frame = getenv("TACSCAN_DUMP_FRAME") ? atoi(getenv("TACSCAN_DUMP_FRAME")) : 100; } }
    for (int f=0; f<120; f++){
        cur_frame = f;
        long before = draw_calls;
        struct timespec a,b,c;
        clock_gettime(CLOCK_MONOTONIC,&a);
        run_cpus_to_cycles();
        clock_gettime(CLOCK_MONOTONIC,&b);
        run_segag80();
        clock_gettime(CLOCK_MONOTONIC,&c);
        t_cpu += (b.tv_sec-a.tv_sec)*1e3 + (b.tv_nsec-a.tv_nsec)/1e6;
        t_vec += (c.tv_sec-b.tv_sec)*1e3 + (c.tv_nsec-b.tv_nsec)/1e6;
        if (f<5 || f==30 || f==60 || f==119)
            printf("frame %3d: draw_calls +%ld (total %ld)\n", f, draw_calls-before, draw_calls);
    }
    printf("TOTAL draw_calls over 120 frames: %ld   draw hash %016llx\n",
           draw_calls, draw_hash);
    printf("per frame on this host:  Z80 %.3f ms   vector gen + housekeeping %.3f ms"
           "   (Z80 = %.0f%%)\n", t_cpu/120, t_vec/120, 100.0*t_cpu/(t_cpu+t_vec));
#ifdef MZ80_PROFILE
    mz80_prof_dump(120);
#endif
    return 0;
}

/* ── Dead CPU cores, stubbed ────────────────────────────────────────────────
 * cpuintrf.c is the generic dispatcher for every core AAE supports and names the
 * second 6809 and the 68000 unconditionally. Tac/Scan is a Z80 game and never runs
 * either, but the linker still wants the symbols. Stubs here rather than compiling
 * two whole cores into a harness that exists to profile the Z80. If one of these is
 * ever REACHED, the harness is running something it was not meant to. */
#include "m6809/m6809.h"
int m6809_1_IPeriod = 0, m6809_1_ICount = 0, m6809_1_IRequest = 0;
int m6809_2_IPeriod = 0, m6809_2_ICount = 0, m6809_2_IRequest = 0;
int (*m6809_1_readHandler)(int) = 0;
void (*m6809_1_writeHandler)(int,int) = 0;
void m6809_1_SetRegs(m6809_Regs *r){ (void)r; }
void m6809_1_GetRegs(m6809_Regs *r){ (void)r; }
unsigned m6809_1_GetPC(void){ return 0; }
void m6809_1_reset(void){}
void m6809_1_execute(void){}
int (*m6809_2_readHandler)(int) = 0;
void (*m6809_2_writeHandler)(int,int) = 0;
void m6809_2_SetRegs(m6809_Regs *r){ (void)r; }
void m6809_2_GetRegs(m6809_Regs *r){ (void)r; }
unsigned m6809_2_GetPC(void){ return 0; }
void m6809_2_reset(void){}
void m6809_2_execute(void){}
void m68k_init(void){}
void m68k_set_cpu_type(unsigned t){ (void)t; }

/* The sample SDK: the harness profiles the Z80, not the audio. */
void v_playSample(int i,int v,int l){ (void)i;(void)v;(void)l; }
void v_stopSample(int v){ (void)v; }
int  v_samplePlaying(int v){ (void)v; return 0; }

/* pokey sound (dead code on Tac/Scan) */
void Update_pokey_sound(void){}
char chip[8192];
