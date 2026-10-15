# 13 — Over USB: upload, run, debug

The UVMC2's USB port is enough to put a game on the SD card, start it and debug it
on the console: no card reader, no SWD probe, no IDE. Everything here comes with the
SDK (`sdk/tools/uvmc2.py`, `sdk/uvm2-sdk/monitor`, `sdk/uvm2-sdk/sdbridge`,
`sdk/uvm2-sdk/prebuilt`); Vectrex Studio's IDE drives the same pieces from its menus
(Build → Upload to Cartridge, Debug → Debug on Cartridge).

Needs **picotool** (macOS: `brew install picotool`) and Python 3. macOS and Linux;
not Windows yet.

## The quick version

```sh
cd examples/hello_uvmc2
make uvm2 UVM2_USB_MONITOR=1                       # a build that carries the monitor
../../sdk/tools/uvmc2.py deploy build_uvm2/hello_uvmc2.um2
```

`deploy` writes the `.um2` to the card (with `MONITOR.BIN` beside it), checks its
CRC-32, and starts it — about 5 seconds for a 350 KB game. The cartridge can be in
the multicart's menu or running another game built with the monitor. The game also
stays on the card, for the menu.

| command | does |
|---|---|
| `uvmc2.py deploy game.um2` | onto the card and running |
| `uvmc2.py upload FILE [PATH]` | any file onto the card (a romset: `upload tacscan.zip roms/tacscan.zip`) |
| `uvmc2.py run game.elf` | run an image from SRAM, nothing written to the card |
| `uvmc2.py log` | the running game's `DEBUG_PRINT` / `vpy_log` text |
| `uvmc2.py gdb [game.elf]` | the port gdb attaches to, and the command |
| `uvmc2.py ports` | what is on USB |

## The two programs that answer on USB

The multicart's firmware drops its USB port when it starts a game, so something else
has to answer. Two programs do, with one protocol and one USB id (2E8A:10DB):

**The debug monitor** (`MONITOR.BIN` on the card's root) — inside a game built with
`UVM2_USB_MONITOR=1`. At start-up the game reads it off the card into the PSRAM and
runs it from there, once a frame, on the game's core: TinyUSB with a serial port is
about 20 KB, which a big game has not got in SRAM. A monitor build costs the game
about 2 KB of SRAM and nothing in speed that we could measure. If `MONITOR.BIN` is
not on the card the game simply runs without it.

**The SD bridge** — for when no game is running: from the multicart's menu, picotool
sends the chip to the bootrom's BOOTSEL mode (the firmware has the pico-sdk's USB
reset interface) and runs the bridge from SRAM. The flash, Ralf's firmware, is never
written. The bridge writes files to the card and shows the upload on the console:
the turning Vectrex Studio logo, the file's name, a bar. When it is done, the game is
loaded into SRAM through BOOTSEL and started.

The monitor shows the same screen while it takes an upload, in place of the game's
picture.

Both are prebuilt in `sdk/uvm2-sdk/prebuilt/`; `uvmc2.py` puts `MONITOR.BIN` on the
card with every upload, so a monitor build always finds the current one.

## Debugging with gdb

The monitor has a **second USB serial port** that speaks gdb's remote protocol. No
server, no probe:

```sh
make uvm2 UVM2_USB_MONITOR=1 UVM2_DEBUG_CFLAGS=-Og    # -Og: lines and locals as written
../../sdk/tools/uvmc2.py deploy build_uvm2/hello_uvmc2.um2
arm-none-eabi-gdb build_uvm2/pico/hello_uvmc2.elf -ex "target remote $(../../sdk/tools/uvmc2.py gdb)"
```

Attaching pauses the game. `break`, `continue`, `step`, `next`, `finish`, `bt`,
`print`, `set var`, `info registers`, Ctrl-C and `detach` work; watchpoints do not.
While the game is held the console shows its last picture with a pause sign in the
top right corner. `detach` (or quitting gdb) takes every breakpoint out and lets the
game run.

How: the Cortex-M33's **monitor-mode debug**. A breakpoint is a `BKPT` the monitor
writes over the game's code (a `.um2` runs from SRAM) and takes out again; with
`DEMCR.MON_EN` set it raises the DebugMonitor exception instead of halting the core,
and the monitor's handler waits there while the USB interrupt, at a higher priority,
goes on answering gdb. Core 1 keeps redrawing the last picture.

Limits:

* **Breakpoints work from the game's first frame on.** The monitor starts at the end
  of the first frame; a line that only runs before it (early in `main()`) never
  stops — and a `BKPT` there would fault.
* **Only the game's core** (core 0). Core 1 runs the SDK's replay.
* **One debugger at a time**: gdb, or the IDE's Debug on Cartridge.

## The game's text

`DEBUG_PRINT` (VPy) and `vpy_log("x %d\n", x)` (C) write to an RTT ring; in a
monitor build the monitor sends it over USB. `uvmc2.py log` prints it; the IDE shows
it in its build output as `[rtt] …`. Writing never waits: with nobody reading, the
text is dropped and the game runs the same. In a `.um2` the ring lives in the PSRAM
(4 KB), not in SRAM.

## Where things are in the PSRAM

The monitor uses a 256 KB window at `0x117C0000` (uncached alias `0x157C0000`):

| at | what |
|---|---|
| `0x157C0000` | `MONITOR.BIN`, its data and bss (~40 KB) |
| `0x157E0000` | the reset button's copy of `.data` ([06](06-sound-input-sd.md)) |
| `0x157F0000` | the RTT ring (4 KB) |
| `0x157F1000` | the pause sign's commands |

A game that uses the PSRAM itself should keep out of it.

## The protocol

Text lines, answers ending in CR LF, lines the monitor sends unasked starting with
`!`. The upload and run commands are listed in `sdk/tools/uvmc2.py`; the debugger's
(`Z z H c s R P Q D`, events `!S`) in `sdk/uvm2-sdk/monitor/debug.c`; gdb's port in
`monitor/gdb.c`.
