/*
 * uvm2_psram.h — result of the UVM2 PSRAM probe. See uvm2_psram.c.
 *
 * The structure is global and volatile on purpose: with SWD attached it can be read as it
 * stands, with no LED blink codes to decode.
 */
#ifndef UVM2_PSRAM_H
#define UVM2_PSRAM_H

/* The SAME chip, read WITHOUT going through the XIP cache. A verification that reads
 * through the cache says nothing about what is in the PSRAM — it caught us out with the
 * loader, and again with the command list. addressmap.h: XIP_NOCACHE_NOALLOC_BASE =
 * 0x14000000. */
#define UVM2_PSRAM_NO_CACHE 0x15000000u

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel. Without it, reading this structure out of an image that is NOT loaded returns
 * ANOTHER game's code, and a non-zero `probed` reads as "the probe ran". It happened: it
 * gave an "already mapped" that was garbage. An improbable word is the difference between
 * a datum and a coincidence.
 *
 * PSR3, not PSR1: the structure has grown with the bootrom probe. If it stayed at PSR1, an
 * EARLIER image still sitting in the cartridge's RAM would pass the sentinel and its old
 * fields would be read at the new offsets. The sentinel goes up with the format or it is
 * not a sentinel. */
#define UVM2_PSRAM_MAGIC 0x50535233u   /* "PSR3" */

