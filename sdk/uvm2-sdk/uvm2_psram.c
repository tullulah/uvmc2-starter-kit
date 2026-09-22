/*
 * uvm2_psram.c — does the UVM2 have PSRAM, and do we have to bring it up ourselves?
 *
 * The schematic says yes: U3 hangs off the same QSPI bus as the flash (U6, a W25Q128) and
 * its select, PSRAM_CS, comes out of pin 58 = GPIO47. GPIO47 is QMI CS1n in function F9.
 *
 * What the schematic CANNOT say, and this file answers:
 *   1. whether the chip is populated at all (it looks marked DNP)
 *   2. which chip it is and how big it is (READ_ID)
 *   3. whether the cartridge firmware ALREADY leaves it mapped, in which case there is
 *      nothing to bring up and reading 0x11000000 is enough
 *
 * The registers come from the pico-sdk headers: making up an address here is expensive.
 *
 * THE RESULTS ARE GLOBAL ON PURPOSE. With SWD attached they can be read directly, with no
 * blink codes to decode:
 *
 *     probe-rs read b32 --chip RP235x <&uvm2_psram_result> 8
 */

#include <stdint.h>
#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "pico/bootrom.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/sio.h"
#include "hardware/clocks.h"
#include "uvm2_psram.h"

/* GPIO47 = PSRAM_CS on the UVM2 (pin 58). */
#define PSRAM_CS_GPIO   47u
/* Table 3 of the datasheet: F9 on GPIO 0/8/19/47 is QMI CS1n. */
#define FUNCSEL_QMI_CS1N 9u

#define CMD_RESET_ENABLE 0x66u
#define CMD_RESET        0x99u
#define CMD_READ_ID      0x9Fu
/* Quad commands, for the XIP window (the probe did not need them). */
#define CMD_QUAD_READ  0xEBu
#define CMD_QUAD_WRITE 0x38u
/* APS6404: 0x35 enters QPI, 0xF5 leaves it. In QPI the chip IGNORES single-line commands,
 * which is exactly what we were seeing: a real transfer (rx_timeouts = 0, BUSY and CS1
 * correct) and an answer of 0x00. And the cartridge is USB-C powered, so switching the
 * console off does not cut its power: it can still be in QPI from an earlier session,
 * including one of our own probes. */
#define CMD_EXIT_QPI     0xF5u
/* AP Memory. */
#define AP_MF_ID         0x0Du

/* A device that does not answer must not be able to hang the cartridge. */
#define QMI_SPIN_LIMIT   1000000u

/* The XIP window of CS1. */
#define PSRAM_XIP_BASE   0x11000000u

/* Reset value of M1_RFMT. If it differs, somebody configured CS1 before we did. */
#define UVM2_M1_RFMT_RESET 0x00001000u

volatile uvm2_psram_result_t uvm2_psram_result;

static void spin(uint32_t n) { while (n--) __asm volatile ("nop"); }

/* RP2350 pads come up ISOLATED, and a pad with ISO set does nothing whatever its FUNCSEL
 * says. That already cost a whole session: CS1 was not driving and the self-diagnostic
 * read 0xFF without a single clue. */
static void configure_cs1_pad(void)
{
    pads_bank0_hw->io[PSRAM_CS_GPIO] &= ~PADS_BANK0_GPIO0_ISO_BITS;
    pads_bank0_hw->io[PSRAM_CS_GPIO] |=  PADS_BANK0_GPIO0_IE_BITS;
    pads_bank0_hw->io[PSRAM_CS_GPIO] &= ~PADS_BANK0_GPIO0_OD_BITS;
    io_bank0_hw->io[PSRAM_CS_GPIO].ctrl = FUNCSEL_QMI_CS1N;
}

static void direct_begin(void)
{
    uint32_t spins = 0;
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    /* Wait for any XIP transfer still in flight to finish. */
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
}

static void direct_end(void)
{
    /* Release CS1 BEFORE switching off: ASSERT_CSxN still applies with EN at zero, and a
     * stuck chip select would corrupt the XIP reads of the flash on CS0. */
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
}

/* OE IS DRIVEN IN SINGLE-LINE MODE TOO.
 *
 * There used to be an ASSUMPTION written here as if it were a fact: "on a single line SD0
 * is always an output, so OE was not needed". If that is false we have never driven MOSI —
 * and then no chip receives anything and we read the idle bus: 0x00 on one line, 0xcc in
 * quad. Which is EXACTLY what we kept seeing.
 *
 * What gave it away: the same transaction against the FLASH — known good, because the
 * firmware boots from it — also returned zero. With both chips mute down the same path, the
 * chip stops being the suspect.
 *
 * `uvm2_tx_oe` lets it be turned off so an A/B comparison needs no recompile. */

/* THE DIRECT-MODE CLOCK DIVIDER. Without setting it, DIRECT_CSR keeps whatever it had, and
 * with CLKDIV at zero THERE IS NO CLOCK: the QMI transfers nothing, no chip receives
 * anything, and everyone looks mute.
 *
 * MEASURED WITH A SCOPE on 2026-08-19: with the hammer loop running, SCK and MOSI were
 * FLAT, millivolts of noise. No clock and no data. And the serious part: the original probe
 * (uvm2_psram_probe) did not set it either, so the earlier diagnosis — "the chip is there
 * and does not answer" — rested on transactions that may never have been emitted. Only the
 * QPI probes set it, and they are the only ones that ever saw anything other than zero
 * (0xcc).
 *
 * 6 = 25 MHz with a 150 MHz system clock. */
#define DIRECT_CLOCK_DIV 6u
static void direct_clock(uint32_t div);

int uvm2_tx_oe = 1;

/* 1 = interrogate the chip even when the window is already mapped. It exists so the full
 * path can be run where the hardware DOES work, for comparison. */
int uvm2_psram_force = 0;

static void tx(uint8_t byte)
{
    uint32_t spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    qmi_hw->direct_tx = uvm2_tx_oe ? (QMI_DIRECT_TX_OE_BITS | byte) : (uint32_t)byte;
}

static uint8_t rx(void)
{
    uint32_t spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    if (spins >= QMI_SPIN_LIMIT) uvm2_psram_result.rx_timeouts++;
    return (uint8_t)qmi_hw->direct_rx;
}

/* One byte on its own in QUAD. On four lines the output enable (OE) has to be driven; on a
 * single line SD0 is always an output, which is why it was not needed there. */
static void cs1_xfer_quad1(uint8_t byte)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint32_t spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS
                      | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB)
                      | byte;
    spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

/* One transaction on CS1: assert, push `cmd`, pull `n_rx` bytes, release. The QMI is
 * full-duplex, so every byte sent produces one received; the ones from the command phase
 * are thrown away. */
