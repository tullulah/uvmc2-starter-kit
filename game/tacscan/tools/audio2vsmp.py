#!/usr/bin/env python3
"""
audio2vsmp — convert an audio track into a Vectrex .vsmp voice/sample file.

The Vectrex has no audio DAC: the AY-3-8912 PSG only makes square waves. Voice
(as in the game Spike) is done by ABUSING the 4-bit per-channel VOLUME register
as a crude DAC — you disable the tone/noise and write volume values (0-15) at
the sample rate. So a .vsmp is just low-rate, 4-bit PCM ready to stream to that
register: rough but recognizable.

This tool: audio → mono → downsample → 4-bit quantize → pack (2 samples/byte) →
JSON (base64 payload). It can also PREVIEW the result as a .wav so you can hear
the 4-bit/low-rate degradation on your computer before touching hardware.

Usage:
  python audio2vsmp.py IN.(mp3|mov|wav|…) OUT.vsmp [--rate 8000] [--normalize]
  python audio2vsmp.py IN OUT.vsmp --preview preview.wav   # also write a .wav

Requires: ffmpeg on PATH. Pure-stdlib otherwise.
"""
import argparse
import base64
import json
import os
import struct
import subprocess
import sys
import wave


# ffmpeg binary: env override (Electron passes the bundled ffmpeg-static path)
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

def load_pcm_s16_mono(path, rate, start=0.0, duration=None):
    """Decode `path` to mono s16le at `rate` Hz via ffmpeg; return list[int] (-32768..32767).

    `start`/`duration` (seconds) trim to the same window as the video track so the
    voice matches the recording."""
    cmd = [FFMPEG, "-v", "error"]
    if start and start > 0:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path, "-ac", "1", "-ar", str(rate), "-f", "s16le", "-acodec", "pcm_s16le"]
    if duration and duration > 0:
        cmd += ["-t", str(duration)]
    cmd += ["-"]
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    n = len(raw) // 2
    return list(struct.unpack("<%dh" % n, raw[: n * 2]))


def quantize_4bit(samples, normalize):
    """Map s16 samples → 4-bit (0..15). Linear. Optionally normalize to peak."""
    peak = 32768
    if normalize:
        m = max((abs(s) for s in samples), default=1)
        peak = max(m, 1)
    out = []
    for s in samples:
        # -peak..+peak → 0..15 (8 ≈ silence/mid)
        v = int(round((s / peak * 0.5 + 0.5) * 15))
        out.append(max(0, min(15, v)))
    return out


def pack_nibbles(vals):
    """Pack 4-bit values 2 per byte (low nibble = even sample)."""
    b = bytearray((len(vals) + 1) // 2)
    for i, v in enumerate(vals):
        if i % 2 == 0:
            b[i // 2] = v & 0x0F
        else:
            b[i // 2] |= (v & 0x0F) << 4
    return bytes(b)


def unpack_nibbles(data, n):
    vals = []
    for i in range(n):
        byte = data[i // 2]
        vals.append(byte & 0x0F if i % 2 == 0 else (byte >> 4) & 0x0F)
    return vals


def write_preview_wav(path, vals, rate):
    """Reconstruct what the Vectrex would output (4-bit steps) as a listenable .wav."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for v in vals:
            s = int((v / 15.0 * 2.0 - 1.0) * 32767)
            frames += struct.pack("<h", s)
        w.writeframes(bytes(frames))


def main():
    ap = argparse.ArgumentParser(description="Convert audio to a Vectrex .vsmp 4-bit sample")
    ap.add_argument("input", help="input audio/video (ffmpeg-decodable)")
    ap.add_argument("output", help="output .vsmp")
    ap.add_argument("--name", default=None, help="sample name (default: from output)")
    ap.add_argument("--rate", type=int, default=8000,
                    help="sample rate Hz (default 8000; lower = smaller + easier to stream)")
    ap.add_argument("--normalize", action="store_true", help="normalize to peak amplitude")
    ap.add_argument("--preview", default=None,
                    help="also write a .wav of the 4-bit result so you can HEAR the quality")
    ap.add_argument("--start", type=float, default=0.0,
                    help="trim: start decoding at this timestamp (seconds)")
    ap.add_argument("--duration", type=float, default=None,
                    help="trim: only decode this many seconds from --start")
    args = ap.parse_args()

    name = args.name or os.path.splitext(os.path.basename(args.output))[0]

    print(f"[1/3] decoding → mono {args.rate} Hz…", file=sys.stderr)
    s16 = load_pcm_s16_mono(args.input, args.rate, args.start, args.duration)
    print(f"      {len(s16)} samples ({len(s16)/args.rate:.1f} s)", file=sys.stderr)

    print("[2/3] quantizing to 4-bit…", file=sys.stderr)
    vals = quantize_4bit(s16, args.normalize)
    packed = pack_nibbles(vals)

    vsmp = {
        "version": "1.0",
        "name": name,
        "sampleRate": args.rate,
        "bits": 4,
        "numSamples": len(vals),
        "data": base64.b64encode(packed).decode("ascii"),
    }
    with open(args.output, "w") as f:
        json.dump(vsmp, f)

    size_kb = len(packed) / 1024
    print(f"[3/3] wrote {args.output}  ({size_kb:.0f} KB packed, "
          f"{len(vals)} samples @ {args.rate} Hz)", file=sys.stderr)

    if args.preview:
        write_preview_wav(args.preview, vals, args.rate)
        print(f"      preview → {args.preview}  (open it to hear the 4-bit quality)", file=sys.stderr)


if __name__ == "__main__":
    main()
