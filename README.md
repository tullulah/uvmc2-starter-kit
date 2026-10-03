# UVMC2 Starter Kit

Everything needed to write, build and run a game on the **Ultimate Vectrex
Multicart 2** (UVMC2 / UVM2) — an RP2350 cartridge for the Vectrex — with no
dependency on anything outside this directory.

The kit contains:

* the **SDK** that draws on the Vectrex from an RP2350: the command list, the
  beam model, dual core, the PIO + DMA bus stream, sound, input and an SD reader;
* a **minimal example game** (`examples/hello_uvmc2/`) to copy;
* a **full worked example** (`game/tacscan/`): a Sega G80 arcade game emulated in
  C, with its romset, its sampled sound and its host test harness;
* **documentation** (`docs/`) explaining why each piece is the way it is.

Every comment in this kit is in English, and every number that looks like a magic
constant has a note next to it saying where it was measured.

---

## 1. What you need

All five are **required** — none of them is optional, including Rust.

| tool | minimum | verified with | why |
|---|---|---|---|
| **Arm GNU Toolchain** | any version with `nosys.specs` | **15.2.rel1** (gcc 15.2.1) | the cross compiler. Homebrew's `arm-none-eabi-gcc` does **not** ship `nosys.specs` and the pico-sdk link needs it — use the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) installer (bare-metal AArch32, `arm-none-eabi`) |
| **CMake** | 3.13 | **4.2.0** | pico-sdk 2.2.0 declares `cmake_minimum_required(3.13...3.27)`, so CMake 4.x is fine |
| **Python 3** | 3.6 | **3.14.5** | writes the 20-byte `.um2` header (`sdk/tools/package_um2.py`, stdlib only) |
| **Rust / cargo** | **1.78** | **1.91.1** (stable) | the beam model and the bus stream are two `no_std` crates and **every build links them**. 1.78 is the floor because the `Cargo.lock` files are format v4; the crates are edition 2021 |
| **git** | any | — | `setup.sh` clones the pico-sdk |

Plus a **network connection for the first build**: the crates are not vendored,
so cargo fetches `pio`/`pio-proc` and their dependencies from crates.io once.
After that the build is offline.

### Rust is not optional

`uvm2_pico.cmake` links `libvectrex_draw_cabi.a` unconditionally and stops with
`FATAL_ERROR` if `cargo` is not on the PATH. `UVM2_PIO_STREAM=0` only drops the
crate's `bus` feature — the crate still builds and still links, because the beam
model itself is Rust. There is exactly one implementation of it and it is shared
with another cartridge's firmware; that is the point.

```sh
rustup target add thumbv8m.main-none-eabi     # softfp bare-metal Cortex-M33
```

### Optional, per target

| tool | needed for |
|---|---|
| **ffmpeg** | `make snd` in `game/tacscan` — decoding `samples/*.wav` into the `.vsm` bundle. The bundle ships pre-built, so you only need this to regenerate it. |
| **emscripten** (emsdk) | `make sim` — the WASM build for a browser harness. Not needed for the cartridge. |
| a host C compiler | `make host` / `make host-prof` — the desktop test harness. Any `cc`. |
| **probe-rs** (verified 0.31.0) + a Pico running **Debugprobe** (verified 2.3.1) | reading `uvm2_stats`, live knobs and the command list over SWD on real hardware (`sdk/uvm2-sdk/tools/`); how to turn a Pico into the probe, install probe-rs and wire it: `docs/07-measuring.md`, "Setting up the probe, from nothing". `arm-none-eabi-gdb` too for `probe.sh` / `load.sh`. |

## 2. Set up

```sh
git clone --recursive https://github.com/tullulah/uvmc2-starter-kit.git
cd uvmc2-starter-kit
./setup.sh
```

