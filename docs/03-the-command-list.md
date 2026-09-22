# 03 — The command list

Drawing on this hardware means writing VIA registers in time with a 1.5 MHz
clock. Doing that one write at a time stalls the CPU on every edge and leaves the
beam idle whenever the game is thinking. So instead:

> **Core 0 RECORDS a list of VIA writes. Core 1 REPLAYS it back to back.**

Everything in the SDK is built around that one idea.

## The command

24 bits, stored in **3 bytes**:

```
bits 23..12   delay: bus cycles to idle AFTER this write (0..4095)
bits 11..8    VIA register select   -> A0-A3
bits  7..0    data byte             -> D0-D7
```

In code (`uvm2_bus.h`) a command is built as a 32-bit word and packed on the way
into the buffer:

```c
#define UVM2_CMD(reg, data, delay) \
    (((uint32_t)(delay) << 20) | ((uint32_t)(reg) << 16) | (((uint32_t)(data) & 0xFF) << 8))

#define UVM2_CMD_PACK(w)      ((w) >> 8)            /* 32 bits -> the 24 that matter */
#define UVM2_CMD_REG_DATA(v)  ((v) & 0xFFF)         /* lands on GP0-11 with no shift */
#define UVM2_CMD_DELAY(v)     ((v) >> 12)
```

The 12-bit `(reg<<8 | data)` field lands directly on GPIO0-11, so the executor's
inner loop does no shifting at all. Storing 3 bytes instead of 4 is not
pedantry: the image lives in 496 KB, and a 64 KB buffer holds 21 845 commands
packed against 16 384 unpacked.

The encoding is deliberately byte-identical to the one the cartridge's own games
use, so the two executors can be compared directly.

## Cost

**One command = one bus cycle = 667 ns, plus its delay.**

So a 50 Hz frame is 30 000 bus cycles, and `uvm2_exec()` returns exactly how many
a frame spent. `sdk/uvm2-sdk/tools/uvm2_list_count.c` breaks a real frame down by
VIA register; a representative result looks like:

```
  COMMANDS PER SEGMENT 13.05
  where the commands go (by VIA register):
    PORT_A  DAC: X or Y rate          ...
    T1CL    the ramp's scale          ...
    T1CH    STARTS the ramp           ...
    T1LL    WAITS for it to finish    ...   <- most of the DELAY lives here
```

Two numbers worth carrying in your head:

* **a stroke costs about 13 commands, and that is FLAT in its length** — a long
  stroke and a short one cost the same number of commands, because the length
  lives in T1's value, not in the number of writes;
* **a blanked jump costs about 5 more** on top of that.

Which is why chaining strokes (each starting where the last ended) is the single
cheapest habit there is on this hardware, and why "fewer, longer strokes" beats
"more, shorter strokes" every time.

## The frame

```c
uvm2_frame_begin();        /* reset the list and the counters */
  ... uvm2_draw_* ...      /* records commands */
uvm2_frame_end();          /* publish for core 1; pace to UVM2_HZ */
```

A game normally never calls these: `v_WaitRecal()` in `sdk_rp2350.c` does, and
`vpy_frame_begin()` calls that.

Under dual core, `uvm2_frame_end()` publishes to a double buffer — the buffer for
frame *n* is `n & 1` — and bumps `uvm2_frame_request`. Core 1 bumps
`uvm2_frame_done` when it has replayed it. Both are monotonic 32-bit counters, so
a torn read is impossible and no lock is needed; the ordering is carried by a
barrier core 0 issues before publishing.

## When the list fills up

`stats.dropped` counts commands thrown away because the list was full.

**If `dropped` is not zero, nothing you see on screen is evidence of anything.**
A silent cap reads as "the drawing is broken" and sends you to debug the wrong
file — which is exactly what happened once when a frame reached exactly 8192
commands and quietly lost its tail.

Raise `UVM2_CMD_CAPACITY` (3 bytes per command per buffer, and there are two buffers under dual core), or move the
list to PSRAM with `UVM2_CMDS_IN_PSRAM=1`, or draw less.

## Reads cannot be recorded

A read needs the data bus turned around mid-cycle, so it cannot go in the list.
`uvm2_via_read()` runs directly, and every read in the SDK happens **between
frames**, while `/ZERO` holds the beam clamped at the centre. Reading in the
middle of a frame disturbs the integrators; that window was measured as the
source of most stray bright vectors (see [06](06-sound-input-sd.md)).
