# 10 — Porting an AAE game

**AAE** (Another Arcade Emulator) is a C emulator for vector arcade machines,
derived from MAME's drivers. A port here does not run AAE: it links AAE's
*driver for one game* into the cartridge image as a C library, and points its
draw sink at the Vectrex beam.

`game/tacscan/` is a complete worked example — Sega G80, a Z80 and Sega's own
vector generator. Read it alongside this page; everything below has a concrete
counterpart there.

---

## 0. First, decide whether it can work

Three questions, in this order. Getting them wrong costs days.

### Is it a vector game?

The Vectrex has no framebuffer. A vector arcade game emits line segments, which
is the same shape as what this hardware draws. A raster game does not, and
turning one into vectors is a research project, not a port — see
[11](11-porting-from-mame.md).

### Does the CPU fit?

The RP2350 runs at 150 MHz and has to emulate the original machine **and** build
the command list **and** leave the bus busy. Measure before you commit:

```sh
cd game/<yours> && make host && ./build/host_<x> 120
```

The harness prints per-frame microseconds for CPU vs vector generation. For
Tac/Scan on a desktop it is 0.002 ms Z80 against 0.016 ms vector work; on the
cartridge the ratio changes (the M33 is slower and code may be fetched through
XIP), which is exactly why the game's own build carries a `TS_TELEM` block that
measures the split **on the console**.

Look for an **idle spin**. Most arcade games wait for their vblank interrupt in a
tight loop, and emulating it is pure waste: Tac/Scan spent **94.8% of all Z80
cycles** spinning at `$7A84`. `mz80_set_idle_pc(pc)` skips it. Find the address
with `make host-prof`, and check the one condition that makes it safe: the
variable the loop tests must have exactly one writer, the interrupt handler.

### Does it fit in 496 KB?

The emulated machine's ROM and RAM live in the image's `.bss`, plus the romset
buffer, plus the command list. Tac/Scan: 64 KB of Z80 space + 1 KB of PROM +
~24 KB of romset buffer + the list. A 68000 game with 830 KB of `.bss` does not
fit in SRAM at all and needs PSRAM.

---

## 1. The eight pieces you write

Everything else comes from the kit. In `game/tacscan/` they are:

| piece | file | what it does |
|---|---|---|
| 1. the driver row | `src/aae_machine.c` | declares the machine: CPU, clock, interrupt, fps |
| 2. the globals | `src/aae_machine.c` | the subset of `aaemain.c` your path touches |
| 3. the ROM table | `src/tacscan_romtable.h` | which zip member goes where |
| 4. `getport()` | `src/aae_machine.c` | Vectrex controls → the arcade machine's inputs |
| 5. the stubs | `src/aae_stubs.c` | no-ops for AAE subsystems this game never enters |
| 6. the compat header | `src/aae_compat.h` | types AAE expects that no header defines here |
| 7. the frame loop | `src/main.c` | ~20 lines |
| 8. the Makefile | `Makefile` | sources, flags, `include uvm2.mk` |

Plus, optionally: sound (`src/samples.c`, `src/ts_audio.c`), a libc subset
(`src/libc_stub.c`, `include/`), and a host harness (`tools/host_test.c`).

---

## 2. The driver row

AAE describes every machine as one row of `struct AAEDriver` (`globals.h`). A
port keeps **only its own row**, sized to cover its index in the `GameDef` enum:

```c
struct AAEDriver driver[TACSCAN + 1] =
{
    [TACSCAN] =
    { "tacscan", "Tac/Scan", 0,
      &init_segag80, 0, &run_segag80, &end_segag80,
      0, 0,                             /* game_dips, game_keys: input via getport */
      0, 0,                             /* game_samples, artwork */
      {CPU_MZ80, CPU_NONE, CPU_NONE, CPU_NONE},
      {3000000, 0, 0, 0},               /* cpu_freq: 3 MHz Z80 */
      {1, 0, 0, 0},                     /* cpu_divisions: slices per frame */
      {1, 0, 0, 0},                     /* cpu_intpass_per_frame */
      {INT_TYPE_INT, 0, 0, 0},          /* maskable IRQ, not NMI */
      {0, 0, 0, 0},                     /* int_cpu handlers */
      40, VEC_COLOR, 0,                 /* fps, video type, rotation */
      {0, 1024, 0, 1024}                /* gamerect */
    }
};
int gamenum = TACSCAN;
```

