# 01 — The hardware

## The cartridge

The **Ultimate Vectrex Multicart 2** (UVM2) is an RP2350-based cartridge for the
Vectrex. Its stock firmware shows a menu, reads a `.um2` image off the SD card
into SRAM, and jumps into it. From that instant the firmware is gone: **your
image owns the machine** — clocks, pads, both cores, the bus.

Relevant facts:

* RP2350, dual Cortex-M33, 520 KB of internal SRAM.
* An image loaded by the menu lives in SRAM and has about **496 KB** to play
  with. That is the hard ceiling for code + data + the command list.
* 8 MB of external **PSRAM** on the QSPI bus (chip select CS1 = GPIO47). It is
  not mapped by the firmware; `uvm2_psram.c` brings it up. See below.
* The RP2350's GPIOs go straight to the **Vectrex cartridge edge**.

## The `.um2` file

A 20-byte little-endian header followed by the raw image:

```
offset  size  meaning
0       4     magic "2CMU"
4       4     version = 1
8       4     block count = 1
12      4     load address = 0x20000000
16      4     length, in 32-BIT WORDS  (not bytes)
20      ...   payload
```

`sdk/tools/package_um2.py` writes it; the payload is padded to a word boundary.
The length being in **words** is the sort of detail that costs an afternoon, so
it is worth repeating.

The payload must also contain an RP2350 **IMAGE_DEF** block within its first
4 KB, or the chip refuses to treat it as an image at all — it boots into
nothing. The pico-sdk emits one for us, which is the main reason the build goes
through CMake rather than linking by hand (`sdk/uvm2-sdk/uvm2_start.s` carries a
hand-written one for the path that does not).

## Halt-mode: why the game drives the VIA

A Vectrex draws with an analog beam steered by a **VIA 6522** at `$D000`:

* **Port A** is the X/Y **DAC** (one 8-bit value at a time),
* **Port B** picks which integrator the DAC value is routed to (the *mux*) and
  carries `/RAMP`,
* **Timer 1** decides how long the integrators run — that is the stroke's length,
* the **shift register** carries brightness, `PCR` carries `/ZERO` and `/BLANK`.

A normal cartridge answers the 6809's ROM fetches. We cannot: the code in a
`.um2` is native ARM, so the 6809 has nothing to execute. Instead we assert
`/HALT` permanently and become the bus master, writing the VIA registers the
6809 would have written.

### The one rule

The 6800-family bus does exactly **one access per E period**, and the halves of
the period are not interchangeable:

```
E LOW   (first half)   the master changes the address. The VIA is not looking.
E HIGH  (second half)  the VIA decodes; address, CS and R/W must be STABLE.
E FALL  (the edge)     the write is latched.
```

So: **change the address during E LOW, hold it through E HIGH.** Measured on the
console — presenting within 30 cycles of the fall draws; within 70 cycles the
screen is black. A setup-time violation degrades gracefully; a phase violation
fails outright.

The `~E` signal (the inverted E) is on cart edge pin 12 and free-runs at
1.5 MHz whatever the address or R/W lines do. Its **rising** edge is E's falling
edge, which means one edge signals both "the previous write landed" and "it is
now safe to present the next one". That is what the PIO program synchronises on
(see [05](05-dual-core-pio-dma.md)).

One E period is **667 ns**, so a 50 Hz frame is **30 000 bus cycles**. That
number is the budget for everything drawn in a frame, and no amount of CPU makes
it larger.

## GPIO map

```
GP0-7    D0-D7          GP8-21   A0-A13
GP22     PB6            GP23     /IRQ
GP24     A14            GP25     A15
GP26     R/W            GP27     /HALT
GP29     /NMI           GP31     CLK (~E)
GP34/35/36/39           SD card: SCK / MOSI / MISO / CS
GP47                    PSRAM chip select (QMI CS1n, function F9)
```

GP8 = A0 is confirmed against a reference game; the SD pins were **measured off
the stock firmware over SWD while it sat in its own menu**, not read off the
schematic — the schematic reading was off by two and the symptom was a card that
simply never answered.

## PSRAM

There is 8 MB on CS1 that the firmware does not map. `uvm2_psram_init()` resets
the chip, checks its ID and arms the QMI's M1 XIP window at `0x11000000`.

Three things are worth knowing before you use it:

1. **Writes to `0x11000000` are discarded silently** unless `XIP_CTRL.WRITABLE_M1`
   is set. Silent is the worst failure mode there is.
2. **Verify through the uncached alias, `0x15000000`.** The XIP cache is 16 KB; a
   64 KB buffer verifies fine right after it is written (hot cache) and reads
   back wrong later. Two observations that fit perfectly and still hide a broken
   chip.
3. **It is good for write-once/read-once data at startup** — a romset, a staging
   buffer — and bad for anything timing-critical. The command list stays in SRAM
   for exactly that reason (see the `.time_critical` note in
   `sdk/uvm2-sdk/memmap_psram.ld`).
