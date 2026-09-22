/* rp2350_game_ram.ld — link a VPy game to run from the cartridge's 8 MB PSRAM.
 *
 * 2026-08-02: GAME_RAM moved from internal SRAM to the **PSRAM XIP window at
 * 0x11000000**. Every game gets the full 8 MB; nothing is limited at compile time
 * because of where the cart happens to put it. The old 476 KB SRAM window is gone,
 * and rp2350_game_ram_8m.ld (which claimed 8 MB at 0x20010000 — a fiction, since
 * only 512 KB of SRAM exists there) is now identical to this file.
 *
 * The split that makes this work:
 *   0x11000000 - 0x117FFFFF   GAME_RAM — code + rodata + data + bss, in PSRAM.
 *   0x2007F000                SP, set by launch_trampoline — the stack stays in
 *                             internal SRAM and grows DOWN, so it is fast and is
 *                             never in the QSPI window.
 *   0x2007F000 - 0x2007FFFF   VPy runtime RAM (game variables; ram_layout.rs),
 *                             also internal SRAM.
 *
 * The BIOS keeps ALL of internal SRAM to itself now, so the old warnings about
 * overwriting its .bss / control block / core-1 stack no longer apply.
 *
 * REQUIREMENTS, both firmware-side: the PSRAM must be initialised and its XIP
 * window armed (psram::init) BEFORE a game is loaded, and XIP_CTRL.WRITABLE_M1
 * must be set or the loader's writes are silently discarded.
 *
 * IMPORTANT: the origin appears TWICE — in MEMORY and hardcoded on .game_rom.
 * Changing one and not the other is what made every VPy game FAULT once before.
 */

MEMORY {
    GAME_RAM (rwx) : ORIGIN = 0x11000000, LENGTH = 8192K
    /* The gap left free in internal SRAM while a game runs: between the BIOS's .bss and
     * the game's stack (0x20077000). It overlaps the menu's preview buffer, which a game
     * never uses. For hot tables that do not fit in the PSRAM's XIP cache. */
    GAME_SRAM (rw)  : ORIGIN = 0x20040000, LENGTH = 220K
}

ENTRY(game_main)

SECTIONS {
    .game_rom 0x11000000 : {
        KEEP(*(.game_rom))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)

        /* C++ static constructors. A freestanding image has no C runtime to run
         * these, so rp2350_start.s walks the array before main(). Without it a
         * C++ game whose globals have constructors starts with them unbuilt —
         * it links and boots, then misbehaves far from the cause. KEEP, or
         * --gc-sections drops the array entirely.
         *
         * IT WAS MISSING HERE, and rp2350_game_ram.ld's header already claimed the two
         * scripts were identical. They were not: rp2350_start.s references these two
         * symbols unconditionally, so every game linked with this script died with
         * "undefined reference to __init_array_start" — a message that says nothing about
         * memory and sends you looking for the fault where it is not. */
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
        . = ALIGN(4);
    } > GAME_RAM

    .data : {
        *(.data)
        *(.data.*)
        . = ALIGN(4);
    } > GAME_RAM

    /* Hot code in internal SRAM (2026-09-16): LMA inside the PSRAM image, VMA in
     * GAME_SRAM; rp2350_start.s copies it before main(). Whatever the game marks
     * AAE_SRAM_TEXT (aae-src/aae_fast.h): the 6809 cores and the memory handlers.
     * Empty, and free, in a game that marks nothing. */
    .sram_text : ALIGN(4) {
        __sram_text_vma = .;
        *(.sram_text)
        *(.sram_text.*)
        . = ALIGN(4);
        __sram_text_end = .;
    } > GAME_SRAM AT> GAME_RAM
    __sram_text_lma = LOADADDR(.sram_text);

    /* NOLOAD: it does not travel in the .bin and rp2350_start.s does not zero it; whoever
     * uses it initialises it completely (AAE's dispatch table, cpuintrf.c). */
    .sram_fast (NOLOAD) : ALIGN(4) {
        *(.sram_fast)
        *(.sram_fast.*)
    } > GAME_SRAM

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > GAME_RAM

    /* An EMPTY unwind table, but with its symbols.
     *
     * The sections are discarded (below) because a freestanding program unwinds nothing,
     * but libgcc's unwinder references __exidx_start/__exidx_end anyway if something drags
     * it in. Games linked with -nostdlib never see it; a game using --specs=nosys.specs
     * does: "undefined reference to __exidx_start" as soon as the ROM loader is added.
     * Declaring them empty is the standard fix: the unwinder finds a table of zero entries
     * and does not unwind, which is correct. PROVIDE so anyone defining them themselves is
     * not overridden. */
    PROVIDE(__exidx_start = .);
    PROVIDE(__exidx_end   = .);

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
