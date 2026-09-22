# 06 — Sound, input and the SD card

Everything in this chapter touches the Vectrex bus directly, and the bus belongs
to the beam. That constraint shapes all three.

---

## Sound

### The PSG

The Vectrex's AY-3-8912 is reached through the VIA's handshake: the register
number goes out on Port A, then `BDIR`/`BC1` are pulsed on Port B; then the value
goes out on Port A and `BDIR` is pulsed again.

```c
v_writePSG(reg, value);     /* one register write */
vpy_tone(period, volume);   /* channel A, a square wave */
```

Two invariants that are enforced in `uvm2_audio.c` and `uvm2_input.c`, both of
which were discovered the hard way:

* **`/RAMP` (Port B bit 7) is inside the constants, not at the call site.** Those
  bytes go out on Port B while Port A — which *is* the beam's DAC — carries the
  register number. With bit 7 clear the integrators run free with that garbage on
  the DAC: a bright segment from the origin, on *every* PSG access. Putting it in
  the `#define` means no call site can forget.
* **Bit 6 of register 7 stays zero.** On the AY-3-8912 that bit is port A's
  direction, and on a Vectrex that port is how the buttons are read. Setting it
  makes the controllers unreadable, and there is no legitimate reason for a
  Vectrex game to want that.

### Digitised samples (`.vsmp`)

The Vectrex has no audio DAC. A sample is played by **abusing a channel's 4-bit
volume register as one**: disable that channel's tone and noise, then write
amplitudes at the sample rate. It works at around 8 kHz.

The hard part is not the DAC, it is the bus. A sample write cannot happen
"whenever due" — during a ramp, Port A *is* the X integrator's rate. So samples
are **injected into the draw list**, in the gaps where the beam is already parked.

Those gaps exist, and were measured. Decoding a real stroke (`tools/uvm2_anatomy.c`)
gives a chain of 32-bus-cycle micro-segments:

```
T1CL = 8      wait 0
T1CH = 0      wait 11    <- the ramp runs here
PORT_A = 159  wait 3     <- already stopped: the next segment's Y rate
PORT_B = 0    wait 9     <- mux: sample Y
PORT_B = 1    wait 0
PORT_A = 97   wait 3     <- the next segment's X rate
```

Immediately before each `T1CL` the timer has expired, PB7 is high and the
integrators are frozen. A PSG write there moves nothing. It costs four commands,
and there is one opportunity every 32 cycles against the 187 that separate two
samples at 8 kHz — so there is always room, for about **2% of the bus**.

**The clock is the bus, not the wall.** A list is built one frame before it is
replayed, and building it costs ~7 ms of the 20 it lasts, so a wall-clock read
while emitting says nothing about when the sample will sound. A command's
position in time is its position in *bus cycles* inside the list, which
`uvm2_list_cycles()` already counts. Each voice's cursor therefore advances by
elapsed cycles, not one sample per emission. The pitch comes out exact even
though the opportunities fall unevenly — non-uniform sampling with the right
value at each instant, not jitter.

**One honest limit.** With free refresh (`UVM2_HZ=0`) there is no frame filler,
so if the builder is slower than the bus the executor idles, wall time passes
with no bus cycles, and the sample plays slow in that proportion. With
`UVM2_HZ=50`, bus time and wall time are the same by construction. A game that
wants faithful audio fixes its refresh. It is a trade you choose, not a fault you
discover.

