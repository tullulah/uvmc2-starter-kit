/* uvm2_sd.c — bit-banged SPI to the SD card plus a minimal FAT reader.
 *
 * THE PIN RANGE COMES FROM THE SCHEMATIC AND IS CONFIRMED (GPIO32..39); the one-to-one
 * assignment does NOT. An off-by-one here breaks the driver WITHOUT REPORTING ANYTHING —
 * it talks to the wrong GPIO and the card simply never answers. That is why uvm2_sd_error
 * tells "no card" apart from "did not start": with a single failure code, a wrong pin and
 * an absent card look exactly the same.
 */
#include "uvm2_sd.h"

int uvm2_sd_error = UVM2_SD_OK;

/* THE PINS WERE MEASURED OFF THE CARTRIDGE FIRMWARE ITSELF, not read off the schematic.
 *
 * When it loads a module, the UVM2 firmware leaves the WHOLE high I/O bank at FUNCSEL 31
 * (null) — checked over SWD: from GPIO32 to GPIO47 not one is configured. But while it is
 * sitting in ITS OWN MENU the SD is up and running, and there IO_BANK0 can be read without
 * touching anything. That gave (2026-08-20):
 *
 *     GPIO34  FUNCSEL 1 = SPI0_SCLK   STATUS OETOPAD           -> driving low: SCK
 *     GPIO35  FUNCSEL 1 = SPI0_TX     STATUS 0                 -> MOSI
 *     GPIO36  FUNCSEL 1 = SPI0_RX     STATUS INFROMPAD|IRQ     -> reads high: MISO, pulled up
 *     GPIO39  FUNCSEL 5 = SIO         STATUS OUT|OE|IN|IRQ     -> driving high: CS, active low
 *
 * The previous list came from reading schematic labels at the limit of their resolution and
 * was SHIFTED BY TWO, with CS on yet another pin. The symptom was exactly the one this
 * comment warns about: the card does not answer and uvm2_sd_error stays at NO_INIT.
 *
 * If this ever has to be revisited, that is the recipe: leave the console in the cartridge
 * menu and dump IO_BANK0 (0x40028100, 32 words) over SWD. It is a MEASUREMENT, not a read.
 *
 * THERE IS NO CARD DETECT. The firmware configures no pin for it (32, 33, 37 and 38 are all
 * null in its menu), so SD_DETECT does not exist in this wiring. An earlier version read
 * GPIO38: an unowned pad with an internal pull-down, which always returns 0 — that is, "no
 * card" no matter what. A diagnostic that cannot fail diagnoses nothing. */

/* ── THE HOST SIDE ────────────────────────────────────────────────────────────────────
 * With -DUVM2_SD_HOST the very same FAT code compiles against a FILE instead of the card.
 * It exists so WRITING can be tested — creating a file touches the FAT and the directory —
 * without risking anybody's card, and against real FAT16 and FAT32 images that `fsck_msdos`
 * then validates. Provide uvm2_host_read()/uvm2_host_write() and you have a test bench. */
#ifndef UVM2_SD_HOST
#define PIN_SCK   34
#define PIN_MOSI  35
#define PIN_MISO  36
#define PIN_CS    39

#define PADS_BANK0 0x40038000u
#define IO_BANK0   0x40028000u
#define SIO        0xD0000000u
#define R(a)       (*(volatile uint32_t *)(uintptr_t)(a))

/* GP32-47 live in the HIGH SIO bank: different registers, and the bit is pin-32.
 *
 * THE HIGH BANK OFFSETS ARE NOT THE RP2040 ONES, and that already cost a black screen:
 * there HI_OUT_SET sits at 0x028 and here at 0x01C, so writing the RP2040 number lands on
 * GPIO_OE / GPIO_OE_SET of the LOW bank — that is, on the direction of the CARTRIDGE BUS
 * pins. The symptom was not an error: the Vectrex clock stopped advancing and
 * uvm2_frame_end waited forever.
 *
 * The values come from hardware/regs/sio.h in the pico-sdk, and the static_assert below
 * checks them against that header when it is available. Do not write them from memory. */
#define SIO_HI_IN       0x008u
#define SIO_HI_OUT_SET  0x01Cu
#define SIO_HI_OUT_CLR  0x024u
#define SIO_HI_OE_SET   0x03Cu
#define SIO_HI_OE_CLR   0x044u

#if defined(__has_include)
#  if __has_include(<hardware/regs/sio.h>) && __has_include(<hardware/platform_defs.h>)
     /* sio.h wraps every number in _u(), which lives in platform_defs.h: without it the
      * value is not a constant and the static_assert does not compile. */
#    include <hardware/platform_defs.h>
#    include <hardware/regs/sio.h>
_Static_assert(SIO_HI_IN      == SIO_GPIO_HI_IN_OFFSET,      "SIO_HI_IN");
_Static_assert(SIO_HI_OUT_SET == SIO_GPIO_HI_OUT_SET_OFFSET, "SIO_HI_OUT_SET");
_Static_assert(SIO_HI_OUT_CLR == SIO_GPIO_HI_OUT_CLR_OFFSET, "SIO_HI_OUT_CLR");
_Static_assert(SIO_HI_OE_SET  == SIO_GPIO_HI_OE_SET_OFFSET,  "SIO_HI_OE_SET");
_Static_assert(SIO_HI_OE_CLR  == SIO_GPIO_HI_OE_CLR_OFFSET,  "SIO_HI_OE_CLR");
#  endif
#endif

