# 11 — Porting from MAME

MAME is the reference for what an arcade machine *did*. It is not a library you
can link, and its drivers are C++ device objects wired to a scheduler you cannot
bring along. So a port is never "compile the MAME driver": it is **using the MAME
driver as the specification** and rebuilding it against this kit.

There are three routes. Choosing wrong is the expensive mistake, so start here.

---

## 0. Choose the route

| | route A — emulate via AAE | route B — translate the driver | route C — write it fresh |
|---|---|---|---|
| what runs | the original ROM, on an emulated CPU | your C, reproducing the game's logic | your C, your game |
| fidelity | exact | as good as your reading of the driver | none — it is a new game |
| effort | days, if AAE has the driver | weeks | days |
| needs the romset | yes, on the SD card | usually yes (art, tables, levels) | no |
| CPU budget | the whole emulated machine, per frame | almost nothing | nothing |
| best for | **vector** games AAE already covers | games whose *logic* is simple and whose *data* is the value | anything |

**Route A is the default for a vector arcade game.** It is what
`game/tacscan/` is, and [10](10-porting-an-aae-game.md) covers it end to end. The
rest of this page is about the cases route A does not reach.

---

## 1. Read the MAME driver first

Whatever route you take, the driver file (`src/mame/drivers/<name>.cpp` plus its
`video/` and `audio/` siblings) is the specification. Six things to extract:

