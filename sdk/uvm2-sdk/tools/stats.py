#!/usr/bin/env python3
"""stats.py — read uvm2_stats off a RUNNING console over SWD, without stopping it.

    python3 tools/stats.py <game.elf>                 the core counters, one read
    python3 tools/stats.py <game.elf> --all           every field (a longer read)
    python3 tools/stats.py <game.elf> --fps [SECONDS] the real frame rate (default 10 s)
    python3 tools/stats.py <game.elf> --no-verify     skip the running-image check

The ELF is the one next to the .um2 in the CMake build directory:
build_uvm2/pico/<UVM2_NAME>.elf.

WHY probe-rs AND NOT GDB. `probe-rs read` goes through the AHB-AP and does not halt
the core: measured on the console on 2026-08-24, a knob written with `probe-rs write`
while a game ran, the telemetry kept flowing and the game kept playing. GDB DOES halt
it, and halting the core that drives the Vectrex bus is a phase violation it does not
come back from. See probe.sh, which uses GDB on purpose and says when that is fine.

THREE RULES THIS SCRIPT ENFORCES, each one paid for:

  * THE ADDRESS COMES FROM THE ELF, every run. The counters live in .bss, so adding
    one variable anywhere shifts every address after it. Addresses typed by hand from
    a previous build read back as plausible nonsense; that happened three times.

  * THE ELF MUST BE THE IMAGE THAT IS RUNNING. Two builds of the same game can put
    `uvm2_stats` 1.7 KB apart, and the other build's address reads another variable
    and still looks like a number. So before reading, a few words of one function are
    compared between the ELF and the target's RAM, and the script refuses on a
    mismatch. `--no-verify` skips it, knowingly.

  * THE READ IS ONE CALL, AND SMALL. Every probe-rs invocation attaches to the debug
    port and competes with the drawing core for the AHB bus. A panel polling every
    0.6 s pulled one game from 20 to 11 fps: a rate that does not exist when nobody is
    looking. Large block reads coincided with console freezes on another RP2350 board.
    The default reads the first 24 words; `--all` reads the whole struct and says so.

The field names are parsed from uvm2_bus.h, not listed here, so the report cannot
drift from the struct.
"""
import pathlib
import re
import struct
import subprocess
import sys
import time

CHIP = "RP235x"
SPEED = "1000"
FRAME_CYCLES = 30000          # 1.5 MHz / 50 Hz: the budget of one frame, in bus cycles
CORE_WORDS = 24               # commands .. measured_frames: the fields docs/07 explains
VERIFY_SYMBOL = "uvm2_frame_end"
VERIFY_WORDS = 8

HEADER = pathlib.Path(__file__).resolve().parent.parent / "uvm2_bus.h"


def fail(msg):
    sys.exit("stats.py: " + msg)


def fields_from_header():
    """[(name, word_count)] of uvm2_stats_t, in order. Every field is uint32_t."""
    src = HEADER.read_text()
    m = re.search(r"typedef struct \{(.*?)\}\s*uvm2_stats_t;", src, re.S)
    if not m:
        fail(f"uvm2_stats_t not found in {HEADER}")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        if not decl.startswith("uint32_t "):
            fail(f"unexpected field in uvm2_stats_t: {decl!r} (the reader assumes uint32_t)")
        for item in decl[len("uint32_t "):].split(","):
            im = re.fullmatch(r"\s*(\w+)((?:\[\d+\])*)\s*", item)
            if not im:
                fail(f"cannot parse field {item!r}")
            n = 1
            for d in re.findall(r"\[(\d+)\]", im.group(2)):
                n *= int(d)
            out.append((im.group(1), n))
    return out


