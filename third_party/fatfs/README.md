# FatFs R0.16

ChaN's generic FAT/exFAT file system module, from
<https://elm-chan.org/fsw/ff/>. It is the file system under
`sdk/uvm2-sdk/uvm2_sd.c`, which supplies the disk layer (`disk_read`,
`disk_write`, …) over the cartridge's bit-banged SPI.

* Archive: `ff16.zip`, SHA-256
  `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`
  (fetched 2026-09-28).
* Files kept: `source/{ff.c, ff.h, ffunicode.c, diskio.h, ffconf.h,
  00readme.txt, 00history.txt}` and `LICENSE.txt`. The upstream `diskio.c` and
  `ffsystem.c` are templates and are not used.
* License: BSD-style, one clause (see `LICENSE.txt`).

## Local changes

`ff.c`, `ff.h`, `ffunicode.c` and `diskio.h` are **unmodified**. Only
`ffconf.h` differs from upstream:

| option | upstream | here | why |
|---|---|---|---|
| `FF_FS_EXFAT` | 0 | 1 | Large cards come formatted exFAT. This is the reason FatFs is here. |
| `FF_USE_LFN` | 0 | 1 | exFAT requires it. Static buffer, which is fine: only core 0 touches the card. |
| `FF_CODE_PAGE` | 932 | 437 | Paths are ASCII; 437 keeps the table small. |
| `FF_LBA64` | 0 | 1 | GPT-partitioned cards (some tools format large cards that way). |
| `FF_FS_TINY` | 0 | 1 | Files share the volume's sector buffer: 512 bytes less SRAM per open file. |
| `FF_FS_NORTC` | 0 | 1 | No real-time clock on the cartridge; files get a fixed date. |
| `FF_NORTC_YEAR` | 2025 | 2026 | That fixed date. |

Cost on `game/tacscan`, measured 2026-09-28 against the hand-written FAT16/32
code it replaced: +16 KB of code and read-only data, +1.1 KB of `.bss`.

To upgrade, drop in the new `source/` files, re-apply the table above to the new
`ffconf.h`, rebuild, and run `sdk/uvm2-sdk/tools/uvm2_sd_test.sh`.