static void cs1_xfer(const uint8_t *cmd, uint32_t n_cmd, uint8_t *rx_buf, uint32_t n_rx)
{
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;   /* 1 = ASSERTED (low) */
    for (uint32_t i = 0; i < n_cmd; i++) {
        tx(cmd[i]);
        if (i == 0) uvm2_psram_result.csr_after_cmd = qmi_hw->direct_csr;
        (void)rx();
    }
    for (uint32_t i = 0; i < n_rx;  i++) { tx(0x00);   rx_buf[i] = rx(); }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

/* ---- CS HELD STATIC, FOR A MULTIMETER --------------------------------------
 *
 * WHICH GAP THIS FILLS. Reading EN=1, ASSERT_CS1N=1 and BUSY=1 during a transaction only
 * proves what the INTERNAL REGISTERS say, not what the pin does. And the continuity check
 * on the line was done driving it as a GPIO, which tests the TRACE, not the QMI's path to
 * it. Nobody has ever measured pin 1 of U3 while the QMI drives it, and that is the first
 * remaining fork in the tree.
 *
 * WHY STATIC. A CS that toggles for 2 us is invisible to a multimeter, and a scope on this
 * board means taking it apart and using ONE single ground clip (two on different nodes
 * already destroyed a console). Holding it asserted indefinitely makes it a DC measurement
 * that any multimeter can take:
 *
 *     asserted    -> U3 pin 1 at ~0 V    : the QMI DOES reach the chip. What is left is
 *                                          the chip or its die.
 *     asserted    -> pin 1 at ~3.3 V     : the QMI does NOT drive the pin, whatever the
 *                                          registers claim. The fault is before the chip.
 *     not asserted-> pin 1 at ~3.3 V     : the control, via pull-up R6.
 *
 * THERE IS NO WAY BACK from here: it deliberately leaves CS1 asserted, which is what makes
 * the measurement possible. You leave by resetting.
 */
void uvm2_psram_hold_cs(int asserted)
{
    direct_clock(DIRECT_CLOCK_DIV);   /* without this there is no clock */
    configure_cs1_pad();
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    uint32_t spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    if (asserted) qmi_hw->direct_csr |=  QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    else          qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uvm2_psram_result.csr_after_cmd = qmi_hw->direct_csr;   /* read back, not assumed */
}

/* ---- THE REFERENCE: THE SAME QUESTION, ASKED OF THE FLASH ------------------
 *
 * REFERENCE BEFORE SUSPECT, SAME RIG. Flash U6 shares SCK, MOSI and MISO with the PSRAM and
 * works — the firmware boots from it. So asking it for its JEDEC ID through THE SAME direct
 * mode, the same code and the same pins, changing only which of the two selects is
 * asserted, splits the problem in two:
 *
 *   the flash ANSWERS -> the path (clock, MOSI, MISO sampling, direct mode) is good. What
 *                        fails is U3 or its solder joints.
 *   the flash IS MUTE -> our direct mode is wrong, and U3 has been accused all this time
 *                        of a fault that is ours.
 *
 * IT DOES NOT GO THROUGH THE BOOTROM. do_cmd_cs() below does, and rom_flash_exit_xip() DOES
 * NOT RETURN on this cartridge (measured 2026-08-17). This image runs entirely from SRAM,
 * so taking the bus bothers nobody and there is no need to leave XIP.
 *
 * Returns the three ID bytes packed: (b0<<16)|(b1<<8)|b2. A W25Q128 gives 0xEF4018;
 * 0x000000 or 0xFFFFFF means "no answer".
 */

/* ENTER DIRECT MODE WITHOUT JAMMING THE QMI.
 *
 * On the UVM2, BUSY starts at zero and goes up AS SOON AS we ask for direct mode: we are
 * not inheriting a jammed QMI, we are jamming it ourselves. The explanation that fits the
 * register dumps from both boards: setting EN stops the QMI serving XIP, and an XIP read
 * ALREADY IN FLIGHT can no longer finish — the bus that would service it is taken by direct
 * mode. Deadlock. The third-party firmware reads the flash with a single-line 03h, which is
 * slow, i.e. a much wider window in which to catch a half-finished read; our own firmware
 * does its init from `.data` with XIP idle, which is why we never tripped over it.
 *
 * `cache_off`: on top of the barriers, flush the XIP cache before entering, in case what is
 * in flight is a cache-line fill rather than a program read.
 *
 * Returns how many passes BUSY took to drop; hitting the limit means it never did.
 */
uint32_t uvm2_qmi_enter_direct(int cache_off)
{
    volatile uint32_t *xip_ctrl = (volatile uint32_t *)0x400C8000u;
    uint32_t saved = *xip_ctrl;
    uint32_t passes = 0;

    /* No interrupts: a handler that reads from flash would push another transfer into the
     * very gap we are trying to close. */
    __asm volatile ("cpsid i" ::: "memory");

    if (cache_off) {
        *xip_ctrl = saved & ~0x3u;      /* EN_SECURE | EN_NONSECURE */
        __asm volatile ("dsb" ::: "memory");
    }

    /* Leave NOTHING in flight before taking the bus. */
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++passes < 100000u) { }

    if (cache_off) *xip_ctrl = saved;
    __asm volatile ("cpsie i" ::: "memory");
    return passes;
}

/* THE MINIMUM SEQUENCE.
 *
 * The loader used to call `uvm2_psram_probe()`, which is a DIAGNOSTIC PROBE: it tries
 * several dialects (SPI, QPI, entering and leaving QPI) and promises nothing about the
 * state it leaves the chip in. If it leaves it in QPI, the window — which sends its command
 * on ONE line — reads garbage. And that is exactly what we saw: a correct READ_ID in direct
 * mode, and the uncached read returning a 0/4/8/C pattern, i.e. nobody driving the data
 * lines.
 *
 * Booting needs no diagnosis: reset, check the ID, arm the window.
 */
int uvm2_psram_init(void)
{
    const uint8_t c_rsten[1] = { CMD_RESET_ENABLE };
    const uint8_t c_rst[1]   = { CMD_RESET };
    const uint8_t c_id[4]    = { CMD_READ_ID, 0x00, 0x00, 0x00 };
    uint8_t id[2] = { 0, 0 };

    direct_clock(DIRECT_CLOCK_DIV);
    configure_cs1_pad();

    direct_begin();
    cs1_xfer(c_rsten, 1, 0, 0);
    cs1_xfer(c_rst,   1, 0, 0);
    direct_end();
    spin(30000);                    /* tRST */

    direct_begin();
    cs1_xfer(c_id, 4, id, 2);
    direct_end();

    if (id[0] != 0x0D) return 0;
    uvm2_psram_enable_xip();
    return 1;
}

/* ARM THE PSRAM'S XIP WINDOW (M1).
 *
 * The chip answering its ID tests direct mode, NOT the window: they are two different paths
 * through the QMI. With M1 unconfigured and without WRITABLE_M1, writes to 0x11000000 are
 * discarded SILENTLY — the worst possible failure, because it looks like it works.
 *
 * The two timing constants are the proven ones: MAX_SELECT bounds how long the select may
 * stay low (the APS6404 is DRAM and has to refresh) and MIN_DESELECT the gap between
 * transactions. */
