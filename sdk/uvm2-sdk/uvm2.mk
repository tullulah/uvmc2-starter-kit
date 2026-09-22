# uvm2.mk — drop-in `uvm2` build rule for any C/C++ game that already builds
# for the RP2350 cartridge.
#
# The two targets are the same program: the game speaks the `svc` syscall ABI
# through sdk_rp2350.c either way. What differs is who ANSWERS it — on our
# cartridge the firmware does, on the UVM2 there is no firmware, so uvm2-sdk
# answers from inside the image and drives the VIA over a halt-mode bus. So a
# uvm2 build is the rp2350 build with a different start file, linker script and
# SDK, wrapped in a `.um2` header.
#
# Usage — four lines next to the existing rp2350 rule:
#
#     UVM2_NAME   = dkong                  # → build_uvm2/dkong.um2
#     UVM2_SRCS   = $(GAME_SRCS)           # the game's own sources
#     UVM2_CFLAGS = $(ARM_CFLAGS)          # its compile flags, reused as-is
#     include $(UVM2_SDK)/uvm2.mk
#
# Optional:
#     UVM2_LDLIBS  = -lm                   # extra link libraries
#     UVM2_CXXSRCS = port/foo.cpp          # C++ sources, compiled with g++
#     UVM2_DEPS    = src/generated.h        # prerequisites (generated headers)
#     UVM2_CC      = $(ARM_CC)              # match the project's toolchain
#     UVM2_LDFLAGS_EXTRA = --specs=nosys.specs
#     UVM2_SDK     = .../uvm2-sdk          # if not already set
#
# A game that needs libvpy just lists $(VPY_C_SDK)/vpy.c in UVM2_SRCS.

# Everything is found from THIS makefile, never from $(HOME). uvm2.mk lives in
# <kit>/sdk/uvm2-sdk/, so the kit root is two levels up and every other piece of
# the SDK is a sibling of this directory.
ifndef UVM2_SDK
UVM2_SDK := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
endif
UVMC2_KIT  ?= $(abspath $(UVM2_SDK)/../..)
RP2350_SDK ?= $(UVMC2_KIT)/sdk/rp2350-sdk

# A project that builds rp2350 with a specific toolchain should pass the same
# one here (UVM2_CC = $(ARM_CC)); the default only fits a plain freestanding
# build with no libc.
UVM2_CC      ?= arm-none-eabi-gcc
UVM2_CXX     ?= arm-none-eabi-g++
UVM2_OBJCOPY ?= arm-none-eabi-objcopy
UVM2_BUILD   ?= build_uvm2

# `,` as a variable: a literal comma cannot appear inside a filter-out list.
, := ,

# VPY_DUAL_CORE means "record draws into a buffer that THE CARTRIDGE FIRMWARE's core 1
# drains". On the UVM2 there is no firmware and nobody would drain it, so the flag is
# filtered out below rather than left to fail at run time.
#
# ---- REFRESH RATE, PER GAME ------------------------------------------------
#
#   make uvm2                 50 Hz (default, what the Vectrex BIOS does)
#   make uvm2 UVM2_HZ=60      60 Hz
#   make uvm2 UVM2_HZ=0       UNLIMITED: present as soon as the list is built
#
# WHY IT IS PER GAME AND NOT GLOBAL. The Vectrex has no vsync — it is a vector monitor and
# it redraws when told to — and the vector arcade machines had no fixed refresh either:
# Asteroids redrew as soon as it finished its list, which is why it dimmed a little when the
# screen filled with rocks. Pinning 50 Hz HOLDS the game BACK, but only when it has room to
# spare: in a busy scene the game already takes longer and the limit never appears. So the
# speed changes with the scene, and the threshold where it changes is different in every
# game. That is not decided once for everyone: it is tested.
#
# MIND THIS WHEN COMPARING: refreshing more often also makes it BRIGHTER, because it is more
# passes per second over the same phosphor. If it looks better at 60, part of that may be
# brightness and not smoothness — do not confuse the two.
UVM2_HZ ?= 50
UVM2_CFLAGS += -DUVM2_HZ=$(UVM2_HZ)
# SUBUNITS FOR EVERYONE. The geometry travels in 1/16 of a unit end to end:
# v_directDraw32 (sdk_rp2350.c) calls uvm2_draw_move_abs_q4/delta_q4 and the SDK
# (uvm2_draw.c, UVM2_Q_BITS=4) splits, re-zeroes and calibrates in that unit. It is what was
# validated as optimal in dkong against the reference capture, and it is the ONLY path: the
# integer knobs (merging, Douglas-Peucker, reordering, clipping) no longer exist. Our own
# cartridge's BIOS compiles it the same way (build.rs).
UVM2_CFLAGS += -DUVM2_SUBUNITS

