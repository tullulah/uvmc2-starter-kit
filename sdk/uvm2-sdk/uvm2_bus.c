/*
 * uvm2_bus.c — halt-mode bus transport for the Ultimate Vectrex Multicart 2.
 *
 * Three jobs:
 *   1. uvm2_bus_init()  — take the machine over from the UVM2 firmware and halt
 *                         the 6809 once, for good.
 *   2. uvm2_exec()      — replay a recorded command stream, one VIA write per
 *                         Vectrex bus cycle.  This is the hot loop.
 *   3. single accesses  — one-off writes and the read cycle used for input.
 *
 * Bus phase (VectrexCart::WriteVia, proven on hardware):
 *      wait CLK HIGH → drive address+data+R/W → wait CLK LOW
 * i.e. the bus is updated just after the rising edge and held across the falling
 * edge, where the VIA latches.  Doing it the other way round — driving during
 * the low phase and releasing on the rising edge — changes the bus exactly at
 * the latch point and writes garbage.
 */

#include "uvm2_bus.h"
#ifdef UVM2_PIO_STREAM
#include "uvm2_bus_stream.h"
#endif

uvm2_stats_t uvm2_stats;

/* Linker-provided; C code needs its .bss cleared and nothing else does it. */
extern uint32_t _bss_start, _bss_end;

/* ── Edge waits ───────────────────────────────────────────────────────────────
 * Deliberately not a shared helper with a function call in the middle: the
 * executor has ~333 ns (about 50 CPU cycles at 150 MHz) between the rising edge
 * and the latch to get the new value onto the pins. */
#define UVM2_WAIT_CLK_HIGH()  while ((UVM2_GPIO_IN & UVM2_CLK_MASK) == 0) { }
#define UVM2_WAIT_CLK_LOW()   while ((UVM2_GPIO_IN & UVM2_CLK_MASK) != 0) { }

/* Glitch-free masked update: one store, no intermediate state.  A CLR+SET pair
 * would take the address through $0000 with R/W already low. */
