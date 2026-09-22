/* Generated from the romset by the romtable generator -- do NOT edit by hand.
 * Romset: tacscan.zip  (the ROM_START(tacscan) table in AAE's gameroms.h)
 * The names are the REAL ones from the zip, already matched up here: the
 * cartridge does not guess. */
#ifndef TACSCAN_ROMTABLE_H
#define TACSCAN_ROMTABLE_H

#include "aae_romload.h"

/* The size of each region, from ROM_REGION in gameroms.h. Games that used to
 * point GI[] straight at the embedded array have to reserve their buffer here:
 * the ROM no longer travels inside, but the region still exists. */
#define ROM_REGION0_SIZE 0x10000
#define ROM_REGION1_SIZE 0x10000

/* WHAT THE ROMSET IS CALLED. The launcher used to look for the zip by the
 * .BIN's name, and that is only right by accident: aae_asteroids_sd.bin needs
 * asteroid.zip. It reads it from here, through the header's RSET block. */
const char game_romset_name[] = "tacscan.zip";

static const aae_rom_op ROM_OPS[] = {
    { "1711a.cpu-u25"             , 0x0000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1670c.prom-u1"             , 0x0800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1671a.prom-u2"             , 0x1000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1672a.prom-u3"             , 0x1800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1673a.prom-u4"             , 0x2000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1674a.prom-u5"             , 0x2800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1675a.prom-u6"             , 0x3000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1676a.prom-u7"             , 0x3800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1677a.prom-u8"             , 0x4000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1678b.prom-u9"             , 0x4800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1679a.prom-u10"            , 0x5000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1680a.prom-u11"            , 0x5800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1681a.prom-u12"            , 0x6000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1682a.prom-u13"            , 0x6800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1683a.prom-u14"            , 0x7000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1684a.prom-u15"            , 0x7800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1685a.prom-u16"            , 0x8000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1686a.prom-u17"            , 0x8800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1687a.prom-u18"            , 0x9000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1688a.prom-u19"            , 0x9800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1709a.prom-u20"            , 0xA000, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "1710a.prom-u21"            , 0xA800, 0x0800, 0x0000, 0, AAE_ROM_PLAIN },
    { "s-c.xyt-u39"               , 0x0000, 0x0400, 0x0000, 1, AAE_ROM_PLAIN },
};

#define ROM_OPS_N ((int)(sizeof ROM_OPS / sizeof ROM_OPS[0]))

#endif /* TACSCAN_ROMTABLE_H */
