/* WPA2-PSK key derivation; see wpa.c. */
#ifndef CPU_WIFI_WPA_H
#define CPU_WIFI_WPA_H

#ifdef WPA_HOST
#include <stdint.h>
#define WPA_U8  uint8_t
#define WPA_U32 uint32_t
#else
#define WPA_U8  unsigned char
#define WPA_U32 unsigned int
#endif

void wpa_sha1(const WPA_U8 *msg, WPA_U32 len, WPA_U8 *out20);
void wpa_hmac_sha1(const WPA_U8 *key, WPA_U32 klen, const WPA_U8 *a,
                   WPA_U32 alen, const WPA_U8 *b, WPA_U32 blen, WPA_U8 *out20);
/* PBKDF2-HMAC-SHA1, 4096 iterations: the pairwise master key. */
void wpa_pmk(const char *pass, WPA_U32 plen, const WPA_U8 *ssid,
             WPA_U32 slen, WPA_U8 *pmk32);
/* KCK | KEK | TK, 16 bytes each. */
void wpa_ptk(const WPA_U8 *pmk32, const WPA_U8 *aa, const WPA_U8 *spa,
             const WPA_U8 *anonce, const WPA_U8 *snonce, WPA_U8 *ptk48);
void wpa_mic(const WPA_U8 *kck, const WPA_U8 *frame, WPA_U32 len,
             WPA_U8 *mic16);

#endif