void uvm2_psram_enable_xip(void)
{
    const uint32_t Q = 2u;   /* quad width   */
    const uint32_t S = 0u;   /* serial width */

    *(volatile uint32_t *)0x400C8000u |= (1u << 11);   /* XIP_CTRL.WRITABLE_M1 */

/* PAGEBREAK: SPLIT BURSTS AT THE PAGE BOUNDARY.
 *
 * The APS6404L has 1024-byte pages, and a linear burst crossing that boundary WRAPS AROUND
 * INSIDE THE PAGE instead of continuing into the next one. The symptom only shows up when
 * you copy for real: with two lone words at far-apart addresses no transaction crosses a
 * page, and the chip looks perfect.
 *
 * And the trap that kept it hidden: a verification that reads THROUGH THE CACHE says
 * nothing about the chip. The XIP cache is 16 KB; a 64 KB list verifies fine right after it
 * is written (hot cache) and reads back wrong later (lines already evicted, coming from the
 * chip). Both observations fit without contradicting each other, which is why the check
 * seemed to clear it. To test the CHIP you have to read through the uncached alias,
 * 0x15000000. */
#ifndef UVM2_PSRAM_PAGEBREAK
#define UVM2_PSRAM_PAGEBREAK 2u        /* qmi.h: 0=NONE 1=256 2=1024 3=4096 */
#endif

    qmi_hw->m[1].timing =
/* THE DIVIDER IS TUNABLE, and for a reason: the 2 comes from a board whose firmware pins
 * the system clock at 150 MHz. Here we arrive from a bootrom reboot and NOBODY has checked
 * what frequency it starts at — a divider meant for another clock breaks READS (which need
 * round-trip timing) and not writes, which is exactly the symptom: the copy verifies fine
 * through the cache and the code cannot be executed. */
/* MAX_SELECT bounds how long CS may stay low in one go, and the DRAM's refresh depends on
 * it. The APS6404L gives tCEM = 8 us as an absolute maximum.
 *
 * At 150 MHz each unit is 64 cycles = 427 ns, and an access already in flight when the
 * limit hits still FINISHES (about 1.1 us more). With 15: 6.4 + 1.1 = 7.5 us out of 8 — it
 * fits, but with 0.5 us of margin, and that margin is only ever stressed by a LONG copy:
 * two lone writes never hold CS low long enough. Which is why the PSRAM looked fine.
 * 8 units: 3.4 + 1.1 = 4.5 us, half the limit. */
#ifndef UVM2_PSRAM_MAX_SELECT
#define UVM2_PSRAM_MAX_SELECT 15u
#endif

#ifndef UVM2_PSRAM_CLKDIV
#define UVM2_PSRAM_CLKDIV 2u
#endif
          (UVM2_PSRAM_PAGEBREAK << QMI_M1_TIMING_PAGEBREAK_LSB)
        | (UVM2_PSRAM_CLKDIV << QMI_M1_TIMING_CLKDIV_LSB)
        | (1u  << QMI_M1_TIMING_RXDELAY_LSB)
        | (UVM2_PSRAM_MAX_SELECT << QMI_M1_TIMING_MAX_SELECT_LSB)   /* refresh: SEE BELOW */
        | (4u  << QMI_M1_TIMING_MIN_DESELECT_LSB)
        | (1u  << QMI_M1_TIMING_COOLDOWN_LSB);
    /* ON MAX_SELECT = 15.
     *
     * PAGEBREAK=1024 and MAX_SELECT=8 were set once on the strength of two measurements
     * that turned out to be tainted: the copy was verified by reading the CACHE, so neither
     * the "COPY DONE" nor the sweep offsets said anything about the chip. A reference board
     * has been reading and writing this same APS6404L for months with 15 and no split — its
     * `R` variant does cross the row boundary in a linear burst — and that is the only
     * configuration with hours of flight behind it. Departing from it needs a good
     * measurement, and there was none.
     *
     * (The reasoning below is kept because the tCEM argument is correct and will have to be
     * revisited if a clean measurement ever asks for it.)
     *
     * PAGEBREAK IS NOT OPTIONAL ON THIS CHIP -- IF the variant does not cross rows.
     *
     * The APS6404L has 1024-byte pages: a linear burst crossing that limit wraps around
     * INSIDE the page instead of continuing into the next one. Without a split there, a long
     * copy writes the first 1024 bytes where they belong and then batters that same page
     * with everything else.
     *
     * It slipped through because the test that passed the PSRAM wrote two words at
     * addresses 4 MB apart: two short transactions, neither crossing a page. The symptom
     * only appears on a real copy — the loader saw the FIRST word of the payload right and
     * the LAST one wrong, which is exactly this shape. */

    qmi_hw->m[1].rcmd = CMD_QUAD_READ;               /* 0xEB, suffix 0 */
    qmi_hw->m[1].rfmt =
          (S  << QMI_M1_RFMT_PREFIX_WIDTH_LSB)
        | (Q  << QMI_M1_RFMT_ADDR_WIDTH_LSB)
        | (Q  << QMI_M1_RFMT_SUFFIX_WIDTH_LSB)
        | (Q  << QMI_M1_RFMT_DUMMY_WIDTH_LSB)
        | (Q  << QMI_M1_RFMT_DATA_WIDTH_LSB)
        | (1u << QMI_M1_RFMT_PREFIX_LEN_LSB)         /* 8 bits */
        | (0u << QMI_M1_RFMT_SUFFIX_LEN_LSB)
        | (6u << QMI_M1_RFMT_DUMMY_LEN_LSB);         /* 24 bits = 6 quad cycles */

    qmi_hw->m[1].wcmd = CMD_QUAD_WRITE;              /* 0x38 */
    qmi_hw->m[1].wfmt =
          (S  << QMI_M1_WFMT_PREFIX_WIDTH_LSB)
        | (Q  << QMI_M1_WFMT_ADDR_WIDTH_LSB)
        | (Q  << QMI_M1_WFMT_SUFFIX_WIDTH_LSB)
        | (Q  << QMI_M1_WFMT_DUMMY_WIDTH_LSB)
        | (Q  << QMI_M1_WFMT_DATA_WIDTH_LSB)
        | (1u << QMI_M1_WFMT_PREFIX_LEN_LSB)
        | (0u << QMI_M1_WFMT_SUFFIX_LEN_LSB)
        | (0u << QMI_M1_WFMT_DUMMY_LEN_LSB);
}

/* BRING THE QMI UP OURSELVES. There is no environment to inherit: the cartridge launcher
 * copies the image into SRAM and calls rom_reboot(RAM_IMAGE) — we come from a RESET, and
 * the bootrom, launching an image in RAM, needs no XIP at all and leaves the QMI at its
 * minimum. (Measured: M0_TIMING 40000004 and M0_RFMT 00001000, essentially reset values,
 * against 60007203 and 000492A8 on a board that boots from flash.)
 *
 * What is missing are the QSPI bus PADS. On the RP2350 pads come up ISOLATED, and setting
 * the function without clearing ISO leaves a mute pin — the same trap that already cost a
 * session with the PSRAM select. There are six here: clock, four data and the flash select.
 *
 * It is exactly the symptom: the QMI accepts the transfer and nothing comes out of the
 * pins, because it is driving into an isolation buffer.
 */
