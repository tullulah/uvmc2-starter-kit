# 12 — Calibrating a console

The beam is analog. Every Vectrex has its own DAC offset, its own sample-and-hold
capacitors, its own analog switches and its own integrators, and they drift with
age and temperature. A game built with this SDK draws with **one** set of beam
constants, and those constants belong to the **console**, not to the game. That
set is the calibration.

**The compiled defaults were measured on one console.** On a console whose analog
parts differ, the same image can draw text that leans into diagonals, columns that
cascade to one side, or glyphs that fall apart, while native cartridges look fine
on the same machine. That is not a broken image. It is an uncalibrated console.

---

## Where the calibration lives

`config/uvm2.cfg` on the SD card. Every game built with this SDK reads it at start
up, before the game's own `main` runs. Calibrate once from any game and every game
on that console uses it.

It is plain text, one field per line, so it can be read and edited on a PC:

```
scale 140
t1_tail_q8 640
zero 25
bright 84
hold_y_min 4
hold_y_max 15
neg_rate_x 0
neg_rate_y 0
drift_x 0
drift_y 0
```

A missing field keeps its compiled default. With no file at all, the whole
calibration is the compiled defaults. Nothing on screen tells you which case you
are in; `uvm2_have_calibration` does (0 = no file was found).

| field | default | unit | in the wizard | what it is |
|---|---|---|---|---|
| `zero` | 35 (`0x23`) | DAC value | **ZERO** | The value primed into the zero reference, which is the beam's origin. See [the zero](#the-zero-the-one-that-breaks-text) below. |
| `bright` | 127 | 0..127 | **BRIGHT** | The starting intensity. A game that sets its own intensity overrides it. |
| `scale` | 160 | divisor | **SCALE** | `DRAW_SCALE`. **Larger = shorter strokes.** |
| `t1_tail_q8` | 640 | 1/256 of a T1 count | **TAIL** | How far the ramp keeps going after T1 expires. 640 = 2.5 E cycles, measured 2026-09-15. |
| `hold_y_min`, `hold_y_max` | 4, 15 | E cycles | no | Y sample-and-hold window for the swept-text path and the re-zero. |
| `neg_rate_x`, `neg_rate_y` | 0 | 1/256 | no | Trim for negative DAC rates: the DAC does not deviate the same at +k as at −k. |
| `drift_x`, `drift_y` | 0 | 1/256 per jump | no | Per-jump drift compensation. Off by default: only the magnitude was ever measured. |

The fields the wizard does not show can still be edited in the file. The wizard
saves every field, including those, so hand-edited values survive a save.

Settings that belong to a **game** rather than the console (refresh, start-up
menu, rotation, audio output) live in `config/<GAME>.CFG`, and only for games that
declare them (`uvm2_config_game`, see [08](08-api-reference.md)).

---

## Opening the wizard

**Hold buttons 2 and 3, and launch the game with button 4 as usual.** The wizard
opens before the game.

It is 2+3 because in the cartridge's own menu button 4 launches the game and
button 1 goes back, so any combination with button 1 never reaches the game. The
menu uses neither 2 nor 3.

This works in every game built with this SDK, C and VPy alike. A game built
before 2026-09-28 does not have it; rebuild it. Some games also open the wizard
from their own menu (`uvm2_config_wizard()`), and those show their own settings as
well.

If the combination seems to do nothing, read `uvm2_boot_combo` over SWD
([07](07-measuring.md)):

| value | meaning |
|---|---|
| −1 | never checked: the image predates the feature, or it is a bench without core 1 |
| 0 | checked, 2+3 were not held |
| 1 | held, the wizard ran |
| 2 | could not check: core 1 never refreshed the controller |

### Controls

* **Up / down** chooses a field.
* **Left / right** changes it, continuously while held.
* **Button 4** saves to `config/uvm2.cfg` and starts the game. Release it and
  press it again: the press that launched the game does not count.

