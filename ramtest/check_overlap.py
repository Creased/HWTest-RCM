#!/usr/bin/env python3
"""Prove the in-IRAM BCL LZ self-extraction is overlap-safe.

loader/loader.c relocates the compressed stream so its aligned copy ends at
0x40038000 and expands forward into 0x40008000.  This walks the exact BCL
stream and verifies that every output write remains below the first unread
input byte, including long back-reference copies where input does not advance.
"""
from pathlib import Path
import sys

IPL_LOAD_ADDR = 0x40008000
IPL_RELOC_TOP = 0x40038000


def read_var(data: bytes, pos: int):
    value = 0
    while True:
        if pos >= len(data):
            raise ValueError("truncated variable-length value")
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            return value, pos


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_overlap.py <payload.lz> <payload_unc.bin>")

    packed = Path(sys.argv[1]).read_bytes()
    raw_size = Path(sys.argv[2]).stat().st_size
    if not packed:
        raise SystemExit("empty compressed payload")

    aligned = (len(packed) + 3) & ~3
    input_base = IPL_RELOC_TOP - aligned
    marker = packed[0]
    inpos = 1
    outpos = 0
    minimum_gap = 1 << 30

    def check_write(last_output_offset: int):
        nonlocal minimum_gap
        gap = (input_base + inpos) - (IPL_LOAD_ADDR + last_output_offset)
        minimum_gap = min(minimum_gap, gap)
        if gap <= 0:
            raise SystemExit(
                f"unsafe self-extraction overlap: unread input reached output "
                f"at in={inpos} out={last_output_offset} gap={gap}"
            )

    while inpos < len(packed):
        symbol = packed[inpos]
        inpos += 1
        if symbol != marker:
            check_write(outpos)
            outpos += 1
            continue

        if inpos >= len(packed):
            raise SystemExit("truncated marker")
        if packed[inpos] == 0:
            inpos += 1
            check_write(outpos)
            outpos += 1
            continue

        length, inpos = read_var(packed, inpos)
        _offset, inpos = read_var(packed, inpos)
        if length:
            check_write(outpos + length - 1)
        outpos += length

    if outpos != raw_size:
        raise SystemExit(f"decoder size mismatch: stream yields {outpos}, expected {raw_size}")

    print(
        f"  self-extract overlap safe: minimum unread-input gap {minimum_gap} bytes "
        f"(expanded {raw_size}, packed {len(packed)})"
    )


if __name__ == "__main__":
    main()