static inline uint32_t hi(int pin) { return 1u << (pin - 32); }

static void cfg_pin(int pin, int is_output)
{
    /* The pad comes up ISOLATED on the RP2350: without clearing ISO the pin is mute and
     * says nothing about it. */
    volatile uint32_t *pad = (volatile uint32_t *)(uintptr_t)(PADS_BANK0 + 4 + 4*pin);
    /* PDE is SET in the reset value (0x0116). Leaving it is the sister trap of ISO: on
     * MISO that internal pull-down fights the board's pull-up — the one you can see as a
     * high read on GPIO36 in the firmware menu — and can pull the line down. */
    *pad = (*pad & ~((1u<<8) | (1u<<7) | (1u<<2))) | (1u<<6) | (is_output ? 0u : (1u<<3));
                                       /* ISO=0, OD=0, PDE=0, IE=1, PUE on inputs */
    R(IO_BANK0 + 8*pin + 4) = 5u;                        /* FUNCSEL 5 = SIO   */
    if (is_output) R(SIO + SIO_HI_OE_SET) = hi(pin);
    else           R(SIO + SIO_HI_OE_CLR) = hi(pin);
}
static inline void set_pin(int pin, int v)
{
    if (v) R(SIO + SIO_HI_OUT_SET) = hi(pin);
    else   R(SIO + SIO_HI_OUT_CLR) = hi(pin);
}
static inline int get_pin(int pin) { return (R(SIO + SIO_HI_IN) >> (pin - 32)) & 1u; }

/* Half a clock cycle. Slow while starting up (the card demands it until it leaves idle)
 * and fast afterwards; without the two speeds it either never starts or takes forever. */
static volatile int s_slow = 1;
static void tick(void) { volatile int n = s_slow ? 24 : 1; while (n--) { } }

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        set_pin(PIN_MOSI, (out >> i) & 1);
        tick();
        set_pin(PIN_SCK, 1);             /* rising edge: MISO is sampled */
        in = (uint8_t)((in << 1) | get_pin(PIN_MISO));
        tick();
        set_pin(PIN_SCK, 0);
    }
    return in;
}
static void cs(int on) { set_pin(PIN_CS, !on); }   /* CS is active LOW */

static uint8_t command(uint8_t idx, uint32_t arg, uint8_t crc)
{
    xfer(0xFF);
    xfer((uint8_t)(0x40 | idx));
    xfer((uint8_t)(arg >> 24)); xfer((uint8_t)(arg >> 16));
    xfer((uint8_t)(arg >> 8));  xfer((uint8_t)arg);
    xfer(crc);
    for (int i = 0; i < 10; i++) {           /* R1: first byte without bit 7 */
        uint8_t r = xfer(0xFF);
        if (!(r & 0x80)) return r;
    }
    return 0xFF;
}

static int s_sdhc = 0;

int uvm2_sd_init(void)
{
    uvm2_sd_error = UVM2_SD_OK;
    cfg_pin(PIN_SCK, 1); cfg_pin(PIN_MOSI, 1); cfg_pin(PIN_CS, 1);
    cfg_pin(PIN_MISO, 0);
    set_pin(PIN_SCK, 0); set_pin(PIN_MOSI, 1); cs(0);
    s_slow = 1;

    /* 80 pulses with CS high: the card asks for them before it enters SPI mode. */
    for (int i = 0; i < 10; i++) xfer(0xFF);

    cs(1);
    if (command(0, 0, 0x95) != 0x01) { cs(0); uvm2_sd_error = UVM2_SD_NO_INIT; return 0; }

    uint8_t r = command(8, 0x1AA, 0x87);     /* a v2 card answers 0x01 + 4 bytes */
    int v2 = (r == 0x01);
    if (v2) for (int i = 0; i < 4; i++) xfer(0xFF);

    for (int attempt = 0; ; attempt++) {
        command(55, 0, 0xFF);
        if (command(41, v2 ? 0x40000000u : 0, 0xFF) == 0x00) break;
        if (attempt > 20000) { cs(0); uvm2_sd_error = UVM2_SD_NO_INIT; return 0; }
    }
    if (v2) {                                 /* CMD58: does it address by blocks? */
        if (command(58, 0, 0xFF) == 0x00) {
            uint8_t ocr = xfer(0xFF); xfer(0xFF); xfer(0xFF); xfer(0xFF);
            s_sdhc = (ocr & 0x40) != 0;
        }
    }
    cs(0);
    s_slow = 0;
    return 1;
}

#else   /* UVM2_SD_HOST */
int uvm2_host_read(uint32_t lba, unsigned char *b);
int uvm2_host_write(uint32_t lba, const unsigned char *b);
int uvm2_sd_init(void) { return 1; }
#endif

