# 07 — Measuring

There is no console on a cartridge. Everything here exists so that a fault can be
told apart from a *different* fault without one.

## The counters

`uvm2_stats` (`uvm2_bus.h`) is a plain global struct; read it over SWD, or surface
it on screen. The ones that decide whether any other number is worth reading:

| field | read it as |
|---|---|
| `dropped` | commands lost because the list was full. **Non-zero means nothing on screen is evidence.** |
| `recals` | cumulative recalibrations. If it does not climb, `Recalibrate` is *not being called* — "it does not work" and "it never ran" are different investigations. |
| `commands`, `bus_cycles`, `exec_cycles` | the frame's size, in commands and in bus cycles (667 ns each; 30 000 = 50 Hz). |
| `vectors`, `moves` | lit strokes vs blanked repositions. The ratio is the chaining. |
| `overrun` | frames whose stream exceeded the 50 Hz budget. |
| `us_frame_last/min/max`, `vectors_at_max`, `slow_frames` | the **real** frame period, end to end. |
| `us_exec`, `us_input`, `us_rest`, `us_wait` | where core 1's time goes. |
| `vectors_last`, `moves_last`, `ramp_cycles_last` | snapshots taken at `frame_end` and never cleared. |

That last row matters more than it looks. The live counters are reset in
`frame_begin` and fill up as the frame is built, so a debugger sampling at an
arbitrary moment mostly catches them at **zero** — a game that spends 40 ms
emulating a CPU and 2 ms drawing is in the "reset, not yet drawn" window almost
always. Read the `_last` fields.

### Reading them over SWD

Without a probe the counters can still be surfaced on screen with
`uvm2_print_text`, and the LED is the channel of last resort — **solid red means
a hardfault** (bad pointer, stack overflow, unaligned access), not a game
spinning (`isr_hardfault` in `uvm2_pico_main.c`). Everything below needs the
probe.

#### The probe and the wiring