**`gamenum` must be the real enum value**, not 0: the driver files switch on it.
`SegaG80.c` does `switch (gamenum)` to pick its port handlers and its security
PROM, and `run_segag80()` checks `gamenum == TACSCAN`. The rest of the array is
zeroed `.bss` and never read, because `cpu_control.c` only ever looks at
`driver[gamenum]`.

Enum values: `CPU_NONE/6502Z/6502/MZ80/6809/68000/CCPU`,
`INT_TYPE_NONE/NMI/INT` (plus `INT_TYPE_68K1..7`), and
`VEC_BW_BI/16/64/256`, `VEC_COLOR`, `RASTER`, `RASTER_32`.

### fps is game speed

On most of these machines the logic advances on one interrupt per frame, so the
refresh rate **is** the game speed. Tac/Scan is a 40 Hz board (15468480 Hz ÷ 3 ÷
0x1f788 = 40.00 Hz exactly, which is also what MAME's `segag80v.cpp` says), and
free-running at 42.8 fps it played 7% fast. So the port calls
`uvm2_set_refresh(40)` and the number comes from the hardware, not from taste.

---

## 3. The globals

AAE's `aaemain.c` carries a large pile of globals. A port declares only the ones
its path actually references — the linker tells you which:

```c
unsigned char   *GI[5];            /* region pointers: the emulated memory */
CONTEXTM6502    *c6502[MAX_ACPU];  /* referenced by the dispatch, unused here */
CONTEXTMZ80      cMZ80[MAX_ACPU];  /* the Z80 contexts, used */
int              gamenum, WATCHDOG, total_length, testsw, paused;
colors           vec_colors[1024];
aae_settings     config;
```

`GI[region]` is the base of each memory region. Point them at **static arrays**,
not `malloc`: the cartridge's allocator is a bump arena and the teardown that
would free them never runs.

Size them to what is actually used, not to what AAE allocated. AAE malloc'd
64 KB for Tac/Scan's region 1 and never read past 1 KB of it — that array is the
PROM the vector generator uses as a sine table, indexed to at most 1022. Keeping
it at `0x400` is part of what makes the game fit.

---

## 4. The ROM table

The romset lives on the SD card as `roms/<name>.zip` and is decompressed
**directly onto `GI[]`** — no intermediate copy. The table says which zip member
goes to which offset of which region:

```c
const char game_romset_name[] = "tacscan.zip";

static const aae_rom_op ROM_OPS[] = {
    /*  member name in the zip      addr    size   src_off region mode */
    { "1711a.cpu-u25"             , 0x0000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1670c.prom-u1"             , 0x0800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    ...
    { "s-c.xyt-u39"               , 0x0000, 0x0400, 0x0000, 1, AAE_ROM_PLAIN },
};
```

* `mode`: `AAE_ROM_PLAIN`, or `AAE_ROM_EVEN` / `AAE_ROM_ODD` for a 16-bit
  interleave (the destination is strided: `addr + j*2 [+1]`).
* `src_off`: start at this byte **of the file** — a `ROM_CONTINUE`.
* `file == NULL`: repeat the previous file — a `ROM_RELOAD`.

**The names are the real zip member names**, matched case-insensitively on the
base name so a zip with an inner folder still works. There is deliberately no
fuzzy matching: what AAE calls `035127.02` can be `035127-02.np3` in the zip, and
resolving that on the cartridge would turn "a file is missing" into "the game
behaves oddly". Generate the table with the zip in front of you.

Interleaved loads and `ROM_CONTINUE` need a staging buffer, `AAE_ROM_STAGE`
(0 by default — set it in the Makefile to the largest such file). Plain loads
need none.

`UVM2_ROMZIP_MAX` — the buffer the zip is read into — is derived automatically
from the zip's size at build time by `uvm2.mk`. You do not write it.

If the load fails, `main()` must **not** emulate over zeros. That draws nothing,
and a black screen cannot tell "the ROM is missing" from "the game hung":

```c
if (aae_rom_last_error) {
    for (;;) { v_WaitRecal(); aae_rom_error_screen(aae_rom_last_error); }
}
```

`aae_rom_error_screen` prints the failure *and the path it tried*, in its own
tiny vector font.

---

## 5. Input

The emulated game reads its controls through `getport(n)`, which the driver calls
for its I/O ports. You map the Vectrex's two buttons-and-a-stick onto them:

```c
extern unsigned char currentButtonState;   /* button N = bit N-1 */
extern signed char   currentJoy1X, currentJoy1Y;   /* -127..127 */

int getport(int port)
{
    int b = currentButtonState, jx = currentJoy1X;
    switch (port) {
    case 0:  return (b & 0x04) ? (0xe0 & ~0x20) : 0xe0;  /* coins: ACTIVE LOW */
    case 4: { int v = 0;                                  /* buttons: ACTIVE HIGH */
               if (b & 0x08) v |= 0x01;                   /* start */
               if (b & 0x01) v |= 0x04;                   /* fire  */
               if (b & 0x02) v |= 0x08;                   /* grab  */
               return v; }
    case 6:  if (jx >  20) return (jx >  80) ?  5 :  3;   /* spinner delta */
             if (jx < -20) return (jx < -80) ? -5 : -3;
             return 0;
    default: return 0xff;
    }
}
```

Two traps worth naming, both of which cost a bring-up session here:

* **Polarity is per port.** Sega's coin inputs are active low with a non-zero
  idle value (`0xe0`); its button port is active high. Get one wrong and the
  controls silently do nothing.
* **Some drivers mask.** `sega_fix_dips` does `val & 0xF0`, so an earlier mapping
  that put fire and start on the low nibble was thrown away before the game saw
  it. Take the bit layout from the driver's own key table, not from a guess.

A **spinner** is a *delta per read*, not a position. The driver accumulates it.

---

## 6. Stubs and the compat header

AAE's generic CPU dispatch names every core it supports whether the game uses it
or not, and the `.um2` link has no `--gc-sections` escape for an undefined
symbol. So stub what never runs:

```c
void m68k_pulse_reset(void) { }
int  m68k_execute(int n)    { (void)n; return 0; }
unsigned m6502exec(unsigned n) { (void)n; return 0; }
/* ... */
void save_dips(void) { }
int  log_it(char *fmt, ...) { (void)fmt; return 0; }
int  save_hi_aae(int s,int z,int i) { (void)s;(void)z;(void)i; return 0; }
```

`aae_compat.h` is force-included (`-include`) into every AAE translation unit and
supplies the handful of types the drivers expect but that no AAE header defines
under a freestanding config (`BYTE`, `WORD`, `DWORD`), plus the player-2 stick
globals the input macros reference.

**Stub only what never runs.** If you stub a core the game does enter, it will
appear to work and then behave wrongly, which is much more expensive than a link
error.

---

## 7. Drawing

You write nothing. `third_party/aae/vector.h` already ends in

```c
v_directDraw32(AAE_SX(sx), AAE_SY(sy), AAE_SX(ex), AAE_SY(ey), z);
```

Two things happen there that are worth knowing:

* **Colour becomes intensity as `max(r,g,b)/2`, not the average.** These games
  drove colour monitors; a Vectrex tube has one phosphor. Measured over 880 024
  segments of one game, 85% of strokes have exactly one component lit, so the
  average hands 85% of the picture a third of white's brightness for the same z —
  a dark game with glaring white. `max()` returns the game's own z untouched.
  Below `AAE_Z_CUT` (20) the stroke is not drawn at all.
* **The screen transform is `v * AAE_SCREEN_MUL + AAE_SCREEN_OX`.** The defaults
  (36, −13356, −13968) were measured by **percentile** over attract and gameplay
  for one hardware family — the 1st to 99th percentile of every endpoint — and
  not by min/max, because the AVG does not clip and games happily emit objects
  far outside their own screen. A game with different coordinates defines its own
  `AAE_SCREEN_MUL/OX/OY` before including `vector.h`.

The host harness compiles with `-DNO_PI`, which sets the multiplier to 1 so a
dump reads in the game's own units. Remember that when feeding
`tools/uvm2_list_count.c` — see [07](07-measuring.md).

---

## 8. Sound

Three routes, cheapest first.

1. **Stub it.** `aae_stubs.c` no-ops the driver's sound entry points. Silent, and
   the game plays.
2. **PSG events.** If the machine's sound chip is an AY-3-8910 or close, route
   its register writes to `v_writePSG` — the Vectrex has the same chip.
3. **Samples.** If the sound is analog or an undumped MCU (Tac/Scan's board is a
   netlist in MAME, with the i8035's program uploaded into shared RAM — there is
   nothing to capture at any level), record `.wav` files and play them as
   digitised samples.

For route 3, three small files:

* `src/samples.c` maps AAE's sample interface (`sample_start(channel, num, loop)`
  and friends) onto `v_playSample/v_stopSample/v_samplePlaying`.
