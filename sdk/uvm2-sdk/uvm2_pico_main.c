/* uvm2_pico_main.c — the seam between the pico-sdk runtime and a UVM2 game.
 *
 * With uvm2_start.s we owned the entry: it built the vector table, cleared .bss,
 * ran the C++ constructors, called uvm2_runtime_init() and then main(). The
 * pico-sdk crt0 now does all of that EXCEPT the last two — it hands us a fully
 * initialised C environment and calls main().
 *
 * So main() here is ours: bring the Vectrex side up, then run the game. The game
 * keeps its own `main` and is compiled with -Dmain=uvm2_game_main, which is a
 * rename rather than an edit — none of the ~45 game sources change.
 *
 * Why the runtime init cannot simply live inside each game: it must run before
 * ANY C touches the bus or the LED, and it is where a switched-off console is
 * detected (uvm2_clock_calibrate). Keeping it here means one place gets it right.
 */
#include "uvm2_bus.h"
#include "uvm2_psram.h"
#ifdef UVM2_PIO_STREAM
#include "uvm2_bus_stream.h"
#endif
#include "uvm2_led.h"

int uvm2_game_main(void);          /* the game's own main(), renamed at compile time */
void uvm2_runtime_init(void);      /* uvm2_svc.c */
void uvm2_romzip_load(void);       /* uvm2_romzip.c: reads roms/<game>.zip off the SD */
#ifdef UVM2_DUAL_CORE
void uvm2_core1_start(void);       /* uvm2_core1.c */
#endif

/* PANIC, WITH NO printf IN THE WAY.
 *
 * The SDK's panic prints the reason, and here printing is worse than useless: there is no
 * console and vsnprintf itself runs off the stack (measured: BFAR = 0x20082000, exactly the
 * top of SRAM), so the failure you see is the MESSENGER's and the reason is lost.
 *
 * By defining PICO_PANIC_FUNCTION=uvm2_panic_stash, `panic` becomes a forwarder that calls
 * in here with the message in r0. We save the pointer and stop. The text is resolved
 * afterwards in the ELF, which is where it lives.
 *
 *   0x20080230  marker 0x9A91C000
 *   0x20080234  pointer to the format string (into the payload's .rodata)
 */
void __attribute__((noreturn)) uvm2_panic_stash(const char *fmt, ...);
void uvm2_panic_stash(const char *fmt, ...)
{
    volatile unsigned *g = (volatile unsigned *)0x20080230u;
    g[0] = 0x9A91C000u;
    g[1] = (unsigned)fmt;
    for (;;) { }
}

#ifdef UVM2_PSRAM_IMAGE
/* OUR startup reset, instead of the SDK's (which is __weak).
 *
 * THE PROBLEM: the UVM2's PSRAM chip select is not a QSPI bus pin, it is a GPIO OF BANK 0
 * (uvm2_psram.c: configure_cs1_pad writes to pads_bank0/io_bank0). The SDK's
 * runtime_init_early_resets resets IO_BANK0 and PADS_BANK0, which wipes that configuration
 * and the chip — the one we are executing from — stops answering.
 *
 * Disabling the whole step (PICO_RUNTIME_SKIP_INIT_EARLY_RESETS) does boot, but it leaves
 * the peripherals however the LOADER left them instead of in a known state, and you can
 * tell: unresponsive controllers and shimmer. So we do the same thing the SDK does, with
 * two bits fewer.
 *
 * Masks copied from the SDK itself (0xEFEF3B7F assert / 0x03F3FFF6 release) with bits 6
 * (IO_BANK0) and 9 (PADS_BANK0) removed, read out of resets.h and not out of memory. */
void runtime_init_early_resets(void)
{
    volatile unsigned *set  = (volatile unsigned *)(0x40020000u + 0x2000u);
    volatile unsigned *clr  = (volatile unsigned *)(0x40020000u + 0x3000u);
    volatile unsigned *done = (volatile unsigned *)(0x40020000u + 0x0008u);
    const unsigned bank0   = (1u << 6) | (1u << 9);      /* IO_BANK0 | PADS_BANK0 */
    const unsigned assert_ = 0xEFEF3B7Fu & ~bank0;
    const unsigned release = 0x03F3FFF6u & ~bank0;

    *set = assert_;
    *clr = release;
    while ((*done & release) != release) { }
}
#endif

