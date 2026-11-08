#!/usr/bin/env python3
"""Prove the loader's in-IRAM deflate self-extraction is overlap-safe.

loader/loader.c moves the raw deflate stream so its aligned copy ends at
IPL_RELOC_TOP and expands it forward from IPL_LOAD_ADDR, in the same
memory. This replays the stream through zlib one input byte at a time and
checks that everything written while that byte is being read stays below the
first unread input byte, with a margin. loader/inflate.c reads input only as
a symbol needs it and writes each symbol (a back-reference's whole copy
included) once it is decoded, as zlib does when fed a byte at a time.

    check_inflate.py <payload.lz> <payload_unc.bin>
"""
import sys
import zlib

IPL_LOAD_ADDR = 0x40008000
IPL_RELOC_TOP = 0x40038000
MARGIN = 256            # bytes the writer must stay below the reader


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check_inflate.py <payload.lz> <payload_unc.bin>")
    packed = open(sys.argv[1], "rb").read()
    raw = open(sys.argv[2], "rb").read()

    aligned = (len(packed) + 3) & ~3
    input_base = IPL_RELOC_TOP - aligned
    if IPL_LOAD_ADDR + len(raw) > IPL_RELOC_TOP:
        sys.exit("check_inflate: the expanded payload runs past IPL_RELOC_TOP")

    d = zlib.decompressobj(-15)
    out = bytearray()
    min_gap = 1 << 30
    for k in range(len(packed)):
        out += d.decompress(packed[k:k + 1])
        # While byte k is read, the next unread byte is k + 1; the writer
        # has reached len(out).
        gap = (input_base + k + 1) - (IPL_LOAD_ADDR + len(out))
        min_gap = min(min_gap, gap)
        if gap < MARGIN:
            sys.exit("check_inflate: unsafe overlap at input %d, output %d, gap %d"
                     % (k, len(out), gap))
    out += d.flush()
    if bytes(out) != raw:
        sys.exit("check_inflate: the stream does not expand to the payload")
    print("  self-extract overlap safe: minimum unread-input gap %d bytes "
          "(expanded %d, packed %d)" % (min_gap, len(raw), len(packed)))


if __name__ == "__main__":
    main()