void uvm2_qmi_bring_up(void)
{
    /* TWO BLOCKS, not one. The pad (PADS_QSPI) says what the pin is like electrically; the
     * FUNCSEL (IO_QSPI) says WHO DRIVES IT. Configuring the pad and not the function leaves
     * a mute pin — the very mistake that cost a session with the PSRAM select, and one I
     * then repeated: the pads read 0x56 (no isolation, IE set) both before and after, i.e.
     * that side was already fine.
     *
     * CTRL of each pin in IO_QSPI, and FUNCSEL 0 = XIP (driven by the QMI). */
    {
        volatile uint32_t *io = (volatile uint32_t *)0x40030000u;
        static const unsigned ctrl[6] = { 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C };
        int k;
        for (k = 0; k < 6; k++) {
            volatile uint32_t *r = (volatile uint32_t *)((char *)io + ctrl[k]);
            *r = (*r & ~0x1Fu) | 0u;      /* FUNCSEL = 0: XIP */
        }
    }

    {
    volatile uint32_t *pq = (volatile uint32_t *)0x40040000u;
    int i;
    /* [1]=SCLK [2..5]=SD0..SD3 [6]=SS. [0] is VOLTAGE_SELECT; leave it alone. */
    for (i = 1; i <= 6; i++) {
        uint32_t v = pq[i];
        v &= ~(1u << 8);      /* ISO: drop the isolation */
        v &= ~(1u << 7);      /* OD: output NOT disabled */
        v |=  (1u << 6);      /* IE: input enabled (the QMI has to read) */
        pq[i] = v;
    }
    }
}

/* THE STATE WE INHERIT, exactly as it is, touching nothing.
 *
 * BUSY never drops on the UVM2 and does drop on a reference board, with the SAME code. The
 * QMI cannot be reset (the chip boots from it, so it is not in the reset block) and the
 * RP2040's stream counter does not exist on the RP2350, so there is no way to force it
 * loose. What can be done is to LOOK at the state each board hands us and subtract.
 *
 * Call this FIRST, before touching anything, or you measure your own effect. */
void uvm2_qmi_snapshot(uvm2_qmi_state *e)
{
    /* THE QSPI PADS, which live in THEIR OWN block (0x40040000) and not with the normal
     * GPIOs. We had never looked at them: the select pad (GPIO47, which is a normal pad) was
     * configured by hand and the bus pads were assumed fine because the firmware boots from
     * that flash. But our image runs from SRAM: from the moment it starts, NOBODY uses the
     * flash, so an isolated pad would go unnoticed.
     *
     * MOSI stuck at 3.3 V and SCK still, with the QMI declaring the transfer good, is
     * exactly what an output-disabled pad looks like. */
    {
        volatile uint32_t *pq = (volatile uint32_t *)0x40040000u;
        e->pad_sclk = pq[1];   /* GPIO_QSPI_SCLK */
        e->pad_sd0  = pq[2];   /* GPIO_QSPI_SD0  */
        e->pad_ss   = pq[6];   /* GPIO_QSPI_SS   */
        e->fn_sclk  = *(volatile uint32_t *)0x40030014u;   /* SCLK_CTRL */
        e->fn_sd0   = *(volatile uint32_t *)0x40030024u;   /* SD0_CTRL  */
    }
    e->direct_csr = qmi_hw->direct_csr;
    e->xip_ctrl   = *(volatile uint32_t *)0x400C8000u;   /* XIP_CTRL */
    e->m0_timing  = qmi_hw->m[0].timing;
    e->m0_rfmt    = qmi_hw->m[0].rfmt;
    e->m0_rcmd    = qmi_hw->m[0].rcmd;
    e->m1_timing  = qmi_hw->m[1].timing;
}

/* Put the FLASH into a known state: leave continuous-read mode and reset it.
 *
 * WHY IT MATTERS FOR THE PSRAM. The probe code is correct — proven on a reference board:
 * mf_id 0x0D, kgd 0x5D — so what changes on the UVM2 is the ENVIRONMENT: the .um2 image
 * starts on a machine the third-party firmware configured, with its flash in XIP and the
 * QMI set up its way, instead of owning it from reset.
 *
 * It is safe: the image runs entirely from SRAM, nobody is reading the flash, and the
 * bootrom reinitialises it on the next reboot. */
void uvm2_flash_exit_xip(void)
{
    const uint8_t exit_seq[3] = { 0xFFu, 0x66u, 0x99u };
    int k;
    direct_clock(DIRECT_CLOCK_DIV);
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    {
        uint32_t spins = 0;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    }
    for (k = 0; k < 3; k++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
        tx(exit_seq[k]); (void)rx();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
}

uint32_t uvm2_flash_id(void)
{
    direct_clock(DIRECT_CLOCK_DIV);   /* without this there is no clock */
    const uint8_t cmd = 0x9Fu;          /* JEDEC ID: no address, three bytes */
    uint8_t id[3] = { 0, 0, 0 };
    uint32_t spins = 0;
    int i;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }

    /* TAKE IT OUT OF XIP MODE FIRST. The cartridge firmware boots from this flash, and a
     * flash in `continuous read` does NOT answer normal commands: it sits there waiting for
     * addresses. That is what rom_flash_exit_xip() is for — and it does not return here. So
     * it is done by hand, three lone bytes, each in its own selection:
     *   0xFF  reset continuous read mode
     *   0x66  enable reset       0x99  reset
     * It is safe: this image runs entirely from SRAM, nobody is reading the flash, and the
     * bootrom reinitialises it on the next reboot. */
    uvm2_flash_exit_xip();

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    tx(cmd); (void)rx();
    for (i = 0; i < 3; i++) { tx(0x00); id[i] = rx(); }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;

    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

/* ---- HOW LONG /CS STAYS LOW ------------------------------------------------
 *
 * THE APS6404 IS DRAM INSIDE and has to refresh: its **tCEM is 8 us**, which means it is
 * entitled to abandon any transaction that holds /CS low for longer. A chip that aborts on
 * tCEM goes mute EXACTLY like ours did.
 *
 * The code here already reasoned about tCEM, but counting CLOCKS: "12 bytes in quad at
 * 25 MHz is ~1 us, plenty". That ignores how long the software takes between bytes — and in
 * direct mode every byte carries a TXFULL/RXEMPTY poll, which are register accesses, with
 * the select LOW the whole time. The estimate can be an order of magnitude short, and
 * nobody had measured it.
 *
 * It is measured with the core's cycle counter (DWT), which gives 6.7 ns of resolution at
 * 150 MHz — a `time_us_32()` cannot tell 2 us from 8.
 */
uint32_t uvm2_psram_cs_low_ns(void)
{
    direct_clock(DIRECT_CLOCK_DIV);   /* without this there is no clock */
    const uint8_t cmd[4] = { CMD_READ_ID, 0, 0, 0 };
    uint8_t id[2];
    uint32_t t0, t1;

    /* DWT: enable the trace unit and the cycle counter. */
    *(volatile uint32_t *)0xE000EDFC |= (1u << 24);   /* DEMCR.TRCENA  */
    *(volatile uint32_t *)0xE0001000 |= 1u;           /* DWT_CTRL.CYCCNTENA */

    configure_cs1_pad();
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
    uint32_t spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }

    t0 = *(volatile uint32_t *)0xE0001004;            /* DWT_CYCCNT */
    cs1_xfer(cmd, 4, id, 2);                          /* assert, transfer, release */
    t1 = *(volatile uint32_t *)0xE0001004;

    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;

    {
        uint32_t cycles = t1 - t0;
        uint32_t hz = clock_get_hz(clk_sys);
        if (!hz) return 0;
        /* ns, without floats and without overflowing: cycles * (1e9/hz). */
        return (uint32_t)((unsigned long long)cycles * 1000000000ull / hz);
    }
}