# The command list's cap, per game. See the comment in uvm2_draw.c: with no pacer a game
# draws more per frame and the multicart's 8192 falls short. Watch stats.dropped.
ifneq ($(UVM2_CMD_CAPACITY),)
UVM2_CFLAGS += -DUVM2_CMD_CAPACITY=$(UVM2_CMD_CAPACITY)u
endif

# The command list in the external PSRAM instead of SRAM. See uvm2_draw.c: in SRAM it
# competes with the game (dkong leaves 3788 bytes free), in the PSRAM there are 8 MB to
# spare.
#   make uvm2 UVM2_CMDS_IN_PSRAM=1 UVM2_CMD_CAPACITY=32768
ifeq ($(UVM2_CMDS_IN_PSRAM),1)
UVM2_CFLAGS += -DUVM2_CMDS_IN_PSRAM=1
endif

# THE EXPERIMENT'S CONTROL: the list in PSRAM, replayed from a copy in SRAM. It spends in
# SRAM what we were trying to save; it tells you whether the fetch or the data is at fault.
ifeq ($(UVM2_CMDS_STAGE_SRAM),1)
UVM2_CFLAGS += -DUVM2_CMDS_STAGE_SRAM=1
endif

# The PIO+DMA bus stream (the shared vectrex-bus crate). It goes through the ENVIRONMENT
# because what reads it is uvm2_pico.cmake, not the compiler.
ifeq ($(UVM2_PIO_STREAM),1)
export UVM2_PIO_STREAM := 1
endif

# THE STREAM LIST'S SIZE (vectrex-bus's LIST_BUF), in words and per game. Default 12288,
# which is 98 KB of the image's 496.
#
# MIND THIS, AND IT IS WORTH 98 KB: IN DUAL CORE THAT LIST IS NOT USED. The only thing in
# the whole SDK that opens it is `vbus_list_begin()` in uvm2_draw.c, and that call lives
# inside the `#else` of `#ifdef UVM2_DUAL_CORE` — i.e. ONLY on the single-core path (and in
# our own cartridge's BIOS, which has its own link to bus::list_begin). With
# UVM2_DUAL_CORE=1, the default since 2026-08-12, `IN_LIST` stays at zero, `push` goes
# through the 64-word batches and LIST_BUF is NEVER written: 98 KB of dead SRAM in every
# dual-core game's image.
#
# So a tight dual-core game lowers it to the minimum (64) and spends those 98 KB on
# something that actually draws. A SINGLE-core game cannot: there the list IS the transport,
# and if it overflows `list_fire` releases it half built and leaves the beam parked wherever
# it happens to be. Measure before lowering it: one word per command, and TWO if the command
# carries a delay (see uvm2_exec in uvm2_bus.c).
#
# It goes through the ENVIRONMENT: uvm2_pico.cmake reads it and passes it to cargo.
ifneq ($(UVM2_LIST_MAX),)
export UVM2_LIST_MAX := $(UVM2_LIST_MAX)
endif

# An artificial delay between frames, in us. It separates TIME from the PSRAM. See
# uvm2_draw.c. (UVM2_PSRAM_START, below, powers the PSRAM up without using it, which
# isolates "the chip is active" from "the list lives there".)
ifneq ($(UVM2_DELAY_US),)
UVM2_CFLAGS += -DUVM2_DELAY_US=$(UVM2_DELAY_US)
endif

# The romset in PSRAM instead of SRAM: 23 KB only used at startup. See uvm2_romzip.c — it
# goes through the UNCACHED alias, which is the only thing that does not corrupt the data.
ifeq ($(UVM2_ROMZIP_IN_PSRAM),1)
UVM2_CFLAGS += -DUVM2_ROMZIP_IN_PSRAM=1
endif

