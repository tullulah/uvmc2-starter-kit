/* Where the hot parts live on the cart (2026-09-16).
 * On the RP2350 cart the game runs from PSRAM behind a 16 KB XIP cache; measured on
 * esb: 99.3% hits but ~36,000 misses per frame, each a QSPI line fill. Two placement
 * attributes, both empty on hosts and on the .um2 (which already runs from SRAM):
 *   AAE_SRAM_FAST  data in the NOLOAD SRAM section (whoever uses it fills it).
 *   AAE_SRAM_TEXT    code copied to SRAM by rp2350_start.s before main(). */
#ifndef AAE_FAST_H
#define AAE_FAST_H
#if defined(VPY_DUAL_CORE) && !defined(UVM2_PICO_RUNTIME)
#define AAE_SRAM_FAST __attribute__((section(".sram_fast")))
#define AAE_SRAM_TEXT   __attribute__((section(".sram_text")))
#else
#define AAE_SRAM_FAST
#define AAE_SRAM_TEXT
#endif
#endif