/* ---- THE CLOCK, ALSO FOR A MULTIMETER --------------------------------------
 *
 * We know (2026-08-19) that the QMI drives CS1 all the way to pin 1 of U3: asserted gives
 * 0 V and released 3.3 V. What NOBODY has looked at is whether the CLOCK gets there in the
 * meantime. If SCK does not toggle, the chip cannot answer however well the select arrives
 * — and that would explain everything without the chip being dead.
 *
 * ALSO WITH A MULTIMETER, no scope needed: one transaction lasts microseconds and a tester
 * cannot see it, but repeating it NON-STOP the clock spends half its time high, and in DC
 * that reads as ~1.65 V. Stopped, it reads a fixed level (0 or 3.3).
 *
 *     ~1.6 V on U3 pin 6  -> the clock DOES arrive. The chip gets select and clock and is
 *                            still mute: the suspect is U3.
 *     a fixed 0 or 3.3 V  -> the QMI is not clocking. The fault is QMI configuration, not
 *                            the chip.
 *
 * IT DRAWS NOTHING on purpose: the screen stays black, and that is the signal that it is
 * hammering. You leave by resetting.
 */

/* DIRECT_CSR right after writing a byte into the queue. */
volatile uint32_t uvm2_csr_after_tx = 0;

/* A BURST of transactions, not an endless loop. It hands control back so the program keeps
 * drawing: that way "it is hammering" has a POSITIVE signal on screen instead of the
 * absence of a picture — which is indistinguishable from a hang, and was the weak link in
 * the scope measurement. A scope is happy with bursts: it triggers on an edge, not on a
 * mid-level. */
void uvm2_psram_burst(unsigned n)
{
    const uint8_t cmd[4] = { CMD_READ_ID, 0, 0, 0 };
    uint8_t id[2];
    unsigned i;

    direct_clock(DIRECT_CLOCK_DIV);
    configure_cs1_pad();
    for (i = 0; i < n; i++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
        uint32_t spins = 0;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
        /* THE MISSING SNAPSHOT. CS moves — we move it with a bit — but neither clock nor
         * data come out. What is missing is whether the write reaches the queue and is
         * never drained, or never arrives: TXEMPTY (bit 11) and TXLEVEL (bits 12-14) say. */
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = 0x9Fu;
        uvm2_csr_after_tx = qmi_hw->direct_csr;
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        cs1_xfer(cmd, 4, id, 2);
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
    }
}

void uvm2_psram_hammer(void)
{
    direct_clock(DIRECT_CLOCK_DIV);   /* without this there is no clock */
    const uint8_t cmd[4] = { CMD_READ_ID, 0, 0, 0 };
    uint8_t id[2];

    configure_cs1_pad();
    for (;;) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_EN_BITS;
        uint32_t spins = 0;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
        cs1_xfer(cmd, 4, id, 2);
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_EN_BITS;
    }
}

/* ---- THE BOOTROM PROBE ------------------------------------------------------
 *
 * WHAT THE OTHER ONE WAS MISSING. The probe below talks to the chip through direct mode, by
 * hand, and measured that the QMI DOES transfer (rx_timeouts = 0, CS1 and BUSY correct) and
 * that the chip is mute — at 25, 12.5, 5 and 1.25 MHz. That led to "software exhausted,
 * time for a scope". It was false, and our own SDK says so in hardware/flash.h:
 *
 *   "The ROM uses this device information to control some low-level flash API behaviour,
 *    such as issuing an XIP exit sequence to CS 1 IF ITS SIZE IS NONZERO."
 *
 * FLASH_DEVINFO reports NONE for CS1 unless the OTP says otherwise, so **that sequence has
 * never been sent to the PSRAM**: not by the bootrom, not by the cartridge firmware (its
 * author: "I actually never used it so far"), not by us. And it fits what the other probe
 * already suspected: an APS6404 in QPI ignores single-line commands, the cartridge is USB-C
 * powered and does not lose power when the console is switched off, so it may have been in
 * QPI since one of our own probes. Lowering the clock does not uncover that: it is not a
 * speed problem, it is a language problem.
 *
 * So nothing is invented here. FLASH_DEVINFO is told CS1 has a nonzero size and the bootrom
 * is left to run ITS sequence. It is the same piece pico-sdk 2.3.0's hardware_psram uses
 * for the same purpose (psram_detect_size), ported to our 2.2.0 with public API only and
 * without patching the SDK.
 *
 * IT WRITES NOTHING TO THE FLASH. The two commands issued from here are 0x9F, identifier
 * reads; flash_do_cmd leaves XIP as it found it.
 */
#define AP_KGD_ID  0x5Du   /* known good die; this is what the SDK looks at, not the MF ID */

/* From EID to size, the same as psram_eid_to_size() in 2.3.0. */
static uint32_t eid_to_size(uint8_t kgd, uint8_t eid)
{
    if (kgd != AP_KGD_ID) return 0;
    uint32_t mb = 1u << 20;
    uint8_t  s  = (uint8_t)(eid >> 5);
    if (s == 4)                          return mb * 16u;
    if (eid == 0x26 || s == 2 || s == 3) return mb * 8u;
    if (s == 1)                          return mb * 4u;
    return mb * 2u;
}

/* The SDK's flash_do_cmd(), with the chip select as a parameter.
 *
 * IT IS NOT A COPY FOR FUN. Two things are needed that the SDK does not give here:
 *   - CS1. flash_do_cmd hardcodes CS0; the parameterised version is flash_do_cmd_cs, and
 *     that only lands in pico-sdk 2.3.0.
 *   - For it to exist at all. The whole block lives inside `#if !PICO_NO_FLASH` and our
 *     images are no_flash, so the link does not find it — verified.
 * What is copied is the sequence, and it only uses public API from pico/bootrom.h.
 *
 * The step that matters is rom_flash_exit_xip(), and its documentation in pico/bootrom.h
 * also says that rom_connect_internal_flash() initialises the GPIO of the second chip
 * select "if it has been configured by OTP or by WRITING THE BOOTRAM COPY OF
 * FLASH_DEVINFO". So the bootrom even sets up the pad for us. */

/* Breadcrumbs: the last value surviving in step_bootrom is the step that did not return. A
 * hang leaves no trace except the one you put there in advance — this probe hung once and
 * all we learned was "somewhere in one of six calls". */
