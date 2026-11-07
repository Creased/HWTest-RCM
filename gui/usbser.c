/*
 * hwtest GUI - the USB serial port's class: a CDC ACM device.
 *
 * The descriptors a PC reads, the requests it sends a serial port, and the
 * bytes in either direction, over whichever controller driver the SoC
 * needs (usbser.h). Output goes out only while a program on the PC holds
 * the port open (DTR set), as a UART's bytes go nowhere when nobody
 * listens; what comes in is read like a UART's, R included.
 */
#include <string.h>

#include <soc/hw_init.h>
#include <soc/t210.h>
#include <soc/timer.h>
#include <usb/usb_descriptor_types.h>
#include <utils/types.h>

#include "usbser.h"

/* pid.codes' test pair, for a device that is not released under an ID of
 * its own. */
#define USBSER_VID 0x1209
#define USBSER_PID 0x0001

static const u8 s_dev[18] = {
    18, USB_DESCRIPTOR_DEVICE, 0x00, 0x02,      /* USB 2.0                     */
    0xEF, 0x02, 0x01,                           /* a function behind an IAD    */
    64,                                         /* control endpoint packet     */
    USBSER_VID & 0xFF, USBSER_VID >> 8,
    USBSER_PID & 0xFF, USBSER_PID >> 8,
    0x00, 0x01,                                 /* release 1.00                */
    1, 2, 0,                                    /* vendor, product, no serial  */
    1,                                          /* one configuration           */
};

static const u8 s_qual[10] = {
    10, USB_DESCRIPTOR_DEVICE_QUALIFIER, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,
    0,                                          /* no other-speed configuration */
    0,
};

/* The configuration: an interface association, the communications
 * interface with its functional descriptors and notification endpoint,
 * and the data interface with its two bulk endpoints. */
#define CFG_LEN 75
#define CFG_NOTIF_INTERVAL 51
#define CFG_OUT_MPS 65
#define CFG_IN_MPS  72
static u8 s_cfg[CFG_LEN] = {
    9, USB_DESCRIPTOR_CONFIGURATION, CFG_LEN, 0, 2, 1, 0,
    0xC0, 0,                                    /* self-powered, draws nothing */

    8, 0x0B, 0, 2, 0x02, 0x02, 0x00, 0,         /* IAD: interfaces 0-1, ACM    */

    9, USB_DESCRIPTOR_INTERFACE, 0, 0, 1, 0x02, 0x02, 0x00, 0,
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10            */
    5, 0x24, 0x01, 0x00, 0x01,                  /* call management, data on 1  */
    4, 0x24, 0x02, 0x02,                        /* ACM: line coding, line state */
    5, 0x24, 0x06, 0x00, 0x01,                  /* union: 0 controls 1         */
    7, USB_DESCRIPTOR_ENDPOINT, USBSER_EP_NOTIF, 0x03, USBSER_NOTIF_MPS, 0, 8,

    9, USB_DESCRIPTOR_INTERFACE, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, USB_DESCRIPTOR_ENDPOINT, USBSER_EP_OUT, 0x02, 0x00, 0x02, 0,
    7, USB_DESCRIPTOR_ENDPOINT, USBSER_EP_IN,  0x02, 0x00, 0x02, 0,
};

static const u8 s_lang[4] = { 4, USB_DESCRIPTOR_STRING, 0x09, 0x04 };   /* en-US */
static u8 s_vendor[2 + 2 * 6], s_product[2 + 2 * 14];

static void _str(u8 *d, const char *s)
{
    u32 n = strlen(s);
    d[0] = 2 + 2 * n;
    d[1] = USB_DESCRIPTOR_STRING;
    for (u32 i = 0; i < n; i++) {
        d[2 + 2 * i] = s[i];
        d[3 + 2 * i] = 0;
    }
}

