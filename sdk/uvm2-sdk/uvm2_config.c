/* uvm2_config.c — see uvm2_config.h for the reason behind every decision. */
#include "uvm2_config.h"
#include "uvm2_draw.h"
#include "uvm2_sd.h"
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"

/* The model's knobs live in Rust and come out as symbols; the SDK's live here. */
extern volatile uint32_t DRAW_SCALE, T1_EXTRA_Q8;
extern volatile int32_t  NEG_RATE_X, NEG_RATE_Y;   /* vectrex-draw, see uvm2_config.h */
extern volatile int32_t  uvm2_drift_x, uvm2_drift_y;
extern volatile int32_t  uvm2_zero_offset;
void uvm2_draw_intensity(int brightness);

#define SD_PATH  "config/uvm2.cfg"

volatile int uvm2_have_calibration = 0;

/* THE LAST FLASH SECTOR, with the size coming from the SDK itself (4096) instead of by hand.
 * `PICO_FLASH_SIZE_BYTES` is defined by the target's linker script. */
#ifndef UVM2_CONFIG_FLASH_OFF
#define UVM2_CONFIG_FLASH_OFF  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#endif

/* A signature to tell "not calibrated" from "calibrated to zeros", which is the classic
 * invisible-contract trap: a zero reads exactly like "not needed". */
#define SIGNATURE  0x43414C31u   /* "CAL1" */

struct saved { uint32_t sig; struct uvm2_config c; uint32_t sum; };

/* THE FIELDS, IN A TABLE, AND NOT A CHAIN OF strcmp.
 *
 * Every new field cost its `strcmp` in the reader and its `put_field` in the writer, and with
 * six more this file went from 1215 to 1981 bytes of code — enough for dkong, which runs
 * ENTIRELY from SRAM, not to link. With the table, adding a field costs ONE line and no code,
 * which is what is needed if this is going to keep growing.
 *
 * The order is the text file's and the struct's; the compiler supplies the offset.
 *
 * TWO FILES, AND THE REASON IS WHOSE SETTINGS THEY ARE.
 *
 * Beam calibration belongs to the CONSOLE: the zero, the brightness, the scale, Y's hold, the
 * negative rates and the drift come from THAT machine's tube and capacitors, and they apply
 * equally to every game. That lives in `config/uvm2.cfg`; the first game to start creates it
 * and the rest use it as their base.
 *
 * The GAME's settings do not: whether the menu appears at power-up, or whether the drawing is
 * rotated, belong to each game. Sharing them forces Donkey Kong — which is vertical — to have a
 * horizontal/vertical switch that means nothing, and makes hiding the menu in one game hide it
 * in all of them. Those live in `config/<GAME>.cfg`, and each game DECLARES which ones it uses
 * (uvm2_config_game): the wizard only shows those, and only those get saved.
 *
 * The `bit` field is 0 for the console's and the setting's bit for the game's. */
#define FIELD(n, b) { #n, (uint16_t)((unsigned char *)&((struct uvm2_config *)0)->n - (unsigned char *)0), (b) }
static const struct { const char *n; uint16_t off; unsigned bit; } FIELDS[] = {
    FIELD(scale, 0), FIELD(t1_tail_q8, 0), FIELD(zero, 0), FIELD(bright, 0),
    FIELD(hold_y_min, 0), FIELD(hold_y_max, 0),
    FIELD(neg_rate_x, 0), FIELD(neg_rate_y, 0),
    FIELD(drift_x, 0), FIELD(drift_y, 0),
    FIELD(hz, UVM2_SETTING_HZ), FIELD(start_menu, UVM2_SETTING_MENU), FIELD(rotate, UVM2_SETTING_ROTATE),
};
volatile int32_t uvm2_setting_hz = 50, uvm2_setting_menu = 1, uvm2_setting_rotate = 0;

/* Which game this is and which of its own settings it uses. Declaring nothing behaves as
 * before: a single file, and no game settings in the wizard. */
static char     s_game_path[32];
static unsigned s_game_settings;

unsigned uvm2_config_game_settings(void) { return s_game_settings; }