`game/tacscan/` shows the whole path: `make snd` converts `samples/*.wav` into
`build/tacscan.vsm`, a bundle that **ships on the SD card** next to the `.um2`
(linking it in overflowed the loader's SRAM line and hardfaulted the console).
The name must stay 8.3 or the reader will not find it and the game is silent —
the generator checks.

---

## Input

```c
v_readButtons();            /* -> currentButtonState */
v_readJoystick1Analog();    /* -> currentJoy1X, currentJoy1Y  (-127..127) */
vpy_j1_x();  vpy_j1_y();  vpy_j1_button(n);   /* n = 1..4 */
```

Reads cannot be recorded into the command list (a read needs the data bus turned
around mid-cycle), so they run **between frames**, while `/ZERO` holds the beam
clamped at the centre. Under dual core they happen on core 1 as part of its loop,
and the syscalls the game calls answer from a cache — which they always did, so
nothing changes on the game side.

Even there they are not free. Measured on hardware: **with the input read removed
entirely, stray bright vectors dropped from 4-5 per frame to 1.** That is why
`/RAMP` is held asserted-off through the whole conversion, and why the constants
carry bit 7.

Two modes, chosen with `uvm2_input_set_analog()`:

* **digital** (default): drive the DAC to 0, let the comparator settle, probe once
  above and once below. Tells "pushed" from "centred", scaled to ±127 so ordinary
  `if (x > 32)` game code behaves as it does on a real cartridge.
* **analog**: successive approximation, the BIOS's `Joy_Analog` algorithm. Costs
  about seven extra read cycles per axis. **The sign bit is the first thing the
  comparator decides** — an earlier version started at 0x40 and never touched bit
  7, so it could only return 0..0x7F, a centred stick read ~64, and every game saw
  "hard right".

---

## The SD card

`uvm2_sd.c` is a bit-banged SPI driver plus a minimal FAT16/FAT32 reader *and
writer*. It exists because the stock firmware loads the `.um2` and steps aside —
it does not serve romsets or anything else.

```c
int      uvm2_sd_init(void);
uint32_t uvm2_sd_read(const char *path, unsigned char *dst, uint32_t max);
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from);
int      uvm2_sd_create(const char *path, const unsigned char *data, uint32_t n);
int      uvm2_sd_overwrite(const char *path, const unsigned char *data, uint32_t n);
int      uvm2_sd_write(const char *path, const unsigned char *data, uint32_t n);
```

Paths take one subdirectory, which is what `roms/<game>.zip` needs. Names are 8.3,
upper-cased, compared case-insensitively on the base name.

Things worth knowing:

* **There is no card-detect pin.** The firmware configures none, so "no card" and
  "did not start" are told apart by `uvm2_sd_error`, not by a GPIO. An earlier
  version read GP38 — an unowned pad with an internal pull-down that always
  returns 0, i.e. "no card" no matter what. *A diagnostic that cannot fail
  diagnoses nothing.*
* **The pins were measured, not read.** The schematic reading was shifted by two
  with CS on a different pin entirely, and the symptom was a card that never
  answered. They were recovered by dumping `IO_BANK0` over SWD while the firmware
  sat in its own menu with the SD up.
* **Mounting checks a real BPB**, not "the two bytes I looked at are non-zero":
  jump opcode, 512 bytes/sector, and a power-of-two sectors/cluster. Sector 0 on
  these cards is an MBR, and an MBR's boot code can hold anything at offsets 11
  and 13 — a volume invented from those fails later with `MISSING`, which looks
  exactly like a file that is not there.
* **Writing updates every FAT copy and the FAT32 FSInfo.** A FAT updated in one
  copy is a card the PC calls corrupt, and a stale FSInfo makes `fsck_msdos`
  complain. The write path was validated against real FAT16 and FAT32 images made
  with `newfs_msdos`, with `fsck_msdos -n` clean afterwards.
* **Data before directory entry.** If a write is cut off half way, what is left
  is clusters in use with nothing pointing at them — lost space a `chkdsk`
  reclaims — rather than an entry pointing at garbage, which the PC reads as a
  corrupt file.

### Romsets

`uvm2_romzip.c` reads `roms/<game>.zip` into a buffer and publishes an `'RMZ1'`
descriptor; `third_party/aae/romzip.c` unzips members by name (junzip + puff) and
**verifies the CRC-32 the zip itself carries**, so a wrong ROM says so instead of
breaking half way through a game.

The buffer size is derived from the zip's own size at build time, not written by
hand in every Makefile. If the romset is missing or too big, the game paints the
failure and the path it tried, and keeps painting it — it does not emulate over
zeros, which draws nothing and is indistinguishable from a hang.
