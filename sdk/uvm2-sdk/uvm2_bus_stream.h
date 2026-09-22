/* uvm2_bus_stream.h — the C face of the shared `vectrex-bus` crate.
 *
 * THERE IS NO IMPLEMENTATION HERE, and that is deliberate: the stream driver (the batch
 * ring, the DMA, the PIO program and its phase calibration) lives ONCE, in sdk/vectrex-bus,
 * and it is linked both by our own cartridge's firmware (as an rlib) and by this image (as
 * a staticlib). Any decision that appeared in this file would be a second implementation
 * disguised as a header.
 */
#ifndef UVM2_BUS_STREAM_H
#define UVM2_BUS_STREAM_H

#include <stdint.h>

void     vbus_install(uint32_t out_base, uint32_t out_count, uint32_t out_dirs, uint32_t park);
uint32_t vbus_word(uint32_t bus_word);
uint32_t vbus_repeat(uint32_t n);
uint32_t vbus_silence(void);
void     vbus_push(uint32_t word);
void     vbus_flush(void);
void     vbus_drain(void);
void     vbus_sm_stop(void);
void     vbus_sm_start(void);
/* A frame's list as a single DMA (see vectrex-bus): between begin and end the pushes
 * accumulate; wait = the bus has finished with the previous one; fire = it comes back
 * immediately. */
void     vbus_list_begin(void);
void     vbus_list_end(void);
void     vbus_list_wait(void);
void     vbus_list_fire(void);

/* Counters by index. THE ORDER IS FIXED BY cabi/src/lib.rs and the two move together. */
#define VBUS_PUSHES     0u
#define VBUS_STALLS     1u
#define VBUS_BATCH_SENT 2u
#define VBUS_BATCH_WAIT 3u
#define VBUS_FULL_SEEN  4u
#define VBUS_OVERRUNS   5u
uint32_t vbus_stat(uint32_t idx);

void uvm2_stream_start(void);

/* The pin directions read back RIGHT AFTER installing. Compare against the same read taken
 * later over SWD: if it is right here and wrong later, the SM has restarted. */
extern uint32_t uvm2_stream_dirs_after_install;

#endif