void uvm2_config_game(const char *name, unsigned settings)
{
    int p = 0;
    const char *d = "config/";
    s_game_settings = settings;
    if (!name || !*name) { s_game_path[0] = 0; return; }
    while (*d) s_game_path[p++] = *d++;
    /* 8.3 and upper case, which is what the card driver understands. */
    while (*name && p < 15) {
        char ch = *name++;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        s_game_path[p++] = ch;
    }
    d = ".CFG";
    while (*d) s_game_path[p++] = *d++;
    s_game_path[p] = 0;
}
#define N_FIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))
static int32_t *field_of(struct uvm2_config *c, int i)
{
    return (int32_t *)((unsigned char *)c + FIELDS[i].off);
}

/* THE CHECKSUM, ALSO OVER THE TABLE. It used to be one line per field with its prime, and
 * every new field could be forgotten there without any warning: a saved calibration would pass
 * the check with a field half missing. Walking the table, a new field joins by itself. */
static uint32_t sum_of(const struct uvm2_config *c)
{
    uint32_t h = 0x9E3779B9u;
    for (int k = 0; k < N_FIELDS; k++)
        h = h * 16777619u ^ (uint32_t)*field_of((struct uvm2_config *)c, k);
    return h;
}

void uvm2_config_current(struct uvm2_config *c)
{
    c->scale  = (int32_t)DRAW_SCALE;
    c->t1_tail_q8 = (int32_t)T1_EXTRA_Q8;
    c->zero    = uvm2_zero_offset;
    c->bright  = uvm2_draw_intensity_current();
    c->hold_y_min = uvm2_hold_y_min;
    c->hold_y_max = uvm2_hold_y_max;
    c->neg_rate_x = NEG_RATE_X;
    c->neg_rate_y = NEG_RATE_Y;
    c->drift_x = uvm2_drift_x;
    c->drift_y = uvm2_drift_y;
    c->hz       = uvm2_setting_hz;
    c->start_menu = uvm2_setting_menu;
    c->rotate     = uvm2_setting_rotate;
}

void uvm2_config_apply(const struct uvm2_config *c)
{
    if (c->scale > 0)  DRAW_SCALE  = (uint32_t)c->scale;
    T1_EXTRA_Q8     = (uint32_t)c->t1_tail_q8;
    uvm2_zero_offset = c->zero;
    if (c->bright >= 0) uvm2_draw_intensity(c->bright);
    /* A ZERO HERE MEANS "the file is old", not "no hold". A calibration saved before these
     * two fields existed brings them in as zero, and sampling for zero cycles would leave Y
     * uncharged: they are ignored and the usual values stay. It is the invisible-contract trap,
     * and here it can be seen coming. */
    if (c->hold_y_min > 0) uvm2_hold_y_min = c->hold_y_min;
    if (c->hold_y_max > 0) uvm2_hold_y_max = c->hold_y_max;
    /* These CAN be zero: zero means "no correction", which is the honest default. */
    NEG_RATE_X = c->neg_rate_x;
    NEG_RATE_Y = c->neg_rate_y;
    uvm2_drift_x = c->drift_x;
    uvm2_drift_y = c->drift_y;
    uvm2_setting_hz   = (c->hz == 60) ? 60 : (c->hz == 0) ? 0 : 50;   /* 50, 60 or 0 = flat out (unlocked) */
    uvm2_setting_menu = c->start_menu ? 1 : 0;
    uvm2_setting_rotate = c->rotate ? 1 : 0;
    uvm2_draw_rotate((int)uvm2_setting_rotate);
}

/* ── THE TEXT FILE ON THE SD ────────────────────────────────────────────────────────
 *
 * `key value` per line, signed decimal. Text and not binary on purpose: it can be read and
 * edited from the PC, which while we are still tuning the beam is worth more than convenience
 * — and a calibration you cannot read is a calibration you cannot argue about. */
static int read_int(const char *s, int32_t *out)
{
    int32_t v = 0; int sign = 1, any = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    if (any) *out = v * sign;
    return any;
}

