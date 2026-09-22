/*
 * uvm2_input.h — buttons, joysticks and PSG over the halted Vectrex bus.
 * These perform real bus read cycles, so they run between frames (see the
 * note in uvm2_input.c), never inside a recorded command stream.
 */
#ifndef UVM2_INPUT_H
#define UVM2_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSG register 14, raw: active-low, J1 in bits 0-3, J2 in bits 4-7. The button
 * syscalls reshape this differently — see uvm2_svc.c. */
uint8_t uvm2_read_buttons(void);

/* SYS_READ_AXES — (J1X << 24) | (J1Y << 16) | (J2X << 8) | J2Y, each an i8. */
uint32_t uvm2_read_axes(void);

/* Digital axes (the hardware-proven path) scale to +/-127 so ordinary game code
 * behaves as on a real cart.  Successive-approximation analog reads are
 * implemented but NOT hardware-validated — opt in explicitly. */
void uvm2_input_set_analog(int enable);

/* SYS_PSG_WRITE / SYS_PSG_READ. */
void    uvm2_psg_write(uint32_t reg, uint32_t value);
uint8_t uvm2_psg_read(uint32_t reg);

#ifdef __cplusplus
}
#endif

#endif /* UVM2_INPUT_H */
