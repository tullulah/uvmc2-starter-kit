/* romzip — pull a ROM out of a MAME-style .zip romset.
 *
 * The game does not carry the ROM inside: it finds it in `ROMS/<game>.zip` on the card.
 * This is the piece that extracts it, and it is written to live on ALL THREE targets: the
 * ZIP is read through a `JZFile`, which is a callback interface (read/seek/tell). On the
 * host that backend is stdio; on the cartridge it is the SD driver. The library never
 * notices.
 *
 * junzip (public domain) + puff (Mark Adler's reference inflate): both were already in the
 * AAE tree, and AAE already uses them for exactly this. puff uses neither malloc nor stdio,
 * which is what makes it valid on bare metal.
 *
 * IT VERIFIES THE CRC-32 the ZIP itself carries. Without that, a wrong ROM starts up and
 * breaks half way, and the player has no way of knowing the file is the problem.
 */
#include <string.h>

/* junzip prints its errors with fprintf(stderr, ...) and defines a backend over FILE*. On
 * the host that is useful; on the cartridge there is no stdio, so there it is built with
 * RA_ROMZIP_NO_STDIO and the warnings go away. This is NOT cosmetic: without it junzip.c
 * does not compile on bare metal.
 *
 * No errors are lost — ra_romzip_load RETURNS a code for every case, which is what the
 * caller needs in order to tell the player something useful. The fprintf only duplicated
 * that information somewhere nobody reads it. */
#if defined(ROMZIP_NO_STDIO) || defined(RA_ROMZIP_NO_STDIO)
#  define fprintf(...) ((void)0)
#  define stderr       ((void *)0)
/* And junzip's FILE* backend is neutralised whole. It still compiles — its declaration is
 * in the header and cannot be removed without touching the library — but it ends up as dead
 * code that `--gc-sections` takes away. On the cartridge the JZFile reads from MEMORY, not
 * from files.
 *
 * It is neutralised here rather than by patching junzip.c on purpose: that library is
 * public domain and untouched, so it can be updated without re-applying anything. */
#  define fread(b, s, n, f)  ((size_t)0)
#  define ftell(f)           ((long)0)
#  define fseek(f, o, w)     (-1)
#  define ferror(f)          (0)
#  define fclose(f)          ((void)0)
#  define fopen(p, m)        ((FILE *)0)
#  define printf(...)        ((void)0)
#else
#  include <stdio.h>
#endif

#include "romzip.h"   /* brings the FILE/SEEK_* stand-in before junzip.h */
#include "junzip.h"
#include "junzip.c"   /* it does not include its own header: it goes AFTER junzip.h */

/* CRC-32 (the PKZIP polynomial), computed without a table so as not to spend 1 KB of RAM
 * on something that runs ONCE at startup. Slower, and it does not matter. */
static unsigned long crc32_of(const unsigned char *p, unsigned long n)
{
    unsigned long c = 0xFFFFFFFFul;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320ul & (-(long)(c & 1)));
    }
    return c ^ 0xFFFFFFFFul;
}

typedef struct { int found; JZFileHeader hdr; } Pick;

static int pick_first(JZFile *zip, int idx, JZFileHeader *h, char *name, void *user)
{
    Pick *p = (Pick *)user;
    (void)zip; (void)idx; (void)name;
    if (!p->found) { p->found = 1; p->hdr = *h; }
    return 1;
}

/* ── THE ZIP THE LAUNCHER LEFT IN MEMORY ────────────────────────────────────
 *
 * There is no file system on the cartridge: something reads roms/<GAME>.ZIP off the card,
 * leaves it as it is in RAM and publishes a descriptor whose address it writes into the
 * game's own header:
 *
 *     game_header[3] ──> [ 'RMZ1' ][ zip base ][ size ][ rsv ]
 */
size_t romzip_mem_read(JZFile *f, void *buf, size_t n)
{
    RomzipMem *m = (RomzipMem *)f;
    if (m->pos >= m->len) return 0;
    if (n > m->len - m->pos) n = m->len - m->pos;
    memcpy(buf, m->base + m->pos, n);
    m->pos += n;
    return n;
}
static size_t romzip_mem_tell(JZFile *f) { return ((RomzipMem *)f)->pos; }
static int romzip_mem_seek(JZFile *f, size_t off, int whence)
{
    RomzipMem *m = (RomzipMem *)f;
    size_t p;
    if (whence == SEEK_SET)      p = off;
    else if (whence == SEEK_CUR) p = m->pos + off;
    else                         p = m->len - off;   /* SEEK_END, off counts backwards */
    if (p > m->len) return -1;
    m->pos = p;
    return 0;
}
static int  romzip_mem_error(JZFile *f) { (void)f; return 0; }
static void romzip_mem_close(JZFile *f) { (void)f; }

