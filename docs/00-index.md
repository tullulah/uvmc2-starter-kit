# Documentation

Read them in order the first time. Each one answers a question the next one
assumes you already have an answer to.

**In a hurry?** [09 — Source map](09-source-map.md) is the index from "I want to
know X" to the one file that answers it. If you are about to port an arcade game,
[11](11-porting-from-mame.md) decides *which kind* of port it is and
[10](10-porting-an-aae-game.md) does it.

| | |
|---|---|
| [01 — The hardware](01-the-hardware.md) | What a UVMC2 is, what `.um2` means, and why a game drives the Vectrex's VIA itself |
| [02 — Assembling a project](02-assembling-a-project.md) | Makefile → `uvm2.mk` → CMake → pico-sdk → `.um2`, and how to start a new game |
| [03 — The command list](03-the-command-list.md) | The 24-bit command, the executor, the frame budget |
| [04 — Drawing](04-drawing.md) | The beam model: ramps, subunits, re-zeroing, intensity, and what actually costs time |
| [05 — Dual core, PIO and DMA](05-dual-core-pio-dma.md) | The core split, the PIO program, the DMA ring, and the one timing rule |
| [06 — Sound, input and the SD card](06-sound-input-sd.md) | The PSG, samples injected into the draw list, controllers, FAT16/32 |
| [07 — Measuring](07-measuring.md) | The host tools, what to measure, and the traps that made earlier measurements lie |
| [08 — The API](08-api-reference.md) | Complete reference: libvpy, the host contract, the SDK, the syscalls |
| [09 — Source map](09-source-map.md) | What lives in which file, and an index from *question* to *file* |
| [10 — Porting an AAE game](10-porting-an-aae-game.md) | The eight pieces you write, from the worked example |
| [11 — Porting from MAME](11-porting-from-mame.md) | The three routes, and how to choose between them |

Two conventions worth knowing before you start:

* **Comments carry provenance.** When a constant has a note saying "measured on
  the console 2026-08-19", that is not decoration: it means the number was not
  reasoned out, and changing it needs another measurement, not another argument.
* **Failures are made loud.** Several counters exist only so that "it is broken"
  and "it never ran" cannot look the same (`stats.dropped`, `stats.recals`,
  `uvm2_sd_error`, the `#error` in `uvm2_bus.h`). Respect them; most of them are
  there because something once failed silently for a week.