Saving creates `config/` and the file if they do not exist. If the save fails,
`uvm2_sd_error` says why ([06](06-sound-input-sd.md)).

---

## Reading the screen

### With ZERO selected: lines of text

A small high-score table, drawn **the way a game draws its text**: three calls per
stroke and no re-zero between glyphs. The SDK's own `uvm2_print_text` re-centres
the beam before every glyph, which would hide exactly this error, so the pattern
uses `uvm2_print_text_chained`. A long line above the rows and another to their
left are the reference: each is one ramp, so they barely carry the error.

**Adjust ZERO until every row runs parallel to the top line and every column
stands parallel to the left one.** A wrong zero tilts the rows into diagonals and
garbles the glyphs.

### Any other field: the wheel and the two squares

**The wheel**, an octagon and its eight spokes, has one right answer anybody can
see:

| what you see | what it means |
|---|---|
| the octagon closes | long strokes arrive where they were aimed |
| the spokes meet in **one** point | every jump back to the centre lands there |
| each spoke ends on its corner | jumps and strokes agree about distance |
| opposite spokes make straight lines at 45° | X and Y move alike, positive and negative |

**The two squares** are the same size: the top one is 4 long strokes, the bottom one
is 40 short ones. Because they measure the same, any difference between them comes
from the **number** of strokes, not their length:

| what you see | adjust |
|---|---|
| the 40-stroke square opens, the 4-stroke one does not | **TAIL**: the fixed error per stroke |
| both shrink or grow together | **SCALE** |
| both fine, but the whole picture is too small or too large | **SCALE** |

The dots along the bottom square are its 40 joints: a stroke's ends are brighter
than its middle. They are not a fault.

### A good order

1. **ZERO** first, on the text. It moves everything else.
2. **TAIL**, until the 40-stroke square closes like the 4-stroke one.
3. **SCALE**, for size.
4. **BRIGHT** last, to taste.

---

## The zero: the one that breaks text

The zero reference is one of the analog multiplexer's channels, and every ramp
measures its velocity from it (`dx = xsh − rsh`, `dy = rsh − ysh` in the beam
model). If the value held there is not this console's true zero, **every ramp
carries a constant extra velocity**. A long stroke barely shows it, because it is
one ramp. A row of text is dozens of short ramps with no re-zero in between, so the
error accumulates along the row: the row leans into a diagonal, and the next row
starts from a different place.

That is exactly what a tester's console showed with the AAE ports in September
2026. The photo showed the high-score table with every row tilted and the score
column cascading down and to the side, while native cartridges looked fine.

What we know about the right value:

* The SDK's default, `0x23`, comes from **Vectorblade**, an open-source Vectrex
  game that ships it as the *factory* value of a calibration the player adjusts
  per console. It was never meant to be universal.
* Bus captures of the **VecFever** cartridge (Star Wars, Empire Strikes Back,
  Vector Kong and Major Havoc) show a zero block identical to ours, register for
  register and gap for gap. The one exception is this value: VecFever writes
  `0x07` (and `0x00`/`0x08` in Vector Kong).
* The zero block charges the reference only partially (about 7 cycles towards the
  value, then 4 towards `0xFF`). So the level it actually reaches depends on each
  console's analog parts too. That is why the value is calibrated, not fixed.

To try a value without the wizard, write `zero 7` (or any other) in
`config/uvm2.cfg`. The wizard is the better tool, because the text shows the
effect live.

---

## What the calibration does not reach

Some console-dependent timings are compile-time only, fixed at values taken from
reference captures:

* the Y sample-and-hold window on ordinary strokes and jumps (about 10–11 E);
* how often the beam is re-zeroed (`uvm2_zero_jump`, `uvm2_zero_every`);
* blank and settle times.

Their provenance notes are in `uvm2_draw.c` and `vectrex-draw/src/ramp.rs`. If a
console still draws badly after the wizard, those are the next suspects. Bring a
photo and, if possible, an SWD dump of `uvm2_stats`.
