/*
 * hwtest GUI - a Joy-Con's wired HID protocol, spoken directly.
 *
 * bdk's driver (input/joycon.c) brings a Joy-Con up on its rail and polls
 * it for its buttons and sticks, and that is all it exposes: its packet
 * code is static. The tests want more of the protocol - the calibration in
 * the flash, rumble, the motion sensor's samples, the camera's frames - so
 * this speaks the same wired format itself, two ways:
 *
 *  - alongside the driver: one report dropped into its polling, such as
 *    the calibration read or a rumble burst. The driver reads the reply as
 *    it reads any other and carries on.
 *  - instead of it (gui_jc_own): a screen that streams from a Joy-Con polls
 *    it itself, under hardware flow control, and the driver is left alone
 *    until the screen is closed. It then finds the Joy-Con silent and brings
 *    it up again, which takes a few seconds.
 *
 * The format is the driver's: a UART header (magic, size), a command header,
 * then the HID report as Bluetooth would carry it. Offsets into a report
 * count from its report id, as in dekuNukem's notes and CTCaer's jc_toolkit,
 * where the sequences come from.
 */
#ifndef HWTEST_GUI_JCWIRE_H
#define HWTEST_GUI_JCWIRE_H

#include <utils/types.h>

/* side: 0 the left Joy-Con (UART-C), 1 the right (UART-B). */

/* ---- alongside the driver ---- */

typedef struct {
    u16 cx, cy;          /* where the firmware takes the centre to be        */
    u16 xmax, ymax;      /* full travel from there, right and up             */
    u16 xmin, ymin;      /* full travel from there, left and down            */
    bool user;           /* the owner's own calibration, not the factory's   */
} jc_cal_t;

/* The stick calibration from the flash: the owner's if one has been set,
 * else the factory's. False when nothing answers (a third-party pad has no
 * such flash) or what comes back is not a calibration. */
bool gui_jc_cal_read(int side, jc_cal_t *out);

/* Rumble on (a strong, steady buzz) or off. The Joy-Con keeps the last
 * setting, so one report each way is enough. It plays nothing until rumble
 * has been enabled, which the driver does once, at the first connection:
 * a Joy-Con that was brought up again (after a sensor screen, a drop) has
 * to be asked again. */
void gui_jc_rumble_enable(int side);
void gui_jc_rumble(int side, bool on);

/* ---- instead of the driver ---- */

#define JC_RPT_LEN     49      /* a Bluetooth-sized report, as jc_toolkit sends them */
#define JC_OUT_SUBCMD  0x01    /* output: rumble and a subcommand           */
#define JC_OUT_RUMBLE  0x10    /* output: rumble only                       */
#define JC_OUT_MCU     0x11    /* output: a request to the NFC/IR controller */
#define JC_IN_SUBCMD   0x21    /* input: a subcommand's reply               */
#define JC_IN_STD      0x30    /* input: buttons, sticks, motion            */
#define JC_IN_MCU      0x31    /* input: the same plus the controller's data */

void gui_jc_own(int side);
void gui_jc_release(int side);

/* One output report; byte 1, the packet id, is filled in. */
void gui_jc_send(int side, u8 *rpt, u32 len);
/* A subcommand with its arguments, and its reply: true when acknowledged
 * within a few tries. The reply, when wanted, is the 0x21 report. */
bool gui_jc_subcmd(int side, u8 subcmd, const u8 *args, u32 len, u8 *reply);
/* Ask for an input report, as the driver does between its commands. */
void gui_jc_poll(int side);
/* The next input report, into out: its length, or 0 when none arrived in
 * wait_ms. Reports that are not HID are skipped. */
u32  gui_jc_recv(int side, u8 *out, u32 max, u32 wait_ms);
/* The NFC/IR controller's checksum: CRC-8-CCITT, polynomial 0x07. */
u8   gui_jc_crc8(const u8 *p, u32 n);

#endif
