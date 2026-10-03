# scene_demo — vpyent: a scene built from entities, with text in the world.
#
#   make uvm2      ->  build_uvm2/scene_demo.um2   (copy it to the SD card)
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

UVM2_NAME    = scene_demo
UVM2_SRCS    = src/main.c $(VPY_C_SDK)/vpy.c $(VPY_C_SDK)/vpy3d.c $(VPY_C_SDK)/vpyphys.c $(VPY_C_SDK)/vpyfx.c $(VPY_C_SDK)/vpycam.c $(VPY_C_SDK)/vpyimpact.c $(VPY_C_SDK)/vpyent.c
UVM2_CC      = arm-none-eabi-gcc
UVM2_CXX     = arm-none-eabi-gcc
UVM2_OBJCOPY = arm-none-eabi-objcopy

# -DVPY_RP2350 selects the cartridge backend inside vpy.c and sdk_rp2350.c.
# -ffunction-sections/-fdata-sections + --gc-sections keep the unused half of
# libvpy (levels, enemies, animation) out of a 496 KB image.
UVM2_CFLAGS  = -mthumb -mcpu=cortex-m33 -mfloat-abi=soft -ffreestanding -O2 -std=gnu11 \
               -ffunction-sections -fdata-sections -DVPY_RP2350 \
               -Isrc -I$(VPY_C_SDK)/include -I$(PITREX_INC) -I$(UVM2_SDK) \
               $(DENT_POOLS)

# vpy3d's pools, raised so every moving body can be a dented crate with its own
# mesh (vpyent_dent copies it on the first dent): a crate with face centres is
# 14 vertices, 24 faces, 72 face entries and 36 edges, times MAX_BODIES (18),
# plus the shared meshes — physics_demo's numbers, with room to spare.
DENT_POOLS = -DVPY3D_POOL_V=768 -DVPY3D_POOL_F=1280 -DVPY3D_POOL_FV=4096 -DVPY3D_POOL_E=2048

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
