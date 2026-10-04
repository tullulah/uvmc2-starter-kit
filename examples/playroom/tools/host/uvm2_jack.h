/* host stand-in for the SDK's uvm2_jack.h: no jack on the host */
#include <stdint.h>
#define UVM2_JACK_RATE 32000
static inline int  uvm2_jack_init(void) { return 0; }
static inline int  uvm2_jack_space(void) { return 0; }
static inline void uvm2_jack_write_lr(const int16_t *l, const int16_t *r, int n) { (void)l; (void)r; (void)n; }