static inline void uvm2_put_masked(uint32_t value, uint32_t mask)
{
    UVM2_GPIO_OUT_XOR = (UVM2_GPIO_OUT ^ value) & mask;
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

#define SCB_VTOR        UVM2_MMIO(0xE000ED08u)
#define SYST_CSR        UVM2_MMIO(0xE000E010u)
#define NVIC_ICER(i)    UVM2_MMIO(0xE000E180u + 4u * (i))
#define NVIC_ICPR(i)    UVM2_MMIO(0xE000E280u + 4u * (i))
#define PADS_BANK0(n)   UVM2_MMIO(0x40038000u + 4u + 4u * (n))
#define IO_BANK0_CTRL(n) UVM2_MMIO(0x40028000u + 8u * (n) + 4u)

/* GPIO0-27, 29, 31 — 28 and 30 belong to the cartridge (30 = WS2812 LED). */
#define UVM2_CART_PINS      0xAFFFFFFFu
/* PB6, /IRQ, /HALT, /NMI, CLK get pull-ups, as the reference does. */
#define UVM2_PULLUP_PINS    0xA8C00000u
/* SCHMITT | DRIVE=8mA | IE.  OD and ISO clear — RP2350 pads power up isolated,
 * so writing the whole register is what actually connects the pad. */
#define UVM2_PAD_BASE       0x62u
#define UVM2_PAD_PUE        0x08u
#define UVM2_FUNC_SIO       5u

/* Step 1 — must run before ANY other C in the image: .bss still holds whatever
 * was in SRAM (the .um2 carries no zero-fill and the firmware copies the file
 * verbatim), so every static below is garbage until this returns. */
#ifndef UVM2_BIOS   /* the BIOS has its own startup, and _bss_start does not exist there */
void uvm2_cpu_init(void)
{
#ifdef UVM2_PICO_RUNTIME
    /* Under the pico-sdk build the crt0 has already cleared .bss and pointed
     * VTOR at its own table (which carries our SVC handler, since isr_svcall is
     * weak). Doing either again here would be wrong twice over: `_bss_start`
     * and `_bss_end` are symbols from OUR linker script and do not exist, and
     * hardcoding VTOR = 0x20000000 assumes a vector table we no longer emit. */
#else
    for (uint32_t *p = &_bss_start; p < &_bss_end; p++) *p = 0;

    /* Our vector table, or SVC and faults still land in the firmware's. */
    SCB_VTOR = 0x20000000u;
    __asm__ volatile ("dsb \n isb" ::: "memory");
#endif

    /* Silence whatever the firmware left enabled; anything still armed would
     * vector into our stub handlers.  PRIMASK stays CLEAR on purpose — every
     * VPy builtin is an SVC, and masking it escalates SVC to a HardFault. */
    SYST_CSR = 0;
    for (int i = 0; i < 4; i++) { NVIC_ICER(i) = 0xFFFFFFFFu; NVIC_ICPR(i) = 0xFFFFFFFFu; }
}
#endif

/* Step 2 — pads and function select.  Separate from the halt because CLK has to
 * be readable before we commit to anything: everything downstream blocks on its
 * edges, so a console that is switched off must be detectable first. */
void uvm2_bus_pads(void)
{
    for (uint32_t n = 0; n < 32; n++) {
        uint32_t bit = 1u << n;
        if ((UVM2_CART_PINS & bit) == 0) continue;
        PADS_BANK0(n)    = UVM2_PAD_BASE | ((UVM2_PULLUP_PINS & bit) ? UVM2_PAD_PUE : 0u);
        IO_BANK0_CTRL(n) = UVM2_FUNC_SIO;
    }
}

/* Step 3 — take the bus and keep it.  /HALT stays asserted for the life of the
 * program: releasing it between accesses lets the 6809 resume BIOS execution
 * and fight us for the bus. */
#ifdef UVM2_PIO_STREAM
/* Start the stream with THIS board's field layout.
 *
 * out_dirs leaves out GP22 (PB6, which is an INPUT here) and GP23 (which does not exist).
 * They fall inside the `out` range because the address is split — A14/A15 jump over them —
 * but with their direction bit at 0 the pad does not drive and the `out pins` is harmless.
 * It is the same mechanism as the preamble, with not a single branch in the hot path.
 *
 * The park address is $8000 with R/W HIGH: unmapped on the Vectrex, so a parked period
 * reaches no device. Parking at $D00x would re-execute the last VIA write on EVERY falling
 * edge of E. */
#define UVM2_STREAM_OUT_BASE   0u
#define UVM2_STREAM_OUT_COUNT  27u                       /* GP0..GP26: data, A0-A15, R/W */
#define UVM2_STREAM_OUT_DIRS   (((1u << UVM2_STREAM_OUT_COUNT) - 1u) \
                                & ~(1u << 22) & ~(1u << 23))
/* The pin directions the SM actually has set, read back out of IO_BANK0.
 *
 * WHY THIS IS NEEDED: if these come out wrong, the SM runs, drains the FIFO and the pad
 * does not drive — a black screen with not one counter complaining. It already happened on
 * the other cartridge. And there are TWO opposite causes that look identical from outside:
 * that the mask never reached the preamble, or that the SM RESTARTED afterwards and re-ran
 * the preamble with whatever first word was in the FIFO (a drawing word).
 *
 * It is taken right after installing. Compared against the same read taken later over SWD,
 * it separates the two: if it is right here and wrong later, the SM is restarting. */
uint32_t uvm2_stream_dirs_after_install;

static uint32_t read_dirs(void)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < UVM2_STREAM_OUT_COUNT; i++) {
        uint32_t st = *(volatile uint32_t *)(uintptr_t)(0x40028000u + 8u * i);
        if (st & (1u << 13)) m |= (1u << i);      /* OETOPAD: the pad drives */
    }
    return m;
}