#define STEP_ENTER        1
#define STEP_CONNECT      2   /* about to call rom_connect_internal_flash */
#define STEP_EXIT_XIP     3   /* about to call rom_flash_exit_xip         */
#define STEP_LOOP         4   /* about to move the bytes                  */
#define STEP_FLUSH        5   /* about to call rom_flash_flush_cache      */
#define STEP_ENTER_XIP    6   /* about to call rom_flash_enter_cmd_xip    */
#define STEP_LEAVE        7
static uint32_t n_call = 0;
#define CRUMB(p) (uvm2_psram_result.step_bootrom = n_call * 100u + (p))

static void do_cmd_cs(const uint8_t *tx, uint8_t *rx, uint32_t count, int cs)
{
    const uint32_t cs_bit = cs ? QMI_DIRECT_CSR_ASSERT_CS1N_BITS
                               : QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    uint32_t txr = count, rxr = count, spins = 0;

    n_call++;
    CRUMB(STEP_ENTER);

    CRUMB(STEP_CONNECT);
    rom_connect_internal_flash();
    CRUMB(STEP_EXIT_XIP);
    rom_flash_exit_xip();
    CRUMB(STEP_LOOP);

    hw_set_bits(&qmi_hw->direct_csr, cs_bit);
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    /* The QMI only jams when DIRECT_RX fills up, so there is no need to count how many are
     * in flight — but the pass limit stays: a device that does not answer must not be able
     * to hang the cartridge. */
    while ((txr || rxr) && ++spins < QMI_SPIN_LIMIT) {
        uint32_t f = qmi_hw->direct_csr;
        if (txr && !(f & QMI_DIRECT_CSR_TXFULL_BITS))  { qmi_hw->direct_tx = *tx++; txr--; }
        if (rxr && !(f & QMI_DIRECT_CSR_RXEMPTY_BITS)) { *rx++ = (uint8_t)qmi_hw->direct_rx; rxr--; }
    }
    if (spins >= QMI_SPIN_LIMIT) uvm2_psram_result.rx_timeouts++;
    /* Release the select BEFORE switching off, for the same reason as direct_end(). */
    hw_clear_bits(&qmi_hw->direct_csr, cs_bit);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

    CRUMB(STEP_FLUSH);
    rom_flash_flush_cache();
    CRUMB(STEP_ENTER_XIP);
    rom_flash_enter_cmd_xip();
    CRUMB(STEP_LEAVE);
}

/* READ_ID on CS0, to the flash. This is the positive control: if this does not answer
 * EF 40 18, what is broken is the path and not the chip on CS1. */
static uint32_t flash_jedec_id(void)
{
    const uint8_t tx[4] = { CMD_READ_ID, 0x00, 0x00, 0x00 };
    uint8_t       rx[4] = { 0, 0, 0, 0 };
    do_cmd_cs(tx, rx, 4, 0);
    /* A flash's 0x9F carries NO address: it answers from byte 1 on. */
    return ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
}

/* ---- TALKING TO IT IN QPI ---------------------------------------------------
 *
 * A WHOLE transaction on four lines, with the select held from start to end. This is what
 * was missing: cs1_xfer_quad1() sent ONE lone byte and went back to a single line, so
 * nothing has ever been read from the chip in its own language.
 *
 * The command phase drives OE (we drive all four lines); the data phase drops it, so the
 * chip drives instead. On a single line it was not needed because SD0 is always an output —
 * which is how the old probe got away without it.
 */
#define CMD_ENTER_QPI    0x35u

/* CLKDIV lives in DIRECT_CSR, so it is changed with direct mode OFF. Setting it with EN on
 * does not raise an error: it silently keeps the previous value. */
static void direct_clock(uint32_t div)
{
    uint32_t csr = qmi_hw->direct_csr & ~QMI_DIRECT_CSR_EN_BITS;
    csr = (csr & ~QMI_DIRECT_CSR_CLKDIV_BITS)
        | (div << QMI_DIRECT_CSR_CLKDIV_LSB);
    qmi_hw->direct_csr = csr;
}

static void cs1_qpi(const uint8_t *cmd, uint32_t n_cmd, uint8_t *rx, uint32_t n_rx)
{
    const uint32_t Q = QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB;
    uint32_t spins;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    for (uint32_t i = 0; i < n_cmd; i++) {
        spins = 0;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) && ++spins < QMI_SPIN_LIMIT) { }
        /* NOPUSH: the command phase produces no data worth keeping, and without this it
         * fills the receive FIFO and misaligns everything that follows. */
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_NOPUSH_BITS | Q | cmd[i];
    }
    for (uint32_t i = 0; i < n_rx; i++) {
        spins = 0;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) && ++spins < QMI_SPIN_LIMIT) { }
        qmi_hw->direct_tx = Q | 0xffu;          /* no OE: the chip drives now */
        spins = 0;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
        if (spins >= QMI_SPIN_LIMIT) uvm2_psram_result.rx_timeouts++;
        rx[i] = (uint8_t)qmi_hw->direct_rx;
    }

    spins = 0;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) && ++spins < QMI_SPIN_LIMIT) { }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

static void pack(uint8_t *b, volatile uint32_t *dst)
{
    dst[0] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    dst[1] = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
}