# BISECTION: install the stream but draw over SIO. See uvm2_bus.c.
ifeq ($(UVM2_STREAM_INSTALL_ONLY),1)
UVM2_CFLAGS += -DUVM2_STREAM_INSTALL_ONLY=1
endif

ifeq ($(UVM2_PSRAM_START),1)
UVM2_CFLAGS += -DUVM2_PSRAM_START=1
endif

# THE ROMSET BUFFER IS DERIVED FROM THE ZIP. It is not written by hand in 43 Makefiles.
#
# uvm2_romzip.c reads roms/<game>.zip off the SD into a STATIC array in SRAM, and the UVM2
# image lives in 496 KB: "big just in case" does not fit (at 64 KB, dkong would not link).
# But the right number already exists — it is the zip's own size — so it is read from there
# instead of eyeballed game by game. The romset's name is declared by the game in its ROM
# table (game_romset_name), which is the same one it looks for on the card.
#
# Margin: rounded up to the KB plus 1 KB. If the zip changes size, just rebuild.
UVM2_ROMSET_NAME ?= $(shell grep -hoE 'game_romset_name[[:space:]]*\[\][[:space:]]*=[[:space:]]*"[^"]+"' \
                        $(wildcard src/*.h src/*.c) 2>/dev/null | head -1 | sed -E 's/.*"(.*)"/\1/')
UVM2_ROMSET_DIR  ?= $(firstword $(wildcard roms ../roms))
UVM2_ROMSET_ZIP  := $(if $(UVM2_ROMSET_NAME),$(wildcard $(UVM2_ROMSET_DIR)/$(UVM2_ROMSET_NAME)))

ifneq ($(UVM2_ROMSET_NAME),)
ifeq ($(UVM2_ROMSET_ZIP),)
$(warning uvm2: cannot find $(UVM2_ROMSET_DIR)/$(UVM2_ROMSET_NAME) — the romset buffer stays at its default, and if the zip is larger the console will say "no romset")
else
UVM2_ROMZIP_MAX := $(shell echo $$(( ($$(wc -c < '$(UVM2_ROMSET_ZIP)') / 1024 + 2) * 1024 )))
UVM2_CFLAGS += -DUVM2_ROMZIP_MAX=$(UVM2_ROMZIP_MAX)
# AND THE BUFFER, IN PSRAM. It is 23 KB of SRAM occupied FOR THE WHOLE GAME by something
# only used at startup: it is read off the card, decompressed into the game's ROM tables, and
# never needed again. uvm2_romzip.c already supported it — and its comment said "in dkong
# that is more than it has free" — but NOBODY defined the symbol: it lived in an old build
# cache and got lost along the way.
#
# Here the PSRAM is at its best: written once and read once, with nothing that depends on
# timing.
#
# THE IDE'S EMULATOR ALREADY HAS PSRAM (since 2026-09-15: the 8 MB through the XIP window and
# through the uncached alias, with the QMI in direct mode answering the READ ID). This note
# used to say the opposite and told you to build with UVM2_ROMZIP_IN_PSRAM=0 to iterate there
# — from when it was true that the zip in PSRAM booted with dk_rom_error=1 and the 'no romset'
# X. Not any more: the image tested in the emulator is the one that goes to the console. The
# switch still exists for bisecting.
ifneq ($(UVM2_ROMZIP_IN_PSRAM),0)
UVM2_CFLAGS += -DUVM2_ROMZIP_IN_PSRAM=1
endif
endif
endif

# FLAGS THAT BELONG ONLY TO THE .um2, AND NOT TO OUR OWN CARTRIDGE.
#
# `rp2350-cart` (below) builds with the SAME UVM2_CFLAGS as the .um2 — which is what
# guarantees the command list is the same in both places — but there are settings that only
# make sense in an image that lives entirely in 496 KB of SRAM, and that on our own cartridge
# are a LOSS. The clear case is -DAAE_DISPATCH_NOINLINE: it saves ~15 KB of code and on the
# cartridge it takes the game from 45 fps to 27 (measured, see aae_memdispatch.h). Until now
# it went into UVM2_CFLAGS and the cartridge swallowed it whole without anyone saying so. It
# is the sibling of UVM2_SRCS_DROP, which already did this for the SOURCES.
#
#     UVM2_UM2_ONLY = -DAAE_DISPATCH_NOINLINE
#
UVM2_UM2_ONLY ?=
UVM2_CFLAGS += $(UVM2_UM2_ONLY)

UVM2_CFLAGS_CLEAN = $(filter-out -DVPY_DUAL_CORE -Wa$(,)--defsym$(,)DUAL_CORE_FLAG=0x44430001,\
                      $(UVM2_CFLAGS)) -I$(UVM2_SDK)

UVM2_SDK_SRCS = uvm2_bus.c uvm2_draw.c uvm2_input.c uvm2_led.c uvm2_text.c \
                uvm2_sd.c uvm2_romzip.c \
                uvm2_audio.c uvm2_svc.c
UVM2_SDK_OBJS = $(addprefix $(UVM2_BUILD)/sdk_,$(UVM2_SDK_SRCS:.c=.o))

$(UVM2_BUILD)/sdk_%.o: $(UVM2_SDK)/%.c | $(UVM2_BUILD)
	$(UVM2_CC) $(UVM2_CFLAGS_CLEAN) -c $< -o $@

$(UVM2_BUILD)/svc_bridge.o: $(RP2350_SDK)/sdk_rp2350.c | $(UVM2_BUILD)
	$(UVM2_CC) $(UVM2_CFLAGS_CLEAN) -c $< -o $@

# The linker is driven by g++ only when the game has C++ in it; a pure C game
# must not pull the C++ driver in.
UVM2_LINKER = $(if $(UVM2_CXXSRCS),$(UVM2_CXX),$(UVM2_CC))

# ─── LINKING GOES THROUGH THE PICO-SDK, NOT THROUGH uvm2_start.s ────────────
#
# This rule used to link by hand with uvm2_start.s + uvm2_game.ld. That path produces images
# that boot but DRAW A GHOST LIT SEGMENT from the origin, one per frame.
#
# MEASURED on the console 2026-08-11, same game (dkong) and same uvm2-sdk:
#   this way (uvm2_start.s):        ghost
#   via uvm2_pico.cmake (pico-sdk): clean
# And it is not in the drawing: the command list read OVER SWD from the cartridge while it
# was drawing had exactly the commands that light the beam it should have. A stroke starting
# at the origin — a frame with no movement at all — showed the ghost just the same, and the
# same binary did it on two different consoles.
#
# What changes is the startup: uvm2_cpu_init() does not touch the clocks, and under the
# pico-sdk the crt0 does the full runtime_init (plus the IMAGE_DEF and the RCP init that an
# image which does NOT go through the bootrom needs).
#
# uvm2_pico.cmake had existed since 2026-08-04 and this migration was left pending; meanwhile
# every game kept coming out of the dead path. It is done HERE, once, and everyone who
# includes this file inherits it.
#
# CMake wants the includes, the defines and the libraries separately, so they are extracted
# from the same UVM2_CFLAGS/UVM2_LDLIBS the game already defines.
empty :=
space := $(empty) $(empty)
semi  := ;
list   = $(subst $(space),$(semi),$(strip $1))

# The pico-sdk is NOT vendored in the kit (it is 670 MB with its submodules).
# `./setup.sh` clones the exact version this SDK is built against into
# third_party/pico-sdk; point PICO_SDK_PATH here if you already have one.
UVM2_PICO_SDK ?= $(if $(PICO_SDK_PATH),$(PICO_SDK_PATH),$(UVMC2_KIT)/third_party/pico-sdk)
# Homebrew's arm-none-eabi-gcc has no nosys.specs — the Arm GNU Toolchain does,
# and the pico-sdk link needs it. Override if yours lives somewhere else.
UVM2_ARM_TOOLCHAIN ?= $(firstword $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi) \
                                  $(wildcard /opt/arm-gnu-toolchain*/arm-none-eabi))
