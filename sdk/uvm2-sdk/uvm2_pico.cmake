# uvm2_pico.cmake — build a UVM2 SD game through the PICO SDK.
#
# WHY THIS REPLACES uvm2_start.s + uvm2_game.ld (2026-08-04)
# ----------------------------------------------------------
# Our hand-rolled startup produced images the UVM2 refused to launch — not one of
# our ~45 games booted, while the stock ones did from the same card. Proven on hardware:
# the RP2350 (unlike the RP2040) requires an **IMAGE_DEF** metadata block, the
# bootrom validates it, and the UVM2 firmware defers to that validation. Without
# the block our first instruction never executed (a breadcrumb in WATCHDOG_SCRATCH4
# stayed 0); with a block bolted on, the same image walked its whole startup.
#
# So the boot contract belongs to the chip, not to us. crt0.S emits the block from
# `embedded_start_block.inc.S`, and gets the rest of the RP2350 entry right too —
# notably the RCP init that a NOT-through-the-bootrom image needs.
#
# WHAT WE STILL OWN: everything Vectrex-side (uvm2_bus/draw/input/led/text/audio)
# and the `svc` ABI. `isr_svcall` is WEAK in crt0.S, so uvm2_svc_handler simply
# overrides it — no SDK patching.
#
# Usage from a game's Makefile:
#   cmake -S $(UVM2_SDK)/pico -B build_uvm2 \
#         -DUVM2_NAME=dkong -DUVM2_GAME_SRCS="a.c;b.c" -DUVM2_GAME_INCS="inc"
#   cmake --build build_uvm2
#
# Board: olimex_rp2350_xxl, the same one the stock firmware builds against. The UVM2's GPIO map
# matches his pin-for-pin (D0=0, A0=8, PB6=22, /IRQ=23, A14=24, A15=25, R/W=26,
# /HALT=27, /NMI=29, CLK=31), so this is the board definition that fits the wiring.

if(NOT DEFINED UVM2_NAME)
    message(FATAL_ERROR "set -DUVM2_NAME=<game>")
endif()

