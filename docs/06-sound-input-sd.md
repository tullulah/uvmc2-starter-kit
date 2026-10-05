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
The name stays 8.3 — this kit's reader takes long names since it moved to FatFs,
but an 8.3-only reader finds nothing and the game is silent. The generator checks.

### The 16-bit jack, and how to give it voices

The UVMC2 has its own audio output: a PT8211 16-bit DAC on a jack on the
cartridge (GPIO43-45, driven by PIO2 and one DMA channel). It has nothing to do
with the Vectrex's bus or its sound chip, so it costs the drawing nothing.

`uvm2_jack.h` is a **driver, not a sound engine**. It takes mono 16-bit samples at
`UVM2_JACK_RATE` (32 kHz) and gets them out. It has no voices, no mixer and no
files. A game that wants several sounds at once mixes them itself, and that is a
few dozen lines:

```c
#include "uvm2_jack.h"

/* One voice = one sound playing: where its samples are, how far it has got, how
 * loud. Samples here are already 16-bit at 32 kHz; see "resampling" below. */
typedef struct {
    const int16_t *pcm;      /* the sound */
    uint32_t       len;      /* in samples */
    uint32_t       pos;
    int            gain;     /* 0..256, 256 = full */
    int            live, loop;
} voice_t;

#define VOICES 4
static voice_t s_voice[VOICES];
static int     s_jack;       /* 0 = no jack on this board */

void jack_start(void) { s_jack = uvm2_jack_init(); }

void jack_play(int v, const int16_t *pcm, uint32_t len, int gain, int loop)
{
    s_voice[v] = (voice_t){ pcm, len, 0, gain, 1, loop };   /* restarts that voice */
}

static int16_t mix_one(void)
{
    int32_t acc = 0;
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (!v->live) continue;
        acc += ((int32_t)v->pcm[v->pos] * v->gain) >> 9;    /* >> 9: half, see headroom */
        if (++v->pos >= v->len) { if (v->loop) v->pos = 0; else v->live = 0; }
    }
    if (acc >  32767) acc =  32767;                         /* clamp, never wrap */
    if (acc < -32768) acc = -32768;
    return (int16_t)acc;
}

/* Once per game frame. The driver keeps about 50 ms queued and says how many
 * samples it wants to get back there. */
void jack_update(void)
{
    if (!s_jack) return;
    int n = uvm2_jack_space();
    while (n > 0) {
        int16_t buf[128];
        const int k = n > 128 ? 128 : n;
        for (int i = 0; i < k; i++) buf[i] = mix_one();
        uvm2_jack_write(buf, k);
        n -= k;
    }
}
```

Adding a voice is adding one to `VOICES`. The rules that decide whether it sounds
right are elsewhere:

* **Headroom.** Voices add up. Four at full scale clip on the first loud moment,
  and a 16-bit value that wraps is a crack, not distortion. So scale each voice
  down (`>> 9` above is half) and **clamp**, never let it wrap. One game keeps its
  music at three quarters and each effect at its own gain, and *ducks* the music to
  an eighth while a spoken line plays, because a word under music at three
  quarters is unintelligible.
* **One voice per class of sound, not per sound.** Give each kind of sound its own
  voice (footsteps, impacts, speech, music) and let a new sound of the same kind
  restart that voice. Two footsteps at once are a stumble, and a footstep landing
  on the speech voice cuts a word in half. This decides more about how a game
  sounds than the number of voices does.
* **Call `jack_update()` every frame, and keep frames under ~50 ms.** The ring is
  64 ms and the driver keeps 50 ms queued. A frame longer than that drains it. The
  driver re-syncs rather than playing stale data, and you hear a click.
* **Where the samples live.** SRAM is what the game draws with, so a few seconds of
  32 kHz audio (64 KB per second) do not belong there. Load them from the SD card
  into PSRAM (`uvm2_psram_init`, then the XIP window at `0x11000000`), in slices
  across frames with `uvm2_sd_open`/`uvm2_sd_next`, so that loading never stalls
  a frame. Storing them as IMA ADPCM (4 bits per sample) and decoding in the
  mixer divides the space by four.
* **Resampling.** A source that is not 32 kHz needs a fractional step (a 16.16
  position advanced by `src_rate / 32000` per output sample) instead of `pos++`.
  Otherwise it plays at the wrong pitch.
* **No jack, no sound.** `uvm2_jack_init()` returns 0 on a board without the jack
  (the debug cartridge). A game that must run on both falls back to the PSG or to
  the `.vsmp` voices above. The per-game `audio` setting (`UVM2_SETTING_AUDIO`,
  0 = jack, 1 = console chip) lets the player choose, and the game reads it.