int uvm2_psram_probe_qpi(void)
{
    volatile uvm2_psram_result_t *r = &uvm2_psram_result;
    /* 0x9F plus 24 address bits, all in quad. EIGHT bytes are read and kept raw: we have no
     * measurement of the wait cycles of 0x9F in QPI, and assuming a position turns a
     * one-byte skew into a "mute chip". Seeing 0d 5d anywhere in the string is already the
     * answer. */
    const uint8_t c_id[4] = { CMD_READ_ID, 0x00, 0x00, 0x00 };
    uint8_t       b[8];

    /* Sentinel down: this one runs LAST, so if it hung here, the sentinel left set by
     * uvm2_psram_probe() would make the structure look complete. */
    r->magic = 0;

    /* THE CLOCK, SET BY HAND AND WRITTEN DOWN.
     *
     * The first version of this probe did not touch it, and inherited the CLKDIV = 120 that
     * uvm2_psram_probe() ends its sweep with and never restores. At 1.25 MHz the twelve
     * bytes of the quad transaction are ~19 us with the select LOW, and the **APS6404's
     * tCEM is 8 us**: the chip is entitled to walk out half way. So the one test that
     * attacks the language hypothesis was being run out of spec.
     *
     * CLKDIV 6 = 25 MHz: 12 bytes in quad is 24 clocks ~= 1 us, plenty. And the divider is
     * stored in the result, so it never has to be deduced again. */
    r->qpi_clkdiv = 6;
    direct_clock(6);

    configure_cs1_pad();

    /* A. As it stands. If it has been in QPI since an earlier probe, it answers. */
    for (int i = 0; i < 8; i++) b[i] = 0;
    direct_begin();
    cs1_qpi(c_id, 4, b, 8);
    direct_end();
    pack(b, r->qpi_direct);

    /* B. And in case it was in SPI: push it into QPI with 0x35 — on ONE line, which is
     *    where that command is heard — and ask again in quad. Between A and B both
     *    directions of the language are covered. */
    {
        const uint8_t c35[1] = { CMD_ENTER_QPI };
        direct_begin();
        cs1_xfer(c35, 1, 0, 0);
        direct_end();
        spin(30000);
    }
    for (int i = 0; i < 8; i++) b[i] = 0;
    direct_begin();
    cs1_qpi(c_id, 4, b, 8);
    direct_end();
    pack(b, r->qpi_after_35);

    /* C. And once more at 5 MHz (12 bytes ~= 5 us, still under tCEM). Two speeds separate
     *    "it cannot hear me" from "it does not have time", which at this point is the only
     *    distinction left to make without instruments. */
    r->qpi_clkdiv_slow = 30;
    direct_clock(30);
    for (int i = 0; i < 8; i++) b[i] = 0;
    direct_begin();
    cs1_qpi(c_id, 4, b, 8);
    direct_end();
    pack(b, r->qpi_slow);

    r->qpi_probe_run = 1;

    /* ---- SCOPE PROBE MODE (-DUVM2_PSRAM_LOOP) -----------------------------
     *
     * Everything above lasts ~1 us and happens ONCE at startup. On a two-channel scope that
     * cannot be captured: there is nothing to trigger on. With this, the same transaction
     * repeats forever with a gap between bursts, and triggering on the /CS edge is trivial.
     *
     * The game does NOT start in this mode, on purpose: the image is an instrument, not a
     * cartridge. The screen staying black is the signal that you are in the right mode.
     *
     * What to measure, and in this order (reference before suspect, same rig, same probe):
     *   1. SCK on U6 pin 6 — the flash, which works. That is the ruler.
     *   2. SCK on U3 pin 6 — does the same clock reach the mute chip?
     *   3. /CS on U3 pin 1 — is it asserted? If U3 is hard to probe, the pad of R6 is on the
     *      SAME net and is much bigger; it serves as a reference, but does NOT replace the
     *      pin: the last hop is exactly what is in doubt.
     */
#ifdef UVM2_PSRAM_LOOP
    for (;;) {
        direct_begin();
        cs1_qpi(c_id, 4, b, 8);
        direct_end();
        /* A long clean gap between bursts: it separates one from the next on screen and
         * lets the trigger re-arm without catching the tail of the previous one. */
        spin(200000);
    }
#endif
    r->magic = UVM2_PSRAM_MAGIC;   /* this is the last one that runs */
    return (r->qpi_direct[0] || r->qpi_after_35[0]) ? 1 : 0;
}

int uvm2_psram_probe_bootrom(void)
{
    volatile uvm2_psram_result_t *r = &uvm2_psram_result;
    /* This probe runs SECOND, which is why the sentinel is dropped here and raised again at
     * the end: otherwise uvm2_psram_probe() would have left it set with the new half of the
     * structure still unwritten, and an SWD read in between would pass for good. */
    r->magic = 0;

    /* 1. The control, BEFORE touching CS1: that way its value does not depend on anything we
     *    do afterwards. A control measured at the end is not a control. */
    r->flash_jedec = flash_jedec_id();

    /* 2. Declare CS1. The size does not matter as long as it is not NONE — the bootrom only
     *    looks at whether it is zero to decide whether to send the sequence. */
    r->devinfo_cs1_before = flash_devinfo_get_cs_size(1);
    flash_devinfo_set_cs_gpio(1, PSRAM_CS_GPIO);
    flash_devinfo_set_cs_size(1, FLASH_DEVINFO_SIZE_8M);

    /* 3. The pad, or CS1 does not drive however good its FUNCSEL is. */
    configure_cs1_pad();

    /* 4. And now any command at all. THE COMMAND IS NOT THE POINT: the point is that
     *    flash_do_cmd calls the bootrom's connect_internal_flash() + flash_exit_xip(), and
     *    that XIP exit NOW also reaches CS1. On the way past it re-reads the control, which
     *    must come out the same as in step 1. */
    r->flash_jedec_after_cs1 = flash_jedec_id();

    /* 5. To the chip, on one line, down the SAME path that just answered correctly on CS0.
     *    If it was in QPI, the bootrom's XIP exit has already pulled it out.
     *
     *    Eight bytes and not six, the same as psram_detect_size() in 2.3.0: the APS6404's
     *    0x9F DOES carry three address bytes, so the data starts at index 4 — MF_ID, KGD,
     *    EID. Counting those positions wrong gives a zero that looks like a mute chip. */
    {
        const uint8_t c_id[8] = { CMD_READ_ID, 0xff, 0xff, 0xff,
                                  0xff, 0xff, 0xff, 0xff };
        uint8_t       id[8]   = { 0 };
        do_cmd_cs(c_id, id, 8, 1);
        r->mf_id_bootrom = id[4];
        r->kgd_bootrom   = id[5];
        r->eid_bootrom   = id[6];
        /* The SDK looks at the KGD, not the manufacturer. We were looking at the MF_ID. */
        r->found_bootrom = (id[5] == AP_KGD_ID);
        r->size_bootrom  = eid_to_size(id[5], id[6]);
    }

    /* 6. FLASH_DEVINFO, back as it was. This is a PROBE: it answers the question and returns
     *    the machine to the state it found it in. Whoever wants to arm the window will do it
     *    deliberately, with the result in front of them. */
    flash_devinfo_set_cs_size(1, (flash_devinfo_size_t)r->devinfo_cs1_before);

    r->bootrom_probe_run = 1;
    r->magic = UVM2_PSRAM_MAGIC;   /* at the end, with everything written */
    return (int)r->found_bootrom;
}