void uvm2_stream_start(void)
{
    vbus_install(UVM2_STREAM_OUT_BASE, UVM2_STREAM_OUT_COUNT, UVM2_STREAM_OUT_DIRS,
                 UVM2_PARK_BITS | UVM2_RW_MASK);
    uvm2_stream_dirs_after_install = read_dirs();
}
#endif

void uvm2_bus_halt(void)
{
    /* Values go into GPIO_OUT *before* the drivers are enabled, so the pins
     * never glitch through a random state on their way up. */
    UVM2_GPIO_OUT_SET = UVM2_PARK_BITS;
    UVM2_GPIO_OUT_CLR = UVM2_OUT_MASK & ~UVM2_PARK_BITS;   /* includes /HALT low */
    UVM2_GPIO_OE_SET  = UVM2_OUT_MASK;

    /* Let the 6809 finish its instruction and tri-state.  The reference sleeps
     * 20 ms; counting bus cycles instead keeps this independent of whatever
     * core clock the firmware left configured — 30000 cycles is 20 ms exactly. */
    uvm2_bus_delay(UVM2_CYCLES_20MS);
}

/* HOW MANY CPU CYCLES ONE E PERIOD LASTS, measured against the Vectrex's own clock.
 *
 * WHY THIS NUMBER AND NOT "how many MHz". The PIO stream's phase calibration (`nop [14]` in
 * bus_stream.pio) is expressed in PIO cycles, and the PIO runs off the system clock. What
 * decides whether that calibration carries from one board to another is not the nominal
 * frequency, it is the RATIO between the system clock and the E period. On our cartridge it
 * is 100.0 cycles per period (T1_CYCLES_Q8 = 25602).
 *
 * And E is the best ruler there is: 1.5 MHz by definition of the Vectrex, independent of
 * which crystal the board carries and of whatever the bootrom left configured. Reading
 * PLL_SYS gives 150 MHz, but ASSUMING a 12 MHz crystal nobody has measured; this assumes
 * nothing.
 *
 * It is taken over UVM2_E_SAMPLES periods so the cost of detecting the edge (a few cycles)
 * is spread out and does not bias the result. In Q8 to avoid dragging in floating point:
 * 100.0 cycles reads as 25600. */
#define UVM2_E_SAMPLES 256u
uint32_t uvm2_cycles_per_e_q8;

void uvm2_measure_e(void)
{
    uint32_t t0, t1, i;

    *(volatile uint32_t *)0xE000EDFC |= (1u << 24);   /* DEMCR.TRCENA        */
    *(volatile uint32_t *)0xE0001000 |= 1u;           /* DWT_CTRL.CYCCNTENA  */

    /* Start ON an edge, not in the middle of a period: otherwise the first sample is worth
     * a fraction and the average comes out short. */
    UVM2_WAIT_CLK_LOW();
    UVM2_WAIT_CLK_HIGH();
    t0 = *(volatile uint32_t *)0xE0001004;
    for (i = 0; i < UVM2_E_SAMPLES; i++) {
        UVM2_WAIT_CLK_LOW();
        UVM2_WAIT_CLK_HIGH();
    }
    t1 = *(volatile uint32_t *)0xE0001004;

    uvm2_cycles_per_e_q8 = ((t1 - t0) << 8) / UVM2_E_SAMPLES;
}

void uvm2_bus_init(void)
{
#ifndef UVM2_BIOS
    uvm2_cpu_init();
#endif

    uvm2_bus_pads();
    uvm2_bus_halt();
}

/* ── Command stream executor ──────────────────────────────────────────────────
 * Mirrors VectrexCart::ExecuteHaltCommands: R/W is dropped once for the whole
 * batch and the address' high bits stay at $D000 throughout, so each command is
 * a single 12-bit update landing on GPIO0-11.  One command = one bus cycle. */