#ifdef UVM2_SD_HOST
static int read_block(uint32_t lba, unsigned char *buf) { return uvm2_host_read(lba, buf); }
static int write_block(uint32_t lba, const unsigned char *buf) { return uvm2_host_write(lba, buf); }
#else
static int read_block(uint32_t lba, unsigned char *buf)
{
    cs(1);
    if (command(17, s_sdhc ? lba : lba * 512u, 0xFF) != 0x00) { cs(0); return 0; }
    uint8_t t = 0xFF;
    for (int i = 0; i < 200000 && t == 0xFF; i++) t = xfer(0xFF);
    if (t != 0xFE) { cs(0); return 0; }
    for (int i = 0; i < 512; i++) buf[i] = xfer(0xFF);
    xfer(0xFF); xfer(0xFF);                   /* CRC, which we do not check */
    cs(0);
    return 1;
}

/* WRITE ONE BLOCK (CMD24). The driver's only write path, and deliberately so: it is used
 * ONLY to overwrite IN PLACE a file that already exists, without touching the FAT or the
 * directory. Creating or resizing means allocating clusters and rewriting BOTH copies of
 * the FAT, and getting that wrong corrupts the user's card — which is not ours. */
static int write_block(uint32_t lba, const unsigned char *buf)
{
    cs(1);
    if (command(24, s_sdhc ? lba : lba * 512u, 0xFF) != 0x00) { cs(0); return 0; }
    xfer(0xFF);                                /* one guard byte before the token */
    xfer(0xFE);                                /* start-of-block token */
    for (int i = 0; i < 512; i++) xfer(buf[i]);
    xfer(0xFF); xfer(0xFF);                    /* CRC, which the card ignores over SPI */
    /* The data response: xxx00101 = accepted. Anything else is a failure, and it has to be
     * caught here rather than discovered on the next read. */
    uint8_t r = 0xFF;
    for (int i = 0; i < 1000 && (r & 0x11) != 0x01; i++) r = xfer(0xFF);
    if ((r & 0x1F) != 0x05) { cs(0); return 0; }
    /* AND WAIT FOR IT TO RELEASE BUSY. The card holds MISO low while it programs; leaving
     * early leaves the write half done and the next command fails without saying why. */
    for (int i = 0; i < 500000; i++) if (xfer(0xFF) != 0x00) break;
    cs(0);
    return 1;
}

#endif  /* UVM2_SD_HOST */

/* ── FAT16/FAT32 ────────────────────────────────────────────────────────────── */
static uint16_t u16(const unsigned char *b, int o) { return (uint16_t)(b[o] | (b[o+1] << 8)); }
static uint32_t u32(const unsigned char *b, int o) {
    return (uint32_t)b[o] | ((uint32_t)b[o+1] << 8) | ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24);
}

static struct {
    uint32_t start, fat, data, root_cluster;
    uint16_t root_entries; uint32_t root_sector;
    uint8_t  spc; int is32;
    /* To WRITE we need to know how many copies of the FAT there are and how long each one
     * is: a FAT updated in only one copy is a card the PC sees as corrupt. */
    uint8_t  nfats; uint32_t spf; uint32_t total_clusters;
    uint32_t fsinfo;            /* FSInfo sector (FAT32); 0 if there is none */
} V;

/* A real BPB, not "the two bytes I looked at are non-zero".
 *
 * Sector 0 of this card is an MBR, and an MBR's boot code can hold ANYTHING at offsets 11
 * and 13 — which in a VBR are bytes-per-sector and sectors-per-cluster. If by chance they
 * are non-zero, sector 0 passes for a VBR, an invented volume gets mounted and the lookup
 * fails with MISSING, which is the very same symptom as a file that is not there. Three
 * conditions an MBR does not meet by accident: */
static int is_bpb(const unsigned char *b)
{
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;          /* jump to the boot code */
    if (u16(b, 11) != 512) return 0;                     /* bytes per sector */
    unsigned spc = b[13];
    return spc != 0 && (spc & (spc - 1)) == 0;           /* a power of two */
}

struct uvm2_sd_diag uvm2_sd_diag;

