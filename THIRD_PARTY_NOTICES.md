# Third-party notices

The kit has two licenses, by directory:

| part | license |
|---|---|
| everything except the two directories below: the docs, the examples, `setup.sh`, the tools | **MIT**, © 2026 Daniel Ferrer: [LICENSE](LICENSE) |
| `third_party/aae/` and `game/tacscan/` | **GNU GPL v3.0**: [third_party/aae/COPYING](third_party/aae/COPYING), [game/tacscan/COPYING](game/tacscan/COPYING) |

Tac/Scan is GPL because it is built on AAE, which is GPL-3.0. Files in those two
directories written solely by Daniel Ferrer are also available under MIT.

**Two things in them are not covered by the GPL or by any other license here:**

* the older MAME-derived code and Neil Bradley's MZ80, which keep their own,
  more restrictive terms (below);
* Sega's ROMs and sound samples.

So the Tac/Scan example as it stands is for study, not for redistribution.

## `sdk/`: the UVMC2 SDK

A git submodule ([uvmc2-sdk](https://github.com/tullulah/uvmc2-sdk)), MIT, with
its own [`THIRD_PARTY_NOTICES.md`](sdk/THIRD_PARTY_NOTICES.md) (FatFs, pico-sdk
files, the PiTrex vector font).

## `third_party/aae/`: Another Arcade Emulator (AAE)

AAE is by **Tim Cottrill** (<https://github.com/tcottrill/AAE>). AAE's own code is
licensed under the **GNU General Public License v3.0**. Upstream's README says the
GPL "does not cover the MAME-derived portions of the code, which remain the
copyright of the MAME team and the original authors". This copy came from
**Malban**'s PiTrex port of AAE
([pitrex-baremetal/aae](https://github.com/malbanGit/pitrex-baremetal)), which
replaced the assembler CPU cores with C ones. We modified it further.

| files | origin | copyright | license |
|---|---|---|---|
| `aae_access_count.h`, `aae_fast.h`, `aae_memdispatch.h`, `aae_perf.h`, `aae_romload.c/.h`, `aae_romload_gi.c`, `romzip.c/.h` | written for this kit | Daniel Ferrer | GPL-3.0 with the rest of the directory, and also MIT |
| `acommon.c`, `cpu_control.c/.h`, `globals.h`, `dips.h`, `loaders.h`, `log.h`, `samples.h`, `vector.h`, `rand.c/.h` | AAE via the PiTrex port, modified | Tim Cottrill, Malban | GPL-3.0, with MAME-derived parts |
| `cpuintrf.c/.h`, `mame.h`, `mamecommon.h`, `driver.h`, `osd_cpu.h`, `osdepend.h`, `input.h`, `m6809/`, `z80/z80.h`, `musashi/softfloat/mamesf.h` | early MAME (0.29–0.90) via AAE | the MAME team and the original authors (6809 core: John Butler) | the MAME license of that era (**non-commercial**) |
| `SegaG80.c/.h`, `SegaG80snd.c`, `segacrypt.c` | AAE's Sega G80 driver, from MAME's `segag80` driver | Tim Cottrill, the MAME team | GPL-3.0 / MAME license |
| `mz80/mz80.c`, `mz80/mz80.h` | **MZ80** Z80 emulator, **modified** | © 1996–2000 Neil Bradley | MZ80's own terms: no sale, and *"modified versions of MZ80 may not be publicly redistributed without author approval"*. Credit: "Multi-Z80 CPU emulator by Neil Bradley". |
| `m6502/m6502.h` | **M6502** emulator header | © 1996–1998 Neil Bradley | same terms as MZ80. Credit: "Multi-6502 CPU emulator by Neil Bradley". |
| `musashi/m68k*.h` | **Musashi** 3.32 | © Karl Stenerud | MIT (in the headers) |
| `musashi/softfloat/` (except `mamesf.h`) | **SoftFloat** Release 2b | John R. Hauser | SoftFloat license (the notice is in the headers, which must keep it) |
| `pokey.h` | **PokeySound** | © 1997 Ron Fries | GNU LGPL v2 |
| `puff.c/.h` | **puff** 2.3 | © 2002–2013 Mark Adler | zlib license |
| `junzip.c/.h` | **JUnzip**, modified | Joonas Pihlajamaa | public domain |
| `mathbox.h` | Atari mathbox | © 1991–1996 Eric Smith | no license terms given |

## `game/tacscan/`: Tac/Scan data

| files | copyright | status |
|---|---|---|
| `roms/` (`tacscan.zip` and the loose PROMs) | **Sega** (*Tac/Scan*, 1982) | copyrighted game data. **Not licensed** by this repository. |
| `samples/*.wav` | recordings of Sega's game audio, MAME-style sample set | copyrighted. **Not licensed** by this repository. |

The game's C code under `game/tacscan/src/` and its tools are ours (GPL-3.0 as
part of the port, and also MIT). The game, its name and its data belong to Sega.

## Fetched by `setup.sh` or the build, not included

| component | license |
|---|---|
| [Raspberry Pi pico-sdk](https://github.com/raspberrypi/pico-sdk) 2.2.0, into `third_party/pico-sdk` | BSD-3-Clause. Every `.um2` links it. |
| Rust crates used by the SDK (`pio`, `pio-proc` and dependencies) | permissive (MIT / Apache-2.0 and similar); see the SDK's notices |

## Credits

* **Ralf & Jason**, for the Ultimate Vectrex Multicart 2 and its firmware.
* **Malban**, for Vectorblade, VIDE and the PiTrex, and for the calibration values
  this kit starts from.
* **Tim Cottrill** (AAE), the **MAME team**, and everyone who documented the
  Vectrex.

## Trademarks

Vectrex, Sega and Tac/Scan are trademarks of their respective owners. This
project is not affiliated with or endorsed by any of them.