| in MAME | what it tells you | where it lands here |
|---|---|---|
| `ROM_START(...)` / `ROM_LOAD` | which chip image goes to which address | the `aae_rom_op` table ([10 §4](10-porting-an-aae-game.md)) |
| the address maps (`map(0x0000, 0x7fff).rom()` …) | the memory layout and which ranges are I/O | the handler table (route A: the driver's own; route B: yours) |
| `MCFG_CPU_ADD` / `maincpu(...)` and its clock | which CPU, at what frequency | `driver[].cpu_type` / `cpu_freq` |
| the interrupt wiring (`set_vblank_int`, `set_periodic_int`) | how often, and IRQ vs NMI | `cpu_int_type`, `cpu_intpass_per_frame` |
| `set_refresh_hz(...)` or the crystal derivation | **the game's speed** | `driver[].fps` + `uvm2_set_refresh()` |
| the input ports (`PORT_START` / `PORT_BIT`) | bit layout and polarity per port | `getport()` ([10 §5](10-porting-an-aae-game.md)) |

Take the refresh rate from the **derivation**, not the rounded comment. Tac/Scan
is 15468480 ÷ 3 ÷ 0x1f788 = 40.00 Hz exactly, and on these machines the refresh
rate *is* the game speed — at 42.8 fps it played 7% fast.

Take the input polarity from the **port definitions**, not from what looks
natural. Active-low with a non-zero idle value is common, it varies per port
within one machine, and getting it wrong makes the controls silently do nothing.

---

## 2. Is it a vector game?

This is the question that decides whether a port is a port or a project.

**Vector hardware** — Atari's AVG/DVG, Cinematronics' CCPU, Sega's G80, Vectorbeam
— emits line segments. That is the same shape as what the Vectrex draws, and the
whole kit is built around it. MAME calls these `VECTOR` in their machine config
and they have a `vector_device` in the driver.

**Raster hardware** has a framebuffer, tiles and sprites. The Vectrex has none of
those. Converting one is a *content* problem, not a code problem: something has
to turn each frame's tile and sprite state into line art, and do it inside the
frame budget. That is a pipeline someone builds per game, and it is **not in this
kit**. Do not start a raster port expecting the SDK to help; the SDK draws the
lines you give it.

The closest thing the SDK offers is `v_directGapped` / `uvm2_draw_delta_patterned`
— one ramp with up to 16 holes toggled along it, so a row of pixels costs one
ramp instead of one stroke per lit run. It is a real mechanism and it is used for
sweeping text. It was also measured against stroke text and did not win on the
scene it was built for, so treat it as a tool, not as a raster answer.

---

## 3. Route A — through AAE

Covered in [10](10-porting-an-aae-game.md). The only MAME-specific notes:

* **AAE's drivers are already MAME ports**, usually of an older MAME. Where AAE
  and current MAME disagree, MAME is almost always right and AAE is the thing you
  have. Read both.
* **The ROM table is a transcription of `ROM_START`**, with three shapes:

  | MAME | here |
  |---|---|
  | `ROM_LOAD("f", addr, len, CRC)` | `{ "f", addr, len, 0, region, AAE_ROM_PLAIN }` |
  | `ROM_LOAD16_BYTE` (even / odd) | `AAE_ROM_EVEN` / `AAE_ROM_ODD` |
  | `ROM_CONTINUE(addr, len)` | a second row with `src_off` set |
  | `ROM_RELOAD(addr, len)` | a row with `file == NULL` |
  | `ROM_REGION(len, "tag", ...)` | one `GI[]` array, sized to what is actually read |

  The member **names** are the zip's, not MAME's labels — check the zip.
* **`ROM_REGION` sizes are an upper bound, not a requirement.** AAE allocated
  64 KB for a region whose highest read is 1022; sizing it to `0x400` is part of
  what makes a game fit 496 KB. Find the real high-water mark with
  `-DAAE_ACCESS_COUNT` (host-only) or by reading the driver.
* **Cores other than Z80 are not in this kit** — `m6502/`, `m6809/` and
  `musashi/` are headers only. Bring the `.c` in from AAE.

---

## 4. Route B — translate the driver

When the game's *logic* is simple and its *data* is what matters — level layouts,
sprite shapes, tables — emulating a whole CPU to run it is paying a lot for
little. Translating means writing the game in C and keeping the original's data.

It is more work and it is not exact. Do it when route A does not fit the budget,
or when the machine is not a vector machine and you are going to redraw the art
anyway.

### What the kit gives you

* **The draw path, unchanged.** Nothing in `sdk/` knows about AAE. Call
  `v_directDraw32` (or libvpy) and you are done.
* **`aae_rom_load_bases(ops, n, bases)`** — the romset loader against *your own*
  arrays rather than AAE's `GI[]`. It exists precisely for this: it is in a
  separate file from the AAE wrapper so a hand-written port does not have to drag
  `globals.h` in. You still declare `game_romset_name[]`, and the zip still gets
  its CRC checked.
* **`third_party/aae/aae_memdispatch.h`** — per-page read/write dispatch with a
  fast path for plain memory, if you end up emulating *part* of a machine.
* **The failure screen.** `aae_text(s, y, scale)` draws one centred line in the
  loader's tiny vector font, for anything you need to say when there is no
  console.

### Method

1. **Get the game running in MAME with a debugger**, and use it as the ground
   truth. Its trace, its memory watchpoints and its tile/sprite state are the
   only way to know what "correct" is.
2. **Find the data.** Level tables, object tables, sprite shapes, text strings.
   Extract them from the romset, with MAME telling you the addresses.
3. **Write the logic against the data**, one screen at a time, comparing against
   MAME as you go.
4. **Verify by comparison, not by eye.** Hash what you draw, or dump a frame and
   diff it — the host harness pattern in `game/tacscan/tools/host_test.c` is the
   shape: a fake SDK, an FNV hash over every coordinate, and a run count.

### The trap that this route keeps hitting

Translating a switch statement, a jump table or a dispatch chain **drops cases
silently**. One port shipped with a `case` missing its `break`, which made an
opcode a no-op and froze the scoreboard; it affected every port that shared the
file and was only found by diffing a PC trace against the original. If you
translate a dispatch, verify it by trace, not by reading it again.

---

## 5. Route C — write the game

`examples/hello_uvmc2/` plus libvpy. Nothing from MAME is involved, and the
budget stops being a question. Worth naming as an option because "port X" often
turns out to mean "I want a game that feels like X on a Vectrex", and that is
enormously cheaper.

---

## 6. Whichever route: the budget

A 50 Hz frame is **30 000 bus cycles** and no amount of CPU changes that. Within
it:

* a lit stroke costs ~13 commands, **flat in its length**;
* a blanked jump costs ~5 more;
* the emulated CPU (route A) and the list building both have to finish inside the
  frame, though dual core overlaps them with the beam.

So the first estimate for any port is: **how many strokes does one frame of this
game draw?** MAME will tell you — a vector driver's display list is right there.
Multiply by ~18 and compare against 30 000. Then measure properly with
`tools/uvm2_list_count.c` ([07](07-measuring.md)).

---

## 7. ROMs

The kit ships one romset because it is the worked example's data and the build
derives its buffer size from it. Any other game's romset is the porter's to
obtain and to be entitled to. Nothing in the SDK embeds a ROM: the model is
deliberately MAME's — the image reads `roms/<game>.zip` off the SD card at
startup, checks its CRC, and says so on screen if it is missing.
