@ uvm2_pico_svc.S — install our SVCall handler into the pico-sdk vector table.
@
@ crt0.S declares `isr_svcall` WEAK (decl_isr_bkpt: a bare `bkpt #0`), so a STRONG
@ definition of that name replaces it with no patching of the SDK.
@
@ A real branching stub, not `.thumb_set isr_svcall, uvm2_svc_handler`: the alias
@ form assembled without complaint but left crt0's weak symbol winning — the
@ vector table still pointed at 0x2000011f, its breakpoint stub. Four bytes and a
@ tail branch is a small price for a definition the linker cannot mistake.
@
@ It matters more than it looks: EVERY libvpy builtin arrives as `svc #N`. Left
@ unfixed, the first one hits crt0's `bkpt` and the game dies on its first draw
@ call — which would look exactly like the boot failure this whole change fixes.
@
@ No stack use, no clobber: the branch is a tail call, so the exception frame
@ uvm2_svc_handler expects is exactly as the core pushed it.
    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .text.isr_svcall, "ax", %progbits
    .global isr_svcall
    .type isr_svcall, %function
    .thumb_func
isr_svcall:
    b.w     uvm2_svc_handler
    .size isr_svcall, . - isr_svcall
