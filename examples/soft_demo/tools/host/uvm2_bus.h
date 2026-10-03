/* host stand-in for the SDK's uvm2_bus.h: only what main.c reads */
#include <stdint.h>
typedef struct { uint32_t dropped; } uvm2_stats_t;
extern uvm2_stats_t uvm2_stats;