int uvm2_psram_probe(void)
{
    direct_clock(DIRECT_CLOCK_DIV);   /* it did NOT: see the note above */
    volatile uvm2_psram_result_t *r = &uvm2_psram_result;
    r->magic = 0;   /* set at the END: a half-filled structure is worthless */

    /* 1. THE XIP WINDOW IS NOT TOUCHED YET.
     *
     *    The first version read 0x11000000 first, "to see whether it was already mapped".
     *    With CS1 UNCONFIGURED, that read leaves the QMI stuck: DIRECT_CSR read 0x01810802
     *    on entry, i.e. BUSY = 1 with EN = 0. From then on direct_begin burns its million
     *    passes waiting for BUSY to drop, carries on anyway, and rx() never sees any data:
     *    it returns 0x00.
     *
     *    So the 0x00 from READ_ID — and the 0xcccccccc from the window — were caused by
     *    LOOKING. Registers first, since reading them costs nothing. */
    /* 2. The QMI state ON ENTRY. This goes BEFORE anything else, and its absence was the
     *    mistake in the first version: the M1 registers were read AFTER probing and came out
     *    configured, which does not distinguish "it arrived like that from the firmware"
     *    from "we left it like that". A register read late is not evidence. */
    r->m1_timing_before  = qmi_hw->m[1].timing;
    r->m1_rfmt_before    = qmi_hw->m[1].rfmt;
    r->m1_rcmd_before    = qmi_hw->m[1].rcmd;
    r->direct_csr_before = qmi_hw->direct_csr;

    /* 3. If CS1 is ALREADY configured, the firmware has brought it up and the chip will be
     *    in QPI. Sending single-line SPI commands there is not harmless: they are read as
     *    something else and break what was set up (it happened, and the window started
     *    returning 0xcccccccc). So leave it alone. */
    if (r->m1_rfmt_before != UVM2_M1_RFMT_RESET) {
        /* Only NOW is it safe to look at the window: with CS1 configured, reading it is a
         * normal read. */
        const volatile uint32_t *xip = (const volatile uint32_t *)PSRAM_XIP_BASE;
        for (int i = 0; i < 4; i++) r->xip_before[i] = xip[i];
        r->already_mapped = 1;
        r->probed = 1;
        r->magic  = UVM2_PSRAM_MAGIC;
        /* EARLY EXIT, and it has to be skippable. If somebody already brought the window up
         * there is no need to interrogate the chip... except when the interrogation is
         * precisely what is being tested. That happened: on a reference board the firmware
         * brings the PSRAM up before launching the game, the probe left through here
         * returning 1, and that 1 was read as "the chip answers" when it meant "it was
         * already mapped". READ_ID never ran. */
        if (!uvm2_psram_force) return 1;
    }

    /* 4. Nobody has brought it up: we do it ourselves. */
    configure_cs1_pad();

    const uint8_t c_rsten[1] = { CMD_RESET_ENABLE };
    const uint8_t c_rst[1]   = { CMD_RESET };
    /* READ_ID carries three address bytes before the data. */
    const uint8_t c_id[4]    = { CMD_READ_ID, 0x00, 0x00, 0x00 };

    direct_begin();
    cs1_xfer(c_rsten, 1, 0, 0);
    cs1_xfer(c_rst,   1, 0, 0);
    direct_end();

    spin(30000);            /* settling after the reset */

    uint8_t id[2] = { 0, 0 };
    direct_begin();
    cs1_xfer(c_id, 4, id, 2);
    direct_end();

    r->mf_id_spi = id[0];

    /* If it does not answer on one line, it may be in QPI. Pull it out and retry. This IS
     * safe even if it was not in QPI: 0xF5 on four lines to a chip listening on one is a
     * byte it does not recognise, not a mode change. */
    if (id[0] != AP_MF_ID) {
        direct_begin();
        cs1_xfer_quad1(CMD_EXIT_QPI);
        direct_end();
        spin(30000);

        direct_begin();
        cs1_xfer(c_rsten, 1, 0, 0);
        cs1_xfer(c_rst,   1, 0, 0);
        direct_end();
        spin(30000);

        id[0] = id[1] = 0;
        direct_begin();
        cs1_xfer(c_id, 4, id, 2);
        direct_end();
        r->mf_id_after_qpi_exit = id[0];
    }

    /* Speed sweep. CLKDIV lives in DIRECT_CSR, so it is changed with direct mode off and
     * then re-entered. */
    {
        static const uint8_t divs[4] = { 6, 12, 30, 120 };
        for (int k = 0; k < 4; k++) {
            uint32_t csr = qmi_hw->direct_csr;
            csr = (csr & ~QMI_DIRECT_CSR_CLKDIV_BITS)
                | ((uint32_t)divs[k] << QMI_DIRECT_CSR_CLKDIV_LSB);
            qmi_hw->direct_csr = csr;

            direct_begin();
            cs1_xfer(c_rsten, 1, 0, 0);
            cs1_xfer(c_rst,   1, 0, 0);
            direct_end();
            spin(30000);

            uint8_t idk[2] = { 0, 0 };
            direct_begin();
            cs1_xfer(c_id, 4, idk, 2);
            direct_end();

            r->clkdiv_tested[k] = divs[k];
            r->id_by_clkdiv[k]  = ((uint32_t)idk[0] << 8) | idk[1];
        }
    }

    /* The wiring, since the chip will not talk. FUNCSEL 5 = SIO. */
    io_bank0_hw->io[PSRAM_CS_GPIO].ctrl = 5u;
    sio_hw->gpio_hi_oe_set = 1u << (PSRAM_CS_GPIO - 32);
    sio_hw->gpio_hi_set    = 1u << (PSRAM_CS_GPIO - 32);
    spin(2000);
    r->cs_reads_when_high  = (sio_hw->gpio_hi_in >> (PSRAM_CS_GPIO - 32)) & 1u;
    sio_hw->gpio_hi_clr    = 1u << (PSRAM_CS_GPIO - 32);
    spin(2000);
    r->cs_reads_when_low   = (sio_hw->gpio_hi_in >> (PSRAM_CS_GPIO - 32)) & 1u;
    /* And now the continuity hint: release the output with the internal pulls OFF, so the
     * external network decides and not us. */
    {
        uint32_t pad = pads_bank0_hw->io[PSRAM_CS_GPIO];
        pads_bank0_hw->io[PSRAM_CS_GPIO] = (pad & ~(PADS_BANK0_GPIO0_PUE_BITS |
                                                    PADS_BANK0_GPIO0_PDE_BITS))
                                         | PADS_BANK0_GPIO0_IE_BITS;
        /* driven to ZERO, then released: if there is an external pull-up, it rises on its own */
        sio_hw->gpio_hi_oe_set = 1u << (PSRAM_CS_GPIO - 32);
        sio_hw->gpio_hi_clr    = 1u << (PSRAM_CS_GPIO - 32);
        spin(2000);
        sio_hw->gpio_hi_oe_clr = 1u << (PSRAM_CS_GPIO - 32);
        spin(20000);
        r->cs_float_after_low = (sio_hw->gpio_hi_in >> (PSRAM_CS_GPIO - 32)) & 1u;

        /* control: driven to ONE and released. Here it should read 1 in both cases, so it
         * only serves to show the read is not stuck. */
        sio_hw->gpio_hi_oe_set = 1u << (PSRAM_CS_GPIO - 32);
        sio_hw->gpio_hi_set    = 1u << (PSRAM_CS_GPIO - 32);
        spin(2000);
        sio_hw->gpio_hi_oe_clr = 1u << (PSRAM_CS_GPIO - 32);
        spin(20000);
        r->cs_float_after_high = (sio_hw->gpio_hi_in >> (PSRAM_CS_GPIO - 32)) & 1u;

        pads_bank0_hw->io[PSRAM_CS_GPIO] = pad;   /* the pad, as it was */
    }

    /* Leave it as it was: release and hand the pin back to the QMI. */
    sio_hw->gpio_hi_oe_clr = 1u << (PSRAM_CS_GPIO - 32);
    io_bank0_hw->io[PSRAM_CS_GPIO].ctrl = FUNCSEL_QMI_CS1N;

    r->mf_id  = id[0];
    r->kgd    = id[1];
    r->found  = (id[0] == AP_MF_ID);
    r->probed = 1;
    r->magic  = UVM2_PSRAM_MAGIC;

    /* ON PURPOSE it neither arms the XIP window nor writes anything. This is a PROBE: first
     * you find out which chip is there and whether somebody had already brought it up.
     * Arming it blind could stomp on whatever the cartridge firmware has set up on CS1, and
     * this cartridge is not ours. */
    return r->found;
}
