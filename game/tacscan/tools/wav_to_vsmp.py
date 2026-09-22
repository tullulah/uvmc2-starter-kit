#!/usr/bin/env python3
"""wav_to_vsmp — Tac/Scan's 22 sounds, from .wav to a 4-bit sample bundle.

WHY THIS GAME NEEDS THIS AND SNOW BROS DOES NOT. Snow Bros keeps its music in its
own ROM: a Z80 writing YM3812 registers, so you can run the sound program, log the
writes and reduce them to PSG events (tools/snd_rip.c + snd_conv.py). Tac/Scan has
none of that. Its board is the Sega Universal Sound Board: an i8035 whose program
is NOT in the romset — the game uploads it into shared RAM at 0xd000 — and the
sound is produced by ANALOGUE circuitry, which MAME emulates with a netlist
(nl_segausb.cpp), not with a register-level chip. There is no stream to rip; what
there is are AAE's .wav files, which is where all of this came from.

So the route is samples: 4 bits through the PSG's volume DAC. See the uvm2_smp.h
header in the uvm2-sdk for the playback model.

THE QUANTISER IS NOT REWRITTEN HERE: it is imported from
tools/audio2vsmp.py, which is what defines the .vsmp format (and what
the IDE uses). Two packers for one format are two formats, and the day they
diverged the symptom would be "it only sounds wrong on the cartridge".

THE OUTPUT IS A BUNDLE ON THE SD CARD, NOT A C TABLE, and that is not a
preference. A .um2 image runs entirely from the UVM2's SRAM, and the loader
reserves the low part: linking the 121 KB of sounds pushed the image past
0x20060000 and the console came up with a solid red LED — a hardfault. The bundle
goes on the card next to the .um2 and the player reads it into PSRAM at startup,
the same way these games already load their romset.

IT NORMALISES EACH SOUND TO ITS OWN PEAK, and that has a price: the RELATIVE
loudness between effects is lost (the laser and the explosion come out equally
loud). In exchange, 4 bits give 24 dB of total range and a quietly recorded effect
falls below the DAC's step — i.e. inaudible. Between "badly balanced" and "cannot
be heard" the first one wins. --no-normalize is the control, and MIX below is the
per-sound correction that puts the balance back where the game wants it.

  python3 tools/wav_to_vsmp.py                     -> build/tacscan.vsm
  python3 tools/wav_to_vsmp.py --preview build/snd  also write a .wav per sound
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
# audio2vsmp.py sits next to this file and is what DEFINES the .vsmp format
# (4-bit quantisation, the packing, and the preview writer). This script only
# decides which sounds go into the bundle and how loud each one is.
sys.path.insert(0, HERE)

try:
    from audio2vsmp import load_pcm_s16_mono, quantize_4bit, pack_nibbles, write_preview_wav
except ImportError:
    sys.exit("cannot import audio2vsmp.py -- it should be in this same directory")

MAGIC = b"VSMB"

# ── THE MIX, AND IT IS A GAME DECISION, NOT A FORMAT ONE ─────────────────────
#
# Per-sound gain applied AFTER normalisation (normalising first and scaling after
# is the only order that works: normalise divides by the sound's own peak, so a
# gain applied before it is simply undone).
#
# WHY ANY OF THIS IS NEEDED. Normalisation is what flattened the balance: every
# sound arrives at the DAC at full scale, so the ship's engine — which drones
# continuously — comes out exactly as loud as a laser shot, and being continuous it
# wins. The arcade's analogue board had no such problem; ours is self-inflicted.
#
# WHICH SOUNDS ARE THE ENGINE, from SegaG80snd.c's TacScan_sh_w: sounds 0, 1 and 2
# go to kVoiceShipRoar (5) and are the roar rising, its loop and its fall. They are
# also the ONLY looping sounds in the game — every other sample_start passes
# loop = 0 — which is exactly why they mask everything else.
#
# 0.5 IS -6 dB AND IT COSTS RESOLUTION: at half scale the roar spans 8 of the 16
# DAC steps instead of 16. That is affordable for a noise bed and would not be for
# a tone. Turn it down further only if the shots still do not cut through; the
# floor is about 0.25, where the roar starts to sound like a rattle rather than an
# engine.
MIX = {
    "01.wav": 0.5,   # ship roar, rising      \
    "02.wav": 0.5,   # ship roar, the loop     > kVoiceShipRoar, the continuous bed
    "03.wav": 0.5,   # ship roar, falling     /
}


# ── THE CREST FACTOR IS WHAT IS LEFT, AND IT IS NOT THE MIXER'S FAULT ────────
#
# Measured on the console (tools/smp_stats.py, mixer telemetry): with a SINGLE voice
# playing — no mixing, no division, no clipping — the median |deviation| from silence
# is 1 of 8. The DAC spends its life a step or two from the midpoint and only reaches
# the rails on transients. In 4 bits that is exactly what is heard as grain, and no
# threshold in the mixer can fix it, because nothing in the mixer caused it.
#
# Normalising to peak is what causes it: what touches the peak is a short transient,
# so the BODY of the sound ends up far below full scale. Measured on the sources:
#
#     01.wav  (ship roar)   median 17.2% of peak   crest  5.8x
#     plaser                median  6.5%           crest 15.4x
#     pexpl                 median  5.9%           crest 16.9x
#
# The two the player calls weak are the two with a crest of 15-17.
#
# SO THE CURVE IS A POWER LAW, y = sign(x) * |x|**SHAPE, applied to the normalised
# signal. It is the whole compressor: full scale maps to itself (1**a == 1) so the
# peak never moves and nothing has to be limited afterwards, while everything below
# it rises — and a quiet sample rises PROPORTIONALLY MORE, which is precisely the
# "raise the median, leave the peak" that 4 bits want. It also improves the balance
# for free: the sounds with the most headroom below the peak are the ones that gain
# most, and those are the effects, not the bed.
#
# NO ATTACK, NO RELEASE, NO PUMPING, and that is why it is this and not a real
# compressor: a time-varying gain would breathe on a bed that never stops, and a
# 4-bit DAC has no resolution to spare for the artefacts. This is memoryless — the
# same input sample always gives the same output — so it cannot pump by construction.
#
# 0.5 — A SQUARE ROOT — AND IT WAS SWEPT, not picked. Four bits quantise the result,
# so what the curve does in theory and what survives into the file are different
# questions; these are the medians and (p90) of |deviation| out of 8, measured on the
# packed nibbles:
#
#     exponent     1.0    0.7    0.6    0.5    0.4
#     plaser      1(2)   1(3)   1(4)   2(4)   2(5)
#     pexpl       1(2)   1(3)   1(4)   2(4)   2(5)
#     slaser      2(5)   3(6)   4(6)   4(7)   5(7)
#     01 (roar)   1(2)   1(2)   1(3)   2(3)   2(3)
#
# 0.6 was the first guess and it is not enough: the two sounds the player called weak
# still come out with a median of 1, because 0.194 of full scale is 1.55 steps and
# the rounding takes it back. 0.5 is where they reach 2 — the first exponent where
# the improvement survives quantisation. Below it the medians stop moving and only
# the noise floor keeps rising, so it buys nothing.
#
# The cost is that noise floor, which rises with everything else, and more clipping
# in the mixer: louder bodies make bigger sums. Both are measurable — smp_stats.py
# reads the clipping back off the console.
SHAPE = 0.5


def apply_gain(pcm, normalize, gain, shape):
    """DC-remove, normalise to peak, compress, then scale. Returns s16 samples.

    Kept here and not in audio2vsmp.py on purpose: that module defines the .vsmp
    FORMAT and is shared with the IDE, while how loud a sound should be and how much
    of its dynamic range to trade away is this game's business.

    THE ORDER IS NOT INTERCHANGEABLE. DC first, because a power law amplifies quiet
    samples most and an offset IS a quiet sample that never goes away — tunnelh.wav
    carries 16% of its peak as DC, and normalising a 4426-peak file multiplies that
    offset sevenfold before the curve ever sees it. Normalise second, because the
    curve is defined on a signal that reaches 1.0. MIX gain LAST, because it is a
    deliberate balance between sounds: shaping after it would partly undo the
    ducking, the loudest thing being also the one with the most room to rise."""
    if not pcm:
        return pcm
    dc = sum(pcm) / len(pcm)
    pcm = [s - dc for s in pcm]
    if normalize:
        peak = max((abs(s) for s in pcm), default=1) or 1
        pcm = [s * 32767.0 / peak for s in pcm]
    if shape != 1.0:
        pcm = [(1.0 if s >= 0 else -1.0) * (abs(s) / 32767.0) ** shape * 32767.0
               for s in pcm]
    if gain != 1.0:
        pcm = [s * gain for s in pcm]
    return [max(-32768, min(32767, int(round(s)))) for s in pcm]


def main():
    ap = argparse.ArgumentParser()
    # 12000, AND IT IS THE INJECTION RATE THAT CHOOSES IT. The file's rate is not
    # what the DAC hears: samples are injected at the first drawing gap past the
    # period, so the emission rate is a property of the draw list, not of the file.
    # Traced on hardware (uvm2_smp_traza_*) it is 11344 Hz -- mean gap 132 bus
    # cycles, worst 373. A 6 kHz file fed at 11.3 kHz is simply each sample played
    # twice: no extra detail, only the interpolation the ear does for free. 12 kHz
    # is the first rate the stream can actually carry. Above it the gaps start to
    # decide and the extra samples are dropped, not heard.
    #
    # It doubles the bundle (121 -> 242 KB), which the PSRAM does not notice:
    # UVM2_SMP_BUNDLE_MAX is 1 MB. The old 6 kHz was chosen when the bundle was
    # still linked INTO the image; it has lived on the card since.
    ap.add_argument("--rate", type=int, default=12000,
                    help="output Hz (12000 default; the DAC is fed at ~11.3 kHz)")
    ap.add_argument("--no-normalize", action="store_true")
    # The mix knob, so trying a balance does not mean editing this file:
    #   make snd SND_ARGS="--gain 01.wav=0.35 --gain 02.wav=0.35 --gain 03.wav=0.35"
    ap.add_argument("--gain", action="append", default=[], metavar="FILE=V",
                    help="override MIX for one sound (1.0 = full scale); repeatable")
    ap.add_argument("--shape", type=float, default=SHAPE,
                    help=f"compression exponent ({SHAPE} default; 1.0 = none, "
                         f"lower compresses harder and lifts the noise floor)")
    ap.add_argument("--preview", default=None,
                    help="directory to write one .wav per sound, to HEAR the result")
    ap.add_argument("-o", "--output",
                    default=os.path.join(GAME, "build", "tacscan.vsm"))
    args = ap.parse_args()

    # THE NAME HAS TO BE 8.3, AND THIS IS CHECKED BECAUSE THE FAILURE IS SILENT.
    # uvm2_sd.c only matches short directory entries (it skips the VFAT ones) and its
    # `a83` truncates the base to 8 characters: "aae_tacscan.vsm" is looked up as
    # "AAE_TACSVSM", the card holds "AAE_TA~1.VSM", nothing matches, the bundle never
    # loads and the game is simply mute. That is how this failed on hardware the first
    # time, and nothing anywhere reported it.
    base = os.path.basename(args.output)
    stem, _, ext = base.partition(".")
    if len(stem) > 8 or len(ext) > 3 or "." in ext or not stem:
        sys.exit(f"{base}: the name must be 8.3 (<=8 before the dot, <=3 after) or the "
                 f"cartridge will not find it on the card and the game will be silent.")

    mix = dict(MIX)
    for spec in args.gain:
        name, _, val = spec.partition("=")
        if not val:
            sys.exit(f"--gain {spec}: expected FILE=VALUE, e.g. --gain 01.wav=0.4")
        mix[name] = float(val)

    samples_dir = os.path.join(GAME, "samples")
    with open(os.path.join(samples_dir, "samples.json")) as f:
        names = json.load(f)

    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
    os.makedirs(os.path.dirname(args.output), exist_ok=True)

    # Each entry is the .vsmp as the player reads it: .word rate, .word n, packed
    # nibbles (even sample in the low one).
    blobs = []
    for name in names:
        path = os.path.join(samples_dir, name)
        if not os.path.exists(path):
            print(f"  MISSING {name}: goes into the table as a hole", file=sys.stderr)
            blobs.append(None)
            continue
        gain = mix.get(os.path.basename(name), 1.0)
        raw = load_pcm_s16_mono(path, args.rate)
        pcm = apply_gain(raw, not args.no_normalize, gain, args.shape)
        vals = quantize_4bit(pcm, False)     # already scaled; do not re-normalise
        packed = pack_nibbles(vals)
        blobs.append(struct.pack("<II", args.rate, len(vals)) + packed)
        if args.preview:
            write_preview_wav(os.path.join(args.preview, os.path.basename(name)),
                              vals, args.rate)
        # THE MEDIAN IS THE NUMBER THAT MATTERS, not the span. The span only says the
        # transient reached the rails, which normalisation guarantees and which was
        # already true when the game sounded weak; what the ear calls loudness is
        # where the sound spends its time. Same statistic smp_stats.py reads back off
        # the console, so the two can be compared directly.
        dev = sorted(abs(v - 8) for v in vals)
        p50 = dev[len(dev) // 2] if dev else 0
        before = sorted(abs(v - 8) for v in quantize_4bit(raw, not args.no_normalize))
        p50b = before[len(before) // 2] if before else 0
        span = max(vals) - min(vals) + 1 if vals else 0
        print(f"  [{len(blobs)-1:2d}] {name:14s} {len(vals):6d} smp "
              f"{(len(packed) + 8)/1024:5.1f} KB  gain {gain:4.2f}  "
              f"{span:2d}/16 steps   median |dev| {p50b} -> {p50}", file=sys.stderr)

    # Header, then the blobs 4-byte aligned. A hole is offset 0: the player returns
    # NULL for it and nothing plays, rather than some OTHER sound playing.
    head = len(MAGIC) + 4 + 4 * len(blobs)
    offsets, payload = [], bytearray()
    for b in blobs:
        if b is None:
            offsets.append(0)
            continue
        while (head + len(payload)) % 4:
            payload += b"\0"
        offsets.append(head + len(payload))
        payload += b

    out = bytearray(MAGIC)
    out += struct.pack("<I", len(blobs))
    for o in offsets:
        out += struct.pack("<I", o)
    out += payload

    with open(args.output, "wb") as f:
        f.write(out)
    print(f"\n{args.output}: {len(blobs)} sounds, {len(out)/1024:.0f} KB at {args.rate} Hz"
          f"\ncopy it to the SD card next to the .um2", file=sys.stderr)


if __name__ == "__main__":
    main()