static int mount(unsigned char *b)
{
    if (!read_block(0, b)) return 0;
    if (!(b[510] == 0x55 && b[511] == 0xAA)) return 0;

    uvm2_sd_diag.magic    = 0x47444453u;                 /* 'SDDG' */
    uvm2_sd_diag.sec0_b0  = b[0];
    uvm2_sd_diag.sec0_bps = u16(b, 11);
    uvm2_sd_diag.sec0_spc = b[13];

    uint32_t part = 0;
    if (!is_bpb(b)) {
        /* MBR: all FOUR entries, not just the first — a card may carry the partition in
         * any of them, and an unused entry has type 0. */
        for (int i = 0; i < 4; i++) {
            const int e = 0x1BE + 16 * i;
            if (b[e + 4] == 0) continue;                 /* type 0 = unused entry */
            uint32_t lba = u32(b, e + 8);
            if (!lba) continue;
            unsigned char v[512];
            if (!read_block(lba, v)) continue;
            if (!is_bpb(v)) continue;
            part = lba;
            for (int k = 0; k < 512; k++) b[k] = v[k];
            break;
        }
        if (!part) return 0;
        uvm2_sd_diag.via_mbr = 1;
    }
    V.start = part;
    V.spc = b[13];
    uint16_t reserved = u16(b, 14);
    uint8_t  nfats = b[16];
    V.root_entries = u16(b, 17);
    uint32_t spf = u16(b, 22);
    V.is32 = (spf == 0);
    if (V.is32) { spf = u32(b, 36); V.root_cluster = u32(b, 44); V.fsinfo = V.start + u16(b, 48); }
    else        { V.fsinfo = 0; }
    V.fat = part + reserved;
    V.nfats = nfats; V.spf = spf;
    V.root_sector = V.fat + (uint32_t)nfats * spf;
    V.data = V.root_sector + (V.root_entries * 32u + 511u) / 512u;
    /* HOW MANY CLUSTERS THE VOLUME HAS. Without this cap, looking for a free one runs off
     * the end of the FAT and "finds" garbage outside the file system. */
    {
        uint32_t tot = u16(b, 19);
        if (tot == 0) tot = u32(b, 32);
        V.total_clusters = (tot > (V.data - V.start))
                         ? (tot - (V.data - V.start)) / (V.spc ? V.spc : 1u) + 2u : 0u;
    }

    uvm2_sd_diag.start         = V.start;
    uvm2_sd_diag.spc           = V.spc;
    uvm2_sd_diag.is32          = V.is32;
    uvm2_sd_diag.root_cluster  = V.root_cluster;
    uvm2_sd_diag.fat           = V.fat;
    uvm2_sd_diag.root_sector   = V.root_sector;
    uvm2_sd_diag.data          = V.data;
    uvm2_sd_diag.root_entries  = V.root_entries;
    return V.spc != 0;
}

/* ── WRITING: create a ONE-CLUSTER file ──────────────────────────────────────────────
 *
 * It only ADDS: it takes a cluster the FAT marks free and a free directory entry. It does
 * not move, does not delete and does not touch anything already in use. If something does
 * not add up it aborts BEFORE writing — better to lose the calibration than to leave the
 * user's card damaged.
 *
 * One cluster is enough: the configuration file is four lines. Bigger files would need
 * chained clusters, and that is not needed here. */

/* Writes one FAT entry into ALL of its copies. A FAT updated in a single copy is a card
 * the PC sees as corrupt — and the user notices that, not us. */
static int fat_set(uint32_t c, uint32_t value, unsigned char *b)
{
    const uint32_t off = V.is32 ? c * 4u : c * 2u;
    const uint32_t sec = off / 512u, within = off % 512u;
    for (uint8_t f = 0; f < V.nfats; f++) {
        const uint32_t lba = V.fat + (uint32_t)f * V.spf + sec;
        if (!read_block(lba, b)) return 0;
        if (V.is32) {
            uint32_t v = u32(b, (int)within);
            v = (v & 0xF0000000u) | (value & 0x0FFFFFFFu);   /* the top 4 bits are reserved */
            b[within] = (uint8_t)v; b[within+1] = (uint8_t)(v >> 8);
            b[within+2] = (uint8_t)(v >> 16); b[within+3] = (uint8_t)(v >> 24);
        } else {
            b[within] = (uint8_t)value; b[within+1] = (uint8_t)(value >> 8);
        }
        if (!write_block(lba, b)) return 0;
    }
    return 1;
}

/* The first FREE cluster (FAT entry at 0), already marked as end of chain. 0 if there is
 * none left. */
static uint32_t alloc_this(uint32_t c, unsigned char *b);

/* A FAT SECTOR IS READ ONCE, NOT ONCE PER CLUSTER.
 *
 * This used to walk cluster by cluster and read the WHOLE sector off the card for EACH of
 * them: in FAT32, 128 entries fit in a sector, so it did 128 times the reads it needed
 * (256 in FAT16). On a 16 GB card with the first gap far in, that is tens of thousands of
 * bit-banged SPI reads — and on the console it looks like a hang, which is exactly what it
 * looked like at the end of the calibration.
 *
 * AND IT STARTS FROM THE FSInfo HINT, which exists precisely for this: FAT32 keeps "the
 * next free cluster" and we maintain it as we allocate. If the hint lies — it may, it is
 * only a hint — the sweep restarts from the beginning. */
static uint32_t alloc_cluster(unsigned char *b)
{
    const uint32_t end = V.total_clusters ? V.total_clusters : 0xFFFFFFu;
    uint32_t from_off = 2;
    if (V.is32 && V.fsinfo && read_block(V.fsinfo, b)
        && u32(b, 0) == 0x41615252u && u32(b, 484) == 0x61417272u) {
        const uint32_t hint = u32(b, 492);
        if (hint >= 2 && hint < end) from_off = hint;
    }
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t c0 = pass ? 2u : from_off;
        const uint32_t c1 = pass ? from_off : end;
        const uint32_t per_sector = V.is32 ? 128u : 256u;
        for (uint32_t base = c0 - (c0 % per_sector); base < c1; base += per_sector) {
            const uint32_t off = V.is32 ? base * 4u : base * 2u;
            if (!read_block(V.fat + off / 512u, b)) return 0;
            for (uint32_t k = 0; k < per_sector; k++) {
                const uint32_t c = base + k;
                if (c < c0 || c >= c1 || c < 2) continue;
                const uint32_t within = V.is32 ? k * 4u : k * 2u;
                const uint32_t v = V.is32 ? (u32(b, (int)within) & 0x0FFFFFFFu)
                                          : u16(b, (int)within);
                if (v != 0) continue;
                return alloc_this(c, b);
            }
        }
    }
    return 0;
}