* `src/ts_audio.c` defines `v_sampleData(idx)` — the SDK declares it **weak**,
  because only the game knows which `.vsmp` is its laser — and loads the bundle
  with `uvm2_smp_bundle_load("<name>.vsm")`.
* `tools/wav_to_vsmp.py` converts `samples/*.wav` into that bundle.

**The bundle ships on the card, not in the image.** Linking 121 KB of samples
pushed one build past the loader's SRAM line and the console came up with a solid
red LED — a hardfault, not a hang. And **the name must be 8.3**: `uvm2_sd.c`
matches short directory entries and truncates the base to 8 characters, so
`aae_tacscan.vsm` was looked up as `AAE_TACSVSM` while the card held
`AAE_TA~1.VSM`. No match, no bundle, and a game that boots perfectly and says
nothing.

---

## 9. The Makefile

```make
UVMC2_KIT   ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
AAE_SRC     ?= $(UVMC2_KIT)/third_party/aae
UVM2_SDK    ?= $(UVMC2_KIT)/sdk/uvm2-sdk

CFLAGS = -DROMZIP_NO_STDIO -DMZ80_IDLE_SKIP -mthumb -mcpu=cortex-m33 \
         -mfloat-abi=soft -ffreestanding -O3 -std=gnu11 \
         -ffunction-sections -fdata-sections -DVPY_RP2350 \
         -D'CCNT0(x)=do{}while(0)' -include src/aae_compat.h \
         -Iinclude -Isrc -I$(AAE_SRC) -I$(VPY_C_INC) -I$(PITREX_INC)

AAE_SRCS   = $(AAE_SRC)/aae_romload.c $(AAE_SRC)/aae_romload_gi.c \
             $(AAE_SRC)/romzip.c $(AAE_SRC)/puff.c \
             $(AAE_SRC)/SegaG80.c $(AAE_SRC)/SegaG80snd.c \
             $(AAE_SRC)/mz80/mz80.c $(AAE_SRC)/cpuintrf.c \
             $(AAE_SRC)/cpu_control.c $(AAE_SRC)/rand.c $(AAE_SRC)/acommon.c
LOCAL_SRCS = src/main.c src/libc_stub.c src/aae_stubs.c src/aae_machine.c \
             src/samples.c src/ts_audio.c

UVM2_NAME   = aae_tacscan
UVM2_SRCS   = $(LOCAL_SRCS) $(AAE_SRCS)
UVM2_CFLAGS = $(CFLAGS)
UVM2_HZ    ?= 0
include $(UVM2_SDK)/uvm2.mk
```

