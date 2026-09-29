/* Forced into ts_jack.c with -include when building the host probe: it points the two PSRAM
 * windows at one ordinary buffer and declares it. Keeping it here rather than in ts_jack.c
 * means the game file carries no host-test scaffolding beyond the two #ifndefs. */
extern unsigned char *probe_psram;
#define TS_PSRAM_WRITE probe_psram
#define TS_PSRAM_READ  probe_psram

/* The 1 MHz timer is an MMIO address on the cartridge and nothing on a desktop. The probe
 * counts microseconds itself, so the loader's deadline behaves: with the fake card returning
 * instantly, `probe_us` advancing per call is what makes the do/while terminate the way it
 * does on hardware instead of reading the whole file in one pass. */
extern unsigned probe_us;
#define TS_NOW() (probe_us += 200u)
