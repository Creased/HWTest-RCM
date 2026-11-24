/*
 * cpu_wifi/wpa.c against the published vectors: FIPS 180-2's SHA-1
 * examples, RFC 2202's HMAC-SHA1 cases, and IEEE 802.11-2016 J.4.2's
 * passphrase-to-PSK mappings, which are the ones that matter - a PMK one
 * bit out and the handshake fails with nothing to say why.
 *
 *   cc -DWPA_HOST -Wall -Wextra -Werror -o test_wpa \
 *      host_tools/test_wpa.c cpu_wifi/wpa.c && ./test_wpa
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../cpu_wifi/wpa.h"

static int fails;

static void check(const char *what, const uint8_t *got, const char *hex)
{
    uint8_t want[64];
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        sscanf(hex + 2 * i, "%2x", &b);
        want[i] = (uint8_t)b;
    }
    if (memcmp(got, want, n) == 0) {
        printf("ok   %s\n", what);
        return;
    }
    printf("FAIL %s\n  want %s\n  got  ", what, hex);
    for (size_t i = 0; i < n; i++)
        printf("%02x", got[i]);
    printf("\n");
    fails++;
}

int main(void)
{
    uint8_t out[48];

    /* SHA-1, FIPS 180-2 A.1 and A.2. */
    wpa_sha1((const uint8_t *)"abc", 3, out);
    check("sha1(abc)", out, "a9993e364706816aba3e25717850c26c9cd0d89d");
    wpa_sha1((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmno"
                              "mnopnopq", 56, out);
    check("sha1(two blocks)", out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    /* HMAC-SHA1, RFC 2202 cases 1 and 2. */
    uint8_t k1[20];
    memset(k1, 0x0b, sizeof(k1));
    wpa_hmac_sha1(k1, 20, (const uint8_t *)"Hi There", 8, 0, 0, out);
    check("hmac case 1", out, "b617318655057264e28bc0b6fb378c8ef146be00");
    wpa_hmac_sha1((const uint8_t *)"Jefe", 4,
                  (const uint8_t *)"what do ya want for nothing?", 28, 0, 0, out);
    check("hmac case 2", out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    /* The same message split in two, which is how the frames arrive. */
    wpa_hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya ", 11,
                  (const uint8_t *)"want for nothing?", 17, out);
    check("hmac, split message", out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    /* PSK, IEEE 802.11-2016 J.4.2. */
    wpa_pmk("password", 8, (const uint8_t *)"IEEE", 4, out);
    check("psk password/IEEE", out,
          "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    wpa_pmk("ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, out);
    check("psk ThisIsAPassword/ThisIsASSID", out,
          "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
    /* Not a published vector: a passphrase of exactly one HMAC block and
     * the longest SSID there is, which is where an implementation tends
     * to go wrong. Checked against PBKDF2-HMAC-SHA1 (python hashlib). */
    wpa_pmk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 64,
            (const uint8_t *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, out);
    check("psk 64-byte pass / 32-byte ssid", out,
          "4fd16ee24bd1d8f9e7ebd86cbd802d0b3acfd23cb08de414da4e1690e474b857");

    /* The PTK's halves line up with the pieces the handshake uses: a
     * known PMK and nonces give a KCK the MIC can be checked against. */
    uint8_t pmk[32], ptk[48], mic[16];
    wpa_pmk("ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, pmk);
    const uint8_t aa[6]  = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5 };
    const uint8_t spa[6] = { 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5 };
    uint8_t an[32], sn[32];
    memset(an, 0x11, sizeof(an));
    memset(sn, 0x22, sizeof(sn));
    wpa_ptk(pmk, aa, spa, an, sn, ptk);
    /* Swapping the two ends must not change it: the inputs are sorted. */
    uint8_t ptk2[48];
    wpa_ptk(pmk, spa, aa, sn, an, ptk2);
    if (memcmp(ptk, ptk2, 48) == 0) {
        printf("ok   ptk is the same from either end\n");
    } else {
        printf("FAIL ptk differs between the two ends\n");
        fails++;
    }
    wpa_mic(ptk, (const uint8_t *)"eapol frame", 11, mic);
    uint8_t dig[20];
    wpa_hmac_sha1(ptk, 16, (const uint8_t *)"eapol frame", 11, 0, 0, dig);
    if (memcmp(mic, dig, 16) == 0) {
        printf("ok   mic is hmac-sha1 truncated to 128 bits\n");
    } else {
        printf("FAIL mic does not match hmac-sha1-128\n");
        fails++;
    }

    printf(fails ? "\n%d FAILED\n" : "\nall OK\n", fails);
    return fails ? 1 : 0;
}
