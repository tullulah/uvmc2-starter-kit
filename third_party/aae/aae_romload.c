/* aae_romload.c — implementation. See aae_romload.h for the why. */
#include <stdint.h>
#include <string.h>
#include "romzip.h"   /* brings the FILE/SEEK_* substitute, and then junzip.h */
#include "aae_romload.h"

#include <vectrex/vectrexInterface.h>   /* v_directDraw32 */

#ifndef ROMZIP_NO_STDIO
#include <stdio.h>
#include <stdlib.h>
#endif

/* Where to find the romset when this is NOT a cartridge (host and WASM simulator). The
 * IDE drops the zip there inside the emscripten FS; it is the simulator's "SD card". */
#ifndef AAE_ROMZIP_FS_PATH
#define AAE_ROMZIP_FS_PATH "/roms/game.zip"
#endif

int aae_rom_last_error = 0;

/* ── SAY IT ON SCREEN ───────────────────────────────────────────────────────
 *
 * Without this, a missing romset leaves the emulator running over zeros: it draws nothing,
 * and that is black — indistinguishable from a hang, from a broken port or from a badly
 * flashed cartridge. The console has no console: if it is not painted, nobody knows.
 *
 * ── THE ERROR, AS TEXT, WITH THE PATH ──────────────────────────────────────
 *
 * This used to be a frame with an X and the code in bars. It was enough to say "something
 * is wrong here", but not to fix it: with the cross in front of you, you have to go to the
 * source to find out what three bars mean, and it does not say WHICH file it looked for.
 *
 * ITS OWN, TINY FONT, for the same reason there was none before: 43 ports share this file.
 * It is a 5x7 grid with A-Z, 0-9 and four symbols; each glyph is a run of (x<<4)|y bytes
 * with 0xFE = lift the pen and 0xFF = end. The table is 312 bytes.
 *
 * And it is drawn with v_directDraw32, not with v_rasterText: raster text does not exist in
 * the simulator's SDK, and a diagnostic that fails to compile on one of the three targets
 * is no use — precisely when what is being diagnosed is that the game will not start.
 *
 * THE TEXT IS IN ENGLISH, like the marquees of the arcade machines we port. */
static const unsigned char FONT[] = {
    0x00, 0x04, 0x26, 0x44, 0x40, 0xFE, 0x02, 0x42, 0xFF, 0x00, 0x06, 0x36,
    0x45, 0x44, 0x33, 0x03, 0xFE, 0x33, 0x42, 0x41, 0x30, 0x00, 0xFF, 0x45,
    0x36, 0x16, 0x05, 0x01, 0x10, 0x30, 0x41, 0xFF, 0x00, 0x06, 0x36, 0x45,
    0x41, 0x30, 0x00, 0xFF, 0x46, 0x06, 0x00, 0x40, 0xFE, 0x03, 0x33, 0xFF,
    0x46, 0x06, 0x00, 0xFE, 0x03, 0x33, 0xFF, 0x45, 0x36, 0x16, 0x05, 0x01,
    0x10, 0x30, 0x41, 0x43, 0x23, 0xFF, 0x00, 0x06, 0xFE, 0x40, 0x46, 0xFE,
    0x03, 0x43, 0xFF, 0x16, 0x36, 0xFE, 0x26, 0x20, 0xFE, 0x10, 0x30, 0xFF,
    0x36, 0x31, 0x20, 0x10, 0x01, 0xFF, 0x00, 0x06, 0xFE, 0x46, 0x03, 0x40,
    0xFF, 0x06, 0x00, 0x40, 0xFF, 0x00, 0x06, 0x24, 0x46, 0x40, 0xFF, 0x00,
    0x06, 0x40, 0x46, 0xFF, 0x10, 0x01, 0x05, 0x16, 0x36, 0x45, 0x41, 0x30,
    0x10, 0xFF, 0x00, 0x06, 0x36, 0x45, 0x44, 0x33, 0x03, 0xFF, 0x10, 0x01,
    0x05, 0x16, 0x36, 0x45, 0x41, 0x30, 0x10, 0xFE, 0x22, 0x40, 0xFF, 0x00,
    0x06, 0x36, 0x45, 0x44, 0x33, 0x03, 0xFE, 0x23, 0x40, 0xFF, 0x45, 0x36,
    0x16, 0x05, 0x04, 0x42, 0x41, 0x30, 0x10, 0x01, 0xFF, 0x06, 0x46, 0xFE,
    0x26, 0x20, 0xFF, 0x06, 0x01, 0x10, 0x30, 0x41, 0x46, 0xFF, 0x06, 0x20,
    0x46, 0xFF, 0x06, 0x10, 0x23, 0x30, 0x46, 0xFF, 0x00, 0x46, 0xFE, 0x06,
    0x40, 0xFF, 0x06, 0x23, 0x46, 0xFE, 0x23, 0x20, 0xFF, 0x06, 0x46, 0x00,
    0x40, 0xFF, 0x10, 0x01, 0x05, 0x16, 0x36, 0x45, 0x41, 0x30, 0x10, 0xFF,
    0x15, 0x26, 0x20, 0xFE, 0x10, 0x30, 0xFF, 0x05, 0x16, 0x36, 0x45, 0x44,
    0x00, 0x40, 0xFF, 0x06, 0x46, 0x23, 0x42, 0x41, 0x30, 0x10, 0x01, 0xFF,
    0x30, 0x36, 0x02, 0x42, 0xFF, 0x46, 0x06, 0x03, 0x33, 0x42, 0x41, 0x30,
    0x10, 0x01, 0xFF, 0x45, 0x36, 0x16, 0x05, 0x01, 0x10, 0x30, 0x41, 0x42,
    0x33, 0x03, 0xFF, 0x06, 0x46, 0x20, 0xFF, 0x13, 0x04, 0x05, 0x16, 0x36,
    0x45, 0x44, 0x33, 0x13, 0x02, 0x01, 0x10, 0x30, 0x41, 0x42, 0x33, 0xFF,
    0x01, 0x10, 0x30, 0x41, 0x45, 0x36, 0x16, 0x05, 0x04, 0x13, 0x43, 0xFF,
    0x00, 0x46, 0xFF, 0x20, 0x21, 0xFF, 0x00, 0x40, 0xFF, 0x13, 0x33, 0xFF
};
static const unsigned short FONT_OFS[] = { 0, 9, 23, 32, 40, 48, 55, 66, 75, 84, 90, 97, 101, 107, 112, 122, 130, 143, 154, 165, 171, 178, 182, 188, 194, 201, 206, 216, 223, 231, 240, 245, 255, 267, 271, 288, 300, 303, 306, 309 };

