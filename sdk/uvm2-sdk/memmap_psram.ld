/* memmap_psram.ld — link the UVM2 image into the EXTERNAL PSRAM.
 *
 * A literal copy of the pico-sdk's memmap_no_flash.ld (rp2350) with ONE line changed: the
 * RAM region goes from 0x20000000/256k to 0x11000000/8192k. Everything else — IMAGE_DEF,
 * vector table, section order, the scratch banks — is preserved, which is the reason for
 * copying rather than writing a new one.
 *
 * THE QUESTION IT ANSWERS: the cartridge loader reads the load address out of the .um2
 * header and copies there. Does it honour an address in PSRAM? Our probe measured that
 * PSRAM as MUTE, but it measured it from INSIDE an already-loaded game: if the firmware
 * only initialises it when it needs it, that measurement would never have seen it. The only
 * way to know is to ask it to load there.
 *
 * The scratch banks deliberately stay in internal SRAM: they belong to the RP2350, not to
 * the external chip.
 */
/* Based on GCC ARM embedded samples.
   Defines the following symbols for use by code:
    __exidx_start
    __exidx_end
    __etext
    __data_start__
    __preinit_array_start
    __preinit_array_end
    __init_array_start
    __init_array_end
    __fini_array_start
    __fini_array_end
    __data_end__
    __bss_start__
    __bss_end__
    __end__
    end
    __HeapLimit
    __StackLimit
    __StackTop
    __stack (== StackTop)
*/

MEMORY
{
    RAM(rwx) : ORIGIN =  0x11000000, LENGTH = 8192k
    /* WRITABLE DATA DOES NOT GO IN PSRAM.
     *
     * With .data and .bss there, crt0 starts by zeroing tens of KB of PSRAM THROUGH THE
     * CACHE while fetching its own instructions from that same chip. Measured: after that
     * wipe the PSRAM stops answering reads (it returns a periodic 00/CC8C/8888 pattern),
     * runtime_run_initializers swallows a zero from its table, does `blx 0` and jumps to
     * address zero — HardFault with INVSTATE. That was the "PC 11000199 and then black" of
     * a whole afternoon.
     *
     * WHERE THIS REGION STARTS MATTERS. The loader stays resident in the low part of SRAM
     * and carries the payload EMBEDDED inside it: with a 280 KB game, its image reaches to
     * ~0x20049000. If .data landed at 0x20040000, the loader would be stomping on itself
     * while copying — reading out of an array it has just overwritten.
     *
     * 0x20060000 leaves 384 KB for the loader and 128 KB here, and stops at the top at
     * 0x20080000, where the scratch banks and the stack begin. */
    SRAM(rwx) : ORIGIN = 0x20060000, LENGTH = 128k
    /* .bss GOES BACK TO PSRAM, and .data stays in SRAM. Mind the reason, because it
     * changed.
     *
     * .bss was first moved to SRAM in the belief that crt0's zeroing — tens of KB written
     * into the chip it is executing from — was what left the PSRAM mute. The cause turned
     * out to be ANOTHER one: the chip select is a bank-0 GPIO and runtime_init_early_resets
     * was clearing it. With that fixed, .bss in PSRAM has no reason to hurt; moving it only
     * helped because it reduced traffic and so took longer to show.
     *
     * And it is not optional: one of the games has ~830 KB of .bss. It cannot possibly fit
     * in the RP2350's 520 KB of SRAM ("region SRAM overflowed by 576712 bytes"), so for the
     * games that motivated all this, PSRAM is the only option.
     *
     * .data DOES stay in SRAM, and that reason has not changed: the SDK's mutexes are taken
     * with ldaexb/strexb and exclusive accesses do NOT work over XIP memory.
     *
     * MIND WHAT MOVES HERE AND WHAT DOES NOT.
     *
     * .bss YES: crt0 zeroes it itself, it is not copied from anywhere.
     * .data NO: the crt0 of a `no_flash` image HAS NO copy loop — it goes from the CPUID
     * check to zeroing .bss and from there to platform_entry, because it assumes whoever
     * loaded the image already left the data at its final address. Putting it in SRAM
     * leaves it with nobody to bring it in: the initialiser table lives inside .data, so
     * uninitialised memory is read and `blx` jumps to a made-up address (CFSR=0x100,
     * IBUSERR). Verified: 0x20040000 held 31F1080A FE3BB809 ... i.e. noise. So .data goes
     * back to SRAM and the LOADER does the copy, with the addresses it pulls out of the ELF.
     *
     * It is needed because the SDK's mutexes live in .data and are taken with EXCLUSIVE
     * accesses (ldaexb/strexb). Exclusive monitors do not work over XIP-mapped memory: the
     * strexb always fails and the loop never exits. Measured: a game spinning forever at
     * 0x11000292, inside mutex_try_enter_block_until, with no fault and no exception —
     * IPSR=0, simply stuck. */
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
}

