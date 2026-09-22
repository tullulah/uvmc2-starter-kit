#!/usr/bin/env python3
"""smp_stats — read the sample mixer's telemetry off a running console over SWD.

IT RESOLVES THE SYMBOLS FROM THE ELF ITSELF, and that is the whole point of it
existing rather than a handful of `probe-rs read` calls. These counters live in
.bss, so adding one variable shifts every address after it; twice now a reading was
taken with addresses from the previous build and came back as plausible nonsense
(once as DAC values above 15, once as a voice histogram that disagreed with the
emission counter by a factor of 25). An address typed by hand is a bug waiting for
a rebuild.

  python3 tools/smp_stats.py <firmware.elf> [threshold-in-the-flashed-build]

The threshold is only used to mark a row "<- current". It is passed in because
nothing on the device reports it, and a marker that is merely assumed is worse than
none: this one sat on the wrong row for a whole reading after the build moved on.

WHAT IT ANSWERS. `sum_hist[n][bucket]` is the distribution of the mixer's RAW sum —
before any division — split by how many voices were mixed. From that, the cost of
ANY divide threshold can be computed without flashing it: for a threshold T the
value emitted is `sum` when n < T and `sum / 2` when n >= T, and the DAC clamps it
to -8..+7. So the table below is a prediction for every candidate at once, measured
on one recording of one session, rather than one round trip per guess.
"""
import re
import subprocess
import sys

CHIP = "RP235x"
SPEED = "1000"
BUCKETS = 64
BIAS = 32            # bucket index = sum + BIAS, saturating
DEV_LO, DEV_HI = -8, 7


def symbols(elf):
    """name -> address, from the ELF that is actually flashed."""
    for nm in ("arm-none-eabi-nm", "llvm-nm", "nm"):
        try:
            out = subprocess.run([nm, elf], check=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout.decode()
            break
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    else:
        sys.exit(f"cannot read symbols from {elf}: no working nm")
    syms = {}
    for line in out.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+\S\s+(\S+)$", line.strip())
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    return syms


def read_words(addr, count):
    out = subprocess.run(
        ["probe-rs", "read", "--chip", CHIP, "--speed", SPEED,
         "b32", hex(addr), str(count)],
        check=True, stdout=subprocess.PIPE).stdout.decode()
    vals = [int(t, 16) for t in out.split() if re.fullmatch(r"[0-9a-fA-F]{8}", t)]
    if len(vals) != count:
        sys.exit(f"read {hex(addr)}: expected {count} words, got {len(vals)}")
    return vals


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def cost(hist, voices, threshold):
    """Clipped samples if the divide kicked in at `threshold` voices."""
    clipped = total = 0
    for n in range(voices + 1):
        for b, cnt in enumerate(hist[n]):
            if not cnt:
                continue
            total += cnt
            v = b - BIAS
            out = int(v / 2) if n >= threshold else v      # C truncates toward zero
            if out < DEV_LO or out > DEV_HI:
                clipped += cnt
    return clipped, total


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    current = int(sys.argv[2]) if len(sys.argv) == 3 else None
    sym = symbols(sys.argv[1])
    need = ["uvm2_smp_sum_hist", "uvm2_smp_n_hist", "uvm2_smp_clip_lo",
            "uvm2_smp_clip_hi", "uvm2_smp_dev_min", "uvm2_smp_dev_max"]
    missing = [s for s in need if s not in sym]
    if missing:
        sys.exit(f"{sys.argv[1]} has no {', '.join(missing)} — is this the build "
                 f"with the mixer telemetry still compiled in?")

    # The voice count comes from the array the firmware actually allocated, not
    # from a constant repeated here.
    voices = (sym["uvm2_smp_clip_hi"] - sym["uvm2_smp_n_hist"]) // 4 - 1
    flat = read_words(sym["uvm2_smp_sum_hist"], (voices + 1) * BUCKETS)
    hist = [flat[n * BUCKETS:(n + 1) * BUCKETS] for n in range(voices + 1)]
    nh = read_words(sym["uvm2_smp_n_hist"], voices + 1)
    clip_hi, clip_lo, dev_max, dev_min = (
        read_words(sym["uvm2_smp_clip_hi"], 1)[0],
        read_words(sym["uvm2_smp_clip_lo"], 1)[0],
        s32(read_words(sym["uvm2_smp_dev_max"], 1)[0]),
        s32(read_words(sym["uvm2_smp_dev_min"], 1)[0]))

    total = sum(nh)
    if not total:
        sys.exit("nothing recorded: no sample has been mixed since the last reset.")

    print(f"emitted after the divide:  {dev_min:+d} .. {dev_max:+d}   of {DEV_LO:+d} .. {DEV_HI:+d}")
    print(f"clamped: {clip_lo + clip_hi} of {total}  ({100.0*(clip_lo+clip_hi)/total:.3f}%)"
          f"   [{clip_lo} low, {clip_hi} high]\n")

    print("raw sum per voice count (the input to the decision):")
    print("  n    samples      %   |sum| reach          p50   p99   beyond +-8")
    for n in range(1, voices + 1):
        tot = sum(hist[n])
        if not tot:
            continue
        vals = [(b - BIAS, c) for b, c in enumerate(hist[n]) if c]
        lo, hi = vals[0][0], vals[-1][0]
        run, p50, p99 = 0, None, None
        for v, c in sorted(vals, key=lambda t: abs(t[0])):
            run += c
            if p50 is None and run >= tot * 0.50: p50 = abs(v)
            if p99 is None and run >= tot * 0.99: p99 = abs(v)
        out = sum(c for v, c in vals if v < DEV_LO or v > DEV_HI)
        print(f" {n:2d} {tot:10d}  {100.0*tot/total:5.1f}   {lo:+4d}..{hi:+4d}"
              f"        {p50:4d}  {p99:4d}   {100.0*out/tot:6.2f}%")

    print("\nwhat each divide threshold would cost, on THIS recording:")
    print("  threshold            clipped      at full scale")
    for t in range(1, voices + 2):
        clipped, tot = cost(hist, voices, t)
        full = sum(nh[n] for n in range(1, min(t, voices + 1)))
        label = ("never divide" if t > voices else
                 f"n > {t-1}" + ("   <- current" if current is not None
                                 and t - 1 == current else ""))
        print(f"  {label:22s} {100.0*clipped/tot:6.3f}%   {100.0*full/total:5.1f}%")


if __name__ == "__main__":
    main()