* **The pitch is only as good as the clock.** The driver sets its divider from the
  system clock the SDK measures against the Vectrex's E at boot. If a jack game
  sounds consistently sharp or flat, that measurement is the first suspect.

### The worked example: Tac/Scan's two paths

`game/tacscan/` plays the same 22 sounds either way, and the player chooses on the
`AUDIO` line of the menu. It is worth reading because it is the whole shape of the
problem, not a snippet:

| | console | jack |
|---|---|---|
| bundle | `TACSCAN.VSM`, 242 KB | `TACSCAN.PCM`, 2.64 MB |
| format | 4-bit, 12 kHz | **16-bit, 32 kHz** |
| how it reaches the ear | the PSG's volume register, written in the gaps of the draw list | a PT8211, off the Vectrex entirely |
| where it lives | SRAM | PSRAM, loaded at startup |
| code | `uvm2_smp.*` (SDK) | `src/ts_jack.c` (the game) |

* **The container is shared, and that is the point.** Both games that drive the jack
  use one:

  ```
  "KSFX" | u16 n | u16 - | n x (u32 offset, u32 samples, u32 hz, u16 format, u16 -) | data
  ```

  `format` is the extension point: **0 is 16-bit linear, 1 is IMA ADPCM at four
  bits**. Tac/Scan ships format 0, because not quantising is the whole reason for
  using the jack; ADPCM is there for when 2.64 MB of load is worth trading for a
  quarter of the size. A second container would have meant two readers and two
  generators to keep in step for nothing.
* **A format the reader cannot decode is refused, not played.** ADPCM nibbles read as
  16-bit samples are full-scale noise, and this comes out of a line output into
  somebody's amplifier. Silence is the right answer to a bundle you do not
  understand.
* **The gains live in one place.** `tools/wav_to_pcm.py` imports `MIX` and
  `apply_gain` from `tools/wav_to_vsmp.py`, so the balance between sounds — the ship's
  roar ducked to a half so the lasers carry over it — is one decision with two
  outputs. Copy the numbers into both and the game will sound different depending on
  which output you chose, which is not what the setting is for.
* **Ten voices, and the number comes from the game.** AAE addresses voices by fixed
  id and Tac/Scan's go up to 9. The console path was written with four once, and the
  result on hardware was that the shots sounded and the engines did not: everything
  from voice 4 up was dropped by a range check, silently.
* **The headroom is one shift, and the content chose it.** The generator prints each
  sound's median `|sample|`: they run 3573..8736 of 32767, i.e. 11% to 27%. Three
  voices at their medians sum to about half of full scale with a shift of one, so
  nothing clamps in normal play and a single sound still comes out at half scale.
  Measured on the host with `make jack-preview`: four voices at once clamp **6 samples
  out of 24000**. Peaks clamping is the right trade — a clamp is a flattened
  transient, a wrap is a crack.
* **The load happens while the game runs, and there is no loading screen.** 2.64 MB
  off the card is not free, but nothing needs to wait for it: one 8 KB slice
  per frame from the game loop, and a sound plays as soon as **its own** bytes have
  arrived (`ts_jack_avail`). Until then that one sound goes out of the console's path,
  so the first seconds sound like a game rather than like silence. Measured: 322
  frames (~8 s at 40 Hz) for the last sound, 45 (~1.1 s) for the ship's roar — which
  is first in `samples.json`, so what the attract mode reaches first is what lands
  first. **THOSE TWO NUMBERS CAME FROM THE HOST, NOT FROM A CONSOLE**, and this text
  claimed otherwise: `make jack-preview` runs the mixer against a fake card that
  answers instantly, so they were a floor nobody could reach, not a "before".

  MEASURED ON THE CONSOLE (2026-09-29, TSJACK.LOG, SPI0 at 12.5 MHz): the 2.64 MB
  take **13.5 s** over 516 passes, one per frame, err 0. Inside its 6 ms window the
  loader gets 698 KB/s; across the whole load it averages 195 KB/s, because 6 ms of
  a 25 ms frame is a 24% duty cycle. **The card stopped being the bottleneck** — the
  budget is. And 698 against the 1307 KB/s the same driver gives on raw blocks is
  the cost of FatFs (two sectors and a copy per 1024-byte chunk) plus writing into
  PSRAM through the uncached window: a 47% tax, and the place to look next. It runs
  **only if the player chose the jack**, which is decidable because the
  menu is the boot wizard and has already closed by then.