def symbols(elf):
    for nm in ("arm-none-eabi-nm", "llvm-nm", "nm"):
        try:
            txt = subprocess.run([nm, elf], check=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout.decode()
            break
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    else:
        fail(f"cannot read symbols from {elf}: no working nm")
    syms = {}
    for line in txt.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+\S\s+(\S+)$", line.strip())
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    return syms


def elf_words(elf, addr, count):
    """`count` words at run-time address `addr`, as the ELF's PT_LOAD segments hold them."""
    data = pathlib.Path(elf).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        fail(f"{elf} is not a 32-bit ELF")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = \
            struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1 and p_vaddr <= addr and addr + 4 * count <= p_vaddr + p_filesz:
            return list(struct.unpack_from(f"<{count}I", data, p_offset + addr - p_vaddr))
    fail(f"0x{addr:08x} is not in any loaded segment of {elf}")


def read_words(addr, count):
    out = subprocess.run(
        ["probe-rs", "read", "--chip", CHIP, "--speed", SPEED, "b32", hex(addr), str(count)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode:
        fail("probe-rs read failed:\n" + out.stderr.decode().strip() +
             "\n(is the console on, the cartridge seated and the probe plugged in?)")
    vals = [int(t, 16) for t in out.stdout.decode().split() if re.fullmatch(r"[0-9a-fA-F]{8}", t)]
    if len(vals) != count:
        fail(f"read 0x{addr:08x}: expected {count} words, got {len(vals)}")
    return vals


def verify(elf, syms):
    if VERIFY_SYMBOL not in syms:
        fail(f"{VERIFY_SYMBOL} not in {elf}; cannot check the running image (--no-verify)")
    addr = syms[VERIFY_SYMBOL] & ~1          # Thumb: bit 0 marks the mode, not the address
    want = elf_words(elf, addr, VERIFY_WORDS)
    got = read_words(addr, VERIFY_WORDS)
    if want != got:
        fail(f"the console is NOT running {pathlib.Path(elf).name}: {VERIFY_SYMBOL} differs "
             f"between the ELF and the target's RAM.\nReading on would decode another build's "
             f"memory as this one's. Pass the ELF of the image you loaded.")


def report(fields, vals):
    named, i = {}, 0
    for name, n in fields:
        if i >= len(vals):
            break
        named[name] = vals[i] if n == 1 else vals[i:i + n]
        i += n
    for name, v in named.items():
        print(f"  {name:<18} {v if isinstance(v, list) else f'{v:>10}'}")
    print()
    d = named.get("dropped", 0)
    if d:
        print(f"  !! dropped = {d}: the command list overflowed. NOTHING on screen is evidence "
              f"until this is zero.")
    c = named.get("commands", 0)
    if c:
        print(f"  commands {c}, bus_cycles {named.get('bus_cycles', 0)} "
              f"(one 50 Hz frame is {FRAME_CYCLES})")
    mn, mx = named.get("us_frame_min", 0), named.get("us_frame_max", 0)
    if mn and mx:
        print(f"  frame period: min {mn} us ({1e6 / mn:.1f} Hz), max {mx} us ({1e6 / mx:.1f} Hz)")


def fps(stats_addr, fields, seconds):
    """Frames per second from `recals`, which advances once per frame, over a SILENT window.

    The timestamp of each read is its MIDPOINT, so the probe's own latency is not charged
    to the window, and nothing touches the debug port between the two reads."""
    off = 0
    for name, n in fields:
        if name == "recals":
            break
        off += n
    else:
        fail("no `recals` field in uvm2_stats_t")
    addr = stats_addr + 4 * off
    t0 = time.monotonic(); a = read_words(addr, 1)[0]; t1 = time.monotonic()
    time.sleep(seconds)
    t2 = time.monotonic(); b = read_words(addr, 1)[0]; t3 = time.monotonic()
    dt = (t2 + t3) / 2 - (t0 + t1) / 2
    frames = (b - a) & 0xFFFFFFFF
    if frames == 0:
        print("  recals did not move: the frame loop is NOT RUNNING (hung, or not drawing) —")
        print("  a different investigation from a slow one.")
    else:
        print(f"  {frames} frames in {dt:.2f} s = {frames / dt:.2f} fps")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    elf = args[0]
    if not pathlib.Path(elf).is_file():
        fail(f"no such file: {elf}")
    syms = symbols(elf)
    if "uvm2_stats" not in syms:
        fail(f"uvm2_stats not in {elf}")
    addr = syms["uvm2_stats"]
    fields = fields_from_header()
    total = sum(n for _, n in fields)
    if "--no-verify" not in flags:
        verify(elf, syms)
    print(f"  {pathlib.Path(elf).name}: uvm2_stats at 0x{addr:08x}")
    if "--fps" in flags:
        fps(addr, fields, float(args[1]) if len(args) > 1 else 10.0)
        return
    count = total if "--all" in flags else min(CORE_WORDS, total)
    if count > CORE_WORDS:
        print(f"  (--all: reading {count} words in one call — a long read; do not repeat it "
              f"in a loop)")
    report(fields, read_words(addr, count))


if __name__ == "__main__":
    main()
