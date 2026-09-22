@ uvm2_svc_entry.s — SVCall entry shim.
@
@ Vector 11 of the generated image points here.  All it does is hand the
@ exception frame to the C dispatcher: r0-r3 of the caller are stacked at
@ offsets 0-12, and a syscall result written to frame[0] is what exception
@ return reloads into r0.
@
@ The frame pointer is taken BEFORE pushing, or the offsets would shift.
@
@ SP is read with a plain MOV rather than `mrs r0, msp`: the image never sets
@ CONTROL.SPSEL, so thread mode always runs on the main stack and the two are
@ the same register. That also keeps the handler runnable on the IDE's Thumb2
@ interpreter, which implements no MRS.

    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .text.uvm2_svc_handler, "ax"
    .global uvm2_svc_handler
    .type   uvm2_svc_handler, %function
    .thumb_func
uvm2_svc_handler:
    mov     r0, sp                  @ → the stacked exception frame
    push    {r4, lr}                @ r4 only to keep the stack 8-byte aligned
    bl      uvm2_svc_dispatch
    pop     {r4, pc}                @ pc = EXC_RETURN → exception return
    .size   uvm2_svc_handler, . - uvm2_svc_handler
