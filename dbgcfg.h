/*
 * hwtest - hwtest/hwtest.ini, the debug console's settings, read by both
 * hwtest.bin and the GUI. Optional: without it the console is UART-B, and
 * the GUI also sends it over USB serial.
 *
 *   console=uart-b   hwtest.bin and the GUI: uart-b (the right Joy-Con
 *                    rail), uart-c (the left rail; not on a Lite) or uart-a
 *                    (the board's test pads: cluster C, pad 3 TX, pad 2 RX)
 *   gui_usb=1        THE GUI ONLY (hwtest.bin has no USB): 1 also sends the
 *                    GUI's console over the USB-C port, a serial port on
 *                    the PC; 0 leaves USB off
 *   dp_pol=0         THE DOCK TEST ONLY: 1 drives both DisplayPort lanes
 *                    with inverted polarity (SOR_XBAR_POL), a bench test
 *                    for a console whose DP pairs are swapped; 0 normal
 *   dp_swap=0        THE DOCK TEST ONLY: 1 exchanges DP lanes 0 and 1 on
 *                    the pads (the crossbar); 0 normal
 *   wifi_ssid=NAME   THE GUI'S WI-FI SCAN ONLY, up to DBG_WIFI_MAX pairs:
 *   wifi_psk=SECRET  each wifi_ssid starts a network and the wifi_psk
 *                    under it is its passphrase (none: an open network).
 *                    A scan joins the first on its own; picking any of
 *                    them on screen fills its passphrase in. Both values
 *                    run to the end of the line, spaces, # and ; included,
 *                    and are stored in clear.
 */
#ifndef HWTEST_DBGCFG_H
#define HWTEST_DBGCFG_H

#include <utils/types.h>

#define DBG_CFG_PATH "hwtest/hwtest.ini"

#define DBG_WIFI_MAX 8

typedef struct {
    char ssid[33];
    char psk[65];       /* empty for an open network                      */
} dbg_wifi_t;

typedef struct {
    int uart;           /* UART_A, UART_B or UART_C; -1 not set           */
    int gui_usb;        /* 0 or 1; -1 not set                             */
    int dp_pol;         /* 0 or 1; -1 not set: DP lanes inverted (test)   */
    int dp_swap;        /* 0 or 1; -1 not set: DP lanes exchanged (test)  */
    char bad[16];       /* a console= value not understood, or ""         */
    dbg_wifi_t wifi[DBG_WIFI_MAX];   /* wifi_ssid/wifi_psk, in file order */
    u32        wifi_n;
} dbg_cfg_t;

/* The settings from the SD card, which must be mounted; all "not set" when
 * the file is not there. */
void dbg_cfg_read(dbg_cfg_t *c);

/* The passphrase saved for a network, or NULL when it is not in the file
 * (or has no name: a hidden network). Names match as bytes, as the chip
 * reports them; an open network answers "". */
const char *dbg_cfg_psk(const dbg_cfg_t *c, const char *ssid);

/* A UART brought up as main.c brings UART-B up for the console: 115200,
 * no line inversion, RX pulled up, and for UART-C the left rail's 5 V and
 * charge-enable, which bridge its pads to the SoC. */
void dbg_uart_up(u32 uart);

#endif
