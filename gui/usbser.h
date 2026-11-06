/*
 * hwtest GUI - a serial port on a PC, over the console's USB-C port.
 *
 * A CDC ACM device: Windows 10 and later bind usbser.sys to it on their
 * own (it shows up as "USB Serial Device (COMx)"), Linux cdc_acm, macOS
 * its CDC driver, so no driver is installed. usbser.c is the class: its
 * descriptors, the requests a PC sends to a serial port, and the bytes in
 * either direction. The device controller driver under it is a fork of
 * bdk's (usbser_xusb.c for Mariko's XUSB controller, usbser_usb2.c for
 * Erista's USB2 one), reduced to this one device and made to run from the
 * GUI's loop instead of blocking until a PC answers.
 */
#ifndef HWTEST_USBSER_H
#define HWTEST_USBSER_H

#include <utils/types.h>
#include <usb/usbd.h>

typedef enum {
    USBSER_OFF,             /* not started                               */
    USBSER_WAITING,         /* the controller is up, no PC has it yet     */
    USBSER_CONNECTED,       /* a PC configured it; no program has it open */
    USBSER_OPEN,            /* a program opened the port (DTR is set)     */
    USBSER_FAILED,          /* the controller did not come up             */
} usbser_state_t;

bool usbser_start(void);
void usbser_stop(void);
void usbser_poll(void);                 /* from the loop and every send   */
void usbser_send(const u8 *buf, u32 n); /* \n goes out as \r\n            */
int  usbser_getc(u8 *c);                /* 1 when a byte came             */
usbser_state_t usbser_state(void);

/* ---- between usbser.c and a controller driver ---- */

/* What a class request asks of the driver. */
typedef enum {
    USBSER_REQ_STALL,       /* not one of ours                           */
    USBSER_REQ_ACK,         /* no data stage: the status stage           */
    USBSER_REQ_IN,          /* send buf[0..len) to the PC, then status   */
    USBSER_REQ_OUT,         /* receive len bytes, hand them to usbser_out */
} usbser_req_t;

usbser_req_t usbser_class(const usb_ctrl_setup_t *s, u8 *buf, u32 *len);
void usbser_out(const usb_ctrl_setup_t *s, const u8 *buf, u32 len);

/* A descriptor for GET_DESCRIPTOR, or NULL (the request is stalled). The
 * configuration's packet sizes and polling interval follow the speed. */
const u8 *usbser_descriptor(u8 type, u8 index, bool high_speed, u32 *len);

/* The serial port's three endpoints, as the configuration declares them. */
#define USBSER_EP_OUT   0x01    /* bulk, data from the PC                 */
#define USBSER_EP_IN    0x81    /* bulk, data to the PC                   */
#define USBSER_EP_NOTIF 0x82    /* interrupt, serial state; never sent    */
#define USBSER_NOTIF_MPS 16

typedef struct {
    int  (*init)(void);                 /* controller up, waits for a PC  */
    void (*end)(void);
    void (*poll)(void);
    bool (*configured)(void);
    bool (*lost)(void);                 /* the PC went: start over        */
    u32  (*max_packet)(void);           /* bulk, at the speed in use      */
    bool (*tx_idle)(void);
    void (*tx)(const u8 *buf, u32 len); /* one transfer, len < max_packet */
    u32  (*rx)(u8 *buf, u32 max);       /* what came; keeps a read queued */
} usbser_ctrl_t;

extern const usbser_ctrl_t usbser_xusb, usbser_usb2;

#endif