#ifdef UVM2_PIO_STREAM
void uvm2_bus_return_to_stream(void);   /* defined with the single accesses, below */

/* ── THE SAME EXECUTOR, BUT QUEUEING ──────────────────────────────────────────
 *
 * The list and its format DO NOT CHANGE: it is decoded exactly the same way (reg+data
 * already shifted into bits 8..19, delay in 20..31). What changes is who puts the bits on
 * the bus and who counts the periods: here it is the PIO state machine, which synchronises
 * against ~E in hardware. That is the difference that matters — a word that arrives late
 * costs it one period, not a write in the wrong phase.
 *
 * And that is why this tolerates the list living in PSRAM: a cache miss delays the DMA,
 * which responds by filling the FIFO a little later; the SM PARKS in the meantime. With the
 * CPU executor the same miss landed inside a write's window and broke the phase, which is
 * what drew scribbles.
 *
 * THE DELAY IS QUEUED, NOT WAITED FOR. `vbus_repeat(n)` is ONE word that parks for n
 * periods; pushing n park words cost ~8400 FIFO writes per frame to produce exactly the
 * same thing.
 */
UVM2_RAMFUNC uint32_t uvm2_exec(const uint8_t *cmds, uint32_t count)
{
    uint32_t cycles = 0;

    /* Reclaim the bus if a single access took it to SIO between frames. */
    uvm2_bus_return_to_stream();

    while (count--) {
        uint32_t v     = (uint32_t)cmds[0] | ((uint32_t)cmds[1] << 8) | ((uint32_t)cmds[2] << 16);
        uint32_t out   = UVM2_CMD_REG_DATA(v);
        uint32_t delay = UVM2_CMD_DELAY(v);
        cmds += 3;

        /* R/W is LOW by omission in the word: this is a write. */
        vbus_push(vbus_word(UVM2_VIA_WORD(out)));
        cycles++;

        if (delay) {
            vbus_push(vbus_repeat(delay));
            cycles += delay;
        }
    }

    /* FLUSH BEFORE ANYONE READS. The rest of the SDK reads the VIA between frames
     * (controllers, PSG), and a read that overtakes the pending batch sees the old bus.
     * It is the same fault that left buttons 1 and 2 pressed from boot on the other
     * cartridge. */
    vbus_flush();
    return cycles;
}
#else
UVM2_RAMFUNC uint32_t uvm2_exec(const uint8_t *cmds, uint32_t count)
{
    uint32_t cycles = 0;

#ifdef UVM2_STREAM_INSTALL_ONLY
    /* BISECTION: the stream is INSTALLED but not used; drawing goes through SIO.
     *
     * It separates two things that until now travelled together: what `install()` leaves
     * done at startup — pads, FUNCSEL, PIO and DMA out of reset, pin directions — and how
     * the stream leaves the VIA when it closes each frame. With the controller giving 0x89
     * instead of 0xFF with the pins handed over, the SM stopped and the bus drained, the
     * suspect can only be one of those two.
     *
     * The bus has to be taken because install moved the pins to the PIO. It is sticky: it
     * stays on SIO and nobody reclaims it, which is exactly what we want to measure. */
    bus_take();
#endif

    /* Select $D000 on a clean edge, with R/W HIGH — a READ, not a write.
     *
     * This used to leave R/W low, and UVM2_BUS_MASK covers R/W as well as the
     * address and data lines, so the pre-select WAS a write: register 0 (Port B)
     * = 0x00. That clears PB0 (mux ENABLED, channel 0 = the Y sample-and-hold)
     * and PB7 (/RAMP RUNNING), so every frame began by releasing the integrators
     * with the DAC wired to the Y hold. It is the stray bright vector that
     * survived removing the drawing, the input, the audio and the zero-clamp
     * release — because it happened before any of them, even with an empty
     * command stream (s_count = 0, verified over SWD 2026-08-04).
     *
     * The reference executor does `... | c_ReadWriteMask` here for exactly this
     * reason, which is why its game never showed it.
     *
     * BUT ONLY HALF OF IT WAS COPIED. Theirs is:
     *
     *     gpio_put_masked (c_HaltModeOutputs, c_VIABase | 0xC00 | c_ReadWriteMask);
     *                                                     ^^^^^ register 0xC = PCR
     *
     * R/W high and the register are TWO defences, not one. R/W high says "this is a
     * read"; the register picks WHAT breaks if that read is not honoured. With the
     * register at 0, what gets written is PORT B = 0x00: mux enabled and /RAMP
     * RUNNING, i.e. a stroke. With the register at 0xC, what gets written is
     * PCR = 0x00, which leaves CB2 low — beam blanked — and moves nothing.
     *
     * The ghost survived removing the drawing, the input and the audio, and the
     * command list printed on the host comes out clean (the two moves with the beam
     * blanked, only the two strokes lit). The only thing left outside the list is
     * this preamble.
     *
     * AND THE PHASE WAS NOT THEIRS EITHER. Theirs, with Start = CLK low and End = CLK
     * high:
     *
     *     Start(); End(); gpio_put_masked(preamble); Start();
     *
     * that is: it drives with CLK HIGH and holds until CLK falls — the same phase its
     * commands use, and ours. Ours did the opposite (HIGH, LOW, drive), so it changed
     * address and R/W with CLK LOW. The pin is ~E, so CLK low is E HIGH: we were
     * changing the bus in the middle of E high, which is precisely what this project
     * has written down that you do not do, and the next edge latches it anyway. The
     * commands were right; the preamble, which is the only thing outside the list,
     * was not. */
    UVM2_WAIT_CLK_LOW();
    UVM2_WAIT_CLK_HIGH();
    uvm2_put_masked(UVM2_VIA_BASE_BITS | UVM2_RW_MASK | (UVM2_VIA_PCR << 8),
                    UVM2_BUS_MASK);
    UVM2_WAIT_CLK_LOW();

    while (count--) {
        uint32_t v     = (uint32_t)cmds[0] | ((uint32_t)cmds[1] << 8) | ((uint32_t)cmds[2] << 16);
        uint32_t out   = UVM2_CMD_REG_DATA(v);
        uint32_t delay = UVM2_CMD_DELAY(v);
        cmds += 3;

        UVM2_WAIT_CLK_HIGH();
        uvm2_put_masked(out, UVM2_CMD_GPIO_MASK);
        UVM2_GPIO_OUT_CLR = UVM2_RW_MASK;   /* assert WRITE */
        UVM2_WAIT_CLK_LOW();
        cycles++;

        /* R/W STAYS LOW through the delay cycles.
         *
         * It used to go high here, "so the idle cycles do not re-execute the same
         * write". They do not need protecting from that: every register we write
         * (PORTA, PORTB, PCR, ACR, DDRx) takes the same value idempotently, which
         * is why the reference executor holds R/W low for the whole command and
         * only raises it once, after the last one.
         *
         * Raising it costs us something real. In halt mode the data bus is an
         * OUTPUT (UVM2_OUT_MASK), and the address still selects $D000 — so R/W
         * high is a READ of the VIA, and the VIA drives the data lines straight
         * back into our drivers. Contention, on every delay cycle, which is most
         * of them: a vector spends 125 + 16 + 3 cycles in delays and 4 driving.
         *
         * Diffing the two command streams on the host (2026-08-04) showed ours is
         * IDENTICAL to theirs, command for command, data for data, delay for delay
         * — 45 words each for the same square. So the stray vector was never in what
         * we send; it is in how we hold the bus while sending it. */
        while (delay--) {
            UVM2_WAIT_CLK_HIGH();
            UVM2_WAIT_CLK_LOW();
            cycles++;
        }
    }

    /* Park at $0000, R/W high — their `gpio_put_masked(c_HaltModeOutputs,
     * c_ReadWriteMask)`, which drives every halt-mode output low except R/W.
     * We parked at $8000; both are unmapped and neither drives the data bus
     * back at us, but there is no reason to differ from the reference here. */
    UVM2_WAIT_CLK_HIGH();
    UVM2_GPIO_OUT_SET = UVM2_RW_MASK;
    uvm2_put_masked(UVM2_RW_MASK, UVM2_BUS_MASK);
    return cycles;
}
#endif /* UVM2_PIO_STREAM */