static int load_from_sd(struct uvm2_config *c, const char *path, int game_only)
{
    /* THIS FILE'S TWO ENDS CONTRADICTED EACH OTHER, AND NO SIZE SATISFIED BOTH.
     *
     * `uvm2_sd_overwrite` rewrites the WHOLE SECTOR and requires the file to be 512 bytes or
     * more. This asked for `uvm2_sd_read(..., 511)`, and THAT reader fails if the file does not
     * fit ENTIRELY within the maximum — its semantics are the romset loader's, "half a romset is
     * not a romset". At 512 the writer accepts it and the reader throws it away; at 511, the
     * other way round. It saved correctly and never loaded, which is what the console showed.
     *
     * `uvm2_sd_read_from` is the CHUNKED reader and has no such rule: it reads whatever fits
     * from the offset it is given. Here only the first sector matters, which is exactly what
     * the writer touches. */
    static unsigned char buf[520];
    uint32_t n = uvm2_sd_read_from(path, buf, 512, 0);
    if (n == 0) return 0;
    buf[n] = 0;
    int seen = 0;
    for (unsigned char *p = buf; *p; ) {
        unsigned char *ln = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*ln == ' ' || *ln == '\t') ln++;
        if (*ln == '#' || *ln == 0) continue;
        char *v = (char *)ln;
        while (*v && *v != ' ' && *v != '\t') v++;
        if (!*v) continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        int32_t x;
        if (!read_int(v, &x)) continue;
        for (int k = 0; k < N_FIELDS; k++) {
            if (strcmp((char *)ln, FIELDS[k].n)) continue;
            /* From the game's file only what that game declares as its own is honoured: that
             * way a stray `rotate` in the folder does not rotate a game that does not use it. */
            if (game_only && !(FIELDS[k].bit & s_game_settings)) break;
            *field_of(c, k) = x; seen = 1; break;
        }
    }
    return seen;
}

/* ── THE FLASH ───────────────────────────────────────────────────────────────────────── */
static const struct saved *in_flash(void)
{
    return (const struct saved *)(XIP_BASE + UVM2_CONFIG_FLASH_OFF);
}

static int load_from_flash(struct uvm2_config *c)
{
    const struct saved *g = in_flash();
    if (g->sig != SIGNATURE || g->sum != sum_of(&g->c)) return 0;
    *c = g->c;
    return 1;
}

int uvm2_config_load(void)
{
    struct uvm2_config c;
    uvm2_config_current(&c);            /* start from whatever the game was compiled with */
    /* THE SD WINS IF IT EXISTS: that way a setting can be tested by dropping a file in, without
     * going through the wizard or the flash.
     *
     * THE SHARED ONE AS THE BASE — with all its fields, the game ones included: a file from
     * before this separation carries hz or the menu there, and they serve as starting values. */
    int any = load_from_sd(&c, SD_PATH, 0) || load_from_flash(&c);
    /* AND THE GAME'S ON TOP, which may only touch what the game declared as its own. */
    if (s_game_path[0] && load_from_sd(&c, s_game_path, 1)) any = 1;
    if (any) uvm2_config_apply(&c);
    return any;
}

/* SAVING TO FLASH, AND THE CONDITION THAT CANNOT BE HIDDEN.
 *
 * Erasing and programming flash STOPS THE XIP: while it lasts, any core executing or reading
 * from flash hangs. Our core 1 runs the list executor, so **the caller must have stopped it** —
 * the wizard does, because it owns the frame loop. I deliberately do not do it here: stopping
 * it on my own account from a configuration function would be exactly the kind of hidden side
 * effect this project has already paid for.
 *
 * It is done in one go with interrupts off: a sector is 4096 bytes and what we write is 24, but
 * flash can only be erased by sectors. */
/* An integer to text, without printf: pulling that in for four numbers costs 20 KB of flash. */
static int put_int(char *d, int32_t v)
{
    int p = 0;
    if (v < 0) { d[p++] = '-'; v = -v; }
    char t[8]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 7);
    while (k) d[p++] = t[--k];
    return p;
}

static int put_field(char *d, const char *name, int32_t v)
{
    int p = 0;
    while (*name) d[p++] = *name++;
    d[p++] = ' ';
    p += put_int(d + p, v);
    d[p++] = '\n';
    return p;
}

/* SAVING: TO THE SD, AND NOT TO THE FLASH.
 *
 * This cartridge's flash IS NOT OURS: the UVM2's firmware occupies ~15.6 MB of the RP2350's 16,
 * so the "last free sector" was very probably theirs and erasing it can leave it unable to
 * boot. See the `#if 0` note further down.
 *
 * It first tries to OVERWRITE IN PLACE — which touches neither the FAT nor the directory and is
 * the cheapest and safest thing — and if the file is not there, it CREATES it, with its folder
 * if need be.
 *
 * Creation touches the FAT and the directory, so it is tested against REAL FAT16 and FAT32
 * images on the host: `fsck_msdos` reports not one error on either, and macOS mounts the images
 * and reads the file. Without that test I would have had no right to write to anyone's card.
 *
 * CREATING THE FILE IS NORMAL AGAIN, AND IT WAS SWITCHED OFF FOR A FEW HOURS.
 *
 * On 2026-09-14 dkong hung on finishing the calibration with a card that had no `config/`: over
 * SWD, the PC pinned at `uvm2_sd.c:108` in THREE consecutive samples. I switched it off while
 * the cause was being looked for, and it was put back in its place: "we cannot depend on it
 * existing and only overwrite in that case. it should be creatable. fat32 is not an unknown
 * filesystem".
 *
 * And it was not a hang, it was `alloc_cluster` reading a SECTOR of the card for every cluster
 * it looked at. Measured on the host with a FAT32 the size of that card and the first gap at
 * 40,000 clusters:
 *
 *     before  80,009 reads, 136 writes
 *     now        326 reads,  73 writes
 *
 * 245 times fewer, and `fsck_msdos` comes out clean in all three phases. */
