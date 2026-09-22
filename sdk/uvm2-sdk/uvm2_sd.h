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
 * The path accepts ONE subdirectory, which is what roms/<game>.zip needs. */
uint32_t uvm2_sd_read(const char *path, unsigned char *dst, uint32_t max);

/** Creates a ONE-CLUSTER file (max 512 bytes of content) and writes it. If the path carries a
 *  subdirectory and it does not exist, it is created in the ROOT. 1 if it was created. */
int uvm2_sd_create(const char *path, const unsigned char *data, uint32_t n);

/** Overwrites IN PLACE the first sector of a file that ALREADY EXISTS (max 512 bytes). It does
 *  not create, does not resize and does not touch the FAT: it is the only safe thing to do with
 *  a read-only reader. 1 if it was written. See the block in uvm2_sd.c. */
int uvm2_sd_overwrite(const char *path, const unsigned char *data, uint32_t n);

/** Writes a file of ANY size, chaining clusters in the FAT; it creates the folder if missing.
 *  1 if it was written. This is the one for dumping traces, saved games or captures;
 *  `uvm2_sd_create` stays for the one-cluster case and `uvm2_sd_overwrite` for rewriting in
 *  place without touching the FAT. */
int uvm2_sd_write(const char *path, const unsigned char *data, uint32_t n);

/* A CHUNK of the file, starting `from` bytes in. Returns what was copied, which may be less
 * than `max` without that being a failure (unlike uvm2_sd_read, where not fitting IS one). It
 * exists so long captures can be replayed without loading them whole into RAM. */
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from);

/* What the mount understood about the disk, so it can be inspected over SWD without guessing.
 * A MISSING can be an absent file or a misread volume, and from outside they look identical;
 * this separates them. It is read in one go:
 *
 *     tools/probe.sh <&uvm2_sd_diag> 13
 */
struct uvm2_sd_diag {
    uint32_t magic;          /* 'SDDG' = 0x47444453; 0 if it never mounted      */
    uint32_t sec0_b0;        /* byte 0 of sector 0: 0xEB/0xE9 in a VBR          */
    uint32_t sec0_bps;       /* offset 11: bytes per sector                     */
    uint32_t sec0_spc;       /* offset 13: sectors per cluster                  */
    uint32_t via_mbr;        /* 1 = sector 0 was an MBR and we jumped to the partition */
    uint32_t start, spc, is32, root_cluster, fat, root_sector, data, root_entries;
    uint32_t step;           /* 1 = looking for the subdirectory, 2 = the file  */
    uint32_t dir_cluster;    /* the subdirectory's cluster, if it was found     */
    uint32_t entries;        /* directory entries examined in total             */
};
extern struct uvm2_sd_diag uvm2_sd_diag;

/* Last failure, so "no card" and "file missing" do not look the same. */
extern int uvm2_sd_error;
#define UVM2_SD_OK          0
#define UVM2_SD_NO_CARD     1
#define UVM2_SD_NO_INIT     2
#define UVM2_SD_NO_FAT      3
#define UVM2_SD_MISSING     4
#define UVM2_SD_TOO_BIG     5

#endif
