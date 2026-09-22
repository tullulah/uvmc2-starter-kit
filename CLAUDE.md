# Working in this repository

This is a self-contained kit for writing Vectrex games that run on a **UVMC2**
(an RP2350 cartridge). It builds with no dependency on anything outside this
directory except the toolchain and a pinned pico-sdk that `./setup.sh` clones.

`README.md` covers setup and building. For the code itself, do **not** start by
reading the tree — these four pages exist so you do not have to:

| you want to… | read |
|---|---|
| find the file that answers a question | **`docs/09-source-map.md`** — an index from question to file |
| know what a game may call | **`docs/08-api-reference.md`** — the complete surface, all three layers |
| port an arcade game | **`docs/11-porting-from-mame.md`** (which route?) then **`docs/10-porting-an-aae-game.md`** (how) |
| understand why something is the way it is | `docs/00-index.md` → 01–07, in order |

The SDK's own comments are the primary record, and several files are worth
reading in full before touching them: `uvm2_bus.h`, `uvm2_smp.h`,
`bus_stream.pio`, `ramp.rs` (the constants block), `uvm2_config.h`, `uvm2.mk`.

---

## Orientation

```
examples/hello_uvmc2/   the smallest complete game. Copy this to start a new one.
game/tacscan/           the full worked example: an arcade game emulated in C.
sdk/uvm2-sdk/           the runtime that lives inside the .um2 image.
sdk/rp2350-sdk/         the game-facing API (v_directDraw32 &c.) + linker scripts.
sdk/vectrex-draw/       the beam model (Rust, no_std).
sdk/vectrex-bus/        the PIO + DMA bus stream (Rust, no_std).
sdk/vpy-c/              libvpy: shapes, sprites, text, input, sound.
third_party/aae/        the arcade emulator sources game/tacscan is built on.
```

Build and verify:

```sh
./setup.sh --check                       # tools and their VERSIONS, before anything else
cd examples/hello_uvmc2 && make uvm2     # fast; use this to check the SDK still links
cd game/tacscan         && make uvm2     # slower; exercises much more of the kit
```

**Rust is a hard requirement, not an option.** The beam model
(`sdk/vectrex-draw`) and the bus stream (`sdk/vectrex-bus`) are `no_std` crates
and `uvm2_pico.cmake` links them in every build — `UVM2_PIO_STREAM=0` only drops
the `bus` feature. Minimum cargo 1.78 (the lockfiles are format v4); the kit is
verified on 1.91.1. Full prerequisites, with versions, are in `README.md` §1.

A full clean build takes a couple of minutes. **Rebuild after every change to the
SDK** — several of the invariants below are enforced by `#error`, and the build
is the cheapest place to find out.

---

## House rules

**Language.** Every file in this kit — code, comments, docs, commit messages — is
in **English (US)**. There are no exceptions.

**Comments carry provenance.** Where a constant has a note saying it was measured
on hardware on a given date, that is load-bearing. It means the number was *not*
derived, and changing it needs another measurement, not another argument. Do not
delete those notes to tidy up, and do not "simplify" a constant whose comment
explains why it is that exact value.

**No magic numbers.** A new constant arrives with a named fact behind it, or with
the command that re-measures it.

**Failures must be loud.** A lot of this code exists specifically so that two
different faults cannot look the same:

* `uvm2_stats.dropped` — the command list overflowed. **If this is non-zero,
  nothing on screen is evidence of anything.**
* `uvm2_stats.recals` — if it does not climb, recalibration is not *running*,
  which is a different investigation from "it does not work".
* `uvm2_sd_error` — tells "no card" from "did not start" from "file missing".
  There is no card-detect pin; an earlier version read an unowned pad that always
  returned "no card", i.e. a diagnostic that could not fail.
* The `#error` in `uvm2_bus.h` — see below.

When you add a mechanism that can fail quietly, add the counter that proves it
ran. A branch that fires silently has already cost sessions here.

