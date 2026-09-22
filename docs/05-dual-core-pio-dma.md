# 05 — Dual core, PIO and DMA

Three mechanisms that all serve the same end: **keep the beam moving while the
CPU is doing something else.** They are the baseline this SDK is measured
against, not options — `uvm2_bus.h` refuses to compile without them.

---

## Dual core

```
core 0   builds a command list into one of two buffers, publishes it
core 1   replays it over the bus, reads the controls, ticks the audio
```

### What it buys, and what it does not

It does **not** make drawing faster. The replay is paced by the Vectrex's own
1.5 MHz clock; 30 000 bus cycles is a 50 Hz frame by definition and no amount of
CPU makes a bus cycle shorter. One measured game's stream is 69 718 cycles
(46.5 ms) and this changes that number not at all.

What it buys is the **overlap**. Single-core, a frame is

```
[ emulate the CPU ][ build the list ][ replay 46.5 ms ][ pace ]
```

strictly in series, so the game's own time is *added* to the beam's. Split, the
logic for frame *n+1* runs while the beam is still drawing frame *n*, and the
frame costs `max(logic, replay)` instead of their sum. On one game, measured the
day it was found switched off: 51 ms of drawing + 46 ms of logic in series —
13 Hz, purely for not having core 1.

### Who owns the bus

**Core 1, exclusively, from the moment it starts.** That is the whole discipline
and it is not negotiable: two cores driving the same GPIO puts two writers on the
VIA with no arbitration. (That exact fault shipped once on another cartridge,
where 40 games claimed dual-core while compiled single-core.)

It works out cleanly because every bus contact was already funnelled into one
place, `SYS_WAIT_RECAL`: the replay, the button and axis reads, and the audio
tick. Those move to core 1 wholesale. The game still reads buttons and axes
through the same syscalls, which have always answered from a cache rather than
from the wire, so nothing on the game side changes.

Two exceptions:

* `SYS_PSG_WRITE` can happen at any moment, so it goes through a small queue that
  core 1 drains at the frame boundary. The routing is by **which core is
  executing**, not by which function was called, so it stays correct when someone
  adds a new call site.
* The raw `SYS_BUS_READ` / `SYS_BUS_WRITE` pair is **refused** while another core
  owns the pins.

### The handshake

```c
extern volatile uint32_t uvm2_frame_request;  /* frames core 0 has finished building */
extern volatile uint32_t uvm2_frame_done;     /* frames core 1 has finished replaying */
/* the buffer for frame n is n & 1 */
```

Two plain monotonic counters. A torn read is impossible on a 32-bit load, so no
lock is needed; the ordering is carried by a barrier core 0 issues before
publishing.

---

## PIO: one write per E period

`sdk/vectrex-bus/src/bus_stream.pio` is about 12 instructions, and every one of
them is there for a measured reason.

The loop, stripped of its commentary:

```
start:
    wait 0 gpio 31          ; ~E low
    wait 1 gpio 31          ; ~E rising = E falling = the latch
    nop [14]                ; phase calibration
present:
    pull noblock            ; next word, or X (the park word) if the FIFO is dry
    out y, 1                ; sentinel bit 0
    jmp !y, no_write
    out pins, 25            ; address + direction + data, inside E low
    jmp start
```

### Why the waits are in that order

With `wait 1` before `wait 0`, the address change landed **484 ns** from E's
falling edge — 151 ns *inside E high*, the half in which the 6522 decodes and the
address must be still. 11 of 13 changes violated the phase; the VIA latched
nothing; black screen. Swapping the two waits moves the presentation half a
period (−333 ns) into E low.

Measured with the scope on A0 (R214, a 33 Ω resistor, far more accessible than
the 6522's pin 38), against E, with E as the ruler — its period is 667 ns by
definition.

### Why the `nop [14]`

The path known to draw (the direct SIO one) presents at 245 ns into the period;
this stream presented at 465 ns. A **fixed** 221 ns offset with only 25 ns of
spread — not jitter to average away, a constant to correct. Swapping the waits
gave −333 ns (→ 132 ns), and `nop [14]` is 15 PIO cycles at 150 MHz ≈ 100 ns,
landing it where the working path puts it.

The number is `[14]` and not `[16]` because the sentinel added two instructions
before the presentation, and those two cycles had to be given back. **If you add
an instruction to this loop, this number changes.**

### The underrun is the idle state, not a hazard

If the FIFO empties, a plain `out` stalls with the pins **holding the last word**.
A VIA address left selected is re-latched on every subsequent E fall — harmless
for a port register, catastrophic for `T1_HI`, which restarts the ramp. At
1.5 MHz.

`pull noblock` solves it in hardware: with an empty TX FIFO it loads the OSR from
**X** instead of stalling, and X is loaded once, before the loop, with the **park
word** (`$C000`, which decodes to nothing on the Vectrex). A starved stream parks
the bus instead of hammering the VIA — no branch, no DMA, no CPU involvement.

That also means **DMA is an optimisation here, not a correctness requirement**.

### The two sentinel bits

`out` consumes from the *low* bit, so the only place you can look before driving
is bit 0. The payload is therefore shifted up by one, and `stream_word()` in Rust
shifts to match — the two move together or they drift apart.

```
bit0 = 1             a write; payload in bits 1..25
bit0 = 0, bit1 = 0   one period of silence
bit0 = 0, bit1 = 1   PARK for N periods, with N-1 in bits 2..25
```

The silence matters: the analog gaps (beam-on delay, blank settle) are *silence*
on the direct path, and silence is part of how a stroke is formed. Emitting a
write every period turned 2390 writes per frame into 17 314 words and broke
stroke chaining — the `.vec` files drew correctly but the strokes no longer
joined.

The repeated park replaces ~34 park words *per vector* (about 8400 a frame) with
one word. The `-1` is because `jmp y--` jumps while Y is non-zero and decrements
afterwards; `stream_repeat_word()` encapsulates it.

### One warning that cost a week

A `jmp` to a label written **after** the last instruction assembles without error
and points one slot past the program. Unwritten PIO instruction memory reads
`0x0000`, which decodes as `jmp always 0` — so the state machine ran its preamble,
made one pass and then spun at address 0 for ever. No warning, no error, and
every individual thing that was measured (the word layout, the phase, the
preamble, the branch durations) was correct. **Disassemble the `.pio`; do not
read the parser.**

---

## DMA

The DMA ring feeds the PIO FIFO so it never runs dry. Its job is to remove the
*spread*, not the offset: `pull noblock` does not wait, so a word that arrives
late is presented in the next period, and with 220 changes split between the two
halves of E there is no constant offset left to correct — only jitter. The DMA is
what removes it.

In dual core, core 1 pushes the list in 64-word batches
(`vectrex-bus`'s `BATCH_BUF`) and the DMA drains them. Note that the big
`LIST_BUF` (12 288 words, **98 KB**) is *only* used on the single-core path — in
dual core it is never written. A tight dual-core game should set
`UVM2_LIST_MAX=64` and spend those 98 KB on something that draws.

---

## Turning it off to bisect

```sh
make uvm2 UVM2_PIO_STREAM=0     # drive the bus from the core over SIO
make uvm2 UVM2_DUAL_CORE=0      # single core
make uvm2 UVM2_STREAM_INSTALL_ONLY=1   # install the stream but draw over SIO
```

Those exist to split a fault in half, not to ship. Anything measured with them on
is not comparable to the numbers in this kit.