/* ONE RE-ZERO PER LETTER.
 *
 * The whole screen was loose calls to v_directDraw32: 32 glyphs WITHOUT A SINGLE re-zero,
 * with the beam crossing the gap between letters in the dark. Every one of those jumps adds
 * position error that accumulates, and on hardware that is exactly what showed: the first
 * letters of each line clean and the degradation running RIGHTWARDS -- ragged heights, the
 * O of FOUND left open, the final D and P clipped (Daniel, 2026-09-11).
 *
 * A letter is a figure: let it start from zero like any other object. It costs about 211
 * cycles each, 6,750 across the two lines, and this screen draws NOTHING else -- there are
 * 30,000 per frame. It is the one place in the game where the budget genuinely has slack.
 *
 * WEAK on purpose: `v_beamNewStroke` only exists in the cartridge SDK. On the host and in
 * the simulator there are no integrators to drift, and a diagnostic that stops compiling on
 * one of the three targets is no use. */
/* A DEFINITION FOR THE TARGETS THAT HAVE NO BEAM, not a weak declaration.
 *
 * This was `void v_beamNewStroke(void) __attribute__((weak));` and it broke the HOST build:
 * on Mach-O a weak declaration still has to resolve at link time, so `make host` died with
 * "Undefined symbols: _v_beamNewStroke" -- and with it DUMPTILES and DUMPCODES, which are
 * the only way to read which tile code lands in which cell. `weak_import` did not fix it
 * either. The symbol lives in the cartridge SDK and nowhere else; on the host and in the
 * simulator there are no integrators to drift, so an empty one is the honest answer and it
 * cannot fail to link. */
void v_beamNewStroke(void);
#ifndef VPY_RP2350
void v_beamNewStroke(void) { }
#endif

/* One character on the grid, at scale `e`, with its bottom-left corner at (ox,oy). */
static void ae_glyph(int c, int ox, int oy, int e)
{
    v_beamNewStroke();
    int i, n = -1;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') n = c - 'A';
    else if (c >= '0' && c <= '9') n = 26 + c - '0';
    else if (c == '/') n = 36;
    else if (c == '.') n = 37;
    else if (c == '_') n = 38;
    else if (c == '-') n = 39;
    if (n < 0) return;                      /* space and everything else: a gap */
    {
        const unsigned char *g = FONT + FONT_OFS[n];
        int px = 0, py = 0, pen = 0;
        for (i = 0; g[i] != 0xFF; i++) {
            if (g[i] == 0xFE) { pen = 0; continue; }
            {
                int x = ox + ((g[i] >> 4) & 0xF) * e, y = oy + (g[i] & 0xF) * e;
                if (pen) v_directDraw32(px, py, x, y, 90);
                px = x; py = y; pen = 1;
            }
        }
    }
}