---

## Invariants — do not "fix" these

**1. Dual core + PIO + DMA is the baseline, not an option.**
`uvm2_bus.h` refuses to compile without `UVM2_DUAL_CORE` and `UVM2_PIO_STREAM`.
This is an `#error` and not a default because a default was silently stepped on:
three games under daily test spent weeks compiled with neither, and the build
said nothing. `UVM2_BENCH_NO_CORE1` is the escape hatch and it is for benches
that measure the executor in isolation. A game that declares it is lying.

**2. Change the bus address during E LOW, hold it through E HIGH.**
This is the whole timing rule. A setup-time violation degrades; a phase violation
fails outright — black screen. The `nop [14]` in `bus_stream.pio` is the
calibration that puts the presentation where the known-good path puts it, and its
value depends on the instruction count of the loop around it. **Add an
instruction to that loop and you must re-derive the number.**

**3. Core 1 owns the bus, exclusively.**
Two cores driving the same GPIO is two unarbitrated writers on the VIA. Route by
*which core is executing* (`UVM2_CPUID`), never by which function was called —
that way it stays correct when someone adds a call site.

**4. The drawing path has no knobs.**
Three calls per stroke: intensity, absolute jump, delta, all in 1/16 of a unit.
Collinear merging, Douglas-Peucker, stroke reordering, an intensity cache and
clipping all existed once and every one was eventually measured as a shimmer, a
displaced stroke or missing geometry. What the game asks for is what gets drawn.

**5. `/RAMP` lives inside the Port B constants.**
Port A *is* the beam's DAC. Any Port B byte written while Port A carries
something else must have bit 7 set, or the integrators run free with garbage and
draw a bright segment from the origin. It is in the `#define` so no call site can
forget it.

**6. Verify PSRAM through the uncached alias (`0x15000000`).**
The XIP cache is 16 KB. A 64 KB buffer verifies fine when freshly written and
reads back wrong later. Both observations fit, and together they hide a broken
chip.

**7. Reads happen between frames.**
A read needs the data bus turned around mid-cycle, so it cannot be recorded into
the command list, and doing it mid-frame disturbs the integrators. Measured: with
the input read removed entirely, stray bright vectors dropped from 4-5 per frame
to 1.

---

## Measuring

There is no console on a cartridge, so measurement discipline substitutes for it.
`docs/07-measuring.md` has the tools. The rules that cost the most to learn:

* **Check the artefact's mtime before you check the code.** Four identical
  results in a row usually means nothing rebuilt.
* **Confirm the flag reached the compiler** (`flags.make` in the CMake build dir)
  before concluding the flag does nothing.
* **The instrument perturbs the measurement.** A live SWD panel reading state
  pulled one game to 11 fps.
* **One game is not evidence**, and **one console is not evidence**. Global
  render constants swept on a single game are overfitted however clean the curve;
  artefacts once blamed on the drawing turned out to be one worn-out console.
* **Check beam changes on short, dense vectors.** Long strokes cannot fail, so
  they prove nothing.
* **A photograph of a CRT gives you geometry, not brightness.**

---

## Things that are not here

* The cartridge's stock firmware. The `.um2` runs *instead of* it; this kit never
  modifies it.
* The private repository this kit was cut from. Some comments refer to "another
  cartridge" or "a reference capture" — those are real, and deliberately not
  named or included.
* A simulator. `sdk/pitrex-sim/` is the host-side *contract*, which is what makes
  a game buildable for a desktop harness; the harness itself is per game
  (`game/tacscan/tools/host_test.c` is an example).

## Known state

`cargo test` in `sdk/vectrex-draw` has one failing test,
`emit::pentagon::ramp_error_has_no_bias` (mean error per stroke −1.6403). It
fails identically in the crate this kit was cut from, so it is pre-existing. Do
not "fix" it by adjusting a beam constant — that constant was measured.