const u8 *usbser_descriptor(u8 type, u8 index, bool high_speed, u32 *len)
{
    switch (type) {
    case USB_DESCRIPTOR_DEVICE:
        *len = sizeof(s_dev);
        return s_dev;
    case USB_DESCRIPTOR_DEVICE_QUALIFIER:
        *len = sizeof(s_qual);
        return s_qual;
    case USB_DESCRIPTOR_CONFIGURATION: {
        /* Bulk packets of 512 at high speed, 64 at full speed; the
         * notification endpoint polled every 16 ms either way (2^(8-1)
         * microframes, or 16 frames). */
        u16 mps = high_speed ? 512 : 64;
        s_cfg[CFG_OUT_MPS] = s_cfg[CFG_IN_MPS] = mps & 0xFF;
        s_cfg[CFG_OUT_MPS + 1] = s_cfg[CFG_IN_MPS + 1] = mps >> 8;
        s_cfg[CFG_NOTIF_INTERVAL] = high_speed ? 8 : 16;
        *len = sizeof(s_cfg);
        return s_cfg;
    }
    case USB_DESCRIPTOR_STRING:
        if (!index) {
            *len = sizeof(s_lang);
            return s_lang;
        }
        if (index == 1 || index == 2) {
            const u8 *d = index == 1 ? s_vendor : s_product;
            *len = d[0];
            return d;
        }
        return NULL;
    default:
        return NULL;
    }
}

/* ---- the serial port's requests ---- */

#define CDC_SET_LINE_CODING        0x20
#define CDC_GET_LINE_CODING        0x21
#define CDC_SET_CONTROL_LINE_STATE 0x22
#define CDC_SEND_BREAK             0x23

/* 115200 8N1, until the PC sets another; the rate only matters to the PC. */
static u8 s_line[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };
static bool s_dtr;

usbser_req_t usbser_class(const usb_ctrl_setup_t *s, u8 *buf, u32 *len)
{
    if (s->wIndex != 0)                 /* the communications interface */
        return USBSER_REQ_STALL;
    bool out = s->bmRequestType == 0x21, in = s->bmRequestType == 0xA1;

    switch (s->bRequest) {
    case CDC_SET_LINE_CODING:
        return out && s->wLength == sizeof(s_line) ? USBSER_REQ_OUT : USBSER_REQ_STALL;
    case CDC_GET_LINE_CODING:
        if (!in)
            return USBSER_REQ_STALL;
        memcpy(buf, s_line, sizeof(s_line));
        *len = sizeof(s_line);
        return USBSER_REQ_IN;
    case CDC_SET_CONTROL_LINE_STATE:
        if (!out)
            return USBSER_REQ_STALL;
        s_dtr = s->wValue & 1;
        return USBSER_REQ_ACK;
    case CDC_SEND_BREAK:
        return out ? USBSER_REQ_ACK : USBSER_REQ_STALL;
    default:
        return USBSER_REQ_STALL;
    }
}

void usbser_out(const usb_ctrl_setup_t *s, const u8 *buf, u32 len)
{
    if (s->bRequest == CDC_SET_LINE_CODING && len == sizeof(s_line))
        memcpy(s_line, buf, len);
}

/* ---- the port ---- */

#define TX_SIZE 16384
#define RX_SIZE 4096
#define TX_DRAIN_MS 20      /* how long a full queue waits for the PC */

static const usbser_ctrl_t *s_ctrl;
static usbser_state_t s_state = USBSER_OFF;
static u8 s_tx[TX_SIZE], s_rx[RX_SIZE];
static u32 s_tx_head, s_tx_tail, s_rx_head, s_rx_tail;   /* free-running */

usbser_state_t usbser_state(void) { return s_state; }

static void _reset_port(void)
{
    s_dtr = false;
    s_tx_head = s_tx_tail = s_rx_head = s_rx_tail = 0;
}

bool usbser_start(void)
{
    if (s_ctrl)
        return true;
    if (!s_vendor[0]) {
        _str(s_vendor, "hwtest");
        _str(s_product, "hwtest console");
    }
    _reset_port();
    /* Mariko's XUSB device controller; Erista has the USB2 one. */
    s_ctrl = hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01 ? &usbser_xusb : &usbser_usb2;
    if (s_ctrl->init()) {
        s_ctrl->end();
        s_ctrl = NULL;
        s_state = USBSER_FAILED;
        return false;
    }
    s_state = USBSER_WAITING;
    return true;
}

