/* Forced into ts_jack.c with -include when building the host probe: it points the two PSRAM
 * windows at one ordinary buffer and declares it. Keeping it here rather than in ts_jack.c
 * means the game file carries no host-test scaffolding beyond the two #ifndefs. */
extern unsigned char *probe_psram;
#define TS_PSRAM_WRITE probe_psram
#define TS_PSRAM_READ  probe_psram
