# Tac/Scan -> UVMC2 cartridge: a Sega G80 vector arcade game emulated on the
# RP2350. Z80 (the mz80 core) + Sega's own vector generator, drawn through the
# UVM2 SDK. This is the kit's worked example: everything it uses lives under
# <kit>/, nothing is pulled in from outside.
#
# EVERY PATH IS RELATIVE TO THE KIT, never to $(HOME): this project must build
# from wherever it was unpacked. The kit root is two levels up from this file
# (<kit>/game/tacscan/Makefile), and the whole SDK lives under <kit>/sdk.
UVMC2_KIT   ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
AAE_SRC     ?= $(UVMC2_KIT)/third_party/aae
RP2350_SDK  ?= $(UVMC2_KIT)/sdk/rp2350-sdk

PITREX_SIM_SDK ?= $(UVMC2_KIT)/sdk/pitrex-sim
VPY_C_SDK      ?= $(UVMC2_KIT)/sdk/vpy-c
VPY_C_INC   ?= $(VPY_C_SDK)/include
PITREX_INC  ?= $(PITREX_SIM_SDK)/include

RP2350_CC   ?= arm-none-eabi-gcc
OBJCOPY     ?= arm-none-eabi-objcopy

# -std=gnu11: AAE is K&R C. -O3: the Z80 interpreter is CPU-bound. -DVPY_DUAL_CORE
# + --defsym DUAL_CORE_FLAG: run the Z80 emulation on one core and the beam on the
# other (see sdk_rp2350.c).
# TELEMETRY IS OFF HERE AND THE SWITCHES ARE KEPT TOGETHER, because every one of
# them was on at some point and leaving one behind is how this cartridge got vector
# flicker before. To measure again:
#   -DUVM2_MEASURE_DRAW   per-frame beam timing      (sdk_rp2350.c)
#   -DTS_TELEM=1         builder split, us/frame    (src/main.c)
#   -DUVM2_SMP_TELEM=1   sample mixer histograms    (uvm2_smp.c + tools/smp_stats.py)
# MZ80_IDLE_SKIP is NOT telemetry: it is the Z80 idle-spin skip, and it stays.
CFLAGS  = -DROMZIP_NO_STDIO -DMZ80_IDLE_SKIP -mthumb -mcpu=cortex-m33 -mfloat-abi=soft -ffreestanding -O3 -std=gnu11 \
          -ffunction-sections -fdata-sections -DVPY_RP2350 -D'CCNT0(x)=do{}while(0)' \
          -include src/aae_compat.h -Iinclude -Isrc -I$(AAE_SRC) -I$(VPY_C_INC) -I$(PITREX_INC)
LDFLAGS = -nostdlib -Wl,--gc-sections -Wl,-T,$(RP2350_SDK)/rp2350_game_ram.ld -Wa,--defsym,DUAL_CORE_FLAG=0x44430001

# Tac/Scan file set (STARTING POINT — refine as the build links):
#   Z80 CPU (mz80) + interface + frame control + the Sega G80 driver + utils.
# Sound (SegaG80snd: AY8910 + speech samples) is stubbed in aae_stubs.c.
AAE_SRCS = \
  $(AAE_SRC)/aae_romload.c \
  $(AAE_SRC)/aae_romload_gi.c \
  $(AAE_SRC)/romzip.c \
  $(AAE_SRC)/puff.c \
  $(AAE_SRC)/SegaG80.c \
  $(AAE_SRC)/SegaG80snd.c \
  $(AAE_SRC)/mz80/mz80.c \
  $(AAE_SRC)/cpuintrf.c \
  $(AAE_SRC)/cpu_control.c \
  $(AAE_SRC)/rand.c \
  $(AAE_SRC)/acommon.c

# ts_audio.c IS CARTRIDGE-ONLY (it is not in WASM_SRCS): it resolves AAE's sound
# index to an entry of the sample bundle read off the SD card, and in the
# simulator the JS side does that with the .wav files instead.
LOCAL_SRCS = src/main.c src/libc_stub.c src/aae_stubs.c src/aae_machine.c src/samples.c src/ts_audio.c

# aae_tacscan_rp2350: the cartridge recipe lives in uvm2.mk (rp2350-cart), the same for all


