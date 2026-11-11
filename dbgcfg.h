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
 */
#ifndef HWTEST_DBGCFG_H
#define HWTEST_DBGCFG_H

#include <utils/types.h>

#define DBG_CFG_PATH "hwtest/hwtest.ini"

typedef struct {
    int uart;           /* UART_A, UART_B or UART_C; -1 not set           */
    int gui_usb;        /* 0 or 1; -1 not set                             */
    int dp_pol;         /* 0 or 1; -1 not set: DP lanes inverted (test)   */
    int dp_swap;        /* 0 or 1; -1 not set: DP lanes exchanged (test)  */
    char bad[16];       /* a console= value not understood, or ""         */
} dbg_cfg_t;

/* The settings from the SD card, which must be mounted; all "not set" when
 * the file is not there. */
void dbg_cfg_read(dbg_cfg_t *c);

/* A UART brought up as main.c brings UART-B up for the console: 115200,
 * no line inversion, RX pulled up, and for UART-C the left rail's 5 V and
 * charge-enable, which bridge its pads to the SoC. */
void dbg_uart_up(u32 uart);

#endif