* **`make jack-preview`** builds the real mixer for the desktop against the real
  bundle and writes `build/jack_preview.wav`. A mixer whose headroom and looping are
  only ever exercised by flashing a card is one nobody has actually checked. It also
  checks the partial case, which is the normal one now: after one slice the table
  reads but no sound is playable yet.
* **The menu is the SDK's boot wizard**, opened by holding buttons 2 and 3 and
  launching with 4 — there is no in-game gesture and nothing opens on power-up. For
  the game's own settings to appear in it, they have to be declared before `main`,
  which is what `uvm2_game_settings()` is for.

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

**One read, two shapes.** Every axis is read with successive approximation, the
BIOS's `Joy_Analog` algorithm (**the sign bit is the first thing the comparator
decides** — an earlier version started at 0x40 and never touched bit 7, so it could
only return 0..0x7F, a centred stick read ~64, and every game saw "hard right").
The stick's rest is **not** zero — measured X 4..12, Y 31..48 on one console — so
the first reading is taken as the centre and subtracted from every one after.
`uvm2_input_set_analog()` picks the shape:

* **digital** (default): -127 / 0 / +127, past half the travel, so ordinary
  `if (x > 32)` game code behaves as it does on a real cartridge.
* **analog**: the centred value, 0 inside a small dead zone.

There used to be a separate digital read that probed the comparator with the DAC
at ±64. It left a **dot** on the tube: with the brightness up, a spot on the
diagonal that followed the stick, in every game and in the BIOS menu, and none in
Minestorm. Bisected on the console (2026-10-02) by switching parts of the read off
over SWD: the probe was the cause, zeroing the DAC afterwards did not cure it, and
the analog read leaves nothing.

---

### The console's reset button

**Off by default since 2026-10-05; a game opts in with `-DUVM2_RESET_BUTTON`.** The
watch misfires: on the UVMC2 it restarted dkong, the playroom and Major Havoc (on a
press of button 4) with nobody near the button, and each false press also stops the
controllers being read for two frames — controls that respond on and off. Off, it
costs nothing: no bus reads, no warm-start mark, no 4 KB `.data` copy. What follows
is what it does when a game turns it on.

A `.um2` built with it watches the Vectrex's own **reset** button:

| held | does |
|---|---|
| under 1 s | nothing — a brush against the button costs nothing |
| 1 to 3 s, then let go | **the game starts again** from scratch |
| 3 s | **back to the UVMC2's menu**, at once |

**How it sees the button.** The reset is not wired to the cartridge (on the
UVMC2's schematic its one button, SW1, is the RP2350's BOOTSEL, and `/RUN` has
only a pull-up). It does reach the VIA, and a VIA in reset stops its timers. So
between frames core 1 reads T1's high byte twice, 300 bus cycles apart: if it did
not move, the button is down. The idea is Ralf's, from his own UVMC2 games.

**While it is down, the controllers are not read**, nor for two frames after: a VIA
in reset reads back zeros, which on the active-low buttons is "all pressed" — a
short press used to fire buttons 2 and 3 for a frame or two. The game keeps the
last good reading, and the next frame's list programs the VIA again.

**Back to the menu** is `rom_reboot(BOOT_TYPE_NORMAL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS)`,
as Ralf's games do. For the firmware to come back the short way ("loading", then
its menu) and not through the console's logo, the Vectrex BIOS must find its
warm-start mark, `Vec_Cold_Flag` = `$7321` at `$CBFE`: the SDK writes it as core 1
starts, with the 6809 halted (`uvm2_mem_write`). Without it the console
cold-started on the way back — confirmed on the UVMC2, 2026-10-03.

**A restart** boots the image that is still in SRAM again, through the bootrom
(`BOOT_TYPE_RAM_IMAGE`, the way the launcher started it). A `no_flash` image keeps
its initial values in place, so `.data` is copied aside at the top of `main` and
put back before the reboot; `.bss` and the stacks are cleared by the start-up. A
`.data` over 4 KB disables the restart and counts it in `uvm2_restart_refused`.

Over SWD: `uvm2_reset_seen` counts the presses noticed, `uvm2_reset_held_us` is how
long the current one has lasted — so "it never reboots" can be told from "it never
saw the button". Not on the debug cartridge, whose BIOS has its own way back to
its menu (all four buttons held a second).

## The SD card

`uvm2_sd.c` drives the card over the RP2350's **SPI0 peripheral** (GPIO34 SCK,
35 MOSI, 36 MISO; CS is a plain GPIO, 39 on this board) with **FatFs**
(`sdk/third_party/fatfs`) on top: FAT12/16/32 and exFAT, long names, MBR or GPT,
reading and writing. It exists because the stock firmware loads the `.um2` and
steps aside — it does not serve romsets or anything else.

