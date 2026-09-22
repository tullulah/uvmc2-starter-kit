#!/usr/bin/env python3
"""Wrap a flat RP2350 RAM image in the `.um2` header the multicart loader reads.

Header format (20 bytes, every field little-endian):

    [0..4]   magic        "2CMU"
    [4..8]   version      1
    [8..12]  block count   1
    [12..16] load address  where the loader copies the payload (0x20000000 = SRAM)
    [16..20] length        payload length in 32-bit WORDS, not bytes
    [20..]   payload       the image, starting with the Cortex-M vector table
                           (word 0 = initial SP, word 1 = entry point)

THE LENGTH IS IN WORDS, and that is not a guess. The two reference images that
do launch (Asteroids_0_1a.um2 and Centipede_0_3a.um2) declare 0x8AC0 = 35520
while their payload is 142080 bytes -- exactly 35520 x 4. Write bytes here and
the loader copies a quarter of the image, which looks like a corrupt build
rather than a wrong header.
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"2CMU"
VERSION = 1
BLOCK_COUNT = 1


def build_um2(payload: bytes, load_addr: int) -> bytes:
    # A word-counted payload has to be a whole number of words.
    padding = (-len(payload)) % 4
    words = (len(payload) + padding) // 4
    header = MAGIC + struct.pack("<III", VERSION, BLOCK_COUNT, load_addr) \
             + struct.pack("<I", words)
    return header + payload + b"\x00" * padding


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="flat .bin image")
    ap.add_argument("--out", type=Path, required=True, help="output .um2")
    ap.add_argument("--load-addr", default="0x20000000",
                    help="load address (default 0x20000000, the RP2350 SRAM base)")
    args = ap.parse_args()

    payload = args.input.read_bytes()
    addr = int(args.load_addr, 0)
    image = build_um2(payload, addr)
    args.out.write_bytes(image)
    print(f"package_um2: {args.input} ({len(payload)} bytes) -> "
          f"{args.out} ({len(image)} bytes, load 0x{addr:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
