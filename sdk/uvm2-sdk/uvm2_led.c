/*
 * uvm2_led.c — the only output channel that works when nothing else does.
 *
 * A UVM2 game running from SD has no serial port, no debugger and no printf:
 * if the screen stays black there is otherwise no way to tell "the image never
 * booted" from "the 6809 never halted" from "the VIA writes land in the wrong
 * phase".  The cartridge's WS2812 on GPIO30 gives us that, and costs nothing
 * once bring-up is done.
 *
 * The reference drives it from a PIO state machine; we bit-bang it instead, so
 * the SDK stays free of PIO programs and of the Pico SDK entirely.  Timing
 * comes from DWT's cycle counter, calibrated against the one clock whose
 * frequency we actually know — the Vectrex's own 1.5 MHz CLK.  When that clock
 * is absent (console off, cart not seated) calibration is exactly the thing we
 * need to report, so it times out and falls back to the Pico SDK's default
 * 150 MHz rather than hanging.
 */

#include "uvm2_bus.h"
#include "uvm2_led.h"

#define DEMCR       UVM2_MMIO(0xE000EDFCu)
#define DWT_CTRL    UVM2_MMIO(0xE0001000u)
#define DWT_CYCCNT  UVM2_MMIO(0xE0001004u)

#define LED_PIN     30u
#define LED_MASK    (1u << LED_PIN)

#define PADS_BANK0(n)    UVM2_MMIO(0x40038000u + 4u + 4u * (n))
#define IO_BANK0_CTRL(n) UVM2_MMIO(0x40028000u + 8u * (n) + 4u)

static uint32_t s_cyc_per_us = 150u;    /* refined by uvm2_clock_calibrate() */
static int      s_ready;

static inline uint32_t cyc(void) { return DWT_CYCCNT; }

static inline void spin_cycles(uint32_t start, uint32_t n)
{
    while ((cyc() - start) < n) { }
}

uint32_t uvm2_cycles_per_us(void) { return s_cyc_per_us; }

uint32_t uvm2_clock_calibrate(void)
{
    /* Time 128 Vectrex bus cycles.  Each is 1/1.5 MHz = 2000/3 ns, so 128 of
     * them are 85333 ns, and cycles/µs = elapsed / 85.333.
     *
     * The bounded spin is the point: a missing CLK must return 0, not hang. */
    const uint32_t EDGES   = 128u;
    const uint32_t TIMEOUT = 200000000u;   /* ~1.3 s at 150 MHz */
    uint32_t guard = 0, start, elapsed;

    while ((UVM2_GPIO_IN & UVM2_CLK_MASK) != 0) if (++guard > TIMEOUT) return 0;
    while ((UVM2_GPIO_IN & UVM2_CLK_MASK) == 0) if (++guard > TIMEOUT) return 0;

    start = cyc();
    for (uint32_t i = 0; i < EDGES; i++) {
        while ((UVM2_GPIO_IN & UVM2_CLK_MASK) != 0) if (++guard > TIMEOUT) return 0;
        while ((UVM2_GPIO_IN & UVM2_CLK_MASK) == 0) if (++guard > TIMEOUT) return 0;
    }
    elapsed = cyc() - start;

    /* elapsed / 85.333 µs, in integer arithmetic: × 3 / 256. */
    s_cyc_per_us = (elapsed * 3u) / (EDGES * 2u);
    if (s_cyc_per_us < 10u) s_cyc_per_us = 150u;   /* implausible → default */
    return s_cyc_per_us;
}

void uvm2_led_init(void)
{
    DEMCR    |= (1u << 24);          /* TRCENA */
    DWT_CTRL |= 1u;                  /* CYCCNTENA */

    /* GPIO30 is the one cart pin uvm2_bus_init deliberately leaves alone, so
     * claim it here — the firmware's PIO owns it until we do. */
    PADS_BANK0(LED_PIN)    = 0x62u;  /* SCHMITT | 8mA | IE, ISO clear */
    IO_BANK0_CTRL(LED_PIN) = 5u;     /* SIO */
    UVM2_GPIO_OUT_CLR      = LED_MASK;
    UVM2_GPIO_OE_SET       = LED_MASK;
    s_ready = 1;
}

/* One WS2812 frame: 24 bits, green first, MSB first.
 * A 0 bit is 400 ns high then 850 ns low; a 1 bit is 800 ns then 450 ns. */
void uvm2_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    uint32_t ns  = s_cyc_per_us;                 /* cycles per µs */
    uint32_t t0h = (ns * 40u) / 100u, t1h = (ns * 80u) / 100u;
    uint32_t bit_total = (ns * 125u) / 100u;

    if (!s_ready) return;

    for (int i = 23; i >= 0; i--) {
        uint32_t high = (grb >> i) & 1u ? t1h : t0h;
        uint32_t t    = cyc();
        UVM2_GPIO_OUT_SET = LED_MASK;
        spin_cycles(t, high);
        UVM2_GPIO_OUT_CLR = LED_MASK;
        spin_cycles(t, bit_total);
    }

    /* Latch: the strip needs >50 µs low to accept the frame. */
    { uint32_t t = cyc(); spin_cycles(t, ns * 300u); }
}

void uvm2_led_status(uvm2_status_t code)
{
    switch (code) {
    case UVM2_STATUS_BOOT:     uvm2_led_rgb(0,  0, 24); break;  /* blue   */
    case UVM2_STATUS_NO_CLOCK: uvm2_led_rgb(32, 0,  0); break;  /* red    */
    case UVM2_STATUS_HALTING:  uvm2_led_rgb(24,16,  0); break;  /* amber  */
    case UVM2_STATUS_RUNNING:  uvm2_led_rgb(0, 16,  0); break;  /* green  */
    case UVM2_STATUS_OVERRUN:  uvm2_led_rgb(24, 8,  0); break;  /* orange */
    default:                   uvm2_led_rgb(0,  0,  0); break;
    }
}
