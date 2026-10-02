# 07 — Measuring

There is no console on a cartridge. Everything here exists so that a fault can be
told apart from a *different* fault without one.

## The counters

`uvm2_stats` (`uvm2_bus.h`) is a plain global struct; read it over SWD, or surface
it on screen. The ones that decide whether any other number is worth reading:

| field | read it as |
|---|---|
| `dropped` | commands lost because the list was full. **Non-zero means nothing on screen is evidence.** |
| `ramps_clamped` | ramps started while the zero clamp was on, last frame. **Must be 0**: such a ramp draws a line out of the centre instead of the stroke asked for. See [04](04-drawing.md), "Re-zeroing". |
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

#### Setting up the probe, from nothing

What you need: a **Raspberry Pi Pico** (RP2040) or **Pico 2** (RP2350) — any one;
it becomes the probe and nothing else — a USB cable for it, three jumper wires and
a **2.0 mm JST-PH 3-pin** plug for the cartridge end (see the wiring below). The
official Raspberry Pi Debug Probe works too, but its own cable ends in a 1.0 mm
JST-SH plug and needs an adapter to the cartridge's PH header.

1. **Put the Debugprobe firmware on the Pico.** Download it from Raspberry Pi's
   releases page, <https://github.com/raspberrypi/debugprobe/releases> — the kit is
   verified with **v2.3.1** — and take the file for your board:

   | board | file |
   |---|---|
   | Pico (RP2040) | `debugprobe_on_pico.uf2` |
   | Pico 2 (RP2350) | `debugprobe_on_pico2.uf2` |
   | Raspberry Pi Debug Probe | `debugprobe.uf2` |

   Hold the Pico's **BOOTSEL** button while plugging its USB cable in; it mounts
   as a drive (`RPI-RP2` on a Pico, `RP2350` on a Pico 2). Copy the `.uf2` onto it.
   It reboots on its own as a probe; the drive goes away, which is right.

2. **Install probe-rs** on the computer. The kit is verified with **0.31.0**,
   installed with Cargo (Rust is already a requirement of the kit):

   ```sh
   cargo install probe-rs-tools --locked
   ```

   probe-rs's own documentation has installers too. **On Linux**, a probe is only
   usable by root until probe-rs's udev rules are installed — see the "Probe Setup"
   page of the probe-rs documentation; macOS needs nothing.

3. **Check the probe before touching the cartridge:**

   ```sh
   probe-rs --version                  # verified with 0.31.0
   probe-rs list                       # reads USB descriptors only; does not touch the target
   ioreg -p IOUSB -l -w0 | grep -A30 '"Debugprobe' | grep bcdDevice   # macOS
   lsusb -v -d 2e8a:000c | grep bcdDevice                             # Linux
   ```

   It should list `Debugprobe on Pico (CMSIS-DAP)`, USB `2e8a:000c`; `bcdDevice
   0x0231` is release 2.3.1.

4. **Wire it** (next section), switch the console on with a game running, and
   read something that cannot hurt:

   ```sh
   python3 sdk/uvm2-sdk/tools/stats.py build_uvm2/pico/<UVM2_NAME>.elf
   ```

   It checks first that the ELF is the image running, then prints the counters.
   "Target device did not respond" means the console is off, the cartridge is not
   seated or a wire is wrong — not a broken probe (`probe-rs list` already said the
   probe is fine).

Every command here passes `--chip RP235x`; probe-rs also accepts `RP2350` and
warns that it matched it by wildcard.

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
| `swd_var.py <elf> <name> [value]` | no | read any global by name, or write a 1-, 2- or 4-byte one and read it back: the **live knobs** (below). |
| `list_from_ram.py <elf> out.json` | no | the command list the console is replaying, straight from RAM, for `beam_sim.py`. Static screens only: it reads twice and refuses if they differ. |

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

#### Live knobs: an A/B on the tube without rebuilding

Every runtime knob is a `volatile` global (the list is in
[08](08-api-reference.md), "Drawing"), so it can be changed while a game runs and
the tube watched — the same console, the same brightness, nothing else changed:

```sh
T=sdk/uvm2-sdk/tools
python3 $T/swd_var.py $ELF uvm2_filler_clamp        # read
python3 $T/swd_var.py $ELF uvm2_filler_clamp 0      # the old filler: is the diagonal back?
python3 $T/swd_var.py $ELF uvm2_filler_clamp 1      # and gone again?
python3 $T/swd_var.py $ELF uvm2_pacer_cycles 0      # free refresh, no filler at all
```

A write is gone at the next power cycle. A setting that wins goes into the source
as the default, with the comparison in its comment. When nothing existing isolates
a fault, add a temporary `volatile` switch, flash once, and bisect with it — that
is how the 2026-10-02 dot was traced to one part of the joystick read
([12](12-calibrating-a-console.md), "A diagonal or a dot"). Take the switch out
again once the answer is in.