`-std=gnu11` because AAE is K&R C. `-O3` because the interpreter is CPU-bound.

`UVM2_HZ ?= 0` in the Makefile and `uvm2_set_refresh(40)` in `main()` are not in
conflict: the build-time value is the SDK's pacing cap and the runtime call is the
game declaring its own board rate. Tac/Scan needs free pacing for the sample
injector (with a fixed cap the frame's leftover is spent on the zero-reference
filler, where nothing can be injected, and the audio coverage drops from 91% to
14-38% depending on the scene) while still running its logic at 40 Hz.
`libc_stub.c` is in `UVM2_SRCS` but `uvm2.mk` **drops it** on the `.um2` path
(`UVM2_SRCS_DROP`): the pico-sdk brings newlib and the hand-written `exit`,
`fclose` &c. would collide. The header shims in `include/` stay, and newlib
supplies the bodies.

### The flags worth knowing

| flag | what it buys |
|---|---|
| `-DMZ80_IDLE_SKIP` | the Z80 idle-spin skip. Not telemetry — it stays on. |
| `-DAAE_DISPATCH_NOINLINE` | ~15 KB smaller, noticeably slower. Set it via `UVM2_UM2_ONLY` so it applies to the `.um2` only. |
| `-DAAE_RD_BASE` | banked ROM reads without a call. 13.7% of emulation time in one measured port; costs 1 KB and inlines a test at every access. Per game. |
| `-DAAE_VEC_RAM=4` | Quantum's 8 KB vector RAM that every other driver reserves and never touches. |
| `-DAAE_VEC_COLORS=64` | the 12 KB palette, of which ~17 entries are used. |
| `-DAAE_ROM_STAGE=N` | the staging buffer, only if you use interleave or `ROM_CONTINUE`. |
| `-DAAE_ROM_STAGE_PSRAM=addr` | put that staging buffer in PSRAM. |
| `-DUVM2_ROMZIP_IN_PSRAM=1` | put the romset buffer in PSRAM (on by default when a zip is found). |
| `-DAAE_ACCESS_COUNT` | host-only: how often and where the emulated CPU touches memory. |
| `-DAAE_WATCH_READ` | host-only: which emulated PC reads or writes a given address. |

