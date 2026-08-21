#!/usr/bin/env python3
"""LZ77 compressor producing the exact stream `LZ_Uncompress()` expects.

Vendored from the Payload-Injector-RCM host tools rather than imported
from it: the release CI checks out this repository on its own, so a build
step that reaches into a sibling checkout works on a developer machine and
fails in CI - which is exactly how it failed once. Everything the payload
needs to build has to live here.

The on-device decoder is hekate's vendored Basic Compression Library LZ77
(bdk/libs/compr/lz.c, Marcus Geelnard). Only the *decompressor* ships in the BDK,
so the compressor lives here on the host.

Stream format (mirrors LZ_Uncompress exactly):

    stream[0]              = marker/escape byte ESC
    <byte> != ESC          -> literal
    ESC 0x00               -> a literal ESC byte
    ESC <varint L> <varint D>
                           -> copy L bytes from (outpos - D), byte-wise, so
                              overlapping copies (run-length) are legal
    varint: big-endian 7-bit groups, high bit set means "another byte follows"
            reader: v = 0; do { b = *p++; v = (v << 7) | (b & 0x7f); }
                    while (b & 0x80)

Two rules fall out of the decoder and must be respected when encoding:
  * A match length of 0 is unrepresentable: `ESC 0x00` already means "literal
    ESC", so lengths always start at 1 (their varint's first byte is non-zero).
  * The marker should be the least frequent byte in the input, because every
    literal occurrence of it costs 2 bytes instead of 1.
"""

from __future__ import annotations

MIN_MATCH = 4          # shorter matches rarely pay for their encoding
MAX_CHAIN = 256       # candidate positions examined per hash bucket


def _varint(value: int) -> bytes:
    """Big-endian 7-bit groups, high bit set on every byte but the last."""
    if value == 0:
        return b"\x00"
    groups = []
    while value:
        groups.append(value & 0x7F)
        value >>= 7
    groups.reverse()
    return bytes(g | 0x80 for g in groups[:-1]) + bytes([groups[-1]])


def _varint_len(value: int) -> int:
    return len(_varint(value))


def _pick_marker(data: bytes) -> int:
    """Least frequent byte value - cheapest to escape."""
    hist = [0] * 256
    for b in data:
        hist[b] += 1
    return min(range(256), key=lambda b: hist[b])


def compress(data: bytes) -> bytes:
    """Compress `data` into a stream LZ_Uncompress() decodes back to it."""
    if not data:
        return b""

    esc = _pick_marker(data)
    out = bytearray([esc])
    n = len(data)

    # hash chain: 3-byte key -> recent positions (most recent first)
    chains: dict[bytes, list[int]] = {}

    def find_match(at: int):
        """Longest match for `at`, as (length, distance). (0, 0) if none."""
        if at + MIN_MATCH > n:
            return 0, 0
        best_l = 0
        best_d = 0
        limit = n - at
        for cand in chains.get(data[at:at + 3], ())[:MAX_CHAIN]:
            dist = at - cand
            if dist <= 0:
                continue
            # Overlapping matches (dist < length) are legal: the decoder
            # copies byte-by-byte, so they encode runs efficiently.
            length = 0
            while length < limit and data[cand + length] == data[at + length]:
                length += 1
            if length > best_l:
                best_l = length
                best_d = dist
                if length >= 258:
                    break
        return best_l, best_d

    def worth_it(length: int, dist: int, at: int) -> bool:
        """Is a match cheaper than the literals it replaces?
        cost(match)    = 1 (ESC) + varint(len) + varint(dist)
        cost(literals) = len, +1 for every literal that happens to be ESC
        """
        if length < MIN_MATCH:
            return False
        match_cost = 1 + _varint_len(length) + _varint_len(dist)
        literal_cost = length + data.count(esc, at, at + length)
        return match_cost < literal_cost

    pos = 0
    while pos < n:
        best_len, best_dist = find_match(pos)
        emit_match = worth_it(best_len, best_dist, pos)

        # Lazy matching: if starting one byte later yields a longer match,
        # emit this byte as a literal and take the better match next round.
        if emit_match and pos + 1 < n:
            next_len, next_dist = find_match(pos + 1)
            if next_len > best_len and worth_it(next_len, next_dist, pos + 1):
                emit_match = False

        if emit_match:
            out.append(esc)
            out += _varint(best_len)
            out += _varint(best_dist)
            advance = best_len
        else:
            b = data[pos]
            if b == esc:
                out.append(esc)
                out.append(0x00)     # literal ESC
            else:
                out.append(b)
            advance = 1

        # Index every position we pass over so later matches can find them.
        for i in range(pos, min(pos + advance, n - 2)):
            chains.setdefault(data[i:i + 3], []).insert(0, i)
        pos += advance

    return bytes(out)


def decompress(src: bytes) -> bytes:
    """Reference decoder - a direct port of LZ_Uncompress(), for round-tripping."""
    if len(src) < 1:
        return b""
    esc = src[0]
    out = bytearray()
    i = 1
    n = len(src)
    while i < n:
        b = src[i]
        if b != esc:
            out.append(b)
            i += 1
            continue
        i += 1
        if i < n and src[i] == 0:
            out.append(esc)
            i += 1
            continue

        def read_var() -> int:
            nonlocal i
            v = 0
            while True:
                byte = src[i]
                i += 1
                v = (v << 7) | (byte & 0x7F)
                if not (byte & 0x80):
                    return v

        length = read_var()
        dist = read_var()
        for _ in range(length):
            out.append(out[len(out) - dist])
    return bytes(out)


if __name__ == "__main__":
    import sys
    import zlib

    if len(sys.argv) < 2:
        print("usage: lzpack.py <file> [out]", file=sys.stderr)
        raise SystemExit(2)
    raw = open(sys.argv[1], "rb").read()
    packed = compress(raw)
    back = decompress(packed)
    ok = back == raw
    print(f"in   {len(raw)} B  crc32 {zlib.crc32(raw) & 0xFFFFFFFF:08X}")
    print(f"out  {len(packed)} B  ({100.0 * len(packed) / len(raw):.1f}%)")
    print(f"round-trip: {'OK' if ok else 'MISMATCH'}")
    if not ok:
        raise SystemExit(1)
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(packed)