/* The weak definition. On the cartridge the one in rp2350_start.s wins (a strong
 * definition always beats a weak one); elsewhere this keeps the link standing, with zeros.
 * Verify it by looking at the ELF, not by assuming: on the cartridge game_header has to
 * land in the game's window, not in .bss. */
__attribute__((weak)) const unsigned int game_header[4] = { 0, 0, 0, 0 };
/* WHERE THE ROMSET DESCRIPTOR IS. A NORMAL variable, filled in by whoever loads it;
 * 0 = nobody has, and game_header[3] is consulted instead, as always.
 *
 * NO WEAK SYMBOLS, after two attempts with them failed in two different silent ways:
 *   - an undefined weak `extern`: in ELF it resolves to 0 and the `&sym ? ... : 0` idiom
 *     works, but in Mach-O it does NOT link — the host died with "Undefined symbols".
 *   - a weak definition `const ... = 0`: it links on both, but the compiler FOLDS it inside
 *     this translation unit and the symbol does not even appear in the ELF, so the UVM2's
 *     strong definition can no longer override it. Games would have been left with no
 *     romset, across all the ports, without a warning.
 * An ordinary variable that somebody writes at startup has neither trap and reads the same
 * on all three platforms. */
#ifdef UVM2_PICO_RUNTIME
extern unsigned long uvm2_romzip_desc;   /* uvm2_romzip.c defines and fills it */
#else
unsigned long uvm2_romzip_desc = 0;      /* host and RP2350: nobody fills it, and that is fine */
#endif

/* The zip is in RAM: we can hand out the pointer and save the copy. */
static const unsigned char *romzip_mem_direct(JZFile *f, unsigned long n)
{
    RomzipMem *m = (RomzipMem *)f;
    const unsigned char *p;
    if (m->pos + n > m->len) return 0;
    p = m->base + m->pos;
    m->pos += n;
    return p;
}

/* ── THE WAY IN FOR HOST HARNESSES ──────────────────────────────────────────
 *
 * The cartridge descriptor stores the base in 32 BITS ('RMZ1', base, length), and on a
 * 64-bit host that is no good: macOS on arm64 maps nothing below 4 GB, so the pointer does
 * not fit and truncating it blows up far from here. The format is NOT touched, since it is
 * a contract with the cartridge BIOS; the host publishes the zip through these two
 * wide-pointer variables instead.
 *
 * It only exists where there is stdio, i.e. host and simulator: the cartridge builds with
 * ROMZIP_NO_STDIO and there is neither symbol nor branch there. */
#if !defined(ROMZIP_NO_STDIO) && !defined(RA_ROMZIP_NO_STDIO)
const unsigned char *romzip_host_base = 0;
size_t               romzip_host_len  = 0;
#endif

int romzip_open_cart(RomzipMem *m)
{
    const unsigned int *d;

    memset(m, 0, sizeof *m);
#if !defined(ROMZIP_NO_STDIO) && !defined(RA_ROMZIP_NO_STDIO)
    if (romzip_host_base && romzip_host_len) {
        m->handle.read  = romzip_mem_read;
        m->handle.tell  = romzip_mem_tell;
        m->handle.seek  = romzip_mem_seek;
        m->handle.error = romzip_mem_error;
        m->handle.close = romzip_mem_close;
        m->handle.direct = romzip_mem_direct;
        m->base = romzip_host_base;
        m->len  = romzip_host_len;
        return 1;
    }
#endif
    /* THE UVM2 PUBLISHES IT SEPARATELY. It used to override this game_header[] with a
     * strong definition, and that collided with the one other games generate in their own
     * .S — neither would link. With a symbol of its own, each keeps its own. */
    unsigned long desc = uvm2_romzip_desc;
    if (!desc) desc = (unsigned long)game_header[3];
    if (!desc) return 0;                          /* the launcher published nothing */
    d = (const unsigned int *)desc;
    if (d[0] != ROMZIP_DESC_MAGIC) return 0;       /* not a valid descriptor */

    m->handle.read  = romzip_mem_read;
    m->handle.tell  = romzip_mem_tell;
    m->handle.seek  = romzip_mem_seek;
    m->handle.error = romzip_mem_error;
    m->handle.close = romzip_mem_close;
    m->handle.direct = romzip_mem_direct;
    m->base = (const unsigned char *)(unsigned long)d[1];
    m->len  = (size_t)d[2];
    return 1;
}

