# 09 — Source map

What lives where, one line each, and — at the end — an index from *question* to
*file*. The point of this page is to make reading the whole tree unnecessary.

Sizes are rounded; they tell you whether a file is a glance or a session.

---

## `sdk/uvm2-sdk/` — the runtime inside the `.um2`

| file | ~lines | what it is |
|---|---|---|
| `uvm2.mk` | 350 | **The build rule a game includes.** Every knob, with its rationale. Read this before anything else in the SDK. |
| `uvm2_pico.cmake` | 330 | The CMake behind it: SDK sources, the Rust crates, flags, the `.um2` packaging step. |
| `pico/CMakeLists.txt` | 30 | Thin wrapper; the content is in the `.cmake`. |
| `uvm2_bus.h` | 430 | **The bus contract.** Command encoding, the GPIO map, VIA register and bit names, `uvm2_stats_t`, and the `#error` that enforces dual core + PIO. |
| `uvm2_bus.c` | 600 | The transport: bring-up, `uvm2_exec`, single accesses, E-period measurement. |
| `uvm2_draw.c` | 3300 | **The largest file in the kit.** Turns strokes into commands: splitting, re-zeroing, calibration, the frame double buffer, the pacer, sample injection points. |
| `uvm2_draw.h` | 160 | Its public face plus the runtime knobs. |
| `uvm2_core1.c` | 400 | The second core: replay, input, PSG queue, pacing. |
| `uvm2_pico_main.c` | 150 | The seam between the pico-sdk runtime and the game: what runs, in what order, before `main`. |
| `uvm2_svc.c` | 500 | The `svc` ABI: every syscall number and what it does. |
| `uvm2_svc_entry.s` | 40 | The handler's entry stub (reads the stacked exception frame). |
| `uvm2_start.s` | 100 | The hand-written vector table + IMAGE_DEF. **Documentation, not the live path** — see [02](02-assembling-a-project.md). |
| `uvm2_input.c/.h` | 240 | Buttons, joysticks (digital and SAR analog), PSG access. |
| `uvm2_audio.c/.h` | 230 | `.vmus` / `.vsfx` sequencers, and the PSG mixer shadow. |
| `uvm2_smp.c/.h` | 580+230 | Digitised samples through the volume DAC. **The `.h` carries the whole model**; read it, not the `.c`. |
| `uvm2_sd.c/.h` | 810+70 | Bit-banged SPI + a FAT16/32 reader *and writer*. |
| `uvm2_romzip.c` | 100 | Reads `roms/<game>.zip` off the card and publishes the `'RMZ1'` descriptor. |
| `uvm2_psram.c/.h` | 1150+220 | Brings up the 8 MB on CS1, plus three diagnostic probes. |
| `uvm2_config.c/.h` | 370+145 | Per-**console** beam calibration, on the SD card. The `.h` explains every field. |
| `uvm2_wizard.c` | 280 | The on-screen calibration screen. |
| `uvm2_text.c/.h`, `uvm2_font.h` | 170 | A 4×6 stroke font, ASCII 32..90, and `uvm2_print_text`. |
| `uvm2_led.c/.h` | 155 | The status LED — the bring-up channel of last resort — and clock calibration. |
| `uvm2_bus_stream.h` | 46 | The C face of the Rust bus crate. No implementation here. |
| `memmap_psram.ld` | 350 | Opt-in: link the whole image into PSRAM. Its comments are the best account of what does and does not belong in external memory. |
| `tools/*.c`, `tools/*.py` | ~1500 | Host-side measurement tools. Not part of any build. See [07](07-measuring.md). |
| `tools/stats.py`, `probe.sh`, `load.sh`, `release.sh` | 380 | **SWD tools** for a console on the bench: read `uvm2_stats` without halting, the PC of a hang, load an image without the SD card. Which ones halt the core is in [07](07-measuring.md). |

## `sdk/rp2350-sdk/` — the game-facing backend

| file | ~lines | what it is |
|---|---|---|
| `sdk_rp2350.c` | 510 | **`v_directDraw32` and friends.** The one place the game's API becomes SDK calls. Also the `svc` inline stubs. |
| `rp2350_start.s` | 95 | Game header (`'VPy2'` magic, entry, dual-core flag, `'RSET'` romset name) and the C entry stub. |
| `rp2350_game_ram.ld`, `_ram_8m.ld`, `_sram.ld` | 300 | Linker scripts for the *other* board. The `.um2` does not use them; they document the memory layouts. |

## `sdk/vectrex-draw/` — the beam model (Rust, `no_std`)

| file | ~lines | what it is |
|---|---|---|
| `src/ramp.rs` | 1800 | **`ramp_params`**: splitting a delta into (vx, vy, t1). Every constant and why. The single source of truth for beam geometry. |
| `src/emit.rs` | 1580 | Stroke and chain emission, the debt, and the test benches. |
| `src/lib.rs` | 57 | The crate's surface. |
| `cabi/src/lib.rs` | 150 | The C wrapper (`vx_ramp_params` &c.) plus the panic handler. |

## `sdk/vectrex-bus/` — the PIO + DMA stream (Rust, `no_std`)

| file | ~lines | what it is |
|---|---|---|
| `src/bus_stream.pio` | 280 | **12 instructions and 250 lines of why.** The phase rule, the sentinel bits, the park mechanism. |
| `src/lib.rs` | 670 | Installing the state machine, the DMA ring, batches, the list mode, statistics. |

## `sdk/vpy-c/` and `sdk/pitrex-sim/`

