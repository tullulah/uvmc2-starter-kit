# 02 — Assembling a project

Prerequisites and their versions are in `README.md` §1. The short version: an
Arm GNU Toolchain (not Homebrew's — it has no `nosys.specs`), CMake ≥ 3.13,
Python 3, **cargo ≥ 1.78**, git, and network on the first build.

## The chain, end to end

```
your Makefile                      names the game and its sources
  └─ include $(UVM2_SDK)/uvm2.mk   turns that into a cmake invocation
       └─ sdk/uvm2-sdk/pico/CMakeLists.txt
            └─ uvm2_pico.cmake     the real build: SDK sources, Rust crates, flags
                 └─ pico-sdk 2.2.0 crt0, IMAGE_DEF, clocks, multicore, PIO, DMA
                      └─ arm-none-eabi-gcc  ->  .elf  ->  .bin
                           └─ sdk/tools/package_um2.py  ->  .um2
```

Nothing in that chain looks outside the kit. Every path is derived from
`$(lastword $(MAKEFILE_LIST))`, so the kit builds from wherever it was unpacked.

## The smallest Makefile that works

This is `examples/hello_uvmc2/Makefile`, trimmed to the load-bearing lines:

```make
UVMC2_KIT   ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
UVM2_SDK    ?= $(UVMC2_KIT)/sdk/uvm2-sdk

UVM2_NAME    = hello_uvmc2              # -> build_uvm2/hello_uvmc2.um2
UVM2_SRCS    = src/main.c $(UVMC2_KIT)/sdk/vpy-c/vpy.c
UVM2_CC      = arm-none-eabi-gcc
UVM2_CFLAGS  = -mthumb -mcpu=cortex-m33 -mfloat-abi=soft -ffreestanding -O2 \
               -ffunction-sections -fdata-sections -DVPY_RP2350 \
               -Isrc -I$(UVMC2_KIT)/sdk/vpy-c/include \
               -I$(UVMC2_KIT)/sdk/pitrex-sim/include

include $(UVM2_SDK)/uvm2.mk
```

`uvm2.mk` adds `sdk_rp2350.c`, the whole `uvm2-sdk`, the Rust crates, the linker
script, the startup code and the packaging step.

### Starting a new game

Copy `examples/hello_uvmc2/`, change `UVM2_NAME` and `UVM2_SRCS`. That is it.

## The knobs that matter

All of these are `make` variables; the defaults are in `uvm2.mk` and every one of
them has a long comment there explaining what it cost to learn.

| variable | default | what it does |
|---|---|---|
| `UVM2_DUAL_CORE` | `1` | Core 1 replays the list and reads input while core 0 builds the next frame. |
| `UVM2_PIO_STREAM` | `1` | The replay goes out through PIO + DMA instead of the CPU poking GPIO. Setting it to 0 does **not** remove the Rust dependency — the beam model is Rust and always links. |
| `UVM2_HZ` | `50` | Refresh cap. `60` for 60 Hz mains, `0` = present as soon as the list is ready. |
| `UVM2_CMD_CAPACITY` | 8192 | Commands the list can hold — 3 bytes each, per buffer, two buffers. Watch `stats.dropped`. |
| `UVM2_LIST_MAX` | 12288 | Words in the PIO stream's buffer. **Dead in dual core** — lower it to 64 and reclaim 98 KB. |
| `UVM2_CMDS_IN_PSRAM` | off | Put the command list in PSRAM. Frees SRAM, costs determinism. |
| `UVM2_ROMZIP_IN_PSRAM` | auto | Put the SD romset buffer in PSRAM (write-once, read-once — the ideal case). |
| `UVM2_SRCS_DROP` | `libc_stub.c` | Sources to drop on this path only (the pico-sdk brings newlib; hand-written stubs collide). |
| `UVM2_UM2_ONLY` | empty | Flags that apply to the `.um2` and not to a shared cartridge build. |

### Dual core and PIO are not optional

`uvm2_bus.h` refuses to compile without both:

```
#error "UVM2: UVM2_DUAL_CORE is missing. A game draws with core 1; if this is a
        bench that measures the executor in isolation, declare
        UVM2_BENCH_NO_CORE1 in its build block and say why."
```

That is deliberate. A *default* can be stepped on without anyone noticing, and it
was: three games that were tested daily spent weeks compiling with neither, and
the build said nothing. Measured the day it was found: 51 ms of drawing and 46 ms
of game logic **in series** — 13 Hz, purely for not having core 1.

`UVM2_BENCH_NO_CORE1` is the escape hatch, and it is for benches that measure the
executor in isolation. A game that declares it is lying.

### 50 Hz is a choice, not a fact

The Vectrex has no vsync: it is a vector monitor and it redraws when told to. The
arcade machines had no fixed refresh either — Asteroids redrew as soon as it
finished its list, which is why it dimmed as the screen filled. Pinning 50 Hz
holds a game back only when it has room to spare.

Two things to keep apart when comparing: refreshing more often also makes the
picture **brighter**, because it is more passes per second over the same
phosphor. If it "looks better at 60", some of that is brightness, not smoothness.

## What ends up in the image

`uvm2_pico.cmake` compiles, in one binary:

* your sources and `sdk/rp2350-sdk/sdk_rp2350.c` (the game-facing API);
* the whole of `sdk/uvm2-sdk/` (bus, draw, input, text, LED, SD, romzip, audio,
  samples, the SVC handler, and `uvm2_core1.c` under dual core);
* `libvectrex_draw_cabi.a` and `libvectrex_bus_cabi.a`, built by cargo;
* the pico-sdk pieces it needs: crt0, runtime init, multicore, PIO, DMA, flash.

Board definition: `olimex_rp2350_xxl`, a stock pico-sdk board whose GPIO map
matches the UVM2's.

## A note on the two build paths

There used to be a hand-written link (`uvm2_start.s` + `uvm2_game.ld`). It
produces images that **boot but draw one ghost lit segment from the origin every
frame**. Measured on the console with the same game and the same SDK: hand-linked
→ ghost, via `uvm2_pico.cmake` → clean. It is not in the drawing — the command
list read back over SWD while it was drawing had exactly the right commands. What
differs is the startup: under the pico-sdk, crt0 does the full `runtime_init`
(plus the IMAGE_DEF and the RCP init an image that does **not** come through the
bootrom needs).

`uvm2_start.s` is still in the kit because it documents the vector table and the
IMAGE_DEF block, and because a VPy-generated game emits the same thing inline.
Do not link through it.