UVM2_CMAKE_BUILD   ?= $(UVM2_BUILD)/pico

# `-include foo.h` is TWO words, so a plain $(filter) keeps the flag and throws away the
# header. They are glued together before filtering. Without this, the AAE ports — which
# pre-include their aae_compat.h — fail with half a file of "undeclared" symbols, which does
# not look like a flags problem but like a sources problem.
UVM2_CFLAGS_GLUED = $(subst -include ,-include=,$(UVM2_CFLAGS_CLEAN))

# And the single quotes in defines like -D'CCNT0(x)=do{}while(0)' are the shell's business:
# going through CMake they survive literally and the define comes out with quotes inside.
# Strip them.
quote := '
# Sources that are surplus ON THIS PATH, not in the game. libc_stub.c exists in the 41 AAE
# ports because the old link went with -nostdlib and exit, fclose and company had to be
# filled in by hand. The pico-sdk brings newlib, so the stubs collide with the real thing:
# "multiple definition of 'exit'". They are dropped here, which is where we know which path
# we are on; the game keeps them for its rp2350 build.
UVM2_SRCS_DROP ?= libc_stub.c
UVM2_SRCS_KEPT  = $(foreach s,$(UVM2_SRCS) $(UVM2_CXXSRCS),\
                    $(if $(filter $(UVM2_SRCS_DROP),$(notdir $(s))),,$(s)))
