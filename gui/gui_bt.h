/*
 * hwtest GUI - a Bluetooth host for Nintendo controllers, over the
 * console's own radio (the CYW4356's Bluetooth core, H4 on UART-D).
 *
 * Enough of a stack to find a Joy-Con or a Pro Controller in pairing mode,
 * connect to it, and read its buttons and sticks: HCI, L2CAP's two HID
 * channels, and the controller's own report protocol. One controller at a
 * time. Everything is polled (gui_bt_poll) and nothing waits, apart from
 * the radio's power-up in gui_bt_open.
 */
#ifndef _GUI_BT_H_
#define _GUI_BT_H_

#include <utils/types.h>

#define BT_DEV_MAX 8

typedef enum {
    BT_OFF,           /* radio down                                   */
    BT_FAILED,        /* the radio did not come up: see gui_bt_why()  */
    BT_IDLE,          /* up, nothing going on                         */
    BT_SCANNING,      /* inquiry running                              */
    BT_NAMING,        /* asking the devices found for their names     */
    BT_CONNECTING,    /* paging the controller                        */
    BT_SECURING,      /* pairing, then encryption                     */
    BT_CHANNELS,      /* opening the HID channels                     */
    BT_SETUP,         /* telling the controller what to send          */
    BT_READY,         /* reports arriving                             */
} gui_bt_state_t;

typedef enum {
    BT_PAD_UNKNOWN,
    BT_PAD_JOYCON_L,
    BT_PAD_JOYCON_R,
    BT_PAD_PRO,
} gui_bt_pad_t;

typedef struct {
    u8 addr[6];        /* as on the air: addr[5] is the first byte shown */
    u32 cod;
    s8 rssi;
    u8 psrm;           /* page scan repetition mode, from the inquiry   */
    u16 clock_off;
    char name[40];     /* "" until its name request has answered        */
    bool named;        /* asked (whether or not it answered)            */
} gui_bt_dev_t;

typedef struct {
    gui_bt_pad_t type;
    u32 buttons;       /* jc_gamepad_rpt_t.buttons layout, bits 0-23    */
    u16 stick[4];      /* lx, ly, rx, ry, 12-bit                        */
    u8 batt;           /* 0-8, as the Joy-Con reports it                */
    bool charging;
    u32 reports;       /* 0x30 reports received                         */
    u32 simple;        /* 0x3F (plain HID) reports received             */
    u32 rate;          /* of them in the last second                    */
} gui_bt_pad_state_t;

/* Power the radio up and bring HCI up. Blocks for about half a second.
 * False when the radio does not answer: gui_bt_why() says why. */
bool gui_bt_open(void);
/* Disconnect, power the radio down, and put back what gui_bt_open changed. */
void gui_bt_close(void);
/* Read the radio and move everything along. Call it every few ms. */
void gui_bt_poll(void);

void gui_bt_scan(void);                    /* about 10 s, then the names */
void gui_bt_scan_stop(void);
bool gui_bt_connect(u32 idx);              /* one of gui_bt_dev()        */
void gui_bt_disconnect(void);

gui_bt_state_t gui_bt_state(void);
const char *gui_bt_why(void);              /* the last failure, or ""    */
const char *gui_bt_step(void);             /* what it is doing now       */
u32 gui_bt_dev_count(void);
const gui_bt_dev_t *gui_bt_dev(u32 idx);
int gui_bt_connected(void);                /* index of the device, or -1 */
const gui_bt_pad_state_t *gui_bt_pad(void);
const u8 *gui_bt_local_addr(void);         /* the radio's, after open    */
u32 gui_bt_line_errors(void);              /* UART-D overruns and frames */
/* Whether it had to be paired, and whether that pairing asked it to
 * remember this radio (bonding). */
bool gui_bt_paired(void);
bool gui_bt_bonded(void);
/* ACL packets handed to the radio, and those it says went out. */
void gui_bt_acl_stats(u32 *sent, u32 *done);
/* Bytes read from the radio, and the read loop's calls, since open. */
void gui_bt_rx_stats(u32 *bytes, u32 *polls);
/* A line for the screen's list of steps and the debug console. */
void gui_bt_note(const char *s);
/* Rail Joy-Cons sent to pairing mode (gui_btscreen.c): how many, or -1. */
int gui_bt_rail_pair(void);
/* The last steps, newest first (back = 0), NULL past the end; the sequence
 * number changes with every new one. */
const char *gui_bt_log(u32 back);
u32 gui_bt_log_seq(void);

#endif
