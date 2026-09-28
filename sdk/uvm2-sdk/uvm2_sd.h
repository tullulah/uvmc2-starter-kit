/* uvm2_sd.h — read a file off the UVM2's SD card, from inside the game itself.
 *
 * WHY IT EXISTS. The UVM2's firmware loads the .um2 and steps aside: it does not serve
 * romsets. Our own cartridge does — it reads roms/<game>.zip and publishes a descriptor — and
 * without that the 44 AAE ports paint their "no romset" sign on this board. Embedding the zip
 * in the image works but puts the ROM back into the binary, which is exactly what the move to
 * external ROMs removed.
 *
 * So we read it ourselves. After the reset the module owns the machine, the SD included.
 */
#ifndef UVM2_SD_H
#define UVM2_SD_H

#include <stdint.h>

/* 0 = no card, or it did not start. Non-zero = ready to read. */
int uvm2_sd_init(void);

/* Copies <path> (for example "roms/dkong.zip") into dst. Returns the bytes read, or 0.
 * FAT12/16/32 or exFAT, long names, any depth, case-insensitive (FatFs underneath). */
uint32_t uvm2_sd_read(const char *path, unsigned char *dst, uint32_t max);

/** Creates a 512-byte text file from up to 512 bytes of content, padded with spaces and a
 *  newline so uvm2_sd_overwrite can rewrite it later. Missing folders are created and an
 *  existing file is replaced. 1 if it was created. */
int uvm2_sd_create(const char *path, const unsigned char *data, uint32_t n);

/** Overwrites IN PLACE the first sector of a file that ALREADY EXISTS (max 512 bytes). It does
 *  not create and does not resize: MISSING if the file is not there, TOO_BIG if it is shorter
 *  than 512 bytes. 1 if it was written. */
int uvm2_sd_overwrite(const char *path, const unsigned char *data, uint32_t n);

/** Writes a file of ANY size; missing folders are created and an existing file is replaced.
 *  1 if it was written. This is the one for dumping traces, saved games or captures. */
int uvm2_sd_write(const char *path, const unsigned char *data, uint32_t n);

/* A CHUNK of the file, starting `from` bytes in. Returns what was copied, which may be less
 * than `max` without that being a failure (unlike uvm2_sd_read, where not fitting IS one). It
 * exists so long captures can be replayed without loading them whole into RAM. */
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from);

/* What the mount understood about the disk, so it can be inspected over SWD without guessing.
 * A MISSING can be an absent file or a misread volume, and from outside they look identical;
 * this separates them. It is read in one go:
 *
 *     tools/probe.sh <&uvm2_sd_diag> 12
 */
struct uvm2_sd_diag {
    uint32_t magic;          /* 'SDDG' = 0x47444453; 0 if nothing was ever tried */
    uint32_t step;           /* last call got to: 1 mount, 2 open, 3 data       */
    uint32_t fresult;        /* FatFs FRESULT of the last call (0 = FR_OK)      */
    uint32_t fs_type;        /* 0 none, 1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT       */
    uint32_t volbase;        /* first sector of the volume; 0 = no partition table */
    uint32_t csize;          /* sectors per cluster                             */
    uint32_t n_fatent;       /* clusters + 2                                    */
    uint32_t fatbase, dirbase, database;   /* sectors (dirbase: root cluster on FAT32/exFAT) */
    uint32_t reads, writes;  /* blocks moved since boot: proof the card was touched */
};
extern struct uvm2_sd_diag uvm2_sd_diag;

/* Last failure, so "no card" and "file missing" do not look the same. */
extern int uvm2_sd_error;
#define UVM2_SD_OK          0
#define UVM2_SD_NO_CARD     1
#define UVM2_SD_NO_INIT     2
#define UVM2_SD_NO_FAT      3
#define UVM2_SD_MISSING     4
#define UVM2_SD_TOO_BIG     5   /* also: card or directory full */
#define UVM2_SD_IO_ERROR    6   /* a block failed mid-way; FatFs's reason in uvm2_sd_diag.fresult */

#endif
