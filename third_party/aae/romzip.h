/* Extract a ROM out of a .zip romset. See romzip.c. */
#ifndef ROMZIP_H
#define ROMZIP_H

/* ── A stdio stand-in for bare metal ────────────────────────────────────────
 *
 * junzip.h declares a backend over `FILE*` and its .c uses SEEK_SET/CUR/END. There is no
 * stdio on the cartridge, and without this NOTHING in junzip compiles — even though we do
 * not use that backend, the declaration alone is enough to break the build.
 *
 * The stdio backend stays declared but unused, and `--gc-sections` takes it away: on the
 * cartridge we read from memory, not from files.
 *
 * It goes in THIS header and not in each .c so include order does not matter: whoever
 * includes romzip.h before junzip.h already has it settled. */
#if defined(ROMZIP_NO_STDIO) || defined(RA_ROMZIP_NO_STDIO)

#  ifndef SEEK_SET
#    define SEEK_SET 0
#    define SEEK_CUR 1
#    define SEEK_END 2
#  endif
#endif

/* Every code is a DIFFERENT MESSAGE for the player. A single "error" forces them to guess
 * whether the file is missing, corrupt, or from another version. */
#define RA_ROMZIP_OK             0
#define RA_ROMZIP_NOT_A_ZIP     -1
#define RA_ROMZIP_EMPTY         -2
#define RA_ROMZIP_TOO_BIG       -3
#define RA_ROMZIP_READ_ERROR    -4
#define RA_ROMZIP_INFLATE_ERROR -5
#define RA_ROMZIP_BAD_CRC       -6

#include "junzip.h"   /* RomzipMem embeds a JZFile, so the complete type is needed */
int ra_romzip_load(JZFile *zip, unsigned char *dest, unsigned long dest_max,
                   unsigned long *out_size);

/* The zip the launcher left in memory. `game_header` is a symbol from the game's startup
 * code (rp2350_start.s) and its fourth word points at the descriptor.
 *
 * It only REALLY exists on the cartridge. romzip.c carries a WEAK definition at zero so the
 * host and the simulator link just the same: there, romzip_open_cart() answers "no romset
 * published", which is the truth. One code path, with no per-target #ifdef for somebody to
 * forget to set. */
extern const unsigned int game_header[];
#define ROMZIP_DESC_MAGIC 0x315A4D52u   /* 'RMZ1' */

typedef struct {
    JZFile handle;
    const unsigned char *base;
    size_t len, pos;
} RomzipMem;

/* Returns 1 if a romset has been published and `m` is ready; 0 if there is none. */
int romzip_open_cart(RomzipMem *m);

/* ── by NAME, for multi-file romsets ────────────────────────────────────────
 *
 * A MAME romset brings one file per chip and each one goes to a different address, so "the
 * first one in the zip" is no good: they have to be asked for by name. The name is the
 * EXACT zip member name; reconciling what the AAE source calls a ROM with what the file is
 * really called is done by the romtable generator, with the zip in front of it, and it
 * writes the real name into the game's table. Guessing here would be expensive: in some
 * romsets all four files share a prefix.
 */
int romzip_load_named(JZFile *zip, const char *name, unsigned char *dest,
                      unsigned long dest_max, unsigned long *out_size);


#endif /* ROMZIP_H */