It was bit-banged until 2026-09-29, at ~300 KB/s — 27 seconds to fill the 8 MB of
PSRAM. The peripheral runs at 12.5 MHz, and the clock is deliberately **half** the
25 MHz the SD spec allows in SPI mode: nothing here checks the CRC, so a corrupt
byte would arrive silently and look like a corrupt file system. Check the CRC16
first, then raise `UVM2_SD_BAUD_FAST`. `uvm2_sd_diag.baud` reports the clock the
divider actually produced, which is not what was asked for. Building with
`-DUVM2_SD_BITBANG` brings the old transport back over the same pins, so a card
that reads one way and not the other tells you the speed is the fault.

```c
int      uvm2_sd_init(void);
uint32_t uvm2_sd_read(const char *path, unsigned char *dst, uint32_t max);
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from);
int      uvm2_sd_create(const char *path, const unsigned char *data, uint32_t n);
int      uvm2_sd_overwrite(const char *path, const unsigned char *data, uint32_t n);
int      uvm2_sd_write(const char *path, const unsigned char *data, uint32_t n);
```

Paths are relative to the root, any depth, long names allowed, compared
case-insensitively. `uvm2_sd_create` and `uvm2_sd_write` create missing folders
and replace an existing file.

For a large file read in slices (audio, captures), open it once and keep reading:

```c
typedef struct { uint32_t cluster, sec, pos, len; int ok; /* ... */ } uvm2_sd_file;
int      uvm2_sd_open (const char *path, uvm2_sd_file *f);   /* 1 = found */
uint32_t uvm2_sd_next (uvm2_sd_file *f, unsigned char *dst, uint32_t max);   /* 0 = end */
void     uvm2_sd_close(uvm2_sd_file *f);                     /* optional */
```

`uvm2_sd_read_from` re-mounts and seeks from the start of the file on every
call, which for a large file is O(n²) (loading 1.8 MB of audio took minutes).
`uvm2_sd_next` carries on from where the last slice ended.

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
* **exFAT is why this is FatFs.** Until 2026-09-28 the kit had its own
  FAT16/32 code. Large cards come formatted exFAT, the stock menu reads exFAT, so
  such a card started the `.um2` and then every file the game asked for failed
  with `NO_FAT` — reported by a user as arcade ports that "worked for basic
  functions" until they needed their romset.
* **Every call mounts afresh**, re-initialising the card: with no detect pin that
  is the only way a swapped card is noticed. **Except while a file is open**
  through `uvm2_sd_open`, because a remount would invalidate it. So a game that
  streams can still read or save other files in between; `uvm2_sd_diag.streams`
  says how many are open.
* **`uvm2_sd_diag` says what was mounted** (`fs_type`: 2 FAT16, 3 FAT32, 4 exFAT),
  the raw FatFs `fresult` behind the last error, and `reads`/`writes` block
  counters — if those do not move, the card was never touched.
* **`IO_ERROR` is its own code.** A block that fails mid-file used to return a
  short count with no error at all; now it says so.
* **Tested against real images**: `sdk/uvm2-sdk/tools/uvm2_sd_test.sh` makes
  FAT16, FAT32 and exFAT (MBR and GPT) images with macOS's own tools, reads and
  writes them through `uvm2_sd.c`, and requires `fsck -n` to come out clean and
  the Mac to read back what was written.

* **The calibration lives on the card** (`config/uvm2.cfg`) and is written by
  the calibration screen through `uvm2_sd_create`/`uvm2_sd_overwrite`: see
  [12](12-calibrating-a-console.md).
* **FatFs is compiled with `-Os`**, like the rest of the cold code (start-up,
  calibration, PSRAM bring-up). At `-O3` it is 19.9 KB, and that cost two ports
  their last 13 KB of SRAM.

### Romsets

`uvm2_romzip.c` reads `roms/<game>.zip` into a buffer and publishes an `'RMZ1'`
descriptor; `third_party/aae/romzip.c` unzips members by name (junzip + puff) and
**verifies the CRC-32 the zip itself carries**, so a wrong ROM says so instead of
breaking half way through a game.

The buffer size is derived from the zip's own size at build time, not written by
hand in every Makefile. If the romset is missing or too big, the game paints the
failure and the path it tried, and keeps painting it — it does not emulate over
zeros, which draws nothing and is indistinguishable from a hang.