/* Marks `c` as end of chain and updates the FSInfo. Kept apart so the sweep above does not
 * have to break out of two nested loops. */
static uint32_t alloc_this(uint32_t c, unsigned char *b)
{
    {
        if (!fat_set(c, V.is32 ? 0x0FFFFFFFu : 0xFFFFu, b)) return 0;
        /* THE FAT32 FSInfo, which carries the free-space count. It is only a HINT — the
         * system can recompute it — but leaving it stale makes `fsck_msdos` complain, and a
         * warning on the user's card is ours even if it breaks nothing. Measured: without
         * this, "Free space in FSInfo block (258077) not correct (258076)". */
        if (V.fsinfo) {
            if (read_block(V.fsinfo, b) && u32(b, 0) == 0x41615252u && u32(b, 484) == 0x61417272u) {
                uint32_t free_entries = u32(b, 488);
                if (free_entries != 0xFFFFFFFFu && free_entries > 0) {
                    free_entries--;
                    b[488] = (uint8_t)free_entries; b[489] = (uint8_t)(free_entries >> 8);
                    b[490] = (uint8_t)(free_entries >> 16); b[491] = (uint8_t)(free_entries >> 24);
                }
                b[492] = (uint8_t)(c + 1); b[493] = (uint8_t)((c + 1) >> 8);      /* hint */
                b[494] = (uint8_t)((c + 1) >> 16); b[495] = (uint8_t)((c + 1) >> 24);
                write_block(V.fsinfo, b);      /* if it fails, only the old hint remains */
            }
        }
        return c;
    }
    return 0;
}

static uint32_t next(uint32_t c, unsigned char *b)
{
    uint32_t off = V.is32 ? c * 4u : c * 2u;
    if (!read_block(V.fat + off / 512u, b)) return 0x0FFFFFFF;
    return V.is32 ? (u32(b, (int)(off % 512)) & 0x0FFFFFFFu) : u16(b, (int)(off % 512));
}
static uint32_t sector_of(uint32_t c) { return V.data + (c - 2) * V.spc; }

/* Looks for `n83` (8.3 format, already upper-cased and without the dot: "DKONG   ZIP") in
 * the directory starting at `cluster` (0 = the FAT16 root). Returns 1 and hands back the
 * cluster and the size. */
static int find(uint32_t cluster, const char *n83, uint32_t *out_c, uint32_t *out_len,
                unsigned char *b)
{
    uint32_t sec, remaining;
    if (cluster == 0 && !V.is32) { sec = V.root_sector; remaining = (V.root_entries * 32u + 511u) / 512u; }
    else                         { sec = sector_of(cluster); remaining = V.spc; }
    for (;;) {
        for (uint32_t s = 0; s < remaining; s++) {
            if (!read_block(sec + s, b)) return 0;
            for (int e = 0; e < 512; e += 32) {
                if (b[e] == 0x00) return 0;             /* end of the directory */
                uvm2_sd_diag.entries++;
                if (b[e] == 0xE5 || (b[e+11] & 0x0F) == 0x0F) continue;   /* deleted / VFAT */
                int same = 1;
                for (int i = 0; i < 11; i++) if (b[e+i] != (unsigned char)n83[i]) { same = 0; break; }
                if (!same) continue;
                *out_c   = ((uint32_t)u16(b, e+20) << 16) | u16(b, e+26);
                *out_len = u32(b, e+28);
                return 1;
            }
        }
        if (cluster == 0 && !V.is32) return 0;
        cluster = next(cluster, b);
        if (cluster < 2 || cluster >= 0x0FFFFFF8u) return 0;
        sec = sector_of(cluster); remaining = V.spc;
    }
}

/* "roms/dkong.zip" -> "ROMS       " and "DKONG   ZIP". No wildcards and no long names:
 * the cartridge asks for an exact name, which the game already declares. */
static void to83(const char *s, int n, char *out)
{
    int i = 0, j = 0;
    for (; i < 11; i++) out[i] = ' ';
    for (i = 0; i < n && s[i] && s[i] != '.'; i++)
        if (j < 8) out[j++] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    if (i < n && s[i] == '.') {
        i++; j = 8;
        for (; i < n && s[i]; i++)
            if (j < 11) out[j++] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    }
}

/* READ A CHUNK, NOT THE WHOLE FILE.
 *
 * `uvm2_sd_read` reads the complete file and fails with TOO_BIG if it does not fit in the
 * buffer, which is fine for a 68 KB romset and not for a 20-second capture (some 14 MB of
 * commands). With an offset, the RAM needed stops depending on how long the file is: two
 * frames are read and the next chunk is requested as it goes.
 *
 * `from_off` is a position in BYTES inside the file. It returns what was copied, which may
 * be less than `max` if the file ends first. The file lookup is the same one, so a reader
 * walking frame by frame re-walks the directory on every call — at 50 Hz that is cheap
 * next to reading the data, but it is worth knowing. */
