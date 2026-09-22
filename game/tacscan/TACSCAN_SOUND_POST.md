# Tac/Scan for the UVM2 now has sound — digitised samples through the PSG's volume register

New build for testers. Tac/Scan has been playable on the UVM2 for a while but completely silent, and this fixes that. It needs **two files on the SD card**, not one, so please read the install note before deciding it does not work.

## Why this game was the hard one

Every other arcade port so far gets its audio the same way: run the game's own sound program, log what it writes to its sound chip, reduce that to PSG events. Snow Bros is a Z80 driving a YM3812 — there is a register stream to capture.

Tac/Scan has nothing to capture. Its board is the **Sega Universal Sound Board**: an i8035 whose program is not even in the romset (the game uploads it into shared RAM at $D000), and the sound itself is made by **analogue circuitry**. MAME does not emulate it with a chip model at all — it runs a netlist, `nl_segausb.cpp`. There is no stream to rip, at any level.

So the only route left is samples, and the Vectrex has no DAC. What it has is an AY-3-8912 whose **volume register is 4 bits wide** — which is a DAC if you disable that channel's tone and noise and write amplitudes into it fast enough. That is the old trick behind Spike's voice.

## The part that was actually difficult

Not the DAC. The bus.

Writing the PSG needs the Vectrex bus, and during a vector the bus *is* the drawing: Port A holds the X integrator's rate, so touching it while a ramp is open bends the stroke. Samples cannot be written whenever they fall due.

What makes it work is that a stroke is not one long operation — it is a chain of 32-cycle micro-segments, and immediately before each timer reload the timer has expired, PB7 is high and the integrators are frozen. A PSG write there moves nothing. So the samples are **injected into the draw list itself**, in the gaps where the beam is already parked, and the whole thing costs about 6% of the bus.

The clock is the bus, not the wall. A list is built one frame before it is replayed, so a timer read while building tells you nothing about when a sample will sound; each voice's cursor advances by **elapsed bus cycles** instead. The gaps fall unevenly and the pitch still comes out exact — it is non-uniform sampling with the right value at each instant, not jitter.

## Numbers, measured on hardware rather than hoped for

| | |
|---|---|
| sounds | 22, 4-bit, 12 kHz source |
| bundle | 242 KB, read off the SD into PSRAM at boot |
| actual DAC rate | **~11,300 Hz** — mean gap 132 bus cycles, worst 373 |
| mixing | up to 10 simultaneous voices |
| refresh | paced to 40 Hz, which is Tac/Scan's real rate (15468480 / 3 / 0x1F788 = 40.00) |

Three things I got wrong along the way, in case they save someone else the time:

**The volume register is not a linear DAC.** It is a resistor-ladder attenuator with roughly logarithmic steps — the value I was calling the midpoint actually sits at a sixth of full scale. Writing linear PCM into it distorts every single sample regardless of sample rate or mixing. There is now an inverse table applied at the last moment, after mixing (mixing has to happen in the linear domain; summing two logarithms means nothing).

**The inter-frame gap is not a special case, and I built one anyway.** A game that draws 12 ms of a 25 ms frame leaves 13 ms with no strokes — I assumed audio would drop out there. It does not: that gap is already full of commands from the zero-reference pacer, each carrying its own timer reload. Coverage is 74-89% of the frame depending on the scene, and it does not collapse when the screen empties, which was the thing I was afraid of.

**Normalising each sound to its own peak destroys the mix.** Every effect arrives at full scale, so the ship's engine — which drones continuously — comes out exactly as loud as a laser shot, and being continuous it wins. The engine is now attenuated 6 dB in the bundle itself, and the sources are gently compressed, because with a crest factor of 15–17x the *body* of a shot was sitting three steps from silence while only its transient reached the rails.

## Install

Copy **both** to the root of the SD, next to your other `.um2` files:

```
aae_tacscan.um2    142,184 bytes
tacscan.vsm        247,632 bytes
```

Plus `roms/tacscan.zip` as before.

**The bundle's name has to be exactly `tacscan.vsm`.** The SD reader matches short 8.3 directory entries only and truncates the base to 8 characters, so `aae_tacscan.vsm` gets looked up as `AAE_TACSVSM`, the card holds `AAE_TA~1.VSM`, nothing matches — and the game boots perfectly and says nothing. That failure cost me two separate debugging sessions, once on each cartridge, and nothing anywhere reports it. If you get silence, check this first.

If you had an older `aae_tacscan.vsm` on the card, delete it.

## What would be useful to hear back

- **Does it sound right on your console?** The PSG is being used well outside its intended range and I have only two cartridges to test on.
- **Does the game feel fast or slow?** Tac/Scan's logic advances one step per refresh interrupt, so on this hardware **frame rate is game speed**. It is pinned to 40 Hz for that reason, but I have measured the actual rate on my own cartridge only.
- **Anything that crunches on a busy screen.** Four bits cannot hold five loud voices at once; the mixer sums and clamps up to four voices and halves above that, which clips 3.6-5.3% of samples on a dense scene. Inaudible to me, but "inaudible to me" is one pair of ears.

## Honest limits

Four bits is 24 dB of range for everything, so the balance between effects is hand-set rather than faithful to the arcade. The compression is a deliberate trade: it lifts the body of the effects at the cost of raising the noise floor in the tails.

And the pitch follows the drawing. If the builder is ever slower than the bus, wall time passes with no bus cycles and the samples play slow in that proportion. Pinning the refresh is what keeps that honest, and it is a trade you choose rather than a fault you discover.
