@ rp2350_start.s — RAM-loaded RP2350 game header + C entry stub.
@
@ SYS_LAUNCH reads the game .bin into GAME_RAM (0x20040000), checks the 'VPy2'
@ magic at offset 0, then bx'es to the entry pointer at game_header+4. The BIOS
@ has already done clocks/pins/VIA init and set a valid stack pointer, so the
@ game runs on the BIOS stack — game_main only has to give C a zeroed .bss and
@ call main(). (Mirrors the VPy arm backend's game_main: no SP setup, zero RAM,
@ run.) The .bin (objcopy) contains .text/.rodata/.data but NOT .bss, so .bss is
@ SRAM power-on garbage until we clear it.
    .syntax unified
    .cpu cortex-m33
    .thumb

@ --- image header at offset 0 (linker KEEPs .game_rom first at 0x20040000) ---
    .section .game_rom, "a", %progbits
    .global game_header
    .type game_header, %object
@ reserved[0] = DUAL-CORE flag. 0 = single-core (default; the BIOS runs the game
@ on core 0, drawing inline via svc — unchanged for VPy games). 0x44430001
@ ("DC" v1) = the game records its draws to the shared RAM buffer (svc-free) and
@ the BIOS launches it on core 1 while core 0 materialises the beam in parallel.
@ A game opts in at build time: -Wa,--defsym,DUAL_CORE_FLAG=0x44430001 (and it
@ must be built with -DVPY_DUAL_CORE so sdk_rp2350.c uses the RAM-record path).
    .ifndef DUAL_CORE_FLAG
    .set DUAL_CORE_FLAG, 0
    .endif
game_header:
    .word 0x32795056        @ GAME_MAGIC 'VPy2' (little-endian)
    .word game_main         @ entry point (linker sets the thumb bit)
    .word DUAL_CORE_FLAG    @ reserved[0] = dual-core flag (see above)
    .word 0x00000000        @ reserved[1]: the LAUNCHER writes the romset descriptor
                            @ ('RMZ1') here, so the game cannot use it.

@ --- which romset this game comes from -------------------------------------
@ The launcher used to look for the zip by the .BIN's name, and that is only right
@ by accident: aae_asteroids_sd.bin needs asteroid.zip. So the game SAYS what its
@ romset is called, and the launcher reads that name.
@
@ It goes behind the header and carries ITS OWN MAGIC so images that do not declare
@ it are not broken: anything without 'RSET' here will have code there instead, and
@ the launcher falls back to deducing it from the file name. A game declares it by
@ defining the symbol game_romset_name; it is WEAK, so without it the word is zero.
    .word 0x54455352        @ 'RSET'
    .weak game_romset_name
    .word game_romset_name  @ -> NUL-terminated string, or 0 if not declared
    .size game_header, . - game_header

@ --- C entry point the BIOS jumps to ---
    .text
    .align 2
    .global game_main
    .type game_main, %function
    .thumb_func
game_main:
    @ copy .sram_text [lma, lma+size) -> [vma, end): hot code the game asked to run
    @ from internal SRAM instead of PSRAM (see rp2350_game_ram.ld). Empty = no-op.
    ldr     r0, =__sram_text_lma
    ldr     r1, =__sram_text_vma
    ldr     r2, =__sram_text_end
gm_st_loop:
    cmp     r1, r2
    bhs     gm_st_done
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       gm_st_loop
gm_st_done:
    @ zero .bss [_bss_start, _bss_end) — RP2350 SRAM is not zero-initialised
    ldr     r0, =_bss_start
    ldr     r1, =_bss_end
    movs    r2, #0
gm_bss_loop:
    cmp     r0, r1
    bhs     gm_bss_done
    str     r2, [r0], #4
    b       gm_bss_loop
gm_bss_done:
    @ Run the C++ static constructors — nothing else will (see the linker
    @ script). A C game's array is empty, so this costs it four instructions.
    ldr     r0, =__init_array_start
    ldr     r1, =__init_array_end
gm_ctor_loop:
    cmp     r0, r1
    bhs     gm_ctor_done
    ldr     r2, [r0], #4
    push    {r0, r1}
    blx     r2
    pop     {r0, r1}
    b       gm_ctor_loop
gm_ctor_done:
    bl      main
gm_spin:                    @ main() loops forever; spin if it ever returns
    b       gm_spin
    .size game_main, . - game_main