static uint32_t read_range(const char *path, unsigned char *dst, uint32_t max,
                           uint32_t from_off, uint32_t *len_out)
{
    static unsigned char b[512];
    uvm2_sd_error = UVM2_SD_OK;

    /* No detect pin: if there is no card, it shows up as NO_INIT. */
    if (!uvm2_sd_init()) return 0;
    if (!mount(b)) { uvm2_sd_error = UVM2_SD_NO_FAT; return 0; }

    const char *slash = 0;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;

    uint32_t dir = V.is32 ? V.root_cluster : 0;
    char n83[11];
    if (slash) {
        uint32_t len;
        to83(path, (int)(slash - path), n83);
        uvm2_sd_diag.step = 1;                    /* looking for the subdirectory */
        if (!find(dir, n83, &dir, &len, b)) { uvm2_sd_error = UVM2_SD_MISSING; return 0; }
        uvm2_sd_diag.dir_cluster = dir;
        path = slash + 1;
    }
    uint32_t c, len;
    to83(path, 64, n83);
    uvm2_sd_diag.step = 2;                        /* looking for the file */
    if (!find(dir, n83, &c, &len, b)) { uvm2_sd_error = UVM2_SD_MISSING; return 0; }
    if (len_out) *len_out = len;
    if (from_off >= len) return 0;                /* past the end: zero, and not an error */
    uint32_t want = len - from_off;
    if (want > max) want = max;                   /* whatever fits; NOT a failure */

    uint32_t pos = 0, written = 0;
    while (c >= 2 && c < 0x0FFFFFF8u && written < want) {
        uint32_t sec = sector_of(c);
        for (uint32_t s = 0; s < V.spc && written < want; s++) {
            /* SKIP WHOLE SECTORS WITHOUT READING THEM. A large offset must not cost one
             * block read for every 512 bytes we skip over. */
            if (pos + 512u <= from_off) { pos += 512u; continue; }
            if (!read_block(sec + s, b)) return 0;
            uint32_t begin = (from_off > pos) ? (from_off - pos) : 0;  /* inside this block */
            uint32_t n     = 512u - begin;
            if (n > want - written) n = want - written;
            for (uint32_t i = 0; i < n; i++) dst[written + i] = b[begin + i];
            written += n; pos += 512u;
        }
        c = next(c, b);
    }
    return written;
}

/* A chunk starting at `from_off`: whatever fits, and coming up short is NOT a failure. */
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from_off)
{
    return read_range(path, dst, max, from_off, 0);
}

/* The WHOLE file, with the usual semantics: not fitting IS a failure. That is what the
 * romset loader expects — half a romset is not a romset — and it is why the check lives
 * here and not inside, where the chunked reader needs exactly the opposite. */
uint32_t uvm2_sd_read(const char *path, unsigned char *dst, uint32_t max)
{
    uint32_t len = 0;
    uint32_t n = read_range(path, dst, max, 0, &len);
    if (len > max) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
    return n;
}

/* OVERWRITE IN PLACE THE FIRST SECTOR OF A FILE THAT ALREADY EXISTS.
 *
 * The only thing a read-only reader can safely do: it touches neither the FAT nor the
 * directory entry, so neither the size nor the cluster chain change. That is why it does
 * NOT create the file: if it is not there it returns 0 and the caller says so on screen.
 *
 * `n` has to fit in 512 and the file must be at least that long. The sector is read first
 * and the remainder is padded with spaces and a newline, so the file stays readable text on
 * a PC and does not drag along whatever was there before.
 */
int uvm2_sd_overwrite(const char *path, const unsigned char *data, uint32_t n)
{
    static unsigned char b[512];
    uvm2_sd_error = UVM2_SD_OK;
    if (n == 0 || n > 512) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
    if (!uvm2_sd_init()) return 0;
    if (!mount(b)) { uvm2_sd_error = UVM2_SD_NO_FAT; return 0; }

    const char *slash = 0;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    uint32_t dir = V.is32 ? V.root_cluster : 0;
    char n83[11];
    if (slash) {
        uint32_t len;
        to83(path, (int)(slash - path), n83);
        if (!find(dir, n83, &dir, &len, b)) { uvm2_sd_error = UVM2_SD_MISSING; return 0; }
        path = slash + 1;
    }
    uint32_t c, len;
    to83(path, 64, n83);
    if (!find(dir, n83, &c, &len, b)) { uvm2_sd_error = UVM2_SD_MISSING; return 0; }
    if (len < 512) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }

    const uint32_t lba = sector_of(c);
    if (!read_block(lba, b)) return 0;
    for (uint32_t i = 0; i < n; i++) b[i] = data[i];
    for (uint32_t i = n; i < 512; i++) b[i] = ' ';
    b[511] = '\n';
    return write_block(lba, b);
}

static int make_entry_attr(uint32_t dir, const char *n83, uint32_t cluster, uint32_t len,
                           unsigned char attr, unsigned char *b);

