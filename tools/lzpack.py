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
MAX_CHAIN = 2048       # candidate positions examined per hash bucket
MAX_MATCH = 258        # a candidate this long ends the search, then is
                       # extended to its full length (zero fill runs 14 KB)
SPAN_SKIP = 64         # positions inside a match this long are not searched
SPAN_TAIL = 8          # ...except its last few, where a new match may start


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
    """Compress `data` into a stream LZ_Uncompress() decodes back to it.

    Optimal parsing: every position's match candidates are gathered first,
    then a backward pass picks, at each byte, the cheaper of "one literal"
    and "this match", with the exact stream cost of everything after it
    already known. Greedy-with-lazy was the previous parse; against the
    130392-byte injector ceiling the difference (0.5 % of the image, several
    hundred bytes) is the margin. Takes a couple of seconds, decoder
    unchanged.
    """
    if not data:
        return b""

    esc = _pick_marker(data)
    n = len(data)

    # Pass 1: for each position, the match candidates reachable through the
    # 3-byte hash chain, most recent first so the shortest distance wins
    # among equal lengths. Only lengths that beat the previous candidate are
    # kept, so the list is short and strictly increasing in length.
    #
    # Inside a long match the search is skipped and the match's own
    # continuation is the candidate. Searching there is what made long runs
    # quadratic: near the end of a zero run every one of the MAX_CHAIN
    # candidates matches to the same length, so none ends the walk early.
    chains: dict[bytes, list[int]] = {}
    cands: list[tuple] = [()] * n
    span_at = span_len = span_dist = span_end = 0
    for at in range(n):
        if at + MIN_MATCH > n:
            break
        key = data[at:at + 3]
        lst = chains.get(key)
        if lst is None:
            lst = chains[key] = []
        if at < span_end:
            cands[at] = ((span_len - (at - span_at), span_dist),)
            lst.append(at)
            continue
        res = []
        best = 0
        left = n - at
        limit = min(MAX_MATCH, left)
        seen = 0
        for cand in reversed(lst):
            seen += 1
            if seen > MAX_CHAIN:
                break
            # Overlapping matches (dist < length) are legal: the decoder
            # copies byte-by-byte, so they encode runs efficiently.
            length = 0
            while length < limit and data[cand + length] == data[at + length]:
                length += 1
            if length >= MAX_MATCH:
                while length < left and data[cand + length] == data[at + length]:
                    length += 1
            if length > best and length >= MIN_MATCH:
                best = length
                res.append((length, at - cand))
                if length >= MAX_MATCH:
                    break
        cands[at] = tuple(res)
        lst.append(at)
        if best >= SPAN_SKIP:
            span_at, span_len, span_dist = at, best, res[-1][1]
            span_end = at + best - SPAN_TAIL

    # Pass 2: cost to encode data[pos:], chosen backwards.
    #   cost(match)   = 1 (ESC) + varint(len) + varint(dist)
    #   cost(literal) = 1, or 2 for a literal ESC
    # A candidate is also tried a few bytes shorter: ending a match early
    # can land on a position with a better continuation.
    cost = [0] * (n + 1)
    choice: list = [None] * (n + 1)
    for pos in range(n - 1, -1, -1):
        c = (2 if data[pos] == esc else 1) + cost[pos + 1]
        ch = None
        for length, dist in cands[pos]:
            dist_len = _varint_len(dist)
            for sub in (length, length - 1, length - 2, length - 3):
                if sub < MIN_MATCH:
                    break
                v = 1 + _varint_len(sub) + dist_len + cost[pos + sub]
                if v < c:
                    c = v
                    ch = (sub, dist)
        cost[pos] = c
        choice[pos] = ch

    out = bytearray([esc])
    pos = 0
    while pos < n:
        ch = choice[pos]
        if ch:
            length, dist = ch
            out.append(esc)
            out += _varint(length)
            out += _varint(dist)
            pos += length
        else:
            b = data[pos]
            if b == esc:
                out.append(esc)
                out.append(0x00)     # literal ESC
            else:
                out.append(b)
            pos += 1

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