#### The list the console is replaying, from RAM

```sh
python3 $T/list_from_ram.py $ELF list.json
python3 $T/beam_sim.py list.json lit.svg
```

No card and no dump code in the game: the probe reads the buffer core 1 replayed
last (`uvm2_frame_done & 1`). It is a large read, so do it once, on the screen you
want; and it reads twice and refuses if the two differ, so it is for a static
screen — a menu, a paused game, a test card. For a moving one, have the game call
`uvm2_dump_list` (below). Builds with the list in PSRAM need `--psram
<UVM2_CMD_CAPACITY>`.

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

Build them all with `sdk/uvm2-sdk/tools/build_host_tools.sh`: it links each one
with the shared host stubs and with `-DUVM2_HZ=0` — without that, the 50 Hz lock
pads every list to 30000 cycles and a tool measuring a frame measures the
padding (937 cycles per operation instead of 48).

| tool | answers |
|---|---|
| `uvm2_list_count.c` | How many commands / bus words / bus cycles a **real dumped frame** needs, broken down by VIA register, plus the geometry that explains it (stroke lengths, how many are chained, jump distances, brightness changes, the worst run with no re-centring). |
| `uvm2_anatomy.c` | Decodes the list command by command for a synthetic chain: which stage spends which cycle, writes vs waits. A totals table says *how much*; this says *what*, and it is the only thing that shows a delay hanging off the wrong register. |
| `uvm2_op_cost.c` | The marginal cost of each primitive (move, draw, intensity, chained draw), measured as `frame_begin + primitive + frame_end` minus the empty frame. |
| `uvm2_order.c` | How much reordering would save, in five different orders, on a real dump. |
| `uvm2_ramp_cost.c`, `uvm2_gapped_budget.c`, `uvm2_budget.c` | Ramp and frame-budget models. |
| `uvm2_smp_test.c` | Sample injection coverage against scene complexity. |
| `smp_stats.py` | Reads the mixer histograms `-DUVM2_SMP_TELEM=1` produces. |
| `list_from_rtt.py` | The command list **the console actually ran**, from the debug cartridge BIOS's RTT dump (`CMD_DUMP`, buttons 1+2 held on a frozen frame). Refuses if the passes disagree. |
| `list_from_sd.py` | The same list from the SD card: `uvm2_dump_list` (or `uvm2_dump_list_on_buttons` once a frame) writes the last closed frame with a header — count, frame, `dropped`, `ramps_clamped`, a hash — and this refuses a file whose magic, length or hash is wrong, and warns when `dropped` is not zero. No probe needed. |
| `beam_sim.py` | Plays a list against an ideal beam and reports what the **list** gets wrong: ramps started with the zero clamp on (must be 0), lit cycles under the clamp, the frame's length, and how long the integrators ran **dark and free** with `/RAMP` held open by Port B — a slow dark sweep is a line once the brightness is up; draws what is lit as an SVG, and where the blanked beam went in faint red. |

**From the console to an answer, without halting it.** Three ways to get the list,
by what you have: the **probe** and a static screen (`list_from_ram.py`, above);
the **card** and a game that calls `uvm2_dump_list` (below); or the **debug
cartridge's RTT**, which only that cartridge's BIOS prints — the UVMC2 has none.
For the RTT, freeze the frame, hold 1+2 while `probe-rs attach --chip RP235x
<bios.elf>` (which halts nothing) saves the RTT, then:

```sh
python3 sdk/uvm2-sdk/tools/list_from_rtt.py rtt.log list.json
python3 sdk/uvm2-sdk/tools/beam_sim.py list.json lit.svg
```

**Or without a probe at all**, from the card. In a `.um2` the game calls
`uvm2_dump_list_on_buttons("debug/list.bin", mask)` once a frame; under the
debug cartridge's BIOS the BIOS does it for every game, on buttons 3+4, into
`DEBUG/LIST.BIN` — a file that must already exist on the card at 40 KB or more
(`dd if=/dev/zero of=LIST.BIN bs=1k count=40`), because that BIOS overwrites in
place and cannot grow a file. Then:

```sh
python3 sdk/uvm2-sdk/tools/list_from_sd.py /Volumes/<card>/DEBUG/LIST.BIN list.json
python3 sdk/uvm2-sdk/tools/beam_sim.py list.json lit.svg
```

`uvm2_dump_diag` says whether it ran and, if not, why. Under the BIOS the header's
`dropped` and `ramps_clamped` are read when the dump runs, which can be a frame
later than the list.

That is how the 2026-10-01 "asterisk" was found: the BIOS's own list for the
frame, replayed, drew the first chain of strokes at the centre with the clamp
still on — the model is ideal, so a fault it shows is in the list, not in the
console.

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
