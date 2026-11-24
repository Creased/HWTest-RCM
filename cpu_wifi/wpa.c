/*
 * WPA2-PSK key derivation, for the four-way handshake CPU0 has to run
 * itself.
 *
 * The chip cannot: Nintendo's firmware carries no supplicant, so
 * WLC_SET_WSEC_PMK comes back BCME_UNSUPPORTED (-23) on a console - HOS
 * does the handshake in software and so must this. What is here is the
 * arithmetic only (IEEE 802.11-2016 J.4 and 12.7.1.2); the frames and
 * the state machine are in wifi.c.
 *
 * SHA-1 rather than the security engine: the SE is the BPMP's, this runs
 * on CPU0, and a PBKDF2 of 4096 iterations is a few tens of milliseconds
 * of A57 time. Built into the CPU0 image, and compiled on the host by
 * host_tools/test_wpa.c, which checks it against the published vectors.
 */
#ifdef WPA_HOST
#include <stdint.h>
#include <string.h>
typedef uint8_t  u8;
typedef uint32_t u32;
#else
typedef unsigned char u8;
typedef unsigned int  u32;
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
#endif

#include "wpa.h"

/* ---- SHA-1 (FIPS 180-4) ---------------------------------------------- */

typedef struct {
    u32 h[5];
    u8  buf[64];
    u32 len_lo, len_hi, n;
} sha1_t;

static u32 rol(u32 v, u32 k) { return (v << k) | (v >> (32 - k)); }

static void sha1_block(sha1_t *s, const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
               ((u32)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(sha1_t *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len_lo = s->len_hi = s->n = 0;
}

static void sha1_update(sha1_t *s, const u8 *p, u32 n)
{
    u32 add = n << 3;
    if ((s->len_lo += add) < add)
        s->len_hi++;
    s->len_hi += n >> 29;
    while (n--) {
        s->buf[s->n++] = *p++;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha1_final(sha1_t *s, u8 *out)
{
    u32 lo = s->len_lo, hi = s->len_hi;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56)
        sha1_update(s, &pad, 1);
    u8 len[8] = { (u8)(hi >> 24), (u8)(hi >> 16), (u8)(hi >> 8), (u8)hi,
                  (u8)(lo >> 24), (u8)(lo >> 16), (u8)(lo >> 8), (u8)lo };
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (u8)(s->h[i] >> 24);
        out[4 * i + 1] = (u8)(s->h[i] >> 16);
        out[4 * i + 2] = (u8)(s->h[i] >> 8);
        out[4 * i + 3] = (u8)s->h[i];
    }
}

void wpa_sha1(const u8 *msg, u32 len, u8 *out20)
{
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, msg, len);
    sha1_final(&s, out20);
}

/* ---- HMAC-SHA1 (RFC 2104), up to two message pieces ------------------ */

void wpa_hmac_sha1(const u8 *key, u32 klen, const u8 *a, u32 alen,
                   const u8 *b, u32 blen, u8 *out20)
{
    u8 k[64], pad[64], inner[20];
    sha1_t s;

    memset(k, 0, sizeof(k));
    if (klen > 64)
        wpa_sha1(key, klen, k);
    else
        memcpy(k, key, klen);

    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    if (alen) sha1_update(&s, a, alen);
    if (blen) sha1_update(&s, b, blen);
    sha1_final(&s, inner);

    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5C;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out20);
}

/* ---- PBKDF2-HMAC-SHA1, the PMK from the passphrase ------------------- */

void wpa_pmk(const char *pass, u32 plen, const u8 *ssid, u32 slen, u8 *pmk32)
{
    for (u32 blk = 1; blk <= 2; blk++) {
        u8 salt[36 + 4], u[20], t[20];
        u32 n = slen > 36 ? 36 : slen;
        memcpy(salt, ssid, n);
        salt[n] = 0; salt[n + 1] = 0; salt[n + 2] = 0; salt[n + 3] = (u8)blk;
        wpa_hmac_sha1((const u8 *)pass, plen, salt, n + 4, 0, 0, u);
        memcpy(t, u, 20);
        for (u32 i = 1; i < 4096; i++) {
            wpa_hmac_sha1((const u8 *)pass, plen, u, 20, 0, 0, u);
            for (int j = 0; j < 20; j++)
                t[j] ^= u[j];
        }
        /* 32 bytes wanted: all of the first block, 12 of the second. */
        memcpy(pmk32 + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
}

/* ---- the PRF and the pairwise keys (12.7.1.2) ------------------------ */

static void wpa_prf(const u8 *key, u32 klen, const char *label,
                    const u8 *data, u32 dlen, u8 *out, u32 olen)
{
    u32 llen = 0;
    while (label[llen])
        llen++;
    for (u8 i = 0; i * 20 < olen; i++) {
        u8 in[128], dig[20];
        u32 n = 0;
        memcpy(in, label, llen);
        n = llen;
        in[n++] = 0;
        memcpy(in + n, data, dlen);
        n += dlen;
        in[n++] = i;
        wpa_hmac_sha1(key, klen, in, n, 0, 0, dig);
        u32 left = olen - i * 20;
        memcpy(out + i * 20, dig, left < 20 ? left : 20);
    }
}

void wpa_ptk(const u8 *pmk32, const u8 *aa, const u8 *spa, const u8 *anonce,
             const u8 *snonce, u8 *ptk48)
{
    /* The addresses and the nonces go in smaller first, so both ends
     * build the same input whichever of them is which. */
    u8 data[76];
    const u8 *a1 = aa, *a2 = spa;
    for (int i = 0; i < 6; i++) {
        if (aa[i] != spa[i]) { if (aa[i] > spa[i]) { a1 = spa; a2 = aa; } break; }
    }
    const u8 *n1 = anonce, *n2 = snonce;
    for (int i = 0; i < 32; i++) {
        if (anonce[i] != snonce[i]) {
            if (anonce[i] > snonce[i]) { n1 = snonce; n2 = anonce; }
            break;
        }
    }
    memcpy(data, a1, 6);
    memcpy(data + 6, a2, 6);
    memcpy(data + 12, n1, 32);
    memcpy(data + 44, n2, 32);
    wpa_prf(pmk32, 32, "Pairwise key expansion", data, sizeof(data), ptk48, 48);
}

void wpa_mic(const u8 *kck, const u8 *frame, u32 len, u8 *mic16)
{
    u8 dig[20];
    wpa_hmac_sha1(kck, 16, frame, len, 0, 0, dig);
    memcpy(mic16, dig, 16);      /* HMAC-SHA1-128, AKM 2's MIC */
}