/* Adds a directory entry in the first gap (deleted 0xE5 or end 0x00). Returns 0 if the
 * directory is full — it does not extend it: extending the FAT16 root directory is
 * IMPOSSIBLE (it is fixed size) and extending a FAT32 one would mean chaining clusters.
 * With a full directory, better not to save than to make something up. */
static int make_entry_attr(uint32_t dir, const char *n83, uint32_t cluster, uint32_t len,
                           unsigned char attr, unsigned char *b)
{
    uint32_t sec, remaining;
    if (dir == 0 && !V.is32) { sec = V.root_sector; remaining = (V.root_entries * 32u + 511u) / 512u; }
    else                     { sec = sector_of(dir); remaining = V.spc; }
    for (;;) {
        for (uint32_t s = 0; s < remaining; s++) {
            if (!read_block(sec + s, b)) return 0;
            for (int e = 0; e < 512; e += 32) {
                if (b[e] != 0x00 && b[e] != 0xE5) continue;
                const int was_end = (b[e] == 0x00);
                for (int i = 0; i < 32; i++) b[e + i] = 0;
                for (int i = 0; i < 11; i++) b[e + i] = (unsigned char)n83[i];
                b[e + 11] = attr;                                  /* 0x20 file, 0x10 directory */
                b[e + 26] = (unsigned char)cluster;                /* low cluster */
                b[e + 27] = (unsigned char)(cluster >> 8);
                if (V.is32) {
                    b[e + 20] = (unsigned char)(cluster >> 16);    /* high cluster */
                    b[e + 21] = (unsigned char)(cluster >> 24);
                }
                b[e + 28] = (unsigned char)len;
                b[e + 29] = (unsigned char)(len >> 8);
                b[e + 30] = (unsigned char)(len >> 16);
                b[e + 31] = (unsigned char)(len >> 24);
                /* IF WE TOOK THE END-OF-DIRECTORY MARKER, A NEW ONE GOES RIGHT BEHIND IT.
                 * Without this the walk does not know where to stop and reads garbage as
                 * entries. */
                if (was_end && e + 32 < 512) b[e + 32] = 0x00;
                return write_block(sec + s, b);
            }
        }
        if (dir == 0 && !V.is32) return 0;          /* FAT16 root: cannot be extended */
        dir = next(dir, b);
        if (dir < 2 || dir >= 0x0FFFFFF8u) return 0;
        sec = sector_of(dir); remaining = V.spc;
    }
}

static int make_entry(uint32_t dir, const char *n83, uint32_t cluster, uint32_t len,
                      unsigned char *b)
{
    return make_entry_attr(dir, n83, cluster, len, 0x20, b);
}

/* CREATES AN EMPTY SUBDIRECTORY and returns its cluster (0 if it could not).
 *
 * A directory is a file whose contents are entries, with two mandatory ones at the start:
 * `.` pointing at itself and `..` at the parent — and in `..` the ROOT is written as
 * cluster 0 even though in FAT32 the root does have a real cluster. That detail is in the
 * spec, and skipping it means the PC cannot walk up. */
