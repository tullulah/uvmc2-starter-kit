/* uvm2_game.ld — Linker script for VPy games running from SD card on the
 * Ultimate Vectrex Multicart 2 (UVM2) by Ralf & Jason.
 *
 * Execution model:
 *   The UVM2 firmware loads the game binary from the SD card into SRAM and
 *   jumps to its entry point. Everything (code + data + bss) lives in RAM.
 *   There is no flash XIP copy — the binary is a flat RAM image.
 *
 * RP2350 SRAM layout (520KB total, 0x20000000 - 0x20081FFF):
 *   0x20000000 - 0x2007BFFF  (496KB) Game code + data  ← this script
 *   0x2007C000 - 0x2007FFFF  (16KB)  VPy runtime stack / variables
 *
 * The UVM2 firmware loads the game starting at 0x20000000 — confirmed by
 * analysis of the Asteroids reference .um2 file (load_addr field = 0x20000000,
 * SP initial value = 0x20082000 = top of RP2350 SRAM).
 */

MEMORY {
    GAME_RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 496K
    VPY_RAM  (rw)  : ORIGIN = 0x2007C000, LENGTH = 16K
}

ENTRY(game_main)

SECTIONS {
    /* All code and read-only data go directly into RAM (no flash copy) */
    .text 0x20000000 : {
        KEEP(*(.vectors))    /* ARM Cortex-M vector table — MUST be first */
        KEEP(*(.game_rom))   /* game header / magic word */
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)

        /* C++ static constructors. A freestanding image has no C runtime to
         * run these, so uvm2_start.s walks the array itself before main() —
         * without it a game whose globals have constructors starts with them
         * unbuilt, which fails silently rather than loudly. KEEP, or
         * --gc-sections drops the whole array. */
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;

        . = ALIGN(4);
        _etext = .;
    } > GAME_RAM

    /* Initialized data — LMA == VMA (already in RAM after SD load) */
    .data : {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        _data_end = .;
    } > GAME_RAM

    /* Zero-initialized data — startup code must zero this region */
    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > GAME_RAM

    /* newlib's sbrk grows the heap from `end`. A game linked against a real
     * libc references it even if it never allocates, so provide it right after
     * .bss — PROVIDE, so a project that sets its own via --defsym still wins. */
    PROVIDE(end = _bss_end);
    PROVIDE(_end = _bss_end);

    /* VPy runtime variables (stack pointer, camera, SFX state, etc.) */
    .vpy_runtime : {
        _vpy_ram_start = .;
        . = . + 0x3000;   /* 12KB for VPy generated variables */
        _vpy_ram_end = .;
    } > VPY_RAM

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
