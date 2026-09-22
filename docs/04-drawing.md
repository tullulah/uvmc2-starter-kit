# 04 — Drawing

## The three layers

```
your game            vpy_draw_line(x0,y0,x1,y1, brightness)    ±127 logical units
  |                  or v_directDraw32(...)                    ×127, so ~±16000
  v
sdk_rp2350.c         uvm2_draw_intensity(z)
                     uvm2_draw_move_abs_q4(x, y)               1/16 of a device unit
                     uvm2_draw_delta_q4(dx, dy)
  v
uvm2_draw.c          splits, re-zeroes, applies calibration
  |                  calls the beam model for each ramp
  v
vectrex-draw (Rust)  ramp_params(dx, dy) -> (vx, vy, t1)
  v
the command list     PORT_A, PORT_B, T1CL, T1CH, T1LL, SR, PCR writes + delays
```

Three calls per stroke, always: intensity, absolute jump, delta. There is no
intensity cache, no stroke reordering, no collinear merging, no Douglas-Peucker,
no clipping. All of those existed once, all of them worked on a different
encoding, and every one was eventually measured as a shimmer, a displaced stroke
or missing geometry. **What the game asks for is what gets drawn.**

## How a stroke becomes hardware

The Vectrex draws by running two integrators for a fixed time:

1. `PORT_A` = the **rate** for one axis (a signed 8-bit DAC value),
2. `PORT_B` routes it into the X or Y integrator through the mux,
3. `T1` counts down the **duration**,
4. `SR` carries brightness, `PCR` carries `/BLANK`.

So a delta `(dx, dy)` has to be split between *how fast* and *for how long*:

```
distance ≈ rate × t1 / DRAW_SCALE
```

`ramp_params(dx, dy)` in `sdk/vectrex-draw/src/ramp.rs` does that split. It is the
single implementation of the beam model — the same crate is linked by another
cartridge's firmware, precisely so a discovery made on one board does not have to
be rediscovered on the other.

### The constants, and where each came from

| | default | why |
|---|---|---|
| `DRAW_SCALE` | 160 | The divisor. **Larger = shorter strokes.** With a fixed ramp the scale *is* the duration; tying `s` to `t1` is what makes the drawer's idea of its own position match the real travel (they diverged 21% per vector before). |
| `MIN_T1` | 8 | The dwell floor. 8 is what the reference cartridge measures on 1861 of ~1900 short lit strokes across 8 captured frames. |
| `MIN_T1_START` | 31 | The floor for ramps that start **from rest** (a jump, and the first stroke after one). Below ~31 those come out visibly stepped: the deflection lag is *fixed* (~2 E cycles of beam-on plus 2 of blank settle) while the stroke duration is not, so on an 8-cycle stroke the lag is half of it. That is a property of the tube, not a preference. |
| `DAC_CAP` | 127 | The DAC is 8-bit signed. A rate cannot exceed it, so a long stroke is bounded by duration, not speed. |
| `T1_TRANSPORT` | 160 | The T1 ceiling for blanked jumps — they are allowed to be faster than lit strokes. |
| `T1_EXTRA_Q8` | per board | The ramp's *real* length in 1/256 of a count. The 6522 counts `t1 + 1.5` on a one-shot, so the honest value is not 0. Measure it by closing a polygon of short strokes. |

### Sub-unit geometry

Everything travels in **1/16 of a device unit** (`UVM2_SUBUNITS`, `UVM2_Q_BITS=4`),
end to end: `v_directDraw32` converts once and the SDK splits, re-zeroes and
calibrates in that unit. A 2.5-unit stroke stays 2.5 units. This was validated
against a reference capture and is the only path — the integer knobs that used to
sit alongside it no longer exist.

### The debt

Splitting a real distance into `(rate, t1)` loses a fraction. `ramp_params_chain`
carries that remainder forward as a **debt** and pays it into the next stroke, so
errors do not accumulate along a chain. (Measured: on fine enough input the debt
stops helping — 97.9% → 99.8% of strokes identical to the reference when removed
in Q8 — but on coarse input it is what keeps a long chain straight.)

## Re-zeroing

The integrators drift. Two safety nets, both in `uvm2_draw.c`:

* **`uvm2_zero_jump`** (24 device units): a blanked jump longer than this forces a
  re-zero before the next stroke. This is the one that normally fires.
* **`uvm2_zero_every`** (off by default): re-zero after N ramps regardless. A
  scene of short chained strokes can otherwise run a very long way without one;
  `tools/uvm2_list_count.c` reports the *worst run with no re-centring*, which is
  the number to look at if the picture is sliding.

A re-zero costs commands, so it is a trade, not a free win.

`uvm2_zero_offset` is the value primed into the zero reference and belongs to the
**console**, not to the game (see `uvm2_config.h`). The Vectrex BIOS writes 0
there; other cartridges write a non-zero value per console and per scale, which is
why a calibration file exists.

## What actually costs time

From `tools/uvm2_list_count.c` on real frames:

* **a lit stroke ≈ 13 commands, flat in its length**;
* **a blanked jump ≈ 5 more**;
* **a brightness change is one more write**.

Therefore, in order of impact:

1. **Chain your strokes.** A stroke starting where the last ended pays no jump.
   In one measured port, 62% of strokes were preceded by a jump; in another that
   drew chained paths, 30%.
2. **Fewer, longer strokes.** Length is free; count is not.
3. **Do not re-set intensity you have not changed.**
4. Only then worry about geometry.

Reordering strokes to reduce jumps sounds attractive and mostly does not work:
naive nearest-neighbour makes it *worse* (measured 709 → 882 jumps) because it
eats segments that were the continuation of a chain. The number of jumps is set
by **how many disjoint polylines the game draws**, and reordering can only
shorten them, not remove them. `tools/uvm2_order.c` measures all the variants if
you want the numbers for your own scene.

## Things that are not what they look like

* **The shift register is not "brightness change".** A game that never changes
  intensity still writes `SR` hundreds of times a frame: those are the beam being
  blanked and unblanked around each jump. `uvm2_list_count` separates the two.
* **A photograph of a CRT is not brightness data.** Geometry from a photo is
  evidence; brightness is phosphor.
* **One game is not evidence.** Global render constants swept on a single game
  are overfitted, however clean the curve. And check changes on **short dense**
  vectors: long strokes cannot fail, so they prove nothing.