static uint32_t make_directory(uint32_t parent, const char *n83, unsigned char *b)
{
    const uint32_t c = alloc_cluster(b);
    if (c == 0) return 0;

    for (int i = 0; i < 512; i++) b[i] = 0;
    static const char dot[11]    = { '.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
    static const char dotdot[11] = { '.','.',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
    for (int i = 0; i < 11; i++) b[i] = (unsigned char)dot[i];
    b[11] = 0x10;                                   /* DIRECTORY */
    b[26] = (unsigned char)c; b[27] = (unsigned char)(c >> 8);
    b[20] = (unsigned char)(c >> 16); b[21] = (unsigned char)(c >> 24);
    for (int i = 0; i < 11; i++) b[32 + i] = (unsigned char)dotdot[i];
    b[32 + 11] = 0x10;
    /* `..` pointing at the root is ALWAYS written as cluster 0, in FAT32 too. */
    const uint32_t pc = (parent == V.root_cluster && V.is32) ? 0u : parent;
    b[32 + 26] = (unsigned char)pc;         b[32 + 27] = (unsigned char)(pc >> 8);
    b[32 + 20] = (unsigned char)(pc >> 16); b[32 + 21] = (unsigned char)(pc >> 24);
    if (!write_block(sector_of(c), b)) return 0;
    for (uint32_t sx = 1; sx < V.spc; sx++) {
        static unsigned char z[512];
        for (int i = 0; i < 512; i++) z[i] = 0;
        if (!write_block(sector_of(c) + sx, z)) return 0;
    }
    if (!make_entry_attr(parent, n83, c, 0u, 0x10, b)) return 0;
    return c;
}

/* Resolves the folder part of `path`, creating it if needed, and leaves the file's 8.3
 * name in `n83`. `uvm2_sd_create` and `uvm2_sd_write` both need it, and it used to be
 * duplicated between them. */
static int folder_of(const char **path, char *n83, uint32_t *dir, unsigned char *b)
{
    const char *slash = 0;
    for (const char *p = *path; *p; p++) if (*p == '/') slash = p;
    *dir = V.is32 ? V.root_cluster : 0;
    if (slash) {
        uint32_t c, len;
        to83(*path, (int)(slash - *path), n83);
        if (find(*dir, n83, &c, &len, b)) *dir = c;
        else {
            const uint32_t nd = make_directory(*dir, n83, b);
            if (nd == 0) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
            *dir = nd;
        }
        *path = slash + 1;
    }
    to83(*path, 64, n83);
    return 1;
}

/* A FILE OF ANY SIZE.
 *
 * `uvm2_sd_create` writes a single cluster and `uvm2_sd_overwrite` a single sector, and
 * with that a game cannot dump anything real — not a trace, not a saved game, not a
 * capture. The limitation was ours, not the file system's: chaining clusters in the FAT is
 * exactly what the FAT is for, and the allocator already marks each one as end of chain as
 * it hands it out, so all that is left is to patch the previous one to point at the next.
 *
 * The order matters and is the same one as in `uvm2_sd_create`: data first, directory entry
 * afterwards. If something is cut off half way, what is left is clusters in use with
 * nothing pointing at them — lost space that a chkdsk reclaims — and not an entry pointing
 * at garbage, which the PC reads as a corrupt file.
 *
 * Tested the way it should be: a host harness runs it against real FAT16 and FAT32 images
 * made with newfs_msdos, and `fsck_msdos -n` has to come out clean afterwards. */
int uvm2_sd_write(const char *path, const unsigned char *data, uint32_t n)
{
    static unsigned char b[512];
    uint32_t dir, first = 0, prev = 0, written = 0, i;
    char n83[11];

    uvm2_sd_error = UVM2_SD_OK;
    if (n == 0) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
    if (!uvm2_sd_init()) return 0;
    if (!mount(b)) { uvm2_sd_error = UVM2_SD_NO_FAT; return 0; }
    if (!folder_of(&path, n83, &dir, b)) return 0;

    {
        const uint32_t per_cluster = 512u * (uint32_t)V.spc;
        const uint32_t how_many = (n + per_cluster - 1u) / per_cluster;

        for (i = 0; i < how_many; i++) {
            const uint32_t c = alloc_cluster(b);
            uint32_t sec;
            if (c == 0) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
            /* Chain it: the previous one stops being the end of chain and points here. */
            if (prev) { if (!fat_set(prev, c, b)) return 0; }
            else      { first = c; }
            prev = c;

            for (sec = 0; sec < V.spc && written < n; sec++) {
                uint32_t k = 0;
                while (k < 512u && written < n) b[k++] = data[written++];
                while (k < 512u)               b[k++] = 0;   /* the tail of the last sector */
                if (!write_block(sector_of(c) + sec, b)) return 0;
            }
        }
    }

    return make_entry(dir, n83, first, n, b);
}

int uvm2_sd_create(const char *path, const unsigned char *data, uint32_t n)
{
    static unsigned char b[512];
    uvm2_sd_error = UVM2_SD_OK;
    if (n == 0 || n > 512) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
    if (!uvm2_sd_init()) return 0;
    if (!mount(b)) { uvm2_sd_error = UVM2_SD_NO_FAT; return 0; }

    const char *slash = 0;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    uint32_t dir = V.is32 ? V.root_cluster : 0;
    char n83[11];
    if (slash) {
        uint32_t c, len;
        to83(path, (int)(slash - path), n83);
        if (find(dir, n83, &c, &len, b)) dir = c;
        else {
            /* IT DOES NOT EXIST: CREATE IT. This used to fall back to the root under the
             * same name, and it was a patch job: the reader looks in `config/` and would
             * never have found it there. */
            const uint32_t nd = make_directory(dir, n83, b);
            if (nd == 0) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }
            dir = nd;
        }
        path = slash + 1;
    }
    to83(path, 64, n83);

    const uint32_t c = alloc_cluster(b);
    if (c == 0) { uvm2_sd_error = UVM2_SD_TOO_BIG; return 0; }

    /* The data BEFORE the entry: if something fails, a cluster is left marked in use with
     * nothing pointing at it — lost space, which a chkdsk repairs. The other way round
     * would leave an entry pointing at garbage, which the PC reads as a corrupt file. */
    for (uint32_t i = 0; i < n; i++) b[i] = data[i];
    for (uint32_t i = n; i < 512; i++) b[i] = ' ';
    b[511] = '\n';
    if (!write_block(sector_of(c), b)) return 0;
    /* THE REST OF THE CLUSTER IS NOT ZEROED, AND IT USED TO BE.
     *
     * The old comment said "whatever was there is not ours and confuses whoever reads it",
     * and that is wrong: the file is 512 bytes long and NOBODY reads past its length — that
     * is cluster slack, and no file system looks at it. What it did cost was time: on a
     * 16 GB card the cluster is 32 or 64 KB, that is between 63 and 127 writes of ONE
     * sector each, and a single write on a cheap SD can take tens of milliseconds because
     * it forces a whole block erase. That is SECONDS per file, and on the console it looks
     * like a hang.
     *
     * A directory DOES have to be zeroed — its empty entries must read 0x00 so the walk
     * stops — and `make_directory` still does it. */
    return make_entry(dir, n83, c, 512u, b);
}
