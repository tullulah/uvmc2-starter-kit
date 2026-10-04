# playroom — a small game that walks through the SDK: a hub and four rooms.
#
#   make uvm2      ->  build_uvm2/playroom.um2     (copy it to the SD card)
#
# THE WHOLE RECIPE IS FIVE LINES plus flags. Everything that turns C into a .um2
# — the pico-sdk build, the SDK sources, dual core, PIO+DMA, the 20-byte .um2
# header — lives in <kit>/sdk/uvm2-sdk/uvm2.mk and is included at the bottom.
#
# EVERY PATH IS RELATIVE TO THE KIT, never to $(HOME): this project must build
# from wherever it was unpacked. The kit root is two levels up from this file.
# := and not ?=: MAKEFILE_LIST grows with every include, so a deferred $(lastword ...)
# would be computed from sdk/uvm2-sdk/uvm2.mk instead of from this file. (It happened to
# land on the same directory here only because both sit two levels down.)
ifndef UVMC2_KIT
UVMC2_KIT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
endif
UVM2_SDK    ?= $(UVMC2_KIT)/sdk/uvm2-sdk
RP2350_SDK  ?= $(UVMC2_KIT)/sdk/rp2350-sdk
VPY_C_SDK   ?= $(UVMC2_KIT)/sdk/vpy-c
PITREX_INC  ?= $(UVMC2_KIT)/sdk/pitrex-sim/include

UVM2_NAME    = playroom
UVM2_SRCS    = src/main.c src/meshes.c src/hub.c src/crates.c src/jelly.c src/shapes.c src/flyer.c src/finale.c \
               $(VPY_C_SDK)/vpy.c $(VPY_C_SDK)/vpy3d.c $(VPY_C_SDK)/vpyphys.c $(VPY_C_SDK)/vpyfx.c \
               $(VPY_C_SDK)/vpycam.c $(VPY_C_SDK)/vpyimpact.c $(VPY_C_SDK)/vpyent.c $(VPY_C_SDK)/vpysoft.c \
               $(VPY_C_SDK)/vpybone.c $(VPY_C_SDK)/vpyik.c $(VPY_C_SDK)/vpyease.c
UVM2_CC      = arm-none-eabi-gcc
UVM2_CXX     = arm-none-eabi-gcc
UVM2_OBJCOPY = arm-none-eabi-objcopy

# -DVPY_RP2350 selects the cartridge backend inside vpy.c and sdk_rp2350.c.
# -ffunction-sections/-fdata-sections + --gc-sections keep the unused half of
# libvpy (levels, enemies, animation) out of a 496 KB image.
UVM2_CFLAGS  = -mthumb -mcpu=cortex-m33 -mfloat-abi=soft -ffreestanding -O2 -std=gnu11 \
               -ffunction-sections -fdata-sections -DVPY_RP2350 \
               -Isrc -I$(VPY_C_SDK)/include -I$(PITREX_INC) -I$(UVM2_SDK) \
               $(DENT_POOLS) $(FX_POOL) $(PHYS_TABLES) $(EXTRA_DEFS)

# THE TABLES, SIZED FOR THIS GAME. With the SDK's defaults the image did not fit:
# .bss overflowed RAM by 41508 bytes (2026-10-04). Every source must see the same
# values — each sizes a table in its module.
#
# vpy3d's pools: every mesh of every room is built once at start-up (the pools are
# never given back) — 326 vertices, 346 faces, 1227 face entries and 615 edges,
# printed by the host render (tools/host_render.c, POOLS=1) — and each of the six
# crates in CRATES gets its own copy on its first dent (14, 24, 72, 36 each):
# 410, 490, 1659, 831 in all. These leave about a quarter spare.
DENT_POOLS = -DVPY3D_POOL_V=512 -DVPY3D_POOL_F=640 -DVPY3D_POOL_FV=2176 -DVPY3D_POOL_E=1088

# vpyfx's pool: the finale takes the four pieces apart AND assembles the trophy
# at once — 214 pieces of debris and 114 of trophy, 328 (host render, TRACE=1):
# at the default 192 the pool recycled 136 of them, at 320 still 8. 336.
FX_POOL = -DVPYFX_MAX=336

# vpyphys and vpyent: CRATES is the only room with bodies, and it never has more
# than 11 (six crates, two pyramids, three balls) — the SDK's 64 bodies and 512
# contacts are physics_demo's pit. 16 bodies; 4 contacts a pair at most for the
# 55 pairs of 11 is 220, so 256. A contact table that overflows is counted
# (vpyp_stats), never silent.
PHYS_TABLES = -DVPYP_MAX_BODIES=16 -DVPYP_MAX_CONTACTS=256 -DVPYENT_MAX=16

# THE RESET BUTTON'S WATCH IS OFF IN THIS GAME, for now. On the UVMC2 (2026-10-04) the
# game reset itself as soon as the hub drew, with nobody near the button — the same as
# dkong, which is built without the watch too. The cause is open: the SDK's watch
# (uvm2_core1.c, reset_poll) sees T1 standing still where it is not. The diagnostic build
# counts what the watch sees and shows it on screen, without ever resetting:
#   make uvm2 EXTRA_DEFS="-DUVM2_RESET_WATCH_ONLY -DPLAYROOM_RESET_DIAG"
EXTRA_DEFS ?= -DUVM2_NO_RESET_BUTTON

# THE TWO SWITCHES THAT MATTER, and they are both on by default in uvm2.mk:
#
#   UVM2_DUAL_CORE=1   core 1 replays the command list and reads the controllers
#                      while core 0 builds the next frame. (uvm2_core1.c)
#   UVM2_PIO_STREAM=1  the replay goes out through PIO + DMA instead of the CPU
#                      poking GPIO, so a frame's worth of bus writes is queued
#                      once and hardware keeps the phase. (vectrex-bus)
#
# UVM2_PIO_STREAM=0 falls back to driving the bus from the core directly — same
# picture, less headroom. It does NOT remove the Rust dependency: the beam model
# is Rust too and every build links it (see the README's prerequisites).
#
#   make uvm2 UVM2_PIO_STREAM=0
UVM2_PIO_STREAM ?= 1

# Refresh: 50 Hz like the Vectrex BIOS. UVM2_HZ=0 means "present as soon as the
# list is built", which is what the arcade machines did. See uvm2.mk.
UVM2_HZ ?= 50

include $(UVM2_SDK)/uvm2.mk

clean:
	rm -rf build_uvm2

.PHONY: clean