UVM2_RAMFUNC void uvm2_bus_delay(uint32_t cycles)
{
    while (cycles--) {
        UVM2_WAIT_CLK_HIGH();
        UVM2_WAIT_CLK_LOW();
    }
}

/* ── Single accesses ──────────────────────────────────────────────────────── */

/* Bus cycles spent in single accesses (input reads, PSG writes) since it was
 * last cleared.  uvm2_exec returns its own count, but these do not go through
 * it, so without this the frame pacing has no idea they happened and overshoots
 * 50 Hz by however long the controls and the sound took.  Counted rather than
 * assumed: an input read's cost depends on how many SAR steps an axis needs. */
uint32_t uvm2_single_cycles;

#ifdef UVM2_PIO_STREAM
/* ── SINGLE ACCESSES WHILE THE STREAM OWNS THE BUS ────────────────────────────
 *
 * With the stream running, GP0..GP26 belong to the PIO. uvm2_via_write and uvm2_via_read
 * still speak over SIO — that is their correct and only way to read, because the executor
 * cannot read in the middle of a list — so they write into registers that NO LONGER DRIVE
 * those pins.
 *
 * THE SYMPTOM DOES NOT LOOK LIKE A BUS FAULT. Reading a controller is *write the column
 * into Port B and then read Port A*, and the buttons are active LOW: if the column does not
 * come out, everything reads as "pressed". In asteroids that is the ship rotating, firing
 * and hyperspacing all at once and at random. On the other cartridge the same family of
 * fault left buttons 1 and 2 pressed from boot, and that did not look like a bus problem
 * either.
 *
 * Two things, and both are needed:
 *   1. FLUSH the stream. A read can overtake up to 63 queued writes (~42 us), and then it
 *      reads the previous bus.
 *   2. Hand the pins back to SIO for the access, and to the PIO on the way out.
 *
 * It costs 27 FUNCSEL writes per access, and that is affordable: this happens BETWEEN
 * frames (controllers, axes, PSG), never inside the drawing loop. The SM sits parking in
 * the meantime, which is its resting state by design.
 *
 * The SIO value and direction are not lost when the pins are handed over: GPIO_OUT and
 * GPIO_OE belong to SIO and are still there; only who drives the pad changes. */