`sdk/` is a **git submodule**: the SDK is its own repository,
[uvmc2-sdk](https://github.com/tullulah/uvmc2-sdk), shared with every other
project that builds for the UVMC2. `setup.sh` fetches it if you cloned without
`--recursive`. Changes to the SDK are made and committed **there**, then picked
up here by moving the submodule.

It checks the tools, adds the Rust bare-metal target, and shallow-clones
**pico-sdk 2.2.0** into `third_party/pico-sdk` (it is not vendored: with its
submodules it is 670 MB, and none of them are used here). If you already have a
pico-sdk 2.2.0 checkout, point `PICO_SDK_PATH` at it instead.

## 3. Build

```sh
cd examples/hello_uvmc2 && make uvm2     # -> build_uvm2/hello_uvmc2.um2   (~47 KB)
cd game/tacscan        && make uvm2      # -> build_uvm2/aae_tacscan.um2   (~143 KB)
```

## 4. Run it

### The SD card

FAT32 or exFAT, any size. The multicart's menu and the games both read either.
Games built before 2026-09-28 read FAT32 only; on an exFAT card they start and
then cannot find their romset, so rebuild them.

```
/                      the .um2 images, and any sound bundle a game ships (tacscan.vsm)
roms/<game>.zip        romsets, read by the game itself at start-up
config/uvm2.cfg        this CONSOLE's beam calibration (written by the wizard)
config/<GAME>.CFG      per-game settings, for games that declare any
```

`config/` is created the first time a calibration is saved. Do not copy a
`config/uvm2.cfg` from another console: it describes that console's analog parts,
not yours. See [12](docs/12-calibrating-a-console.md).

### A game

Copy the `.um2` to the root of the card and pick it from the multicart menu.
For `tacscan` also copy:

* `game/tacscan/roms/tacscan.zip` → `roms/tacscan.zip` on the card
  (the game reads its ROM off the card at startup — it is not inside the image);
* `game/tacscan/build/tacscan.vsm` → next to the `.um2`, for the sampled sound.
  It ships pre-built; `make snd` regenerates it from `samples/*.wav` and needs
  `ffmpeg` on the PATH. Keep the name 8.3: this kit reads long names, but not
  every cartridge's reader does.

If a romset is missing the game says so on screen, with the path it tried. It
does not fail silently.

### Calibrating the console

If text leans into diagonals, columns cascade to one side, or glyphs fall apart,
while native cartridges look fine on the same console, the console needs
calibrating. It is not a broken image. **Hold buttons 2 and 3 and launch any game
with 4**: the calibration screen opens first. Button 4 saves and starts the game.
Every game built with this SDK then uses that calibration.
[docs/12](docs/12-calibrating-a-console.md) explains the screen.

---

## 5. Layout

```
setup.sh                   tool check + pico-sdk clone
CLAUDE.md                  orientation for an AI assistant working in this tree
docs/                      how it all works, and why
examples/hello_uvmc2/      the smallest complete game — START HERE
game/tacscan/              the full worked example (arcade emulation)
sdk/                       SUBMODULE: github.com/tullulah/uvmc2-sdk
  uvm2-sdk/                the UVM2 runtime: bus, draw, dual core, sound, SD, syscalls
    uvm2.mk                the build rule a game includes
    pico/ + uvm2_pico.cmake  the pico-sdk build behind it
    tools/                 host-side measurement tools (they are not part of a build)
  rp2350-sdk/              the game-facing backend (v_directDraw32 &c.) + linker scripts
  vectrex-draw/            the beam model, in Rust; shared with another cartridge
  vectrex-bus/             the PIO + DMA bus stream, in Rust
  vpy-c/                   libvpy: a small game library (shapes, text, input, sound)
  pitrex-sim/              the host/simulator side of the same API
  tools/package_um2.py     wraps a .bin in the 20-byte .um2 header
  third_party/fatfs/       ChaN's FatFs: the SD card's file system (FAT + exFAT)
third_party/
  aae/                     the arcade emulator sources tacscan is built on
  pico-sdk/                cloned by setup.sh
```

## 6. Starting your own game

Copy `examples/hello_uvmc2/` anywhere inside the kit, rename it, and edit two
lines of its `Makefile` (`UVM2_NAME`, `UVM2_SRCS`). That is the whole ceremony:
`sdk/uvm2-sdk/uvm2.mk` supplies the rest.

Read `docs/02-assembling-a-project.md` next, then `docs/04-drawing.md` — the beam
is the thing that will surprise you, not the CPU.

To **port an arcade game** instead, start at `docs/11-porting-from-mame.md` (it
decides which kind of port yours is) and then `docs/10-porting-an-aae-game.md`.

`docs/09-source-map.md` is the index from "I want to know X" to the one file that
answers it, and `docs/08-api-reference.md` is the complete API.

## 7. Known state

* **The beam defaults were measured on one console.** Any other console should be
  calibrated once ([12](docs/12-calibrating-a-console.md)). The zero reference's
  default (`0x23`, from Vectorblade) was never meant to be universal. Which value
  fits most consoles is still an open question, and the calibration exists so
  that it does not have to be answered first.

* `cargo test` in `sdk/vectrex-draw` passes, with **one test ignored on
  purpose**: `emit::pentagon::ramp_error_has_no_bias`. It finds a real bias of
  +0.077 units per stroke in `ramp_params`, and the fix would move geometry, so it
  waits for a measurement on the console. `cargo test -- --ignored` runs it. Its
  comment has the suspected cause.
* `sdk/uvm2-sdk/tools/*.c` are host-side measurement tools, not part of any
  build. `stats.py`, `probe.sh`, `load.sh` and `release.sh` there talk to a
  console over SWD. They are compiled by hand; each one's header comment gives the command
  line.