/* One centred line, which SHRINKS ITSELF to fit.
 *
 * The usable field is about +-12,000 units in the space v_directDraw32 receives (the port
 * hands it (x-centre)*scale). A path like "roms/averylongname.zip" is over 20 characters,
 * and at a fixed size it would run off both sides — exactly the message you need to read in
 * full. So the cell size comes from the length: `e` is the maximum, not the value. */
static void ae_text(const char *s, int oy, int e)
{
    int n = 0, i, ox, fits;
    while (s[n]) n++;
    if (!n) return;
    fits = 22000 / (n * 6);
    if (fits < e) e = fits;
    if (e < 40) e = 40;                      /* below this it is unreadable: let it overflow */
    ox = -(n * 6 * e) / 2;
    for (i = 0; i < n; i++) ae_glyph(s[i], ox + i * 6 * e, oy, e);
}

/* The same centred text, for anyone who needs it outside this file: the settings notice
 * shown when a game starts uses this instead of dragging in a font of its own. */
void aae_text(const char *s, int oy, int e) { ae_text(s, oy, e); }

/* The path that was tried. On the cartridge the romset lives in roms/<name> (uvm2_romzip_load
 * builds it with the same rule); on the host $AAE_ROMZIP wins. It is rebuilt here instead of
 * having the loader publish it, so two modules are not tied together by a diagnostic
 * string. */
extern const char game_romset_name[] __attribute__((weak));

static void ae_path(char *dst, int max)
{
    int n = 0;
    const char *p;
#ifndef ROMZIP_NO_STDIO
    p = getenv("AAE_ROMZIP");
    if (!p) p = AAE_ROMZIP_FS_PATH;
    while (*p && n < max - 1) dst[n++] = *p++;
    dst[n] = 0;
    return;
#else
    p = "roms/";
    while (*p && n < max - 1) dst[n++] = *p++;
    if (&game_romset_name) { p = game_romset_name; while (*p && n < max - 1) dst[n++] = *p++; }
    dst[n] = 0;
#endif
}

void aae_rom_error_screen(int code)
{
    static const char *const WHAT[] = {
        "ROM LOAD FAILED",      /* 0, should never be seen */
        "ROM FILE NOT FOUND",   /* -1 RA_ROMZIP_NOT_A_ZIP */
        "ROM FILE MISSING",     /* -2 RA_ROMZIP_EMPTY: the zip lacks what the table asks for */
        "ROM TOO BIG",          /* -3 */
        "ROM READ ERROR",       /* -4 */
        "ROM UNPACK ERROR",     /* -5 */
        "ROM CRC MISMATCH",     /* -6 */
    };
    int n = code < 0 ? -code : code;
    char path[64];
    if (n > 6) n = 0;
    ae_text(WHAT[n], 1500, 260);
    ae_path(path, sizeof path);
    ae_text(path, -1500, 200);
}

/* Staging buffer for interleaved loads. The destination of a ROM_LOAD_16B is strided, so
 * that one does need an intermediate; a plain load is decompressed DIRECTLY onto GI[], with
 * no copy. That is exactly the double copy every game used to pay: the bytes lived once in
 * the embedded array (in RAM, because it is not const) and once in the driver's buffer.
 * Here they only live where they belong.
 *
 * The size comes from the largest romset that uses interleaving: the four 68000 games,
 * whose biggest interleaved file is 8 KB (measured). The romtable generator checks the
 * limit as it generates and fails if any romset exceeds it, so this cannot silently come up
 * short. */
#if AAE_ROM_STAGE > 0
/* UVM2_PICO_RUNTIME is what tells the .um2 apart from everything else (uvm2_pico.cmake
 * defines it). The same Makefile builds the other cartridge, the WASM target and the host
 * harness with these flags, and none of those has this PSRAM at this address — without the
 * guard, putting the stage there means writing into nothing and loading zeros. */
#  if defined(AAE_ROM_STAGE_PSRAM) && defined(UVM2_PICO_RUNTIME)
/* THE STAGE, IN PSRAM. It gets the same treatment as the romset buffer (uvm2_romzip.c):
 * written once and read once, ALL during startup, and then it sits on RAM for the rest of
 * the session with nobody looking at it. Nothing here is timing-dependent — unlike the
 * command list, which is why that one does not move.
 *
 * THROUGH THE UNCACHED ALIAS, like the romset: writing through the normal window corrupts
 * the data (measured in uvm2_romzip.c). And the PSRAM is already up when this runs:
 * whoever fills the 'RMZ1' descriptor that romzip_open_cart opens is uvm2_romzip_load, and
 * that is what initialises it.
 *
 * 6 MB in: the romset lives in the first 4 MB, so the two do not collide. */