#define UVM2_FUNC_SIO 5u
#define UVM2_FUNC_PIO 6u

static void pins_to(uint32_t funcsel)
{
    for (uint32_t i = 0; i < UVM2_STREAM_OUT_COUNT; i++)
        if (UVM2_STREAM_OUT_DIRS & (1u << i))
            *(volatile uint32_t *)(uintptr_t)
                (0x40028000u + 8u * (UVM2_STREAM_OUT_BASE + i) + 4u) = funcsel;
}

/* IT IS TAKEN ONCE AND RELEASED WHEN DRAWING RESUMES, not per access.
 *
 * Taking and releasing it on every uvm2_via_write was the obvious fix and it is WRONG:
 * reading the buttons is EIGHT consecutive accesses (DDRA, the register number, the latch,
 * inactive, DDRA back to input, read, inactive) and that sequence is PSG STROBES, with an
 * order and timings. Bouncing the pins PIO->SIO->PIO between each one inserts a long gap in
 * the middle of the sequence, and the result is not noise: it is a STABLE AND WRONG value.
 * Measured with the `controllers` test: 0x76 instead of 0xFF with nothing pressed, frozen
 * across three reads. A value that dances and a value that is fixed but false are different
 * faults.
 *
 * So the bus stays on SIO from the first single access until the drawing reclaims it.
 * uvm2_exec asks for it when it starts; between frames nobody asks, which is exactly when
 * controllers, axes and PSG are read. */