UVM2_GAME_SRCS   = $(abspath $(UVM2_SRCS_KEPT) $(RP2350_SDK)/sdk_rp2350.c)
UVM2_GAME_INCS   = $(abspath $(patsubst -I%,%,$(filter -I%,$(UVM2_CFLAGS_GLUED))))
UVM2_GAME_DEFS   = $(subst $(quote),,$(patsubst -D%,%,$(filter -D%,$(UVM2_CFLAGS_GLUED))))
UVM2_GAME_PREINC = $(abspath $(patsubst -include=%,%,$(filter -include=%,$(UVM2_CFLAGS_GLUED))))
UVM2_GAME_LIBS   = $(patsubst -l%,%,$(filter -l%,$(UVM2_LDLIBS)))

# Dual core. MIND THE NAMES, which is what kept it switched off:
#
#   VPY_DUAL_CORE  = "record the strokes into a buffer that THE CARTRIDGE FIRMWARE's core 1
#                     drains". On the UVM2 there is no firmware, the image is the whole
#                     program, and nobody would drain it. It is filtered out above and
#                     uvm2_pico.cmake rejects it with an error.
#   UVM2_DUAL_CORE = ours, inside the image: uvm2_core1.c starts core 1 and the replay, the
#                     input, the PSG queue and the 50 Hz pacing all happen there. Same
#                     mechanism as the cartridge: double buffering by frame&1 and two
#                     monotonic counters with a dmb.
#
# It was written in full and NOBODY defined it. It does not make the drawing faster — the
# Vectrex's 1.5 MHz clock sets that — it overlaps the game's logic with the beam's sweep,
# which is what the multicart's own split buys.
#
# DEFAULT SINCE 2026-08-12, validated on the console with dkong: it draws, the controllers
# respond, and the frame sits at 98% of the beam's ceiling (39.51 ms of sweep in a 40.25 ms
# frame). Turn it off with UVM2_DUAL_CORE=0.
#
# MUSIC IN DUAL CORE: RESOLVED. It was once open ("the music does not play in dual core"),
# with the sequencer's pacing as the suspect: single core advanced the track by ELAPSED
# VECTREX TIME and core 1 once per frame. Core 1 now advances it by elapsed time too
# (uvm2_core1.c), and .vmus music plays on the console in dual core — dkong and the ports.
#
#   make uvm2 UVM2_DUAL_CORE=0
UVM2_DUAL_CORE ?= 1
ifeq ($(UVM2_DUAL_CORE),1)
UVM2_GAME_DEFS += UVM2_DUAL_CORE
endif

# UVM2_DEPS lets a project name generated headers the build needs first.
uvm2: $(UVM2_DEPS) | $(UVM2_BUILD)
	cmake -S $(UVM2_SDK)/pico -B $(UVM2_CMAKE_BUILD) \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_TOOLCHAIN_FILE=$(UVM2_PICO_SDK)/cmake/preload/toolchains/pico_arm_cortex_m33_gcc.cmake \
	    -DPICO_SDK_PATH=$(UVM2_PICO_SDK) -DPICO_TOOLCHAIN_PATH=$(UVM2_ARM_TOOLCHAIN) \
	    -DUVM2_SDK_DIR=$(abspath $(UVM2_SDK)) \
	    -DUVM2_NAME=$(UVM2_NAME) \
	    -DUVM2_GAME_SRCS="$(call list,$(UVM2_GAME_SRCS))" \
	    -DUVM2_GAME_INCS="$(call list,$(UVM2_GAME_INCS))" \
	    -DUVM2_GAME_DEFS="$(call list,$(UVM2_GAME_DEFS))" \
	    -DUVM2_GAME_PREINC="$(call list,$(UVM2_GAME_PREINC))" \
	    -DUVM2_GAME_LIBS="$(call list,$(UVM2_GAME_LIBS))" > $(UVM2_CMAKE_BUILD).log
	cmake --build $(UVM2_CMAKE_BUILD) -j8