static unsigned char *const stage = (unsigned char *)(uintptr_t)AAE_ROM_STAGE_PSRAM;
#  else
static unsigned char stage[AAE_ROM_STAGE];
#  endif
#else
/* No stage: this game loads everything straight. The paths that use it return an error, and
 * the generated table would already have broken the BUILD if one were needed. */
#define stage ((unsigned char *)0)
#endif

int aae_rom_load_bases(const aae_rom_op *ops, int n, unsigned char *const *bases)
{
    RomzipMem mem;
    JZFile *zip = 0;
    const char *prev = 0;
    int i;
#ifndef ROMZIP_NO_STDIO
    StdioJZFile sf;
    FILE *fp = 0;
#endif

    aae_rom_last_error = 0;

    if (romzip_open_cart(&mem)) {
        zip = &mem.handle;
    } else {
#ifndef ROMZIP_NO_STDIO
        const char *path = getenv("AAE_ROMZIP");
        if (!path) path = AAE_ROMZIP_FS_PATH;
        fp = fopen(path, "rb");
        if (!fp) { aae_rom_last_error = RA_ROMZIP_NOT_A_ZIP; return 1; }
        jzfile_from_stdio_file(fp, &sf);
        zip = &sf.handle;
#else
        aae_rom_last_error = RA_ROMZIP_NOT_A_ZIP;
        return 1;
#endif
    }

    for (i = 0; i < n; i++) {
        const aae_rom_op *o = &ops[i];
        const char *name = o->file ? o->file : prev;      /* NULL = ROM_RELOAD */
        unsigned char *base = bases[o->region];
        unsigned long got = 0;
        int rc;

        if (!name || !base) { aae_rom_last_error = RA_ROMZIP_READ_ERROR; break; }
        if (o->file) prev = o->file;

        if (o->mode == AAE_ROM_PLAIN && o->src_off == 0) {
            /* The normal case: straight onto GI[], with no intermediate. */
            rc = romzip_load_named(zip, name, base + o->addr, o->size, &got);
            /* ...unless the FILE is bigger than the chunk being asked for, which happens
             * when a ROM_CONTINUE takes the remainder (some games page their program ROMs
             * that way). Then it does not fit in the destination and it has to be brought
             * whole into the stage and only its first part copied. It is detected from the
             * return code instead of being noted in the table: the generator should not
             * need to know file sizes for this to work. */
            if (rc == RA_ROMZIP_TOO_BIG && o->size <= AAE_ROM_STAGE) {
                rc = romzip_load_named(zip, name, stage, AAE_ROM_STAGE, &got);
                if (rc == RA_ROMZIP_OK) {
                    unsigned int n = (got < o->size) ? (unsigned int)got : o->size;
                    memcpy(base + o->addr, stage, n);
                }
            }
        } else if (o->mode == AAE_ROM_PLAIN) {
            /* ROM_CONTINUE: a slice of the file, so it has to be brought in whole. */
            unsigned int n;
            if (o->src_off + o->size > AAE_ROM_STAGE) { aae_rom_last_error = RA_ROMZIP_TOO_BIG; break; }
            rc = romzip_load_named(zip, name, stage, AAE_ROM_STAGE, &got);
            if (rc == RA_ROMZIP_OK) {
                n = (got > o->src_off) ? (unsigned int)(got - o->src_off) : 0u;
                if (n > o->size) n = o->size;
                memcpy(base + o->addr, stage + o->src_off, n);
            }
        } else {
            unsigned int j;
            if (o->size > AAE_ROM_STAGE) { aae_rom_last_error = RA_ROMZIP_TOO_BIG; break; }
            rc = romzip_load_named(zip, name, stage, o->size, &got);
            if (rc == RA_ROMZIP_OK) {
                const unsigned int offset = (o->mode == AAE_ROM_ODD) ? 1u : 0u;
                for (j = 0; j < o->size && j < got; j++)
                    base[o->addr + j * 2u + offset] = stage[j];
            }
        }

#ifdef AAE_ROM_TRACE
        { extern void aae_rom_trace(int, const char *, unsigned, unsigned, unsigned, int, unsigned long);
          aae_rom_trace(i, name, o->addr, o->size, o->src_off, rc, got); }
#endif
        if (rc != RA_ROMZIP_OK) { aae_rom_last_error = rc; break; }
    }

#ifndef ROMZIP_NO_STDIO
    if (fp) fclose(fp);
#endif
    return (i == n) ? 0 : i + 1;
}