static int s_bus_on_sio;

static void bus_take(void)
{
    if (s_bus_on_sio) return;
    vbus_drain();
    vbus_sm_stop();               /* alive but pinless, it still has an opinion on the cycle */
    pins_to(UVM2_FUNC_SIO);
    s_bus_on_sio = 1;
}

void uvm2_bus_return_to_stream(void)
{
    if (!s_bus_on_sio) return;
    pins_to(UVM2_FUNC_PIO);
    vbus_sm_start();
    s_bus_on_sio = 0;
}

#define UVM2_BUS_TAKE()    bus_take()
#define UVM2_BUS_RELEASE() do { } while (0)
#else
#define UVM2_BUS_TAKE()    do { } while (0)
#define UVM2_BUS_RELEASE() do { } while (0)
#define uvm2_bus_return_to_stream() do { } while (0)
#endif

uint32_t uvm2_write_out, uvm2_write_in, uvm2_write_oe;

/* IN OUR OWN CARTRIDGE'S BIOS these two are provided by the firmware (uvm2c.rs) on top of
 * its own bus access: the write as a stream word and the read with the bus free. What
 * follows is the UVM2's SIO path (its pins, its masks). */
#ifndef UVM2_BIOS
UVM2_RAMFUNC void uvm2_via_write(uint32_t reg, uint32_t data)
{
    uint32_t out = UVM2_VIA_BASE_BITS
                 | ((reg & 0x0Fu) << 8)          /* register → A0-A3 */
                 | (data & 0xFFu);               /* data     → D0-D7 */

    UVM2_BUS_TAKE();

    /* WAIT FOR A COMPLETE EDGE, not "until it is high".
     *
     * With only UVM2_WAIT_CLK_HIGH(), if the clock was ALREADY high on entry nothing is
     * waited for and the value is presented at the END of that half — a hair from the
     * transition. That is a phase violation, and on this bus a phase violation does not
     * degrade: it fails outright.
     *
     * MEASURED, comparing the same witnesses in the image that works and in the streamed
     * one, at the instant of presenting the Port A write: address, data, R/W, /HALT and OE
     * are IDENTICAL, and only bit 31 differs — the clock. High on SIO, low with the stream.
     * Hence the reads working and the writes not, and the controller always reading the
     * same wrong value.
     *
     * On SIO it got away with it because of the rhythm in which the calls chain; as soon as
     * bus_take() leaves them in a different phase, they fall outside. uvm2_exec already
     * does it right — WAIT_CLK_LOW and then WAIT_CLK_HIGH — and these two did not. */
    UVM2_WAIT_CLK_LOW();
    UVM2_WAIT_CLK_HIGH();
    uvm2_put_masked(out, UVM2_BUS_MASK);          /* R/W low = write */

    /* WHAT ACTUALLY REACHES THE PADS AT THE MOMENT OF PRESENTING.
     *
     * The reads work — measured: the VIA is selected at $D001, R/W high, /HALT low, and it
     * answers — and yet the PSG is in the wrong state, so what is not arriving are the
     * WRITES. This captures the three witnesses at the instant of presenting, so the
     * streamed image can be compared against the one that works:
     *
     *   _out  what we WANTED to put out
     *   _in   what the pads really show (we read ourselves back: if it does not match _out
     *         within the bus mask, the write is not reaching the pad)
     *   _oe   who is driving
     *
     * It is only taken on the PORT A write (reg 1), which is where the PSG register number
     * goes: the one that decides what gets read afterwards, and therefore the one that
     * explains a wrong and repeatable value. Capturing all of them would leave the last one,
     * which is not comparable. */
    if ((reg & 0x0Fu) == 1u) {
        uvm2_write_out = out;
        uvm2_write_in  = UVM2_GPIO_IN;
        uvm2_write_oe  = *(volatile uint32_t *)(uintptr_t)(0xD0000000u + 0x030u);
    }

    UVM2_WAIT_CLK_LOW();

    /* Parking goes with CLK HIGH, not here. It used to sit right after the WAIT_CLK_LOW,
     * i.e. it changed the address and R/W with E HIGH — the same phase error the
     * executor's preamble had, and on the most-travelled path: every PSG write and
     * every step of a controller read goes through here. The reference does not even
     * park per write: its WriteVia ends on the edge and the parking happens once, in
     * EndDirectViaMode, with a WaitForBusCycleEnd() in front. */
    UVM2_WAIT_CLK_HIGH();
    uvm2_put_masked(UVM2_PARK_BITS, UVM2_BUS_MASK & ~UVM2_DATA_MASK);
    uvm2_single_cycles += 2;

    UVM2_BUS_RELEASE();
}

