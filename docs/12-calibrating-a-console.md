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

### Why three fields are not on the screen

* **`hold_y_min/max` reach less than their name suggests.** They set the Y
  sample-and-hold window only for the swept-text path (`uvm2_draw_sweep_sr`) and
  the re-zero move. Ordinary strokes and jumps, which is almost everything a game
  draws, use their own fixed windows in the beam model, so moving these changes
  nothing in most games. They matter for a game that draws its text with the
  shift register.
* **`neg_rate_x/y` are real, and the wheel shows them.** The DAC does not deviate
  the same at +k as at −k. On a star or on the wheel, the arms where the two axes
  ask for opposite signs (north-west and south-east) do not line up while all the
  others do. Neither ZERO (it moves both axes together) nor SCALE (it is
  symmetric) can fix that. They are the most likely to be worth adding to the
  screen.
* **`drift_x/y` are off on purpose.** The per-jump drift was measured once
  (X −16/256, Y −112/256), but its *model* was never settled: the bench could not
  tell whether the correction should follow the sign of each jump or always push
  the same way. Applied with the wrong model, it made one game worse (platforms
  moved, and a shimmer appeared). Until that is measured, a setting that can only
  be guessed at does not belong on a screen meant for people who are not
  measuring. Most of what it would fix is what ZERO fixes.

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

**On the debug cartridge** the BIOS's own menu opens it: **button 3 — calibration,
button 4 — start**, written under the list. Its BIOS loads `config/uvm2.cfg` at
power-on and applies it to the menu and to every game. It can only *overwrite* that
file, not create it, so the card needs `config/uvm2.cfg` (512 bytes; 511 spaces and
a newline will do) before the first save — without it the menu says `NOT SAVED`
and the setting lasts until power-off. Before 2026-10-02 that BIOS never read the
file at all: every game drew with the compiled-in zero whatever the card said.

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
* **Button 1** cycles the figure — AUTO, TEXT, WHEEL, RINGS — without leaving the
  field you are on, so one value can be judged on more than one figure. It is also
  the FIGURE field, which is not saved: what you calibrate against is a choice made
  at the screen, not a property of the console.
* **Button 4** saves to `config/uvm2.cfg` and starts the game. Release it and
  press it again: the press that launched the game does not count.

Saving creates `config/` and the file if they do not exist. If the save fails,
`uvm2_sd_error` says why ([06](06-sound-input-sd.md)).

---

## Reading the screen

**FIGURE decides what is drawn.** AUTO is the old behaviour: the text below while
ZERO is selected, the wheel and squares (or the game's own figure) otherwise.
TEXT, WHEEL and RINGS force one, whatever field is selected — because one zero
cannot serve two scales: the text and the rings are far apart in size, and a
setting judged on one of them is judged on a third of the picture.

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

### RINGS: the death star's explosion

Star Wars throws concentric rings out from the centre when the death star blows
up, and on a tester's console the big ones came out skewed and open while the
small one was round. RINGS draws the same burst — one ring born per frame, up to
fifty, each sixteen strokes with the same vertex count — so radius and chord grow
in step:

| what you see | what it means |
|---|---|
| every ring fails by the same amount | the fixed per-stroke term: **TAIL** |
| the failure grows with the radius | a term proportional to length: **SCALE** |
| it appears suddenly past some radius | a limit, not a slope |

It is almost all strokes (the jump between rings is two units), so what comes out
wrong came out of the strokes; the wheel is where jumps are tested. **It overruns
the frame on purpose** — fifty rings is 800 vectors, and so is the real explosion.
The readout under it gives the list's cycles and **FIT/OVER**: a list replayed a
piece at a time and deformed geometry look alike on a television, so read that
before adjusting anything.

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

## The screen's shape: ASPECT, WIN X, WIN Y

Three fields that are not about the beam's accuracy but about the glass. Select
any of them and the screen shows a **square with a circle inside** and a **frame**:

* **ASPECT** (256 = 1:1) stretches x against y. Right when the square is square on
  the glass and the circle round — measure the square with a ruler if the eye is
  unsure. One console measured 1:1 by photograph (2026-10-01); a console whose
  size pots are off is not, and this is its fix.
* **WIN X / WIN Y** (deflection units) are the frame's half width and height.
  Right when the frame just touches the edges of what the tube shows. That
  console showed about ±18000 × ±20500.

vpy3d games take the ASPECT by themselves (unless a game sets its own); the
window only when a game asks for it (`vpy3d_use_console_window`), because a wider
window changes what a game composed in the 15500 square shows. The defaults, 256
and 15500, change nothing. Not yet tried on a tube.

## A diagonal or a dot with the brightness up

Two artefacts that showed on every game and in the debug cartridge's menu, on one
console, once the brightness knob was turned up — and not in Minestorm. Neither is
a calibration fault; both were the cartridge moving the blanked beam slowly enough
to be seen. Fixed in the SDK on 2026-10-02; recorded here so the next one is
recognised.

* **A corner-to-corner diagonal.** The filler that pads each frame to 50 Hz ran
  with the zero clamp released and `/RAMP` held open by Port B, and with the mux on
  the Y channel the Y hold followed Port A, so X and Y swept together. Found by
  dumping the list to the card (`uvm2_dump_list`) and replaying it in `beam_sim.py`,
  which reports a dark, free-running beam. The filler now keeps the clamp on;
  `uvm2_filler_clamp = 0` brings the old one back for a comparison over SWD.
* **A dot that follows the stick.** The digital joystick read probed the comparator
  with the DAC at ±64. Bisected over SWD by switching parts of the read off; the
  axes now come from the analog read alone. See `06-sound-input-sd.md`.

A blanked beam is not invisible: what matters is how long it stays. A fast dark
jump leaves a faint thread; a slow sweep or a parked beam leaves a line or a dot.

## What the calibration does not reach

Some console-dependent timings are compile-time only, fixed at values taken from
reference captures:

* the Y sample-and-hold window on ordinary strokes and jumps (about 10–11 E);
* how often the beam is re-zeroed (`uvm2_zero_jump`, `uvm2_zero_every`);
* blank and settle times.

Their provenance notes are in `uvm2_draw.c` and `vectrex-draw/src/ramp.rs`. If a
console still draws badly after the wizard, those are the next suspects. Bring a
photo and, if possible, an SWD dump of `uvm2_stats`.