typedef struct {
    uint32_t magic;         /* UVM2_PSRAM_MAGIC if the probe wrote this      */
    uint32_t probed;        /* 1 = the probe actually ran                    */
    uint32_t found;         /* 1 = the manufacturer ID is the expected one   */
    uint32_t mf_id;         /* byte 0 of READ_ID (0x0D = AP Memory)          */
    uint32_t kgd;           /* byte 1: known-good-die                        */
    uint32_t xip_before[4]; /* 0x11000000 BEFORE touching anything           */
    /* QMI state ON ENTRY, before the probe touches anything. Without this you cannot tell
     * "the firmware left it mapped" from "we mapped it and then broke it": they are two
     * different reads of the same register, and the first one has to be captured. */
    uint32_t m1_timing_before;
    uint32_t m1_rfmt_before;
    uint32_t m1_rcmd_before;
    uint32_t direct_csr_before;
    /* An exhausted rx() and a chip answering zero return THE SAME THING: 0x00. Without
     * counting the timeouts, "no answer" and "answers zero" are the same read — and they
     * are opposite diagnoses. */
    uint32_t mf_id_spi;     /* READ_ID talking on ONE line (SPI)             */
    uint32_t mf_id_after_qpi_exit; /* ...and after pulling it out of QPI     */
    /* Electrical test of the ONLY wire exclusive to U3: its chip select. The rest of the
     * bus (SCK, SD0, SD1) is shared with the flash, and the flash works — the firmware
     * boots from it — so those lines are healthy. GPIO47 is driven as a normal output and
     * what comes back on the pad is read: if it does not follow what we drive, the problem
     * is the wire, not the chip. */
    uint32_t cs_reads_when_high;
    uint32_t cs_reads_when_low;
    /* A CONTINUITY hint, not a test. The output is released and the INTERNAL pulls are
     * turned off, so what is read is what the external network does. The schematic puts a
     * pull-up next to U3: if the trace reaches it, a pin driven to zero and then released
     * comes back to 1 on its own. If it is cut on the chip side, the net floats and stays
     * at 0 out of sheer capacitance. The discriminator is cs_float_after_low. */
    uint32_t cs_float_after_low;
    uint32_t cs_float_after_high;
    /* The same READ_ID at several speeds. A properly wired chip that goes mute often opens
     * up when the clock is lowered. CLKDIV 6 = 25 MHz, 30 = 5 MHz, 120 = 1.25 MHz. */
    uint32_t id_by_clkdiv[4];
    uint32_t clkdiv_tested[4];
    uint32_t rx_timeouts;
    uint32_t csr_after_cmd; /* DIRECT_CSR after pushing the first byte       */
    uint32_t already_mapped;/* 1 = CS1 came configured: we do NOT touch the chip */

    /* ---- THE BOOTROM PROBE (uvm2_psram_probe_bootrom) ----------------------
     *
     * The first probe talks to the chip through direct mode, by hand. This one lets the
     * BOOTROM do the talking, since it is the only piece that knows how to take a CS1
     * device out of XIP — and it only does so if FLASH_DEVINFO says CS1 has a size. By
     * default that is NONE, so that sequence HAS NEVER BEEN SENT. See the comment on
     * flash_devinfo_set_cs_size() in hardware/flash.h. */
    uint32_t bootrom_probe_run;   /* 1 = this probe actually ran               */
    /* POSITIVE CONTROL. The same READ_ID to the FLASH (CS0), known good because the
     * firmware boots from it. A W25Q128 answers EF 40 18. If this comes out wrong, the read
     * path is broken and whatever CS1 says is worthless: it is the "reference before
     * suspect, same rig" rule, only in software and for free. */
    uint32_t flash_jedec;
    uint32_t flash_jedec_after_cs1; /* the same, after arming CS1              */
    uint32_t devinfo_cs1_before;  /* what FLASH_DEVINFO said about CS1 on entry */
    /* And the chip, after the bootrom's exit sequence. */
    uint32_t mf_id_bootrom;       /* byte 4: 0x0D = AP Memory                  */
    uint32_t kgd_bootrom;         /* byte 5: 0x5D = known good die             */
    uint32_t eid_bootrom;         /* byte 6: the size is derived from this     */
    uint32_t found_bootrom;       /* 1 = correct KGD, which is what the SDK looks at */
    uint32_t size_bootrom;        /* bytes derived from the EID, 0 if no answer */
    /* BREADCRUMBS. The first version of this probe hung inside do_cmd_cs and all that was
     * known was "it never got as far as writing flash_jedec" — which covers six calls. This
     * is written BEFORE each step, so the last value that survives IS the step that did not
     * return. It is worth more than any hypothesis: a hang leaves no trace except the one
     * you put there in advance. Code: call*100 + step. See STEP_* in uvm2_psram.c. */
    uint32_t step_bootrom;

    /* ---- TALKING TO IT IN QPI (uvm2_psram_probe_qpi) -----------------------
     *
     * The hypothesis no probe had tested. An APS6404 in QPI ignores single-line commands,
     * and the cartridge is USB-C powered: it does not lose power when the console is
     * switched off, so it may have been in QPI since one of our own probes. Lowering the
     * clock does not uncover that — it is not a speed problem, it is a language problem.
     *
     * And the official pico-sdk 2.3.0 driver confirms it from the other side: its
     * psram_initialize_internal() sends 0x35 (QUAD ENABLE), i.e. it WANTS the chip in QPI.
     * If it is already there, it does not need pulling out: it needs to be spoken to that
     * way.
     *
     * The EIGHT raw bytes of each attempt are kept, without deciding where the ID falls. We
     * have no measurement of the wait cycles of 0x9F in QPI, and assuming a position turns a
     * one-byte skew into "mute chip" — exactly the mistake already made with MF_ID against
     * KGD. Look for 0d 5d in the string and you will see where it lands. */
    uint32_t qpi_probe_run;
    uint32_t qpi_direct[2];    /* READ_ID in QPI, with the chip as it stands  */
    uint32_t qpi_after_35[2];  /* ...and after sending it 0x35 on one line    */
    /* THE CLOCK, WRITTEN DOWN. The first version inherited the CLKDIV = 120 that
     * uvm2_psram_probe() leaves behind at the end of its sweep and never restores — and at
     * 1.25 MHz the quad transaction holds the select low for ~19 us, against the APS6404's
     * **tCEM of 8 us**. The test was running out of spec and nothing showed it. Storing the
     * divider costs one word and saves ever deducing it again. */
    uint32_t qpi_clkdiv;       /* 6 = 25 MHz                                  */
    uint32_t qpi_clkdiv_slow;  /* 30 = 5 MHz                                  */
    uint32_t qpi_slow[2];      /* the same READ_ID in quad, more slowly       */
} uvm2_psram_result_t;

extern volatile uvm2_psram_result_t uvm2_psram_result;

/* Probe. It does NOT arm the XIP window and writes nothing: first find out what is there. */
int uvm2_psram_probe(void);

/* Holds CS1 asserted (or released) INDEFINITELY, so pin 1 of U3 can be measured with a
 * multimeter. It answers what no other test has looked at: whether the QMI really drives
 * the pin, or only its registers say so. It does not return to the previous state — you
 * leave by resetting. See the long comment in the .c. */
