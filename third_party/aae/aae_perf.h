/* Per-phase stopwatches on the cart (2026-09-16), readable over SWD without halting.
 * [0] CPU 0 exec, [1] CPU 1 exec, [2] v_WaitRecal (what core 1 spends WAITING for the
 * beam on core 0 — the single number that says beam-bound vs compute-bound),
 * [3] mathbox, [4] avg_go, [5] sh_update,
 * [6] whole run_cpus_to_cycles, [7] the frame turn minus v_WaitRecal,
 * [8] cpu_run: 6809 #1 loop, [9] cpu_run: 6809 #2 loop, [10] AVG list walk (vg_go),
 * [11] whole cpu_run, [12]/[13] instructions executed by 6809 #1 / #2 (n only),
 * [14] vectors handed to v_directDraw32 (n only), [15] time inside v_directDraw32,
 * i.e. the SDK plus the BIOS list builder -- so [10] minus [15] is AAE's own walk.
 * Microseconds from TIMER0 RAWL, accumulated, plus a call count.
 * Purpose: locate the ~43 ms per frame in esb that neither the real clocks nor the
 * dispatch table in SRAM moved.
 *
 * The XIP cache counters are NOT sampled here: XIP_CTRL is Secure-Privileged and the
 * debugger can read and clear it by itself, so tools/perf_dump.py does that from the
 * host over the same window and no game code touches those registers.
 *
 * THE ARRAYS LIVE IN .sram_fast, WHICH IS NOLOAD: nothing zeroes them, so at reset
 * they hold power-on garbage. AAE_PERF_INIT() at the top of main() is what makes an
 * absolute read meaningful; without it only differences between two reads are. */
#ifndef AAE_PERF_H
#define AAE_PERF_H
/* Cart only: the .um2 has its own uvm2_stats, and the per-instruction counters cost
 * an SRAM read-modify-write per emulated instruction. */
#if defined(VPY_RP2350) && !defined(UVM2_PICO_RUNTIME)
#define AAE_PERF_ON 1
extern volatile unsigned int aae_perf_us[16], aae_perf_n[16];
#define AAE_T() (*(volatile unsigned int *)0x400B0028u)
#define AAE_ACUM(i, t0) do { aae_perf_us[i] += AAE_T() - (t0); aae_perf_n[i]++; } while (0)
#define AAE_COUNT(i) do { aae_perf_n[i]++; } while (0)
#define AAE_PERF_INIT() do { int i_; for (i_ = 0; i_ < 16; i_++) { aae_perf_us[i_] = 0; aae_perf_n[i_] = 0; } } while (0)
#else
#define AAE_T() 0u
#define AAE_ACUM(i, t0) ((void)(t0))
#define AAE_COUNT(i) ((void)0)
#define AAE_PERF_INIT() ((void)0)
#endif
#endif
