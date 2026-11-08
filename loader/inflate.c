/*
 * hwtest - a raw deflate decoder (RFC 1951) for the self-extracting loader.
 *
 * Deflate packs the payload far tighter than the BCL LZ77 the loader used
 * (about 103 KB against 130 KB), which is what leaves room under the RCM
 * limit. Written for this loader alone, after the shape of zlib's puff.c:
 * canonical Huffman codes decoded a bit at a time, every table on the stack
 * (the loader's .bss would sit in memory the expanding payload overwrites),
 * no window of its own (the output is all in memory), no checks a trusted
 * stream built by tools/mkpayload.py does not need beyond the ones that keep
 * a damaged one from running away.
 */
#include "inflate.h"

#define MAXBITS  15
#define MAXLCODE 286
#define MAXDCODE 30
#define FIXLCODE 288

typedef struct {
    const unsigned char *in;
    unsigned char *out;
    unsigned long inpos, outpos;
    unsigned long bitbuf;
    int bitcnt;
} state_t;

typedef struct {
    short *count;     /* codes of each length, [0..MAXBITS]   */
    short *symbol;    /* symbols in canonical order           */
} huff_t;

static int _bits(state_t *s, int need)
{
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        val |= (long)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (unsigned long)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int _stored(state_t *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    unsigned len = s->in[s->inpos] | s->in[s->inpos + 1] << 8;
    unsigned nlen = s->in[s->inpos + 2] | s->in[s->inpos + 3] << 8;
    s->inpos += 4;
    if (len != (~nlen & 0xFFFF))
        return -1;
    while (len--)
        s->out[s->outpos++] = s->in[s->inpos++];
    return 0;
}

static int _decode(state_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= _bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* The canonical code for n symbols of the given lengths. Negative when a
 * length set is over-subscribed. */
static int _construct(huff_t *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (int sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++)
        if (length[sym])
            h->symbol[offs[length[sym]]++] = sym;
    return left;
}

static const short s_lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short s_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short s_dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const short s_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int _codes(state_t *s, const huff_t *lencode, const huff_t *distcode)
{
    for (;;) {
        int sym = _decode(s, lencode);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            s->out[s->outpos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int len = s_lbase[sym] + _bits(s, s_lext[sym]);
            sym = _decode(s, distcode);
            if (sym < 0 || sym >= 30)
                return -1;
            unsigned long dist = s_dbase[sym] + _bits(s, s_dext[sym]);
            if (dist > s->outpos)
                return -1;
            while (len--) {
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

static int _fixed(state_t *s)
{
    short lencnt[MAXBITS + 1], lensym[FIXLCODE];
    short distcnt[MAXBITS + 1], distsym[MAXDCODE];
    short lengths[FIXLCODE];
    huff_t lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < FIXLCODE; sym++) lengths[sym] = 8;
    _construct(&lencode, lengths, FIXLCODE);
    for (sym = 0; sym < MAXDCODE; sym++)
        lengths[sym] = 5;
    _construct(&distcode, lengths, MAXDCODE);
    return _codes(s, &lencode, &distcode);
}

static int _dynamic(state_t *s)
{
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[MAXLCODE + MAXDCODE];
    short lencnt[MAXBITS + 1], lensym[MAXLCODE];
    short distcnt[MAXBITS + 1], distsym[MAXDCODE];
    huff_t lencode = { lencnt, lensym }, distcode = { distcnt, distsym };

    int nlen = _bits(s, 5) + 257;
    int ndist = _bits(s, 5) + 1;
    int ncode = _bits(s, 4) + 4;
    if (nlen > MAXLCODE || ndist > MAXDCODE)
        return -1;
    int index = 0;
    for (; index < ncode; index++)
        lengths[order[index]] = _bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (_construct(&lencode, lengths, 19) != 0)
        return -1;

    index = 0;
    while (index < nlen + ndist) {
        int sym = _decode(s, &lencode);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[index++] = sym;
            continue;
        }
        int len = 0, rep;
        if (sym == 16) {
            if (index == 0)
                return -1;
            len = lengths[index - 1];
            rep = 3 + _bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + _bits(s, 3);
        } else {
            rep = 11 + _bits(s, 7);
        }
        if (index + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[index++] = len;
    }
    if (lengths[256] == 0)
        return -1;
    if (_construct(&lencode, lengths, nlen) < 0)
        return -1;
    if (_construct(&distcode, lengths + nlen, ndist) < 0)
        return -1;
    return _codes(s, &lencode, &distcode);
}

long inflate_raw(const unsigned char *in, unsigned char *out)
{
    state_t s = { in, out, 0, 0, 0, 0 };
    int last, err;
    do {
        last = _bits(&s, 1);
        int type = _bits(&s, 2);
        err = type == 0 ? _stored(&s) : type == 1 ? _fixed(&s)
            : type == 2 ? _dynamic(&s) : -1;
        if (err)
            return -1;
    } while (!last);
    return (long)s.outpos;
}