static int s_allow_create = 1;
void uvm2_config_allow_create(int enable) { s_allow_create = enable != 0; }

/* Writes the fields `mask` asks for (0 = the console's) to `path`. Overwrite in place first,
 * which touches neither the FAT nor the directory; create only if it is not there. */
static int save_to(const char *path, const struct uvm2_config *c, unsigned mask)
{
    char txt[256];
    int p = 0, k;
    for (k = 0; k < N_FIELDS; k++) {
        const unsigned bit = FIELDS[k].bit;
        if (mask ? !(bit & mask) : (bit != 0)) continue;
        p += put_field(txt + p, FIELDS[k].n, *field_of((struct uvm2_config *)c, k));
    }
    if (p == 0) return 1;                       /* nothing to save here: not a failure */
    if (uvm2_sd_overwrite(path, (const unsigned char *)txt, (uint32_t)p)) return 1;
    if (uvm2_sd_error != UVM2_SD_MISSING && uvm2_sd_error != UVM2_SD_TOO_BIG) return 0;
    if (!s_allow_create) return 0;
    return uvm2_sd_create(path, (const unsigned char *)txt, (uint32_t)p);
}

int uvm2_config_save(void)
{
    struct uvm2_config c;
    int ok;
    uvm2_config_current(&c);
    /* The console's to the shared file — the first game to start creates it and they all
     * share it. */
    ok = save_to(SD_PATH, &c, 0);
    /* And the game's to its own, only what that game declares it uses. */
    if (s_game_path[0] && s_game_settings)
        ok = save_to(s_game_path, &c, s_game_settings) && ok;
    return ok;
}

/* ── THE FLASH PATH, HALTED ───────────────────────────────────────────────────────── */
static int save_to_flash_DO_NOT_USE(void)

{
    struct saved g;
    memset(&g, 0xFF, sizeof g);        /* the rest of the page, as the erase leaves it */
    g.sig = SIGNATURE;
    uvm2_config_current(&g.c);
    g.sum = sum_of(&g.c);

    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    memcpy(page, &g, sizeof g < sizeof page ? sizeof g : sizeof page);

    /* ── HALTED: THIS CARTRIDGE'S FLASH IS NOT OURS ─────────────────────────────────
     *
     * The UVM2's firmware (Ultimate Vectrex Multicart 2 v1.1.8a) occupies ~15.6 MB of the
     * RP2350's 16, so the "last free sector" this code assumed is very probably THEIRS.
     * Erasing it can leave the cartridge unable to boot — and that is not a fault another flash
     * fixes, because the thing that flashes IS the firmware.
     *
     * Our .um2 images are loaded from the SD into RAM/PSRAM: we do not own that flash and we
     * have no map saying what is spare. Until we have that map, this does NOT write.
     * Persistence goes through the SD, overwriting IN PLACE a file that already exists —
     * without touching the FAT or the directory, which is the only thing a read-only reader can
     * do safely.
     *
     * The code stays, it is not deleted: when we know which region is free, uncomment it and
     * that is that. An `#if 0` with the reason next to it is worth more than a gap. */
#if 0
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(UVM2_CONFIG_FLASH_OFF, FLASH_SECTOR_SIZE);
    flash_range_program(UVM2_CONFIG_FLASH_OFF, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
#else
    (void)page;
    return 0;                 /* nowhere to save yet: see the block above */
#endif

    /* AND IT IS VERIFIED BY READING. A write that is not read back is an assumption: if the
     * flash was protected or the offset falls outside, this says so now and not three sessions
     * later when somebody notices the calibration "does not save". */
    return load_from_flash(&g.c);
}