void usbser_stop(void)
{
    if (!s_ctrl)
        return;
    s_ctrl->end();
    s_ctrl = NULL;
    s_state = USBSER_OFF;
}

void usbser_poll(void)
{
    static u8 rx[512], chunk[512];

    if (!s_ctrl)
        return;

    /* A PC that went away: the controller starts over, so the next one
     * enumerates it afresh. */
    if (s_ctrl->lost()) {
        s_ctrl->end();
        _reset_port();
        if (s_ctrl->init()) {
            s_ctrl->end();
            s_ctrl = NULL;
            s_state = USBSER_FAILED;
            return;
        }
        s_state = USBSER_WAITING;
        return;
    }

    s_ctrl->poll();
    if (!s_ctrl->configured()) {
        s_state = USBSER_WAITING;
        return;
    }
    s_state = s_dtr ? USBSER_OPEN : USBSER_CONNECTED;

    /* A packet is only taken when it fits whole: otherwise it stays in the
     * controller, which then holds the PC off (no read is queued), and
     * nothing is dropped. A file sent over the port depends on it. */
    if (RX_SIZE - (s_rx_head - s_rx_tail) >= sizeof(rx)) {
        u32 n = s_ctrl->rx(rx, sizeof(rx));
        for (u32 i = 0; i < n; i++)
            s_rx[s_rx_head++ % RX_SIZE] = rx[i];
    }

    /* Nobody listens yet: what was said is kept (the newest TX_SIZE
     * bytes, _tx_put), so a PC that opens the port later still reads the
     * start-up. */
    if (!s_dtr)
        return;
    if (s_tx_head == s_tx_tail || !s_ctrl->tx_idle())
        return;

    /* Short of a full packet, so each transfer ends on a short one and the
     * PC hands it over at once. */
    u32 len = s_tx_head - s_tx_tail, max = s_ctrl->max_packet() - 1;
    if (len > max)
        len = max;
    for (u32 i = 0; i < len; i++)
        chunk[i] = s_tx[(s_tx_tail + i) % TX_SIZE];
    s_ctrl->tx(chunk, len);
    s_tx_tail += len;
}

static void _tx_put(u8 c)
{
    if (s_tx_head - s_tx_tail == TX_SIZE && s_state != USBSER_OPEN) {
        s_tx_tail++;                    /* no PC reading: the oldest goes */
    } else if (s_tx_head - s_tx_tail == TX_SIZE) {
        /* Full: the PC gets a moment to take some, then it is dropped. */
        u32 t0 = get_tmr_ms();
        while (s_tx_head - s_tx_tail == TX_SIZE && s_state == USBSER_OPEN &&
               get_tmr_ms() - t0 < TX_DRAIN_MS)
            usbser_poll();
        if (s_tx_head - s_tx_tail == TX_SIZE)
            return;
    }
    s_tx[s_tx_head++ % TX_SIZE] = c;
}

void usbser_send(const u8 *buf, u32 n)
{
    if (s_ctrl) {
        for (u32 i = 0; i < n; i++) {
            if (buf[i] == '\n')
                _tx_put('\r');
            _tx_put(buf[i]);
        }
    }
    usbser_poll();
}

int usbser_getc(u8 *c)
{
    usbser_poll();
    if (s_rx_head == s_rx_tail)
        return 0;
    *c = s_rx[s_rx_tail++ % RX_SIZE];
    return 1;
}

u32 usbser_read(u8 *buf, u32 max)
{
    usbser_poll();
    u32 n = 0;
    while (n < max && s_rx_head != s_rx_tail)
        buf[n++] = s_rx[s_rx_tail++ % RX_SIZE];
    return n;
}

bool usbser_tx_empty(void)
{
    usbser_poll();
    return s_tx_head == s_tx_tail;
}