void uvm2_psram_hold_cs(int asserted);

/* Repeats READ_ID NON-STOP so the clock can be measured as DC on pin 6 of U3: ~1.6 V = it
 * toggles; a fixed 0 or 3.3 V = the QMI is not clocking. It neither returns nor draws (a
 * black screen IS the signal); you leave by resetting. */
void uvm2_psram_hammer(void);

/* `n` transactions, then RETURNS, so drawing can continue. The signal that it is hammering
 * becomes something you can SEE, instead of the absence of a picture — which is what a hung
 * program looks like too. */
void uvm2_psram_burst(unsigned n);

/* DIRECT_CSR right after writing a byte: says whether the queue accepts and drains. */
extern volatile uint32_t uvm2_csr_after_tx;

/* How long /CS stays LOW in one transaction, in nanoseconds. The APS6404's tCEM is 8000 ns:
 * above that the chip is entitled to walk out, and would go mute just like ours did.
 * Measured with the core's cycle counter, not estimated by counting clocks — which is what
 * was done before and does not include the polling between bytes. */
uint32_t uvm2_psram_cs_low_ns(void);

/* THE REFERENCE. The same READ_ID through the same direct mode but to the FLASH (CS0),
 * which shares clock and data with the PSRAM and is known to work. If it answers, the path
 * is good and the suspect is U3; if it is mute, the fault is ours. No bootrom: here
 * rom_flash_exit_xip() does not return. Returns (b0<<16)|(b1<<8)|b2. */
uint32_t uvm2_flash_id(void);

/* Takes the flash out of continuous read and resets it, so we start from a known state
 * before talking to CS1. See the long note in the .c. */
void uvm2_flash_exit_xip(void);

/* The QMI/XIP state exactly as whichever firmware hands it to us. It is taken BEFORE
 * touching anything; comparing it across two boards is all that is left when the QMI cannot
 * be reset and the code has already been cleared. */
typedef struct {
    uint32_t direct_csr, xip_ctrl, m0_timing, m0_rfmt, m0_rcmd, m1_timing;
    uint32_t pad_sclk, pad_sd0, pad_ss;   /* PADS_QSPI block, 0x40040000 */
    uint32_t fn_sclk, fn_sd0;             /* FUNCSEL, IO_QSPI block 0x40030000 */
} uvm2_qmi_state;

/* Clears the isolation on the QSPI bus pads. It is needed because the image is launched by
 * the bootrom after a reset and, being a RAM image, it does not prepare them. */
void uvm2_qmi_bring_up(void);

/* MINIMAL bring-up: reset, ID and window. Returns 1 if the chip is there. Booting does not
 * need the diagnostic probe, which can leave the chip in QPI. */
int uvm2_psram_init(void);

/* Arms the PSRAM's XIP window (M1) and makes it writable. Without this, writes to
 * 0x11000000 are discarded silently. */
void uvm2_psram_enable_xip(void);

void uvm2_qmi_snapshot(uvm2_qmi_state *e);

/* Enters direct mode closing the gap it jams in: interrupts off, barriers, and optionally
 * the XIP cache disabled. Returns how many passes BUSY took to drop (100000 = it never
 * did). */
uint32_t uvm2_qmi_enter_direct(int cache_off);

/* 1 = drive OE when transmitting on ONE line (the default). It exists so an A/B comparison
 * needs no recompile: the code used to assume that on a single line SD0 drives itself. */
extern int uvm2_tx_oe;

/* 1 = do not take the "already mapped" shortcut: do the READ_ID anyway. */
extern int uvm2_psram_force;

/* The same question, asked in QPI. No bootrom: it cannot hang. */
int uvm2_psram_probe_qpi(void);

/* The same question down the bootrom path.
 *
 * CAREFUL: MEASURED 2026-08-17, rom_flash_exit_xip() DOES NOT RETURN on this cartridge —
 * and already on the first call, the CS0 one, before CS1 is touched. That is why it lives
 * behind -DUVM2_PSRAM_BOOTROM and is not compiled by default: a probe that hangs costs a
 * whole card trip and returns a single bit. */
int uvm2_psram_probe_bootrom(void);

#ifdef __cplusplus
}
#endif

#endif /* UVM2_PSRAM_H */
