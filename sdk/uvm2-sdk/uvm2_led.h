/*
 * uvm2_led.h — cartridge status LED, the bring-up channel of last resort.
 * See uvm2_led.c for why this exists.
 */
#ifndef UVM2_LED_H
#define UVM2_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UVM2_STATUS_OFF = 0,
    UVM2_STATUS_BOOT,       /* blue   — image booted, runtime entered        */
    UVM2_STATUS_NO_CLOCK,   /* red    — no Vectrex CLK: console off/unseated */
    UVM2_STATUS_HALTING,    /* amber  — CLK found, taking the bus            */
    UVM2_STATUS_RUNNING,    /* green  — VIA primed, frames going out         */
    UVM2_STATUS_OVERRUN,    /* orange — a frame exceeded the 50 Hz budget    */
} uvm2_status_t;

void uvm2_led_init(void);
void uvm2_led_rgb(uint8_t r, uint8_t g, uint8_t b);
void uvm2_led_status(uvm2_status_t code);

/* Measure the core clock against the Vectrex CLK.  Returns cycles per µs, or 0
 * if no clock edge arrived — which is itself the diagnosis. */
uint32_t uvm2_clock_calibrate(void);
uint32_t uvm2_cycles_per_us(void);

#ifdef __cplusplus
}
#endif

#endif /* UVM2_LED_H */