int uvm2_psram_ready = -1;   /* 1 = the PSRAM answered, 0 = it did not, -1 = never tried */

int main(void)
{
#ifndef UVM2_STEP_OWNS_INIT
    uvm2_runtime_init();
    /* With the bus already taken: 256 E periods, about 170 us. It leaves the clock/E ratio
     * in uvm2_cycles_per_e_q8, which is what decides whether the PIO stream's phase
     * calibration holds on this board. */
    uvm2_measure_e();
#ifdef UVM2_PIO_STREAM
    /* After measuring E and with the bus already taken: the SM synchronises against ~E, so
     * there is no point starting it before the clock is there. */
    uvm2_stream_start();
#endif
#  if defined(UVM2_CMDS_IN_PSRAM) || defined(UVM2_PSRAM_START) || defined(UVM2_ROMZIP_IN_PSRAM)
    /* UVM2_PSRAM_START: powers the PSRAM up WITHOUT using it, so that "the list lives
     * there" and "the chip is active" can be tested separately. The PIO stream works with
     * the chip off and collapses into a diagonal with the list in PSRAM — even copying it
     * to SRAM before replaying, so reading it is not what breaks. This knob isolates the
     * one variable that was still shared: the QMI being up. */
    /* THE PSRAM, BEFORE ANYONE EMITS A COMMAND, and after uvm2_runtime_init.
     *
     * That order is not cosmetic: the UVM2's PSRAM chip select is a bank-0 GPIO, and
     * runtime_init_early_resets resets IO_BANK0 and PADS_BANK0. Configuring it first is
     * configuring it to be wiped — the fault that cost us the two-stage loader.
     *
     * And it goes HERE and not in uvm2_bus_init() because on the pico-sdk path that
     * function IS NOT CALLED: main() invokes the three steps separately. Putting it there
     * compiled, linked nothing, and left the PSRAM uninitialised; the symptom was a command
     * list that read back as all zeros, i.e. a black screen with the frame counter
     * climbing. It showed up by comparing commands against bus_cycles: 1 cycle per command
     * instead of ~7, because a zero command carries no delay. */
    /* FIRST THE QSPI BUS PINS, which are not ours yet.
     *
     * The multicart launcher does not jump to the module: it reboots through the bootrom
     * with RAM_IMAGE. And the bootrom, knowing it will not execute from flash, leaves the
     * six QSPI pins at FUNCSEL = NULL. The QMI accepts the transfers, BUSY behaves, CS
     * moves — and not one pin drives. Two weeks of "the chip is dead" were this. */
    uvm2_qmi_bring_up();
    uvm2_psram_ready = uvm2_psram_init();
#  endif
    /* The romset, BEFORE the game: its main() calls aae_load_roms() first thing. On our own
     * cartridge the firmware does this; here the UVM2 does not, so we have to read it
     * ourselves. If it fails, the game paints its "no romset" sign and does not hang. */
    uvm2_romzip_load();
#  ifdef UVM2_DUAL_CORE
    /* After the runtime init, never before: core 1 takes the bus from here on,
     * and it must not start until the VIA has been programmed and the 6809 is
     * halted. */
    uvm2_core1_start();
#  endif
#endif
    /* UVM2_STEP_OWNS_INIT: a diagnostic image that brings the machine up itself,
     * one stage at a time. Calling runtime_init here would do every stage before
     * it ever got a say. */
    return uvm2_game_main();
}

/* A HARDFAULT WITH A WITNESS. The pico-sdk default for isr_hardfault is `bkpt`, which
 * without a debugger attached is a silent lockup: on the UVM2 (no SWD on the bench) it
 * looks exactly like a game spinning. Red LED, solid, means the CPU faulted — a bad
 * pointer, a stack overflow, an unaligned access — and not a loop. Both cores share the
 * vector table, so either core landing here lights it. */
void isr_hardfault(void)
{
    for (;;) uvm2_led_rgb(255, 0, 0);
}