ENTRY(_entry_point)

SECTIONS
{
    /* Note unlike RP2040, we start the image with a vector table even for
       NO_FLASH builds. On Arm, the bootrom expects a VT at the start of the
       image by default; on RISC-V, the default is to enter the image at its
       lowest address, so an IMAGE_DEF item is required to specify the
       nondefault entry point. */

    .text : {
        __logical_binary_start = .;
        /* Vectors require 512-byte alignment on v8-M when >48 IRQs are used,
           so we would waste RAM if the vector table were not at the
           start. */
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        __reset_start = .;
        KEEP (*(.reset))
        __reset_end = .;
        /* The three hot-path files are EXCLUDED here and picked up below, in .data, which
         * lives in SRAM. Without the exclusion, this generic rule would claim them. */
        *(EXCLUDE_FILE(*uvm2_bus.c.obj *uvm2_draw.c.obj *uvm2_svc.c.obj *uvm2_svc_entry.s.obj) .text*)
        . = ALIGN(4);
        *(.init)
        *(.fini)
        /* Pull all c'tors into .text */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* Followed by destructors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        *(.eh_frame*)
    } > RAM

    .rodata : {
        . = ALIGN(4);
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > RAM

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > RAM

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > RAM
    __exidx_end = .;

    /* Machine inspectable binary information */
    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > RAM
    __binary_info_end = .;
    . = ALIGN(4);

    .data : {
        __data_start__ = .;
        /* THE BUS HOT PATH, IN SRAM.
         *
         * It lives here — not in .text, which is in PSRAM — because the executor writes ONCE
         * per E period, and a cache miss in the middle of that window makes the write land
         * in the NEXT period: the stroke comes out with the wrong ramp. That shows up as
         * shimmer and the odd very bright stray vector, and it does NOT appear in the
         * counters — measured with a game running from PSRAM: bus_cycles 11745 of 30000,
         * dropped 0, overrun 13 of 3677 frames. There is time to spare; what fails is WHEN
         * each write lands, not how many there are.
         *
         * Being inside .data, it travels in the image (AT> RAM) and the loader copies it. */
        *(.time_critical*)
        /* And the three files whole. Marking only uvm2_exec removed the bright vector but
         * the shimmer stayed: between strokes the draw layer and the svc handler that closes
         * the frame also run, and from PSRAM every cache miss moves the moment each write
         * lands. */
        *uvm2_bus.c.obj(.text*)
        *uvm2_draw.c.obj(.text*)
        *uvm2_svc.c.obj(.text*)
        /* The svc handler is in ASSEMBLY, not in the .c of the same name. */
        *uvm2_svc_entry.s.obj(.text*)
        . = ALIGN(4);
        *(vtable)
        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.jcr)
        . = ALIGN(4);
   } > SRAM AT> RAM

    .tdata : {
        . = ALIGN(4);
		*(.tdata .tdata.* .gnu.linkonce.td.*)
        /* All data end */
        __tdata_end = .;
    } > SRAM AT> RAM
    PROVIDE(__data_end__ = .);

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM
    /* __etext is (for backwards compatibility) the name of the .data init source pointer (...) */
    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)

        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;

        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    /* historically on GCC sbrk was growing past __HeapLimit to __StackLimit, however
       to be more compatible, we now set __HeapLimit explicitly to where the end of the heap is */
    /* The heap and the stack live in SRAM now that .data/.bss have moved; computing these
     * limits over RAM (the PSRAM) left __StackLimit at 0x11800000, i.e. pointing at another
     * chip. It causes no fault by itself — _entry_point sets MSPLIM to 0 — but it is a lying
     * number waiting for somebody to use it. */
    __HeapLimit = ORIGIN(SRAM) + LENGTH(SRAM);

    /* Start and end symbols must be word-aligned */
    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y
    __scratch_y_source__ = LOADADDR(.scratch_y);

    /* .stack*_dummy section doesn't contains any symbols. It is only
     * used for linker to calculate size of stack sections, and assign
     * values to stack symbols later
     *
     * stack1 section may be empty/missing if platform_launch_core1 is not used */

    /* by default we put core 0 stack at the end of scratch Y, so that if core 1
     * stack is not used then all of SCRATCH_X is free.
     */
    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    .stack_dummy (NOLOAD):
    {
        KEEP(*(.stack*))
    } > SCRATCH_Y

    /* stack limit is poorly named, but historically is maximum heap ptr */
    __StackLimit = __StackBottom;
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);

    /* picolibc and LLVM */
    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    /* llvm-libc */
    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    /* Check if data + heap + stack exceeds RAM limit */
    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")

    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")

    /* todo assert on extra code */
}

