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
