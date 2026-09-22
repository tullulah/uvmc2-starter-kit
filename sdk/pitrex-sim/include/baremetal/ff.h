/*
 * Minimal FatFs shim for the PiTrex host simulator.
 *
 * The real baremetal ff.h + libbaremetal implement FatFs against the SD card.
 * Under WASM there is no SD card, so this shim exposes just the FatFs surface a
 * pitrex game uses and backs it with stdio over emscripten's MEMFS (files are
 * preloaded into the module). Part of the "PiTrex platform contract" the sim
 * implements once for any game — see sdk_host.c.
 */
#ifndef PITREX_SIM_FF_H
#define PITREX_SIM_FF_H

#include <stdint.h>

typedef unsigned int  UINT;
typedef unsigned char BYTE;
typedef uint64_t      FSIZE_t;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_NO_FILE,
    FR_DENIED
} FRESULT;

/* Opaque file handle — the shim stashes a host FILE* in `fp`. */
typedef struct { void* fp; } FIL;

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_ALWAYS 0x08

FRESULT f_open (FIL* fp, const char* path, BYTE mode);
FRESULT f_close(FIL* fp);
FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
FSIZE_t f_tell (FIL* fp);
FSIZE_t f_size (FIL* fp);
int     f_eof  (FIL* fp);

#endif /* PITREX_SIM_FF_H */
