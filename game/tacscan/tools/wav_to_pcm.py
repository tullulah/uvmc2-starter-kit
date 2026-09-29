#!/usr/bin/env python3
"""samples/*.wav -> build/tacscan.pcm, the 16-bit bundle for the cartridge's jack.

THE SIBLING OF wav_to_vsmp.py, AND ON PURPOSE. That one makes the bundle the console's
own sound chip can play: 4 bits per sample, written into the PSG's volume register and
injected into the draw list, at 12 kHz. This one makes the bundle the UVMC2's jack plays:
16 bits at 32 kHz straight out of a PT8211, which touches neither the Vectrex bus nor its
PSG.

WHAT THE JACK ACTUALLY REMOVES, stated honestly:

    today (console)          44.1 kHz source -> 12 kHz, 4-bit linear, then a 4-bit
                             volume register written in the gaps of the command list
    the jack                 44.1 kHz source -> 32 kHz, 16-bit, no quantisation and
                             no injection

There is still ONE resample, 44100 -> 32000, and ffmpeg does it here with a proper filter
rather than the cartridge doing it per sample at play time. That is the whole point of
doing it offline.

THE GAINS COME FROM THE OTHER GENERATOR and are not repeated here. `MIX` and `apply_gain`
are imported from wav_to_vsmp, so the balance between sounds — the ship roar ducked to a
half so the lasers can be heard over it — is one decision with two outputs. Copying the
numbers would let the two bundles drift apart, and then the game would sound different
depending on where the sound came out, which is not what the setting is for.

    python3 tools/wav_to_pcm.py                    -> build/tacscan.pcm
    python3 tools/wav_to_pcm.py --rate 22050       a smaller bundle, if the load is slow

Requires ffmpeg on PATH, like its sibling.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
sys.path.insert(0, HERE)

try:
    from audio2vsmp import load_pcm_s16_mono
    from wav_to_vsmp import MIX, apply_gain
except ImportError as e:
    sys.exit(f"cannot import the shared helpers ({e}); they live next to this file")

# THE CONTAINER IS THE ONE THAT ALREADY EXISTS, not a second one.
#
#   "KSFX" | u16 n | u16 - | n x (u32 offset, u32 samples, u32 hz, u16 format, u16 -) | data
#
# It comes from the other game on this cartridge that drives the jack, and its `format` field
# is exactly the extension point this needs: 0 is 16-bit little-endian linear and 1 is IMA
# ADPCM at four bits. Tac/Scan writes 0 — no quantisation at all, which is the whole reason
# for using the jack — and the field is what makes dropping to ADPCM later a flag here and a
# branch in the reader rather than a new format.
#
# Two formats in one container beats two containers. A parallel format would have meant two
# readers, two generators and two things to keep in step for no gain.
MAGIC = b"KSFX"
FMT_S16 = 0      # 16-bit little-endian linear
FMT_ADPCM = 1    # IMA ADPCM, 4 bits: a quarter of the space, and a conversion
# uvm2_jack.h's UVM2_JACK_RATE. The driver takes this rate and nothing else, so a bundle
# at any other rate would need resampling in the mixer — which is what we are avoiding.
JACK_RATE = 32000


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rate", type=int, default=JACK_RATE,
                    help=f"output sample rate (default {JACK_RATE}, the jack's own)")
    ap.add_argument("--no-normalize", action="store_true",
                    help="do not normalise each sound to its own peak")
    ap.add_argument("--shape", type=float, default=1.0,
                    help="power curve applied before the mix gain (1.0 = none)")
    ap.add_argument("--out", default=os.path.join(GAME, "build", "tacscan.pcm"))
    args = ap.parse_args()

    base = os.path.basename(args.out)
    stem, _, ext = base.partition(".")
    if len(stem) > 8 or len(ext) > 3 or "." in ext or not stem:
        sys.exit(f"{base}: the name must be 8.3 or the cartridge will not find it on the "
                 f"card and the jack will be silent. Same rule as the .vsm.")

    if args.rate != JACK_RATE:
        print(f"  NOTE: {args.rate} Hz is not the jack's {JACK_RATE} Hz. The mixer will "
              f"have to resample at play time; see docs/06.", file=sys.stderr)

    samples_dir = os.path.join(GAME, "samples")
    with open(os.path.join(samples_dir, "samples.json")) as f:
        names = json.load(f)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    # One blob per sound, in samples.json order, because that order IS the index AAE asks
    # for (sample_start's `samplenum`). A missing file becomes a hole, not a shift: every
    # sound after it would otherwise be the wrong one.
    blobs, total_s = [], 0.0
    for name in names:
        path = os.path.join(samples_dir, name)
        if not os.path.exists(path):
            print(f"  MISSING {name}: goes into the table as a hole", file=sys.stderr)
            blobs.append(None)
            continue
        gain = MIX.get(os.path.basename(name), 1.0)
        raw = load_pcm_s16_mono(path, args.rate)
        pcm = apply_gain(raw, not args.no_normalize, gain, args.shape)
        blobs.append((len(pcm), struct.pack(f"<{len(pcm)}h", *pcm)))
        secs = len(pcm) / float(args.rate)
        total_s += secs
        peak = max((abs(v) for v in pcm), default=0)
        dev = sorted(abs(v) for v in pcm)
        p50 = dev[len(dev) // 2] if dev else 0
        print(f"  [{len(blobs)-1:2d}] {name:14s} {len(pcm):7d} smp  "
              f"{len(pcm)*2/1024:7.1f} KB  {secs:5.2f} s  gain {gain:4.2f}  "
              f"peak {peak:5d}  median |v| {p50:5d}", file=sys.stderr)

    # The header is fixed-size, so it can be written once the count is known: 8 bytes then
    # 16 per entry. A hole is offset 0 and the reader returns nothing for it, so a missing
    # file plays silence rather than shifting every sound after it onto the wrong index.
    head = 8 + 16 * len(blobs)
    records, payload = [], bytearray()
    for b in blobs:
        if b is None:
            records.append((0, 0, 0, FMT_S16))
            continue
        n, data = b
        while (head + len(payload)) % 4:
            payload += b"\0"
        records.append((head + len(payload), n, args.rate, FMT_S16))
        payload += data

    out = bytearray(MAGIC)
    out += struct.pack("<HH", len(blobs), 0)
    for off, n, hz, fmt in records:
        out += struct.pack("<IIIHH", off, n, hz, fmt, 0)
    out += payload

    with open(args.out, "wb") as f:
        f.write(out)

    print(f"\n{args.out}: {len(blobs)} sounds, {len(out)/1e6:.2f} MB at {args.rate} Hz, "
          f"{total_s:.1f} s of audio", file=sys.stderr)
    print(f"it is loaded into PSRAM at startup (8 MB available, the romset takes the 4 MB "
          f"mark), so it does not touch the {len(out)/1e6:.2f} MB against the image's "
          f"496 KB of SRAM.", file=sys.stderr)
    print("copy it to the SD card next to the .um2", file=sys.stderr)


if __name__ == "__main__":
    main()
