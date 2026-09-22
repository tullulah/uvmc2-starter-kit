/* rp2350_game_sram.ld — the game in the cartridge's internal SRAM.
 *
 * Code, rodata and .data at 0x20040000 and .bss in PSRAM: the 220 KB gap between the BIOS's
 * .bss and the game's stack (0x20077000). The BIOS sees from the header that the entry point
 * falls in that range, copies the .bin from PSRAM to SRAM and starts it there. For whatever
 * fits: the PSRAM is reached over QSPI with a 16 KB XIP cache, and an interpreter with its
 * tables does not fit in that (one of the ports ran at 1 fps). */
MEMORY {
    GAME_RAM (rwx) : ORIGIN = 0x20040000, LENGTH = 220K
    /* .bss (emulated memory, dispatch tables) stays in PSRAM: it is what does not fit. It
     * starts past the raw .bin the loader leaves at 0x11000000 (not strictly necessary, but
     * this way the initial zeroing does not stomp on anything still being read). */
    GAME_BSS (rw)  : ORIGIN = 0x11080000, LENGTH = 7168K
}

ENTRY(game_main)

SECTIONS {
    /* rp2350_start.s copies .sram_text; here the code already runs from SRAM. */
    PROVIDE(__sram_text_lma = 0); PROVIDE(__sram_text_vma = 0); PROVIDE(__sram_text_end = 0);
    .game_rom 0x20040000 : {
        KEEP(*(.game_rom))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)

        /* C++ static constructors. A freestanding image has no C runtime to run
         * these, so rp2350_start.s walks the array before main(). Without it a
         * C++ game whose globals have constructors starts with them unbuilt —
         * it links and boots, then misbehaves far from the cause. KEEP, or
         * --gc-sections drops the array entirely. */
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

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > GAME_BSS

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