# ---- IDE simulator (WASM) --------------------------------------------------
EMCC        ?= emcc
EMSDK        ?= $(HOME)/emsdk
EMSDK_PY     := $(firstword $(wildcard $(EMSDK)/python/*/bin))
# emcc's DIRECTORY, WHICH WAS MISSING: without it, `make sim` died with "emcc: command
# not found" even with the emsdk installed — what was being added to PATH was the emsdk's
# python, not the compiler.
EMSDK_EMCC   := $(EMSDK)/upstream/emscripten
EMCC_PATH    := $(if $(EMSDK_PY),$(EMSDK_PY):,)$(EMSDK_EMCC):$(PATH)
WASM_SRCS    = src/main.c src/aae_stubs.c src/aae_machine.c src/samples.c
WASM_CFLAGS  = -std=gnu11 -O2 -DMZ80_IDLE_SKIP -D'CCNT0(x)=do{}while(0)' \
               -include src/aae_compat.h -Isrc -I$(AAE_SRC) -I$(VPY_C_INC) -I$(PITREX_INC)
WASM_LDFLAGS = -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 \
               -sINITIAL_MEMORY=33554432 -sALLOW_MEMORY_GROWTH=1 \
               -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
               -sMODULARIZE=1 -sEXPORT_NAME=createSimModule -sEXPORTED_RUNTIME_METHODS=callMain,FS

sim: | build_wasm
	PATH="$(EMCC_PATH)" $(EMCC) $(WASM_CFLAGS) $(WASM_SRCS) $(AAE_SRCS) $(PITREX_SIM_SDK)/sdk_host.c \
	        $(WASM_LDFLAGS) -o build_wasm/game.js
	@echo "=== Sim build OK: build_wasm/game.js ==="

build_wasm:
	mkdir -p build_wasm

clean:
	rm -rf build_rp2350 build_wasm

# ---- UVM2 cartridge (the stock multicart firmware, loading from SD) -------
# Same program as the rp2350 build; the shared rule swaps the start file,
# linker script and SDK, and packages the result as .um2. The UVM2 runs a game
# from SRAM only (~496 KB), so a large port may simply not fit.
# AAE_SRC is computed from $(lastword $(MAKEFILE_LIST)), which is LAZY: after
# the include below the last makefile is uvm2.mk, and the path would resolve
# against the SDK instead of this game. Freeze it while we are still the last.
AAE_SRC := $(AAE_SRC)

UVM2_SDK   ?= $(UVMC2_KIT)/sdk/uvm2-sdk
UVM2_NAME   = aae_tacscan

# ---- FREE REFRESH, AND IT IS THE SOUND THAT ASKS FOR IT --------------------
#
# Samples sound as PSG writes PUT INTO THE DRAW LIST, in the gaps where the beam is
# already parked (uvm2_smp.h). With a FIXED refresh, the frame's leftover is spent
# by `frame_filler` on its zero-reference alternation — where the ramp is
# deliberately open and Port A is the discharge current, so nothing can be injected
# there — and the audio only covers the drawn part of the frame: MEASURED with
# tools/uvm2_smp_test.c, 14% with 127 strokes and 38% with 300. The sound would fade
# in and out with whatever is on screen.
#
# With free refresh there is no leftover, the list is drawing end to end and the
# coverage is 91% whatever the scene. The cartridge BIOS already compiles this way
# (build.rs: UVM2_HZ=0); this is the same for the .um2 image.
#
#   make uvm2 UVM2_HZ=50      the control: hear it degrade with the scene
UVM2_HZ    ?= 0
UVM2_SRCS   = $(LOCAL_SRCS) $(AAE_SRCS)
UVM2_CFLAGS = $(CFLAGS)
UVM2_CC     = $(RP2350_CC)
UVM2_CXX    = $(RP2350_CC)
UVM2_OBJCOPY= $(OBJCOPY)
include $(UVM2_SDK)/uvm2.mk

# ---- native harness, and the Z80 profile -----------------------------------
# The cartridge frame is 41 ms of which 23 are the beam (measured: the bus runs at
# 97% of nominal), so what is left is the Z80 and the vector generator. This runs the
# SAME AAE sources on the desktop and says which opcode the emulation spends itself
# on, and whether its cycle count matches a real Z80 — one earlier failure was
# exactly that: a single opcode waiting too long.
#
#   make host            just run it
#   make host-prof       run it with the opcode profile
HOST_CC     ?= cc
# -undefined dynamic_lookup: cpuintrf.c/cpu_control.c name EVERY core AAE supports
# (6502, both 6809s, 68000) whether the game uses them or not. The cartridge links
# because it builds with --gc-sections and those functions are dropped unreferenced;
# ld64 has no equivalent, so the harness allows the undefined symbols instead of
# dragging four unused CPU cores in. Tac/Scan is a Z80 game and never calls them —
# if one is ever reached, this crashes, which is the right outcome.
HOST_CFLAGS  = -std=gnu11 -O2 -w -DMZ80_IDLE_SKIP -D'CCNT0(x)=do{}while(0)' -include src/aae_compat.h \
               -Isrc -I$(AAE_SRC) -I$(VPY_C_INC) -I$(PITREX_INC)
HOST_LDFLAGS = -Wl,-undefined,dynamic_lookup
HOST_SRCS    = src/aae_stubs.c src/aae_machine.c src/samples.c $(AAE_SRCS)

build:
	@mkdir -p build

host: | build
	$(HOST_CC) $(HOST_CFLAGS) tools/host_test.c $(HOST_SRCS) -lm $(HOST_LDFLAGS) -o build/host_ts
	@echo "=== host: build/host_ts ==="

host-prof: | build
	$(HOST_CC) $(HOST_CFLAGS) -DMZ80_PROFILE tools/host_test.c tools/mz80_prof.c \
	    $(HOST_SRCS) -lm $(HOST_LDFLAGS) -o build/host_ts_prof
	./build/host_ts_prof

.PHONY: host host-prof

# ---- the 22 sounds, from .wav to the SD bundle -----------------------------
# The bundle is DATA THAT SHIPS ON THE CARD, not part of the image: linking it made
# the .um2 overflow the UVM2 loader's SRAM line and the console hardfaulted (red
# LED). Copy build/tacscan.vsm next to the .um2 -- and the name must stay 8.3, or
# the reader will not find it and the game is silent (the generator checks).
#
# Out of the build's dependencies on purpose: converting needs ffmpeg, and a fresh
# checkout has to build without it.
#   make snd
#   make snd SND_ARGS="--preview build/snd"    to HEAR it first
snd:
	python3 tools/wav_to_vsmp.py $(SND_ARGS)

.PHONY: aae_tacscan_rp2350 sim clean snd