uint32_t uvm2_read_oe;      /* SIO GPIO_OE at the sampling point: bits 0-7 = we are driving */
uint32_t uvm2_read_state;   /* the whole GPIO_IN at the sampling point */

UVM2_RAMFUNC uint8_t uvm2_via_read(uint32_t reg)
{
    uint32_t out = UVM2_VIA_BASE_BITS | UVM2_RW_MASK | ((reg & 0x0Fu) << 8);
    uint32_t addr_mask = UVM2_BUS_MASK & ~UVM2_DATA_MASK;
    uint32_t state;

    UVM2_BUS_TAKE();

    UVM2_GPIO_OE_CLR = UVM2_DATA_MASK;            /* let the VIA drive D0-D7 */

    /* The same complete edge as in uvm2_via_write, and for the same reason. */
    UVM2_WAIT_CLK_LOW();
    UVM2_WAIT_CLK_HIGH();
    uvm2_put_masked(out, addr_mask);
    UVM2_WAIT_CLK_LOW();

    /* Sample on the next rising edge — the address has had a whole cycle by
     * then.  Edge-for-edge the same as VectrexCart::ReadVia. */
    do { state = UVM2_GPIO_IN; } while ((state & UVM2_CLK_MASK) == 0);

    /* THE STATE AT THE INSTANT OF SAMPLING, which is the one thing that cannot be deduced
     * from outside. The three PSG reads return the SAME value (0x89) whichever register is
     * asked for, so the chip is not being read: the bus is. And a constant value comes
     * either from whoever is driving it or from a resistor.
     *
     * uvm2_read_oe saves SIO's output enable right here. If its bits 0-7 are set, we are
     * DRIVING D0-D7 while trying to read them — i.e. we are reading our own drawing, and
     * that is the whole fault. If they are zero, nobody is driving them and 0x89 is the
     * pads' pull resistance, which accuses the VIA selection (the address) and not the
     * data. */
    uvm2_read_oe    = *(volatile uint32_t *)(uintptr_t)(0xD0000000u + 0x030u);
    uvm2_read_state = state;

    uvm2_put_masked(UVM2_PARK_BITS, addr_mask);
    UVM2_GPIO_OE_SET = UVM2_DATA_MASK;

    /* Leave the clock LOW before returning.  Sampling happens on a rising edge,
     * so without this the caller's next access finds its "wait for CLK high"
     * already satisfied and drives the bus in the dying part of that same high
     * phase — too late for the falling edge that latches it, and the write is
     * simply lost.  Costs half a bus cycle and makes every access start from
     * the same known phase. */
    UVM2_WAIT_CLK_LOW();
    uvm2_single_cycles += 2u;      /* one to address, one to sample */

    UVM2_BUS_RELEASE();
    return (uint8_t)(state & 0xFFu);
}
#endif /* !UVM2_BIOS */