set(PICO_BOARD olimex_rp2350_xxl CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(${UVM2_NAME} C CXX ASM)
# PANIC, READABLE. Before pico_sdk_init() so it also reaches panic.c, which is compiled
# inside the SDK's own library: passing it as a game define is NOT enough — I tried, and the
# symbol did not even appear in the ELF.
#
# With this, `panic` stops printing and calls uvm2_panic_stash, which saves the pointer to the
# message at 0x20080234 and halts. Printing here is worse than useless: there is no console and
# vsnprintf runs off the stack (measured: BFAR = 0x20082000, exactly the top), so all you see
# is the messenger failing and the reason is lost.
if(DEFINED ENV{UVM2_PANIC_STASH} AND NOT "$ENV{UVM2_PANIC_STASH}" STREQUAL "0")
    message(STATUS "UVM2_PANIC_STASH: panic saves the reason at 0x20080230 instead of printing it")
    add_compile_definitions(PICO_PANIC_FUNCTION=uvm2_panic_stash)
endif()

# THE HEAP, WHICH BY DEFAULT IS 2 KB OF NOTHING.
#
# The pico-sdk reserves PICO_HEAP_SIZE = 2048 in a `.heap` section (crt0.S), and a UVM2 image
# allocates no dynamic memory: neither the game nor this SDK calls malloc. With the RAM at the
# edge that is 2 KB thrown away — dkong overflowed by 900 bytes because of them alone.
#
# IT IS NOT SET TO ZERO FOR EVERYONE. There are 44 ports and I cannot prove none of them
# allocates; a malloc that returns NULL fails silently and far from here. So it is OPT-IN, and
# whoever turns it on has a check that proves it for THEIR image:
#
#     arm-none-eabi-nm image.elf | grep -E ' (malloc|_sbrk|_malloc_r)$'
#
# if that prints nothing, nobody can allocate and the heap really is surplus.
if(DEFINED ENV{UVM2_HEAP})
    message(STATUS "UVM2_HEAP: heap of $ENV{UVM2_HEAP} bytes (the pico-sdk uses 2048)")
    add_compile_definitions(PICO_HEAP_SIZE=$ENV{UVM2_HEAP})
endif()

pico_sdk_init()

add_executable(${UVM2_NAME}
    ${UVM2_GAME_SRCS}
    ${UVM2_SDK_DIR}/uvm2_bus.c
    ${UVM2_SDK_DIR}/uvm2_sd.c
    ${UVM2_SDK_DIR}/uvm2_romzip.c
    ${UVM2_SDK_DIR}/uvm2_draw.c
    ${UVM2_SDK_DIR}/uvm2_config.c
    ${UVM2_SDK_DIR}/uvm2_wizard.c
    ${UVM2_SDK_DIR}/uvm2_input.c
    ${UVM2_SDK_DIR}/uvm2_led.c
    ${UVM2_SDK_DIR}/uvm2_text.c
    ${UVM2_SDK_DIR}/uvm2_audio.c
    ${UVM2_SDK_DIR}/uvm2_smp.c
    ${UVM2_SDK_DIR}/uvm2_svc.c
    ${UVM2_SDK_DIR}/uvm2_core1.c
    ${UVM2_SDK_DIR}/uvm2_psram.c
    ${UVM2_SDK_DIR}/uvm2_svc_entry.s
    ${UVM2_SDK_DIR}/uvm2_pico_main.c
    ${UVM2_SDK_DIR}/uvm2_pico_svc.S
)

# A RAM image: the UVM2 firmware copies it to 0x20000000 and launches it. This is
# also what makes crt0 emit the VECTOR_TABLE item the launch path looks for.
pico_set_binary_type(${UVM2_NAME} no_flash)

# SOFTWARE DOUBLES, NOT THE COPROCESSOR. By default the pico-sdk replaces __aeabi_i2d and
# company with wrappers that use the RP2350's DCP (mrc2/mcrr on p4). It works on the board, but
# the EMULATOR has no coprocessors — its own Thumb2 code says so — so `mrc2 p4` leaves the N
# flag set and the `bmi` behind it spins for ever. Symptom: the game stops on the first frame
# (recals=1 after 1200 frames) and draws nothing, with no error at all. Found in mhavoc, which
# uses `double` in AAE's calc_sweep; ports that do not touch doubles never reached those
# routines, which is why it was invisible.
#
# `compiler` keeps libgcc's, which are pure Thumb. It costs cycles only to whoever uses doubles
# — in AAE, one multiplication per frame — and in exchange EVERY game can be debugged in the
# emulator, which is where you iterate.
pico_set_double_implementation(${UVM2_NAME} compiler)
pico_set_float_implementation(${UVM2_NAME} compiler)

# EXPERIMENT: link into the EXTERNAL PSRAM (0x11000000) instead of the internal SRAM.
#
# Turned on with the environment variable UVM2_LOAD_PSRAM=1. The resulting image has to be
# packaged with `--load-addr 0x11000000`, because the .um2 header is what tells the multicart's
# loader where to copy it.
#
# THE QUESTION: does their loader honour an address outside SRAM? We measured that silent
# PSRAM, but FROM INSIDE an already-loaded game — if their firmware only initialises it when it
# needs it, that measurement would not have seen it. Asking it to load there is the only
# test.
if(DEFINED ENV{UVM2_LOAD_PSRAM} AND NOT "$ENV{UVM2_LOAD_PSRAM}" STREQUAL "0")
    message(STATUS "UVM2_LOAD_PSRAM: linking into the external PSRAM (0x11000000)")
    pico_set_linker_script(${UVM2_NAME} ${CMAKE_CURRENT_LIST_DIR}/memmap_psram.ld)
    target_compile_definitions(${UVM2_NAME} PRIVATE UVM2_PSRAM_IMAGE=1)
endif()

# The game keeps its own `main`; rename it so uvm2_pico_main.c can wrap it with
# the runtime init. Scoped to the GAME sources only — as a global flag it also
# renames the `main` in CMake's compiler-probe program and configuration fails.
set_source_files_properties(${UVM2_GAME_SRCS} PROPERTIES
    COMPILE_DEFINITIONS "main=uvm2_game_main")

# Tells uvm2-sdk that crt0 owns .bss and the vector table now.
target_compile_definitions(${UVM2_NAME} PRIVATE UVM2_PICO_RUNTIME=1)

# The game's defines go in as OPTIONS, not as definitions. CMake eats the ones with
# parentheses: -D'CCNT0(x)=do{}while(0)' goes into the list, gives no warning, and does not
# appear in flags.make — the game compiles with CCNT0 undeclared and fails in
# third_party/aae/cpuintrf.c. As a raw option it arrives intact, and besides, each define is a
# single argv element, so braces and parentheses never go through a shell.
#
# C ONLY. The target carries assembly files (uvm2_svc_entry.s), and the assembler chokes: with
# `-include header.h` it starts reading C and emits "bad instruction: typedef signed char
# __int8_t". Same with a define carrying parentheses.
foreach(def ${UVM2_GAME_DEFS})
    target_compile_options(${UVM2_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-D${def}>")
endforeach()

# RAW options from the game (not defines): `-include something.h`, warnings to silence...
# The AAE ports need `-include src/aae_compat.h`, which does not fit as a define.
foreach(opt ${UVM2_GAME_OPTS})
    target_compile_options(${UVM2_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${opt}>")
endforeach()

# OUT OF DATE ON ONE POINT: "we never wrote the core-1 consumer" is no longer true —
# uvm2_core1.c exists (replay, input, the PSG queue and the 50 Hz pacing) and
# uvm2_pico_main.c starts it. What was missing was the switch: it is turned on with
# UVM2_DUAL_CORE in UVM2_GAME_DEFS (make uvm2 UVM2_DUAL_CORE=1). The rejection below is still
# correct and necessary: VPY_DUAL_CORE is a DIFFERENT thing and nobody drains it here.
#
# NOT because the UVM2 is single-core — it carries the same RP2350 we do, and
# The stock firmware's own games use both halves of it (core 0 fills commandBuffer[2][8K],
# core 1 replays it and reads the controls, handshaken through two volatile
# frame counters). What is single-core is OUR UVM2 runtime: we never wrote the
# core-1 consumer for it.
#
# So the flag has to be refused, because -DVPY_DUAL_CORE does not mean "use two
# cores". It means "record draws into a buffer that THE CARTRIDGE FIRMWARE's
# core 1 drains", and on the UVM2 there is no firmware — the image is the whole
# program, and nobody drains it. A game built with it would draw nothing at all.
# Failing here beats failing on the screen.
#
# Worth doing eventually: a second core would not make the drawing faster (the
# replay is paced by the Vectrex's own 1.5 MHz clock and cannot outrun it), but
# it would overlap the game logic with the replay instead of running them back
# to back, which is exactly what that split buys.
if("VPY_DUAL_CORE" IN_LIST UVM2_GAME_DEFS)
    message(FATAL_ERROR "VPY_DUAL_CORE in UVM2_GAME_DEFS: that flag targets the cartridge firmware's core 1, which does not exist here")
endif()

# The game's include dirs go on the GAME SOURCES, not on the target. A port that
# ships freestanding libc shims (asteroids_sbt's include_rp2350/) would otherwise
# shadow the real <stdio.h> for the pico-sdk's own sources, which then lose puts()
# and fail to build. Scoped this way each side gets the headers it expects.
set_source_files_properties(${UVM2_GAME_SRCS} PROPERTIES
    INCLUDE_DIRECTORIES "${UVM2_GAME_INCS}")

# Pre-included headers (`-include foo.h`). The AAE ports use them for their aae_compat.h, and
# without them the game does not compile: dozens of "undeclared" symbols come out that look
# like a sources problem and are not. They go on the GAME's sources, like the includes, so as
# not to inflict them on the pico-sdk.
if(UVM2_GAME_PREINC)
    set(UVM2_PREINC_OPTS "")
    foreach(hdr ${UVM2_GAME_PREINC})
        list(APPEND UVM2_PREINC_OPTS "-include" "${hdr}")
    endforeach()
    set_source_files_properties(${UVM2_GAME_SRCS} PROPERTIES
        COMPILE_OPTIONS "${UVM2_PREINC_OPTS}")
endif()

target_include_directories(${UVM2_NAME} PRIVATE ${UVM2_SDK_DIR})
# hardware_flash: it is NOT for writing to flash — nothing there is touched. It is for
# flash_devinfo_set_cs_size() and flash_do_cmd(), which are the only way to ask the BOOTROM for
# the exit-XIP sequence towards CS1, i.e. towards the PSRAM. See the uvm2_psram_probe_bootrom()
# block. pico_stdlib does not pull it in.
target_link_libraries(${UVM2_NAME} pico_stdlib pico_multicore hardware_dma hardware_pio hardware_flash ${UVM2_GAME_LIBS})

# ── The drawing layer SHARED by both cartridges ────────────────────────────
#
# ONE implementation of the beam model: the same Rust crate our own cartridge's firmware
# links, here as a staticlib inside the image. It used to live in the private repository and
# this carried an `EXISTS` guard so the public tree would still compile without it — but that
# guard kept a second drawing layer ALIVE, which is exactly what was being removed. With the
# crate here, the tree builds on its own and there is no second implementation to maintain.
#
# CMake BUILDS it, it does not look for a prebuilt one: a .a you have to remember to rebuild by
# hand goes stale silently, and a stale binary has already cost us a whole afternoon.
set(VECTREX_DRAW_DIR "${CMAKE_CURRENT_LIST_DIR}/../vectrex-draw")
set(VECTREX_DRAW_TARGET thumbv8m.main-none-eabi)   # SOFTWARE floating point: this image is
                                                   # compiled -mfloat-abi=softfp and the
                                                   # linker compares Tag_ABI_VFP_args even
                                                   # though no float crosses
# THE STREAM IS THE DEFAULT. It used to be opt-in through an environment variable, and an
# environment variable gets forgotten: whoever built without it silently got the SIO executor,
# which is what was happening to dkong, asteroids and the VPy Snow Bros. Now it has to be
# turned OFF by hand (UVM2_PIO_STREAM=0), and then `uvm2_bus.h` breaks the build unless you are
# a bench.
if(NOT DEFINED ENV{UVM2_PIO_STREAM} OR NOT "$ENV{UVM2_PIO_STREAM}" STREQUAL "0")
    # There is no second .a: the stream's ABI comes out of the SAME wrapper as the drawing
    # layer, because a staticlib requires a panic handler and two do not fit in one link. See
    # vectrex-draw/cabi/src/lib.rs.
    set(VECTREX_CABI_FEATURES --features bus)
    target_compile_definitions(${UVM2_NAME} PRIVATE UVM2_PIO_STREAM=1)
endif()

set(VECTREX_DRAW_LIB
    "${VECTREX_DRAW_DIR}/cabi/target/${VECTREX_DRAW_TARGET}/release/libvectrex_draw_cabi.a")

find_program(CARGO_EXE cargo)
if(NOT CARGO_EXE)
    message(FATAL_ERROR
        "cargo not found. The shared drawing layer is Rust: "
        "install rustup and run `rustup target add ${VECTREX_DRAW_TARGET}`.")
endif()

# THE STREAM LIST'S SIZE, PER GAME. Two buffers of 32-bit words in vectrex-bus (LIST_BUF),
# 98 KB at the default value, and in an image that lives entirely in 496 KB they compete with
# uvm2_draw.c's command cap. The difference is that overflowing THIS one only costs an early
# DMA firing, while overflowing the command cap LOSES DRAWING — see the long note in
# vectrex-bus/src/lib.rs. A tight game lowers this one and raises that one:
#
#     make uvm2 UVM2_LIST_MAX=8192 UVM2_CMD_CAPACITY=14336
#
# It goes through an environment variable because what reads it is `option_env!` inside the
# Rust crate. Cargo records that read in the build's fingerprint, so changing it REBUILDS the
# .a; the .a is shared between games, so alternating between two values costs a rebuild of the
# crate, not a wrong image.
if(DEFINED ENV{UVM2_LIST_MAX} AND NOT "$ENV{UVM2_LIST_MAX}" STREQUAL "")
    message(STATUS "UVM2_LIST_MAX: stream list of $ENV{UVM2_LIST_MAX} words (vectrex-bus uses 12288)")
    set(VECTREX_CARGO_ENV ${CMAKE_COMMAND} -E env VECTREX_LIST_MAX=$ENV{UVM2_LIST_MAX})
endif()

add_custom_target(vectrex_draw_lib ALL
    COMMAND ${VECTREX_CARGO_ENV} ${CARGO_EXE} build --release --target ${VECTREX_DRAW_TARGET}
            --manifest-path "${VECTREX_DRAW_DIR}/cabi/Cargo.toml"
            ${VECTREX_CABI_FEATURES}
    BYPRODUCTS ${VECTREX_DRAW_LIB}
    COMMENT "shared drawing layer (vectrex-draw)")
add_dependencies(${UVM2_NAME} vectrex_draw_lib)
target_link_libraries(${UVM2_NAME} ${VECTREX_DRAW_LIB})
target_compile_definitions(${UVM2_NAME} PRIVATE UVM2_VECTREX_DRAW=1)

# ── THE PIO+DMA BUS STREAM, SHARED ───────────────────────────────────────────
#
# The same crate our own cartridge's firmware uses (vectrex-bus), linked here as a staticlib.
# It is not a port and not a transcription: it is THE SAME driver and THE SAME already-assembled
# PIO program, with the `out` width patched at install time (25 pins there, 27 here). The only
# thing specific to this board is how the bus word is assembled, which already lived in
# uvm2_bus.h because the address is split here.

# Keep the SVC handler alive. Nothing in C calls uvm2_svc_handler — it is reached
# only through the vector table — so --gc-sections drops its section, and the
# .thumb_set alias to it disappears with it, silently leaving crt0's weak
# `bkpt #0` stub installed. Naming it as a link root is what makes the override
# actually happen; without this every libvpy builtin faults on its first svc.
target_link_options(${UVM2_NAME} PRIVATE -Wl,--undefined=uvm2_svc_handler)

# VPY_RAM. VPy programs keep their variables at ABSOLUTE addresses
# (0x2007C000..0x20080000, see vpy_codegen arm/ram_layout.rs). uvm2_game.ld had a region for
# them; the pico-sdk's script does not — its RAM runs to 0x20080000 and it can put .bss right
# on top, which is silent corruption in a large game.
#
# The generated .S declares an empty .vpyram section (NOBITS) and this pins it there: if .bss
# ever overlapped it, ld says so and the build FAILS, which is infinitely better than finding
# out on the screen. For C games the section does not exist and the option does nothing.
target_link_options(${UVM2_NAME} PRIVATE -Wl,--section-start=.vpyram=0x2007C000)

# No USB/UART stdio: the cart has neither, and enabling it drags TinyUSB into a
# 50 Hz draw loop.
pico_enable_stdio_uart(${UVM2_NAME} 0)
pico_enable_stdio_usb(${UVM2_NAME} 0)

# pico_add_extra_outputs derives the .bin/.uf2 names from the executable, and
# refuses one with no extension. The SDK normally sets this globally; set it on
# the target so we do not depend on where in the include order that happens.
set_target_properties(${UVM2_NAME} PROPERTIES SUFFIX ".elf")

# The .uf2 only makes sense for an image the bootrom starts. A payload linked into PSRAM is
# loaded by OUR loader, and besides, elf2uf2 rejects it: for a `no_flash` binary it expects SRAM
# addresses and 0x11000000 is not one ("entry point is not in mapped part of file"). Worse, when
# it fails it DELETES the .elf, which is exactly what we need. So there we keep the ELF and make
# the .bin ourselves.
if(DEFINED ENV{UVM2_LOAD_PSRAM} AND NOT "$ENV{UVM2_LOAD_PSRAM}" STREQUAL "0")
    message(STATUS "UVM2_LOAD_PSRAM: .bin only (our loader loads it, not the bootrom)")
    pico_add_bin_output(${UVM2_NAME})
else()
    pico_add_extra_outputs(${UVM2_NAME})
endif()

# Wrap the flat image in the .um2 header the multicart reads.
# A PSRAM payload is not a module: the multicart does not load it, our loader does. Packaging it
# as a .um2 would only be confusing on the card.
set(UVM2_PACKAGER "${UVM2_SDK_DIR}/../tools/package_um2.py")
if(NOT (DEFINED ENV{UVM2_LOAD_PSRAM} AND NOT "$ENV{UVM2_LOAD_PSRAM}" STREQUAL "0"))
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(TARGET ${UVM2_NAME} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${UVM2_PACKAGER}
                $<TARGET_FILE_DIR:${UVM2_NAME}>/${UVM2_NAME}.bin
                --out $<TARGET_FILE_DIR:${UVM2_NAME}>/${UVM2_NAME}.um2
        COMMENT "packaging ${UVM2_NAME}.um2")
endif()
