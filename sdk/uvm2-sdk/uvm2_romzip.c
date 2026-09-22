/* uvm2_romzip.c — read the romset off the SD card and publish it. Here the GAME does it,
 * because the UVM2 firmware does not.
 *
 * THE LOADING MACHINERY DOES NOT CHANGE. romzip.c still reads a zip FROM MEMORY through an
 * 'RMZ1' descriptor in game_header[3]; all that changes is who fills it in. On a cartridge
 * whose firmware serves romsets, the firmware fills it before starting the game; here we
 * fill it ourselves after reading the card.
 *
 * The descriptor exists from the start with its magic at ZERO, so if the read fails
 * romzip_open_cart() returns 0 exactly as always and the game paints its "no romset" sign —
 * which is the correct answer, not a hang.
 */
#include "uvm2_sd.h"

/* HOW MUCH ROMSET FITS. This is a static array in SRAM and the UVM2 image lives in 496 KB,
 * so it cannot be set "big just in case": at 64 KB, one of the games would not link (region
 * RAM overflowed by 37700 bytes). It is set per game, like AAE_ROM_STAGE:
 *
 *     UVM2_CFLAGS += -DUVM2_ROMZIP_MAX=$$((24*1024))
 *
 * The game's zip decides: look at `ls -l roms/<game>.zip` and round up. Coming up short does
 * NOT hang — uvm2_sd_read returns 0 with UVM2_SD_TOO_BIG and the game paints its "no
 * romset" sign. */
#ifndef UVM2_ROMZIP_MAX
#define UVM2_ROMZIP_MAX (24u * 1024u)
#endif

/* Weak: a game with no external ROM does not define it and this comes to nothing. */
__attribute__((weak)) extern const char game_romset_name[];

/* THE ROMSET, IN PSRAM. 23 KB of SRAM that were occupied for the WHOLE session by something
 * only used at startup: it is read off the card, decompressed into the game's ROM tables,
 * and never needed again. For some games that is more than they have left free.
 *
 * THROUGH THE UNCACHED ALIAS (0x15400000). Writing through the normal window CORRUPTS the
 * data: measured, 2512 bad words out of 4096; through the alias, zero. And here the PSRAM is
 * at its best — written once and read once, with nothing timing-dependent about it, which is
 * exactly the opposite of the command list.
 *
 * 4 MB in, so nothing collides if the two ever coexist. */
#ifdef UVM2_ROMZIP_IN_PSRAM
#  ifndef UVM2_ROMZIP_PSRAM_BASE
#    define UVM2_ROMZIP_PSRAM_BASE 0x15400000u
#  endif
static unsigned char *const s_zip = (unsigned char *)(uintptr_t)UVM2_ROMZIP_PSRAM_BASE;
#else
static unsigned char s_zip[UVM2_ROMZIP_MAX];
#endif
static uintptr_t s_desc[4];                    /* [0]=magic [1]=base [2]=size */

/* IT IS NOT CALLED `game_header`, AND THAT IS THE FIX.
 *
 * It used to be: a STRONG game_header[4] that deliberately overrode romzip.c's weak one to
 * publish the descriptor in word 3. That worked for the emulated ports... and broke every
 * game that generates its own strong game_header in its .S. Two strong definitions of the
 * same symbol:
 *
 *     multiple definition of `game_header'
 *
 * So none of those games would link for the UVM2, and the failure did not show in the
 * emulated ports because there the only rival game_header is weak.
 *
 * Publishing it under its own name means nobody fights over anything: the game keeps ITS
 * header and romzip.c looks here first.
 *
 * IT IS DEFINED HERE, which is where it is filled in. For a while it was defined in
 * romzip.c and declared here, and that broke any image linking the SDK WITHOUT romzip.c —
 * the test bench, for instance. The other way round always works: the UVM2 SDK goes into
 * every UVM2 image, and whoever does not link it does not link this either. See the note in
 * romzip.c for the two weak-symbol versions that failed before. */
unsigned long uvm2_romzip_desc = 0;

uint32_t uvm2_romzip_bytes = 0;                /* so it can be diagnosed from outside */
int      uvm2_romzip_error = 0;

void uvm2_romzip_load(void)
{
    uvm2_romzip_desc = (unsigned long)(uintptr_t)s_desc;

    if (!&game_romset_name || !game_romset_name[0]) return;   /* game with no romset */

    char path[80];
    int n = 0;
    const char *p = "roms/";
    while (*p) path[n++] = *p++;
    for (p = game_romset_name; *p && n < (int)sizeof path - 1; p++) path[n++] = *p;
    path[n] = 0;

    uvm2_romzip_bytes = uvm2_sd_read(path, s_zip, UVM2_ROMZIP_MAX);
    uvm2_romzip_error = uvm2_sd_error;
    if (!uvm2_romzip_bytes) return;

    s_desc[1] = (uintptr_t)s_zip;
    s_desc[2] = uvm2_romzip_bytes;
    s_desc[0] = 0x315A4D52u;                   /* 'RMZ1' — written last */
}