/* ── by NAME ────────────────────────────────────────────────────────────────
 *
 * The same as ra_romzip_load but picking the member by name, which is what a MAME romset
 * needs: one file per chip, each to its own address.
 *
 * The comparison is on the BASE NAME and case-insensitive. The first because some zips keep
 * their files inside a folder named after the game and others do not, and that difference
 * should not decide whether a game starts. The second because the name being compared comes
 * from a generated table and the zip can be made by anyone.
 *
 * There is NO fuzzy matching here, on purpose: the generator does that with the zip in
 * front of it and writes down the exact name. Guessing on the cartridge is what turns "a
 * file is missing" into "the game behaves oddly".
 */
typedef struct { int found; JZFileHeader hdr; const char *want; } PickName;

static const char *base_name(const char *p)
{
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == 0x5C) b = p + 1;
    return b;
}

static int equal_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static int pick_named(JZFile *zip, int idx, JZFileHeader *h, char *name, void *user)
{
    PickName *p = (PickName *)user;
    (void)zip; (void)idx;
    if (!p->found && equal_nocase(base_name(name), p->want)) {
        p->found = 1;
        p->hdr = *h;
        return 0;                      /* found: no need to keep looking */
    }
    return 1;
}

int romzip_load_named(JZFile *zip, const char *name, unsigned char *dest,
                      unsigned long dest_max, unsigned long *out_size)
{
    JZEndRecord end;
    PickName pick;
    JZFileHeader lh;
    char lname[128];

    if (jzReadEndRecord(zip, &end) != Z_OK) return RA_ROMZIP_NOT_A_ZIP;

    memset(&pick, 0, sizeof pick);
    pick.want = base_name(name);
    jzReadCentralDirectory(zip, &end, pick_named, &pick);
    /* The walk's return value is not checked: pick_named cuts it short on purpose by
     * returning 0 when it finds a match, and the reader reports that as an error. What
     * decides is whether we found the file. */
    if (!pick.found) return RA_ROMZIP_EMPTY;
    if (pick.hdr.uncompressedSize > dest_max) return RA_ROMZIP_TOO_BIG;

    if (zip->seek(zip, pick.hdr.offset, SEEK_SET)) return RA_ROMZIP_READ_ERROR;
    if (jzReadLocalFileHeader(zip, &lh, lname, sizeof lname) != Z_OK)
        return RA_ROMZIP_READ_ERROR;
    if (jzReadData(zip, &lh, dest) != Z_OK) return RA_ROMZIP_INFLATE_ERROR;
    if (crc32_of(dest, lh.uncompressedSize) != lh.crc32) return RA_ROMZIP_BAD_CRC;

    if (out_size) *out_size = lh.uncompressedSize;
    return RA_ROMZIP_OK;
}

int ra_romzip_load(JZFile *zip, unsigned char *dest, unsigned long dest_max,
                   unsigned long *out_size)
{
    JZEndRecord end;
    Pick pick;
    JZFileHeader lh;
    char lname[128];

    if (jzReadEndRecord(zip, &end) != Z_OK) return RA_ROMZIP_NOT_A_ZIP;

    memset(&pick, 0, sizeof pick);
    if (jzReadCentralDirectory(zip, &end, pick_first, &pick) != Z_OK)
        return RA_ROMZIP_NOT_A_ZIP;
    if (!pick.found) return RA_ROMZIP_EMPTY;
    if (pick.hdr.uncompressedSize > dest_max) return RA_ROMZIP_TOO_BIG;

    if (zip->seek(zip, pick.hdr.offset, SEEK_SET)) return RA_ROMZIP_READ_ERROR;
    if (jzReadLocalFileHeader(zip, &lh, lname, sizeof lname) != Z_OK)
        return RA_ROMZIP_READ_ERROR;
    if (jzReadData(zip, &lh, dest) != Z_OK) return RA_ROMZIP_INFLATE_ERROR;

    /* The check that turns "it does not work" into "it is a different ROM". */
    if (crc32_of(dest, lh.uncompressedSize) != lh.crc32) return RA_ROMZIP_BAD_CRC;

    if (out_size) *out_size = lh.uncompressedSize;
    return RA_ROMZIP_OK;
}