---

## 10. Bring-up order

Do these in order; each one makes the next one debuggable.

1. **`make host`** — does the emulated CPU run, and does anything get drawn?
   The harness hashes every coordinate, because `draw_calls` alone proves nothing
   (a frozen screen keeps its count).
2. **`make host-prof`** — where does the emulated CPU spend itself, and is there
   an idle spin to skip? Check the draw hash is unchanged by the skip, or the
   skip changed the game.
3. **`make uvm2`** — does it link, and does it fit? A `region RAM overflowed by N
   bytes` here is the moment to use the size flags above.
4. **`TACSCAN_DUMP=<file>` + `tools/uvm2_list_count.c`** — how many commands does
   a real frame cost, and does it fit the budget?
5. **On the console** — with `stats.dropped` in view. If it is not zero, stop and
   raise the capacity: nothing you see is evidence until it is.

### The three failures that look like something else

* **A black screen.** Check the romset first (`aae_rom_last_error`), then
  `stats.dropped`, then whether `uvm2_clock_calibrate()` returns 0 — which means
  the console is off or the cartridge is not seated, not that the game is broken.
* **Controls do nothing.** Almost always `getport` polarity or a mask in the
  driver, not the SDK.
* **The game plays too fast or too slow.** The refresh rate is the game speed.
  Pin `uvm2_set_refresh()` to the board's real rate.

---

## 11. What the kit does not ship

`third_party/aae/` contains only what Tac/Scan needs — about 52 files. In
particular the **CPU cores other than Z80 are headers only**: `m6502/`, `m6809/`
and `musashi/` have their `.h` but not their `.c`, because AAE's dispatch
references them and Tac/Scan never runs them.

Porting a 6502 game (most Atari vector games), a 6809 game or a 68000 game means
bringing that core's sources and its driver in from AAE. The kit's structure does
not change; the file list does.