The probe is a second Raspberry Pi **Pico running the Debugprobe firmware**
(Raspberry Pi's CMSIS-DAP probe). The one this kit was measured with enumerates
as `Debugprobe on Pico (CMSIS-DAP)`, USB `2e8a:000c`, and reports
`bcdDevice 0x0231`, which is where Debugprobe puts its release number: **2.3.1**.
Re-check yours, and the host tool, with:

```sh
probe-rs --version                  # verified with 0.31.0
probe-rs list                       # reads USB descriptors only; does not touch the target
ioreg -p IOUSB -l -w0 | grep -A30 '"Debugprobe' | grep bcdDevice   # macOS
lsusb -v -d 2e8a:000c | grep bcdDevice                             # Linux
```

The UVM2 brings SWD out on a **3-pin JST-PH header labelled `DEBUG`** (2.0 mm
pitch, from the cartridge's schematic). Three wires:

| UVM2 `DEBUG` pin | signal | RP2350 pin | Debugprobe Pico |
|---|---|---|---|
| 1 | SWCLK | 33 | GP2 (header pin 4) |
| 2 | GND | — | any GND (e.g. header pin 3) |
| 3 | SWDIO | 34 | GP3 (header pin 5) |

The probe draws its power from USB, not from the cartridge: do **not** connect a
3V3 line. The Vectrex powers the cartridge, so the console must be on for the
target to answer — `probe-rs list` finding the probe while every other command
fails means the console is off or the cartridge is not seated, not that the probe
is broken. A cable with a broken wire has also happened; a probe that cannot
reach a console that is on, check the cable before the firmware.

#### What stops the core, and what does not

This is the rule that cost the most, and the story changed twice, so here is the
current state and its date:

* **`probe-rs read` / `probe-rs write` do not halt the RP2350.** They go through
  the AHB-AP. Measured on the console on 2026-08-24: a knob written with
  `probe-rs write` while a game ran, the telemetry kept flowing and the game kept
  playing.
* **GDB halts it** — `probe-rs gdb` plus `arm-none-eabi-gdb`, which is the only
  way to read the **registers**. Halting the core that drives the Vectrex bus is
  a phase violation, and **on this board a halted core does not come back**:
  `detach` and writing `DHCSR` by hand were both tried, and the only recovery is a
  power cycle. Last confirmed 2026-09-13.
* Older comments in `uvm2_draw.c` say that nothing can be read over SWD on this
  board. They predate the 2026-08-24 measurement and describe the GDB path.

Even non-halting reads are not free: every `probe-rs` call attaches to the debug
port and competes with the drawing core for the bus. A panel polling every 0.6 s
pulled one game from 20 fps to 11 — a rate that does not exist when nobody is
looking. **Read once, after the game has run on its own for a while.**

#### The tools

All in `sdk/uvm2-sdk/tools/`. Each one's header says what it cost to learn.

| tool | halts? | use it for |
|---|---|---|
| `stats.py <elf>` | no | `uvm2_stats` in one read: the first 24 words, or `--all`. Checks first that the ELF is the image actually running. |
| `stats.py <elf> --fps [s]` | no | the real frame rate, from `recals` over a silent window. `recals` not moving means the loop is not running. |
| `smp_stats.py <elf>` | no | the sample mixer's histograms (build with `-DUVM2_SMP_TELEM=1`). |
| `probe.sh [addr] [n]` | **yes** | PC, LR, SP and a block of memory. **Only on a console that is already hung**: the PC of a hang is worth more than any counter. |
| `load.sh <elf>` | no¹ | put an image on the console over SWD instead of the SD card. Start any image from the menu first. |
| `release.sh` | — | kill a session `load.sh` or a stray GDB server left attached. |

¹ `load.sh` attaches through GDB to load, then releases. With `ATTACHED=1` it
stays attached for debugging the startup, and while it is attached the console
stumbles and draws noise — do not judge the image like that.

```sh
ELF=build_uvm2/pico/<UVM2_NAME>.elf                 # next to the .um2
python3 sdk/uvm2-sdk/tools/stats.py $ELF            # dropped first; if non-zero, stop
python3 sdk/uvm2-sdk/tools/stats.py $ELF --fps 10
```

**The address always comes from the ELF, and the ELF must be the running
image.** The counters live in `.bss`, so adding one variable anywhere shifts
everything after it; an address from another build reads another variable and
still looks like a number. `stats.py` resolves it with `nm` every run and refuses
if a function's bytes in the target's RAM differ from the ELF's. For any other
global the SDK keeps for SWD (`uvm2_sd_error`, `uvm2_sd_diag`,
`uvm2_psram_result`, the `ts_us_*` telemetry in `game/tacscan/src/main.c`), do the
same by hand:

```sh
ADDR=$(arm-none-eabi-nm "$ELF" | awk '$3=="uvm2_sd_error"{print $1}')
probe-rs read --chip RP235x --speed 1000 b32 0x$ADDR 1
```

The first twelve words of `uvm2_stats_t` (`uvm2_bus.h`), for reading a raw dump:

```
 0 commands      1 bus_cycles    2 vectors       3 overrun
 4 moves         5 ramp_cycles   6 dropped       7 recals
 8 exec_cycles   9 vectors_last 10 moves_last   11 ramp_cycles_last
```

**Leave nothing attached.** After a block of measurements check
`pgrep -f probe-rs` is empty. A session left attached makes the cartridge
dependent on whatever the next command is.

`us_frame_*` exists because the padding in `frame_end` is computed from the
*drawing's* bus cycles, so the game's own time is **added** to the frame rather
than absorbed by it. The real refresh then drops below 50 Hz and, worse, varies
with the scene — few objects, short frame; many, long frame. That is stutter, and
none of the other counters sees it.

## The host tools

`sdk/uvm2-sdk/tools/` are standalone C programs that link the **real** emitter
(`uvm2_draw.c` and the Rust beam model) against a **simulated** bus, so a frame
can be analysed on a laptop. They are not part of any build; each one's header
gives its command line. Roughly:

```sh
cc -O2 -DUVM2_HOST -DUVM2_BENCH_NO_CORE1 -DUVM2_SUBUNITS -DUVM2_CMD_CAPACITY=65536u \
   -I<kit>/sdk/uvm2-sdk -o /tmp/count tools/uvm2_list_count.c <kit>/sdk/uvm2-sdk/uvm2_draw.c \
   <kit>/sdk/vectrex-draw/cabi/target/release/libvectrex_draw_cabi.a
```

| tool | answers |
|---|---|
| `uvm2_list_count.c` | How many commands / bus words / bus cycles a **real dumped frame** needs, broken down by VIA register, plus the geometry that explains it (stroke lengths, how many are chained, jump distances, brightness changes, the worst run with no re-centring). |
| `uvm2_anatomy.c` | Decodes the list command by command for a synthetic chain: which stage spends which cycle, writes vs waits. A totals table says *how much*; this says *what*, and it is the only thing that shows a delay hanging off the wrong register. |
| `uvm2_op_cost.c` | The marginal cost of each primitive (move, draw, intensity, chained draw), measured as `frame_begin + primitive + frame_end` minus the empty frame. |
| `uvm2_order.c` | How much reordering would save, in five different orders, on a real dump. |
| `uvm2_ramp_cost.c`, `uvm2_gapped_budget.c`, `uvm2_budget.c` | Ramp and frame-budget models. |
| `uvm2_smp_test.c` | Sample injection coverage against scene complexity. |
| `smp_stats.py` | Reads the mixer histograms `-DUVM2_SMP_TELEM=1` produces. |

The input for `uvm2_list_count` is lines of `x0 y0 x1 y1 z` in the units the game
passes to `v_directDraw32`; each port's host harness already dumps that format
(`cd game/tacscan && make host && ./build/host_ts 60 2>/tmp/frame.txt`).

**Mind the units.** The AAE ports compile their host harness with `-DNO_PI`,
which sets the screen multiplier to 1, so the dump is in the *game's* units while
the real target multiplies by 36 and shifts the measured centre to zero. Feeding
the emitter game units measures ramps 36 times too short, and the number that
comes out belongs to no game at all:

```sh
MUL=36 OX=-13356 OY=-13968 /tmp/count /tmp/frame.txt    # AAE ports
/tmp/count /tmp/frame.txt                               # identity
```

## Rules that were learned expensively

* **Compare the binaries before comparing the measurements.** Four identical
  results in a row usually means the artefact never rebuilt. Check its mtime
  before you check the code.
* **A flag that does not reach the compiler is not a flag.** `GAME_CFLAGS`
  appearing twice in a Makefile, the second one winning, cost a day. Check
  `flags.make` in the CMake build directory before measuring.
* **A knob that does not move the picture may be disconnected, not irrelevant.**
  Several counters in this SDK exist purely to prove a branch fires
  (`START_HITS`, `recals`, `uvm2_smp_injected`).
* **The instrument perturbs the measurement.** A live SWD panel reading state
  pulled one game down to 11 fps. Probe quietly, measure in silence.
* **One console is not evidence.** Artefacts once blamed on the drawing turned
  out to be one worn-out console; another drew the same bits cleanly. Any beam
  constant tuned by eye on a single machine is suspect.
* **Bisect with one criterion, written down first**, and check both ends of the
  range. A flash cycle is cheap; converging on the wrong defect is not.
* **An empty grep is not an absence.** `file` reports `data` for files grep skips
  in silence. Read before grepping.
* **A chain that prints nothing failed before its first trace.** If an
  instrumented path produces no output at all, look *upstream* of the first
  print, not at the logic.
