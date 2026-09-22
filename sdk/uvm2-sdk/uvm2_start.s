@ uvm2_start.s — entry stub for a C/C++ game built for the UVM2.
@
@ The RP2350 cartridge is handed a game by a BIOS that has already brought the
@ machine up, so its games start at a bare `game_main`. The UVM2 has no BIOS:
@ the firmware copies the image to SRAM and jumps to the reset vector, and
@ everything after that — clocks, pads, the bus, the syscall handler — is ours.
@ So a UVM2 image begins with a REAL Cortex-M vector table, and vector 11
@ points at our own SVCall handler, which is what makes `svc #N` work at all.
@
@ This is the C counterpart of the table the VPy code generator emits inline for its own
@ UVM2 backend; keep the two in step.

    .syntax unified
    .cpu cortex-m33
    .thumb

@ --- vector table, first bytes of the image ---
    .section .vectors, "ax"
    .align 2
    .word 0x20082000            @ initial SP = top of RP2350 SRAM (520 KB)
    .word game_main + 1         @ Reset_Handler (Thumb bit set)
    .word _uvm2_default_handler @ NMI
    .word _uvm2_default_handler @ HardFault
    .word _uvm2_default_handler @ MemManage
    .word _uvm2_default_handler @ BusFault
    .word _uvm2_default_handler @ UsageFault
    .word _uvm2_default_handler @ SecureFault
    .word _uvm2_default_handler @ reserved
    .word _uvm2_default_handler @ reserved
    .word _uvm2_default_handler @ reserved
    .word uvm2_svc_handler      @ SVCall — every libvpy builtin arrives here
    .word _uvm2_default_handler @ DebugMon
    .word _uvm2_default_handler @ reserved
    .word _uvm2_default_handler @ PendSV
    .word _uvm2_default_handler @ SysTick
    .rept 52                    @ external IRQs
    .word _uvm2_default_handler
    .endr

@ --- IMAGE_DEF, the block the RP2350 demands before it recognises an image ---
@ Without this, the image is not an image: it is a heap of bytes that starts with
@ something resembling a vector table. This is the SECOND time this block broke the
@ UVM2 chain for us — the first time it was papered over by building that target
@ with the pico-sdk, which emits it by itself, and this path, written by hand, ended
@ up without it.
@
@ MEASURED, not assumed: the two reference .um2 images that DO boot on the multicart
@ carry it at payload+0x15c, and the seven words are byte-identical in both. None of
@ our images had it, neither the 320 KB one nor the 5 KB one, and none ever booted.
@
@ It goes inside .vectors so it lands right behind the table, well inside the first
@ 4 KB, which is where it has to be looked for.
    .word 0xffffded3            @ start-of-block marker
    .word 0x10210142            @ IMAGE_DEF: executable, ARM, RP2350
    .word 0x00000203            @ VECTOR_TABLE item, 2 words
    .word 0x20000000            @   -> the table sits at the load base
    .word 0x000003ff            @ LAST_ITEM
    .word 0x00000000            @ no next block
    .word 0xab123579            @ end marker

    .text
    .align 2

@ Unexpected exceptions park here rather than running off into RAM.
    .global _uvm2_default_handler
    .thumb_func
_uvm2_default_handler:
    b       _uvm2_default_handler

@ --- entry ---
@ uvm2_runtime_init clears .bss itself (it must run before any C touches
@ static state), takes the vector table over, configures the pads and halts
@ the 6809. Only then is it safe to call into C.
    .global game_main
    .type game_main, %function
    .thumb_func
game_main:
    bl      uvm2_runtime_init

    @ Run the C++ static constructors. Nothing else will: this image has no C
    @ runtime, and a global object whose constructor never ran looks exactly
    @ like a logic bug at the far end of the program.
    ldr     r4, =__init_array_start
    ldr     r5, =__init_array_end
gm_ctor_loop:
    cmp     r4, r5
    bhs     gm_ctor_done
    ldr     r0, [r4], #4
    blx     r0
    b       gm_ctor_loop
gm_ctor_done:

    bl      main
gm_spin:                        @ main() loops forever; spin if it ever returns
    b       gm_spin
    .size game_main, . - game_main