| file | ~lines | what it is |
|---|---|---|
| `vpy-c/include/vpy.h` | 160 | The game library's API, with the asset formats described per group. |
| `vpy-c/vpy.c` | 1540 | Its implementation: shapes, `.vec`/`.vanim` readers, text, math, the level and enemy runtimes. |
| `pitrex-sim/include/vectrex/vectrexInterface.h` | 75 | **The backend-neutral contract.** 20 declarations; the most important file in the kit per byte. |
| `pitrex-sim/sdk_host.c` | — | The host/WASM implementation of that contract. |
| `sdk/tools/package_um2.py` | 60 | The 20-byte `.um2` header. |

## `third_party/aae/` — the arcade emulator

Only what `game/tacscan` needs is here (~52 files). Other AAE games need more.

| file | ~lines | what it is |
|---|---|---|
| `globals.h` | 505 | `struct AAEDriver`, the `GameDef` enum, `GI[]`, the CPU contexts. **The shape of an AAE port.** |
| `cpuintrf.c` | 1230 | Per-page memory dispatch, handler slots, the CPU-agnostic run interface. |
| `aae_memdispatch.h` | 130 | The read/write fast paths (`aae_rd_byte`, `aae_wr_byte`) and the flags that tune them. |
| `cpu_control.c` | 680 | `run_cpus_to_cycles`: how a frame's worth of emulated CPU and its interrupts are scheduled. |
| `aae_romload.c/.h` | 380 | Loading a romset from a zip into `GI[]`, and the on-screen failure. |
| `romzip.c/.h`, `junzip.c`, `puff.c` | 1500 | Zip member lookup by name + inflate + CRC check. |
| `vector.h` | 140 | **The draw sink**: `add_color_line2i` → `v_directDraw32`, with the colour→intensity mapping and the screen transform. |
| `SegaG80.c`, `SegaG80snd.c`, `segacrypt.c` | 2100 | The Sega G80 driver: vector generator, ports, sound, security PROM. |
| `mz80/mz80.c` | — | The Z80 core, with the idle-spin skip. |
| `m6502/`, `m6809/`, `musashi/`, `z80/` | headers only | Declarations for cores AAE's dispatch names but tacscan never runs. **The `.c` files are not here.** |
| `aae_fast.h`, `aae_perf.h`, `aae_access_count.h` | 150 | Section attributes and opt-in instrumentation. |

## `game/tacscan/` — the worked example

| file | ~lines | what it is |
|---|---|---|
| `Makefile` | 170 | Five targets: `uvm2`, `host`, `host-prof`, `sim`, `snd`. Every flag commented. |
| `src/main.c` | 125 | The frame loop, in 20 lines, plus a telemetry block. |
| `src/aae_machine.c` | 137 | **The `driver[]` row, `getport()` and the ROM load.** The heart of an AAE port. |
| `src/aae_stubs.c` | 53 | No-ops for the AAE subsystems this game never enters. |
| `src/tacscan_romtable.h` | 49 | Generated: the romset name and the `aae_rom_op` table. |
| `src/samples.c`, `src/ts_audio.c` | 85 | AAE's sample interface → the SDK's, and the `.vsm` bundle load. |
| `src/libc_stub.c`, `include/*.h` | 200 | A freestanding libc subset. **Dropped on the `.um2` path** (newlib wins); the headers stay. |
| `tools/host_test.c`, `mz80_prof.c` | 200 | The desktop harness and the Z80 opcode profile. |
| `tools/wav_to_vsmp.py`, `audio2vsmp.py` | 350 | `samples/*.wav` → `build/tacscan.vsm`. |
| `roms/`, `samples/` | — | The romset and the 22 source sounds. |

## `examples/hello_uvmc2/`

100 lines of C and a 50-line Makefile. Copy this to start.

---

## Question → file

| if you want to know… | read |
|---|---|
| how a game is built into a `.um2` | `sdk/uvm2-sdk/uvm2.mk`, then [02](02-assembling-a-project.md) |
| what a command is, and what it costs | `sdk/uvm2-sdk/uvm2_bus.h` (top 60 lines) |
| why a stroke looks the way it does | `sdk/vectrex-draw/src/ramp.rs` (top 250 lines) |
| the bus timing rule | `sdk/vectrex-bus/src/bus_stream.pio` (top 60 lines) |
| what the game may call | `sdk/vpy-c/include/vpy.h`, `sdk/pitrex-sim/include/vectrex/vectrexInterface.h` |
| where the game's API becomes SDK calls | `sdk/rp2350-sdk/sdk_rp2350.c` |
| which core does what | `sdk/uvm2-sdk/uvm2_core1.c` (top 40 lines) |
| why sound is injected into the draw list | `sdk/uvm2-sdk/uvm2_smp.h` (top 60 lines) |
| how the SD card is read, and its traps | `sdk/uvm2-sdk/uvm2_sd.c` (top 40 lines) |
| what PSRAM is safe for | `sdk/uvm2-sdk/uvm2_psram.c` (top 30 lines), `memmap_psram.ld` |
| what a frame actually costs | `sdk/uvm2-sdk/tools/uvm2_list_count.c` |
| how to read `uvm2_stats` on a running console, and how the probe is wired | [07](07-measuring.md) "Reading them over SWD", `sdk/uvm2-sdk/tools/stats.py` |
| how an arcade game is ported | [10](10-porting-an-aae-game.md), then `game/tacscan/src/aae_machine.c` |
| how to go from a MAME driver | [11](11-porting-from-mame.md) |
| what is calibrated per console | `sdk/uvm2-sdk/uvm2_config.h` |

## Files whose *comments* are the documentation

These are worth reading in full even if you never touch the code. They were
written as the record of what was measured, and several of them contain facts
that exist nowhere else:

`uvm2_bus.h` · `uvm2_smp.h` · `bus_stream.pio` · `ramp.rs` (the constants) ·
`uvm2_config.h` · `uvm2.mk` · `memmap_psram.ld` · `aae_memdispatch.h`