ifeq ($(filter 0,$(UVM2_LOAD_PSRAM)),$(UVM2_LOAD_PSRAM))
	cp $(UVM2_CMAKE_BUILD)/$(UVM2_NAME).um2 $(UVM2_BUILD)/$(UVM2_NAME).um2
	@echo "=== Build OK (uvm2 SD game): $(UVM2_BUILD)/$(UVM2_NAME).um2 ==="
else
# A PSRAM-linked payload is deliberately NOT wrapped as .um2 (the multicart
# menu cannot load it; our PSRAM loader takes the flat .bin instead).
	cp $(UVM2_CMAKE_BUILD)/$(UVM2_NAME).bin $(UVM2_BUILD)/$(UVM2_NAME).bin
	@echo "=== Build OK (uvm2 PSRAM payload): $(UVM2_BUILD)/$(UVM2_NAME).bin ==="
endif

$(UVM2_BUILD):
	mkdir -p $(UVM2_BUILD)

uvm2-clean:
	rm -rf $(UVM2_BUILD)

.PHONY: uvm2 uvm2-clean

# ── THE VECTREX STUDIO CARTRIDGE (rp2350): ONE recipe for every project ─────────────────
#
# "EVERY C project must be dual core, PIO and DMA". On our own cartridge the BIOS provides
# that (core 0 executes the list over PIO+DMA; the program runs on core 1) and what the game
# decides is HOW it talks to it: with VPY_DUAL_CORE, sdk_rp2350.c calls the BIOS's SDK through
# its function table (uvm2_config.h is required: hence the SDK's -I), and without it it goes
# through svc one call at a time and with no configuration menu. Every Makefile had its own
# rp2350 rule copied 43 times, nearly all of them over svc; this is the same one for all of
# them, with the SAME sources and defines as the .um2 (UVM2_SRCS/UVM2_CFLAGS): the command
# list that comes out of here is the one that comes out of the UVM2, measured in the emulator.
#
# `libc_stub.c` DOES go in (UVM2_SRCS, not UVM2_SRCS_KEPT): the .um2 drops it because the
# pico-sdk brings libc and the cartridge is -nostdlib. The rule is named the way the project
# file expects (`<name>_rp2350`) and also `rp2350-cart`.
RP2350_BUILD   ?= build_rp2350
RP2350_LDFLAGS ?= -nostdlib -Wl,--gc-sections -Wl,-T,$(RP2350_SDK)/rp2350_game_ram.ld \
                  -Wa,--defsym,DUAL_CORE_FLAG=0x44430001
RP2350_CART_CFLAGS = $(filter-out $(UVM2_UM2_ONLY),$(UVM2_CFLAGS_CLEAN)) -DVPY_DUAL_CORE

rp2350-cart $(UVM2_NAME)_rp2350: $(UVM2_DEPS) | $(RP2350_BUILD)
	$(UVM2_LINKER) $(RP2350_CART_CFLAGS) $(RP2350_LDFLAGS) \
	    $(RP2350_SDK)/rp2350_start.s $(UVM2_SRCS) $(UVM2_CXXSRCS) $(RP2350_SDK)/sdk_rp2350.c \
	    $(UVM2_LDLIBS) -lgcc -o $(RP2350_BUILD)/$(UVM2_NAME).elf
	$(UVM2_OBJCOPY) -O binary $(RP2350_BUILD)/$(UVM2_NAME).elf $(RP2350_BUILD)/$(UVM2_NAME)_sd.bin
	@echo "=== Build (rp2350 SD game, dual core through the BIOS table): $(RP2350_BUILD)/$(UVM2_NAME)_sd.bin ==="

$(RP2350_BUILD):
	mkdir -p $(RP2350_BUILD)
.PHONY: rp2350-cart $(UVM2_NAME)_rp2350
