/*
 * hwtest GUI - the XUSB device controller (Mariko), for the USB serial port.
 *
 * A fork of bdk's eXtensible USB Device driver (bdk/usb/xusbd.c), reduced
 * to the one device usbser.c describes and changed in three ways:
 *  - it never blocks waiting for a PC: init() brings the controller up and
 *    arms the port, poll() handles whatever events came in since, and a
 *    PC that goes away is reported so the port can start over;
 *  - the control endpoint takes a data stage from the PC (a serial port's
 *    SET_LINE_CODING), and class requests go to usbser.c;
 *  - the configuration has a third endpoint, the serial port's interrupt
 *    IN (DCI 5): it is set up and left empty, and a PC polling it gets
 *    NAKs.
 * The controller, PHY and clock sequences are bdk's as they are.
 *
 * Copyright (c) 2020-2025 CTCaer (bdk/usb/xusbd.c)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <usb/usbd.h>
#include <usb/usb_t210.h>

#include <mem/mc.h>
#include <soc/bpmp.h>
#include <soc/clock.h>
#include <soc/fuse.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <soc/t210.h>

#include <memory_map.h>

#include "usbser.h"

#define TRB_SLOTS    16
#define LINK_TRB_IDX (TRB_SLOTS - 1)

/* Endpoint context indexes (DCI): the control endpoint is 0, OUT endpoint n
 * is 2n and IN endpoint n is 2n + 1. */
#define DCI_CTRL     0
#define DCI_BULK_OUT 2
#define DCI_BULK_IN  3
#define DCI_NOTIF    5
#define DCI_N        6

typedef enum {
    XUSB_FULL_SPEED  = 1,
    XUSB_HIGH_SPEED  = 3,
} xusb_speed_t;

typedef enum {
    EP_DISABLED = 0,
    EP_RUNNING  = 1,
} xusb_ep_status_t;

typedef enum {
    EP_TYPE_BULK_OUT = 2,
    EP_TYPE_CNTRL    = 4,
    EP_TYPE_BULK_IN  = 6,
    EP_TYPE_INTR_IN  = 7
} xusb_ep_type_t;

typedef enum {
    XUSB_DEFAULT             = 0,
    XUSB_ADDRESSED_STS_WAIT  = 1,
    XUSB_ADDRESSED           = 2,
    XUSB_CONFIGURED_STS_WAIT = 3,
    XUSB_CONFIGURED          = 4,
} xusb_dev_state_t;

typedef enum {
    XUSB_TRB_NONE        = 0,
    XUSB_TRB_NORMAL      = 1,
    XUSB_TRB_DATA        = 3,
    XUSB_TRB_STATUS      = 4,
    XUSB_TRB_LINK        = 6,
    XUSB_TRB_TRANSFER    = 32,
    XUSB_TRB_PORT_CHANGE = 34,
    XUSB_TRB_SETUP       = 63,
} xusb_trb_type_t;

typedef enum {
    XUSB_COMP_SUCCESS               = 1,
    XUSB_COMP_BABBLE_DETECTED_ERROR = 3,
    XUSB_COMP_SHORT_PKT             = 13,
} xusb_comp_code_t;

typedef struct _event_trb_t
{
    u32 rsvd0;
    u32 rsvd1;

    u32 rsvd2:24;
    u32 comp_code:8;

    u32 cycle:1;
    u32 rsvd3:9;
    u32 trb_type:6;
    u32 ep_id:5;
    u32 rsvd4:11;
} event_trb_t;

typedef struct _transfer_event_trb_t {
    u32 trb_pointer_lo;
    u32 trb_pointer_hi;

    u32 trb_tx_len:24;
    u32 comp_code:8;

    u32 cycle:1;
    u32 rsvddw3_0:1;
    u32 event_data:1;
    u32 rsvddw3_1:7;
    u32 trb_type:6;
    u32 ep_id:5;
    u32 rsvddw3_2:11;
} transfer_event_trb_t;

typedef struct _setup_event_trb_t
{
    usb_ctrl_setup_t ctrl_setup_data;

    u32 ctrl_seq_num:16;
    u32 rsvddw2_0:8;
    u32 comp_code:8;

    u32 cycle:1;
    u32 rsvddw3_0:9;
    u32 trb_type:6;
    u32 ep_id:5;
    u32 rsvddw3_1:11;
} setup_event_trb_t;

typedef struct _status_trb_t
{
    u32 rsvd0;
    u32 rsvd1;

    u32 rsvd2:22;
    u32 interrupt_target:10;

    u32 cycle:1;
    u32 ent:1;
    u32 rsvd3_0:2;
    u32 chain:1;
    u32 ioc:1;
    u32 rsvd3_1:4;
    u32 trb_type:6;
    u32 dir:1;
    u32 rsvd3_2:15;
} status_trb_t;

typedef struct _normal_trb_t
{
    u32 databufptr_lo;
    u32 databufptr_hi;

    u32 trb_tx_len:17;
    u32 td_size:5;
    u32 interrupt_target:10;

    u32 cycle:1;
    u32 ent:1;
    u32 isp:1;
    u32 no_snoop:1;
    u32 chain:1;
    u32 ioc:1;
    u32 idt:1;
    u32 rsvd0_0:2;
    u32 bei:1;
    u32 trb_type:6;
    u32 rsvd0_1:16;
} normal_trb_t;

typedef struct _data_trb_t
{
    u32 databufptr_lo;
    u32 databufptr_hi;

    u32 trb_tx_len:17;
    u32 td_size:5;
    u32 interrupt_target:10;

    u32 cycle:1;
    u32 ent:1;
    u32 isp:1;
    u32 no_snoop:1;
    u32 chain:1;
    u32 ioc:1;
    u32 rsvd0_0:4;
    u32 trb_type:6;
    u32 dir:1;
    u32 rsvd0_1:15;
} data_trb_t;

typedef struct _link_trb_t
{
    u32 rsvd0_0:4;
    u32 ring_seg_ptrlo:28;

    u32 ring_seg_ptrhi;

    u32 rsvd1_0:22;
    u32 interrupt_target:10;

    u32 cycle:1;
    u32 toggle_cycle:1;
    u32 rsvd3_0:2;
    u32 chain:1;
    u32 ioc:1;
    u32 rsvd3_1:4;
    u32 trb_type:6;
    u32 rsvd3_2:16;
} link_trb_t;

typedef struct _xusb_ep_ctx_t
{
    // Common context.
    u32 ep_state:3;
    u32 rsvddW0_0:5;
    u32 mult:2;
    u32 max_pstreams:5;
    u32 lsa:1;
    u32 interval:8;
    u32 rsvddW0_1:8;

    u32 rsvddw1_0:1;
    u32 cerr:2;
    u32 ep_type:3;
    u32 rsvddw1_1:1;
    u32 hid:1;
    u32 max_burst_size:8;
    u32 max_packet_size:16;

    u32 dcs:1;
    u32 rsvddw2_0:3;
    u32 trd_dequeueptr_lo:28;

    u32 trd_dequeueptr_hi;

    u32 avg_trb_len:16;
    u32 max_esit_payload:16;

    // Nvidia context.
    u32 event_data_txlen_acc;

    u32 cprog:8;
    u32 sbyte:7;
    u32 tp:2;
    u32 rec:1;
    u32 cec:2;
    u32 ced:1;
    u32 hsp1:1;
    u32 rty1:1;
    u32 std:1;
    u32 status:8;

    u32 data_offset;

    u32 scratch_pad0;

    u32 scratch_pad1;

    u32 cping:8;
    u32 sping:8;
    u32 toggle_cycle:2;
    u32 no_snoop:1;
    u32 ro:1;
    u32 tlm:1;
    u32 dlm:1;
    u32 hsp2:1;
    u32 rty2:1;
    u32 stop_rec_req:8;

    u32 device_addr:8;
    u32 hub_addr:8;
    u32 root_port_num:8;
    u32 slot_id:8;

    u32 routing_string:20;
    u32 speed:4;
    u32 lpu:1;
    u32 mtt:1;
    u32 hub:1;
    u32 dci:5;

    u32 tthub_slot_id:8;
    u32 ttport_num:8;
    u32 ssf:4;
    u32 sps:2;
    u32 interrupt_target:10;

    u32 frz:1;
    u32 end:1;
    u32 elm:1;
    u32 mrx:1;
    u32 ep_linklo:28;

    u32 ep_linkhi;
} xusb_ep_ctx_t;

/* The event ring's two segments, one transfer ring per endpoint and the
 * endpoint contexts, all 16-byte aligned, where bdk keeps them in IRAM. */
typedef struct {
    event_trb_t event_seg0[TRB_SLOTS];
    event_trb_t event_seg1[TRB_SLOTS];
    data_trb_t  ctrl[TRB_SLOTS];
    data_trb_t  bulk_out[TRB_SLOTS];
    data_trb_t  bulk_in[TRB_SLOTS];
    data_trb_t  notif[TRB_SLOTS];
    volatile xusb_ep_ctx_t ep_ctx[DCI_N];
} xusb_mem_t;

static xusb_mem_t *const s_mem = (xusb_mem_t *)XUSB_RING_ADDR;

typedef struct {
    data_trb_t *base, *enq, *deq;
    u32 cycle;              /* producer cycle                       */
    u32 pending;            /* TRBs queued and not yet completed    */
    u32 left;               /* bytes the last completed one did not move */
} ring_t;

static struct {
    ring_t ring[DCI_N];
    event_trb_t *event_deq;
    u32 event_ccs;
    u32 state;              /* xusb_dev_state_t                     */
    u32 ctrl_seq_num;
    u32 config_num;
    u32 port_speed;
    bool lost;

    /* The control transfer in progress. */
    u32 ep0_wait;           /* XUSB_TRB_DATA or XUSB_TRB_STATUS     */
    bool ep0_out;           /* its data stage comes from the PC     */
    usb_ctrl_setup_t ep0_setup;

    bool rx_queued;         /* a bulk OUT read is on its ring       */
} x;

static data_trb_t *_ring_mem(u32 dci)
{
    switch (dci) {
    case DCI_CTRL:     return s_mem->ctrl;
    case DCI_BULK_OUT: return s_mem->bulk_out;
    case DCI_BULK_IN:  return s_mem->bulk_in;
    default:           return s_mem->notif;
    }
}

static int _mask_wait(u32 reg, u32 mask, u32 val, u32 retries)
{
    do
    {
        if ((XUSB_DEV_XHCI(reg) & mask) == val)
            return USB_RES_OK;
        usleep(1);
        --retries;
    }
    while (retries);

    return USB_ERROR_TIMEOUT;
}

static void _event_ring_init(void)
{
    memset(s_mem->event_seg0, 0, sizeof(s_mem->event_seg0));
    memset(s_mem->event_seg1, 0, sizeof(s_mem->event_seg1));

    // Set Event Ring Segment 0 and 1 Base Addresses, and their sizes.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERST0BALO) = (u32)s_mem->event_seg0;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERST0BAHI) = 0;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERST1BALO) = (u32)s_mem->event_seg1;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERST1BAHI) = 0;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERSTSZ) = (TRB_SLOTS << 16) | TRB_SLOTS;

    x.event_deq = s_mem->event_seg0;
    x.event_ccs = 1;

    // Event Ring Enqueue and Dequeue Pointers.
    u32 evt_ring_addr = (u32)s_mem->event_seg0 & 0xFFFFFFF0;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EREPLO) = (XUSB_DEV_XHCI(XUSB_DEV_XHCI_EREPLO) & 0xE) | evt_ring_addr | XCHI_ECS;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EREPHI) = 0;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERDPLO) = (XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERDPLO) & 0xF) | evt_ring_addr;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERDPHI) = 0;
}

/* An endpoint's context and an empty transfer ring for it, closed by a Link
 * TRB back to its start. Packet sizes follow the port speed, as the
 * configuration descriptor's do. */
static void _ep_ctx_init(u32 dci)
{
    volatile xusb_ep_ctx_t *ctx = &s_mem->ep_ctx[dci];
    ring_t *r = &x.ring[dci];
    data_trb_t *base = _ring_mem(dci);
    bool hs = x.port_speed != XUSB_FULL_SPEED;

    memset((void *)ctx, 0, sizeof(xusb_ep_ctx_t));
    memset(base, 0, sizeof(data_trb_t) * TRB_SLOTS);
    r->base = r->enq = r->deq = base;
    r->cycle = 1;
    r->pending = 0;
    r->left = 0;

    ctx->ep_state = EP_RUNNING;
    ctx->dcs = 1;
    ctx->cec = 3;
    ctx->cerr = 3;

    switch (dci) {
    case DCI_CTRL:
        ctx->ep_type = EP_TYPE_CNTRL;
        ctx->avg_trb_len = 8;
        ctx->max_packet_size = 64;
        break;
    case DCI_BULK_OUT:
    case DCI_BULK_IN:
        ctx->ep_type = dci == DCI_BULK_IN ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
        ctx->avg_trb_len = 512;
        ctx->max_packet_size = hs ? 512 : 64;
        break;
    case DCI_NOTIF:
        ctx->ep_type = EP_TYPE_INTR_IN;
        ctx->avg_trb_len = USBSER_NOTIF_MPS;
        ctx->max_packet_size = USBSER_NOTIF_MPS;
        ctx->max_esit_payload = USBSER_NOTIF_MPS;
        ctx->interval = 7;          /* 2^7 x 125 us: 16 ms, as described */
        break;
    }

    ctx->trd_dequeueptr_lo = (u32)base >> 4;
    ctx->trd_dequeueptr_hi = 0;

    link_trb_t *link = (link_trb_t *)&base[LINK_TRB_IDX];
    link->toggle_cycle   = 1;
    link->ring_seg_ptrlo = (u32)base >> 4;
    link->ring_seg_ptrhi = 0;
    link->trb_type       = XUSB_TRB_LINK;
}

static int _ep_enable(u32 dci)
{
    _ep_ctx_init(dci);
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_RELOAD) = BIT(dci);
    int res = _mask_wait(XUSB_DEV_XHCI_EP_RELOAD, BIT(dci), 0, 1000);
    if (!res)
    {
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_PAUSE) &= ~BIT(dci);
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_HALT)  &= ~BIT(dci);
    }
    return res;
}

static void _ep_disable(u32 dci)
{
    volatile xusb_ep_ctx_t *ctx = &s_mem->ep_ctx[dci];
    u32 ep_mask = BIT(dci);

    // Skip if already disabled.
    if (!ctx->ep_state)
        return;

    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_HALT) |= ep_mask;
    ctx->ep_state = EP_DISABLED;

    // Wait for EP status to change, then clear it.
    _mask_wait(XUSB_DEV_XHCI_EP_STCHG, ep_mask, ep_mask, 1000);
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_STCHG) = ep_mask;
}

static void _disable_eps(void)
{
    _ep_disable(DCI_BULK_OUT);
    _ep_disable(DCI_BULK_IN);
    _ep_disable(DCI_NOTIF);

    // Device mode stop.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) &= ~XHCI_CTRL_RUN;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ST)   |=  XHCI_ST_RC;

    x.config_num = 0;
    x.state = XUSB_DEFAULT;
    x.rx_queued = false;
}

static void _init_phy(void)
{
    // Configure and enable PLLU.
    clock_enable_pllu();

    // Enable IDDQ control by software and disable UTMIPLL IDDQ.
    CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) = (CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) & 0xFFFFFFFC) | 1;

    // Set UTMIPLL dividers and config based on OSC and enable it to 960 MHz.
    clock_enable_utmipll();

    // Set UTMIP misc config.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) = (CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) & 0xFEFFFFE8) | 0x2000008 | 0x20 | 2;
    usleep(2);

    // Set OTG PAD0 calibration.
    u32 fuse_usb_calib = FUSE(FUSE_USB_CALIB);
    // Set HS_CURR_LEVEL.
    XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) = (XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) & 0xFFFFFFC0) | (fuse_usb_calib & 0x3F);
    // Set TERM_RANGE_ADJ and RPD_CTRL.
    XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) = (XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) & 0x83FFFF87) | ((fuse_usb_calib & 0x780) >> 4) | ((u32)(FUSE(FUSE_USB_CALIB_EXT) << 27) >> 1);

    // Set VREG_LEV to 1.
    XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL1) = (XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL1) & 0xFFFFFE3F) | 0x80;

    // Disable power down on usb2 ports pads.
    XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_0) &= 0xDBFFFFFF; // Clear pad power down.
    XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1) &= 0xFFFFFFFB; // Clear pad dr power down.
    XUSB_PADCTL(XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPAD0_CTL0) &= 0xFFFFFFFE; // Clear charging power down.
    XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_0) &= 0xFFFFF7FF; // Clear bias power down.
    (void)XUSB_PADCTL(XUSB_PADCTL_USB2_OTG_PAD0_CTL_1);         // Commit write.

    // Enable USB2 tracking clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_SET)            = BIT(CLK_Y_USB2_TRK);
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_USB2_HSIC_TRK) = (CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_USB2_HSIC_TRK) & 0xFFFFFF00) | 6; // Set trank divisor to 4.

    // Set tracking parameters and trigger it, twice.
    XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) = 0x451E000;
    XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) =  0x51E000;
    usleep(100);
    XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) = 0x451E000;
    usleep(3);
    XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) = 0x51E000;
    usleep(100);

    // TRK cycle done. Force PDTRK input into power down.
    XUSB_PADCTL(XUSB_PADCTL_USB2_BIAS_PAD_CTL_1) |= 0x4000000;

    // Disable USB2 tracking clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_CLR) = BIT(CLK_Y_USB2_TRK);

    // Wait for XUSB PHY to stabilize.
    usleep(30);
}

static void _init_device_clocks(void)
{
    // Disable reset to PLLU_OUT1
    CLOCK(CLK_RST_CONTROLLER_PLLU_OUTA) |= 1;
    usleep(2);

    // Enable XUSB device clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_SET) = BIT(CLK_U_XUSB_DEV);

    // Set XUSB device core clock source to PLLP for a 102MHz result.
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_DEV) = (CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_CORE_DEV) & 0x1FFFFF00) | (1 << 29) | 6;
    usleep(2);

    // Set XUSB Full-Speed logic clock source to FO 48MHz.
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) = (CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) & 0x1FFFFFFF) | (2 << 29);

    // Enable XUSB Super-Speed logic clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_SET) = BIT(CLK_W_XUSB_SS);

    // Set XUSB Super-Speed logic clock source to HSIC 480MHz for 120MHz result and source FS logic clock from Super-Speed.
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) = (CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) & 0x1FFFFF00) | (3 << 29) | 6;

    // Clear reset to XUSB device and Super-Speed logic.
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_SS);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR) = BIT(CLK_U_XUSB_DEV);
    usleep(2);
}

static void _power_down(void)
{
    // Disable clock for XUSB Super-Speed and set source to CLK_M.
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB_SS);
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_SS) &= 0x1FFFFF00;
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_CLR) = BIT(CLK_W_XUSB_SS);

    // Put XUSB device into reset.
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_SET) = BIT(CLK_U_XUSB_DEV);
    usleep(2);

    // Reset Full-Speed clock source to CLK_M and div1.
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_XUSB_FS) = 0;
    usleep(2);

    // Disable XUSB device clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_U_CLR) = BIT(CLK_U_XUSB_DEV);

    // Force UTMIP_PLL power down.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG1) &= (~BIT(15));
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) |= BIT(4) | BIT(0); // UTMIP_FORCE_PD_SAMP_A/C_POWERDOWN.

    // Force enable UTMIPLL IDDQ.
    CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) |= 3;

    // Disable PLLU.
    clock_disable_pllu();

    // Set XUSB_PADCTL clock reset.
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB_PADCTL);

    // Disable XUSB clock.
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_CLR) = BIT(CLK_W_XUSB);
}

/* A TRB onto an endpoint's ring, with the ring's cycle bit, then the
 * doorbell. A Link TRB reached is handed the cycle and followed. */
static void _queue_trb(u32 dci, void *trb)
{
    ring_t *r = &x.ring[dci];

    ((u32 *)trb)[3] = (((u32 *)trb)[3] & ~1u) | (r->cycle & 1);
    memcpy(r->enq, trb, sizeof(data_trb_t));

    data_trb_t *next = &r->enq[1];
    if (next->trb_type == XUSB_TRB_LINK)
    {
        link_trb_t *link = (link_trb_t *)next;
        link->cycle = r->cycle & 1;
        link->toggle_cycle = 1;
        next = (data_trb_t *)(link->ring_seg_ptrlo << 4);
        r->cycle ^= 1;
    }
    r->enq = next;
    r->pending++;

    // Flush data before transfer.
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_WAY, false);

    u32 target_id = (dci << 8) & 0xFFFF;
    if (dci == DCI_CTRL)
        target_id |= x.ctrl_seq_num << 16;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_DB) = target_id;
}

static bool _ctrl_idle(void)
{
    return x.ring[DCI_CTRL].enq == x.ring[DCI_CTRL].deq;
}

static void _ctrl_data(u8 *buf, u32 len, usb_dir_t dir)
{
    if (!_ctrl_idle())
        return;
    data_trb_t trb = {0};
    trb.databufptr_lo = (u32)buf;
    trb.trb_tx_len = len;
    trb.isp = 1;
    trb.ioc = 1;
    trb.trb_type = XUSB_TRB_DATA;
    trb.dir = dir;
    _queue_trb(DCI_CTRL, &trb);
    x.ep0_wait = XUSB_TRB_DATA;
}

static void _ctrl_status(usb_dir_t dir)
{
    if (dir == USB_DIR_IN && !_ctrl_idle())
        return;
    status_trb_t trb = {0};
    trb.ioc = 1;
    trb.trb_type = XUSB_TRB_STATUS;
    trb.dir = dir;
    _queue_trb(DCI_CTRL, &trb);
    x.ep0_wait = XUSB_TRB_STATUS;
}

static void _bulk(u32 dci, u8 *buf, u32 len)
{
    normal_trb_t trb = {0};
    trb.databufptr_lo = (u32)buf;
    trb.trb_tx_len = len;
    trb.isp = 1;
    trb.ioc = 1;
    trb.trb_type = XUSB_TRB_NORMAL;
    _queue_trb(dci, &trb);
}

static int _set_ep_stall(u32 dci, bool stall)
{
    u32 ep_mask = BIT(dci);
    if (stall)
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_HALT) |= ep_mask;
    else
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_HALT) &= ~ep_mask;

    // Wait for EP status to change, then clear it.
    int res = _mask_wait(XUSB_DEV_XHCI_EP_STCHG, ep_mask, ep_mask, 1000);
    if (res)
        return res;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_STCHG) = ep_mask;

    return USB_RES_OK;
}

static void _wait_ep_stopped(u32 dci)
{
    u32 ep_mask = BIT(dci);
    _mask_wait(XUSB_DEV_XHCI_EP_STOPPED, ep_mask, ep_mask, 1000);
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_STOPPED) = ep_mask;
}

/* A stage of the control transfer is over. After a data stage the status
 * stage runs the other way: IN after the PC's data, which is handed to the
 * class first, OUT after ours. */
static void _ctrl_done(u32 left)
{
    if (x.ep0_wait == XUSB_TRB_DATA)
    {
        if (x.ep0_out)
        {
            bpmp_mmu_maintenance(BPMP_MMU_MAINT_INVALID_WAY, false);
            u32 len = x.ep0_setup.wLength > left ? x.ep0_setup.wLength - left : 0;
            usbser_out(&x.ep0_setup, (const u8 *)USB_EP_CONTROL_BUF_ADDR, len);
            _ctrl_status(USB_DIR_IN);
        }
        else
            _ctrl_status(USB_DIR_OUT);
        return;
    }

    if (x.ep0_wait == XUSB_TRB_STATUS)
    {
        if (x.state == XUSB_ADDRESSED_STS_WAIT)
            x.state = XUSB_ADDRESSED;
        else if (x.state == XUSB_CONFIGURED_STS_WAIT)
            x.state = XUSB_CONFIGURED;
        x.ep0_wait = XUSB_TRB_NONE;
    }
}

static void _transfer_event(const transfer_event_trb_t *trb)
{
    u32 dci = trb->ep_id;
    if (dci >= DCI_N)
        return;

    // Advance the ring's dequeue pointer, past a Link TRB.
    ring_t *r = &x.ring[dci];
    data_trb_t *next = &r->deq[1];
    if (next->trb_type == XUSB_TRB_LINK)
        next = (data_trb_t *)(next->databufptr_lo & 0xFFFFFFF0);
    r->deq = next;
    if (r->pending)
        r->pending--;
    r->left = trb->trb_tx_len;

    switch (trb->comp_code)
    {
    case XUSB_COMP_SUCCESS:
    case XUSB_COMP_SHORT_PKT:
        if (dci == DCI_CTRL)
            _ctrl_done(trb->trb_tx_len);
        break;
    case XUSB_COMP_BABBLE_DETECTED_ERROR:
        _wait_ep_stopped(dci);
        _set_ep_stall(dci, true);
        break;
    default:
        // Any other completion ends the transfer as it is.
        break;
    }
}

static void _port_change(void)
{
    u32 status = XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC);
    u32 halt = XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTHALT);
    u32 clear_mask = XHCI_PORTSC_CEC | XHCI_PORTSC_PLC | XHCI_PORTSC_PRC | XHCI_PORTSC_WRC | XHCI_PORTSC_CSC;

    // Port Reset Change (PRC).
    if (status & XHCI_PORTSC_PRC)
    {
        status &= ~clear_mask;
        status |= XHCI_PORTSC_PRC;
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC) = status;
    }

    // Warm Port Reset (WPR).
    if (status & XHCI_PORTSC_WPR)
    {
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTHALT) &= ~XHCI_PORTHALT_HALT_LTSSM;
        (void)XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTHALT);
    }

    // Warm Port Reset Change (WRC).
    if (status & XHCI_PORTSC_WRC)
    {
        status &= ~clear_mask;
        status |= XHCI_PORTSC_WRC;
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC) = status;
    }

    // Reread port status to handle more changes.
    status = XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC);

    // Connect Status Change (CSC): the speed, and a PC that went away.
    if (status & XHCI_PORTSC_CSC)
    {
        if (status & XHCI_PORTSC_CCS)
        {
            x.port_speed = (status & XHCI_PORTSC_PS) >> 10;

            // The control endpoint's packet size for anything below Super Speed.
            volatile xusb_ep_ctx_t *ctx = &s_mem->ep_ctx[DCI_CTRL];
            ctx->avg_trb_len = 8;
            ctx->max_packet_size = 64;
        }
        else
            x.lost = true;

        status &= ~clear_mask;
        status |= XHCI_PORTSC_CSC;
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC) = status;
    }

    // Handle Config Request (STCHG_REQ).
    if (halt & XHCI_PORTHALT_STCHG_REQ)
    {
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTHALT) &= ~XHCI_PORTHALT_HALT_LTSSM;
        (void)XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTHALT);
    }

    // Reread port status to handle more changes.
    status = XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC);

    // Port link state change (PLC).
    if (status & XHCI_PORTSC_PLC)
    {
        status &= ~clear_mask;
        status |= XHCI_PORTSC_PLC;
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC) = status;
    }

    // Port configuration link error (CEC).
    if (status & XHCI_PORTSC_CEC)
    {
        status = XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC);
        status &= ~clear_mask;
        status |= XHCI_PORTSC_CEC;
        XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC) = status;
    }
}

static void _set_address(const usb_ctrl_setup_t *s)
{
    u32 addr = s->wValue & 0xFF;

    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) = (XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) & 0x80FFFFFF) | (addr << 24);
    s_mem->ep_ctx[DCI_CTRL].device_addr = addr;

    _ctrl_status(USB_DIR_IN);
    x.state = XUSB_ADDRESSED_STS_WAIT;
}

static void _set_configuration(const usb_ctrl_setup_t *s)
{
    x.config_num = s->wValue;

    if (!x.config_num)
    {
        _disable_eps();
        _ctrl_status(USB_DIR_IN);
        return;
    }

    _ep_enable(DCI_BULK_OUT);
    _ep_enable(DCI_BULK_IN);
    _ep_enable(DCI_NOTIF);
    x.rx_queued = false;

    // Device mode start.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) |= XHCI_CTRL_RUN;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ST)   |= XHCI_ST_RC;

    _ctrl_status(USB_DIR_IN);
    x.state = XUSB_CONFIGURED_STS_WAIT;
}

/* The endpoint an address names, or -1. */
static int _dci_of(u16 ep_addr)
{
    switch (ep_addr)
    {
    case 0x00:
    case 0x80:            return DCI_CTRL;
    case USBSER_EP_OUT:   return DCI_BULK_OUT;
    case USBSER_EP_IN:    return DCI_BULK_IN;
    case USBSER_EP_NOTIF: return DCI_NOTIF;
    default:              return -1;
    }
}

/* Answer on the control endpoint from a copy in its buffer. */
static void _ctrl_reply(const void *data, u32 size, u16 wLength)
{
    if (size > wLength)
        size = wLength;
    memcpy((u8 *)USB_EP_CONTROL_BUF_ADDR, data, size);
    _ctrl_data((u8 *)USB_EP_CONTROL_BUF_ADDR, size, USB_DIR_IN);
}

static void _ctrl_setup(const usb_ctrl_setup_t *s)
{
    static const u8 dev_status[2] = { USB_STATUS_DEV_SELF_POWERED, 0 };
    static const u8 zero[2] = { 0, 0 };
    u8 reply[2] = { 0, 0 };

    // Unhalt EP0 IN.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_HALT) &= ~XHCI_EP_HALT_DCI_EP0_IN;
    if (_mask_wait(XUSB_DEV_XHCI_EP_HALT, XHCI_EP_HALT_DCI_EP0_IN, 0, 1000))
        return;

    x.ep0_setup = *s;
    x.ep0_out = false;

    if ((s->bmRequestType & 0x60) == USB_SETUP_TYPE_CLASS)
    {
        u8 *buf = (u8 *)USB_EP_CONTROL_BUF_ADDR;
        u32 len = 0;
        switch (usbser_class(s, buf, &len))
        {
        case USBSER_REQ_ACK:
            _ctrl_status(USB_DIR_IN);
            return;
        case USBSER_REQ_IN:
            _ctrl_data(buf, len < s->wLength ? len : s->wLength, USB_DIR_IN);
            return;
        case USBSER_REQ_OUT:
            x.ep0_out = true;
            _ctrl_data(buf, s->wLength, USB_DIR_OUT);
            return;
        default:
            _set_ep_stall(DCI_CTRL, true);
            return;
        }
    }

    switch (s->bmRequestType)
    {
    case (USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_DEVICE):    // 0x00.
        if (s->bRequest == USB_REQUEST_SET_ADDRESS)
            _set_address(s);
        else if (s->bRequest == USB_REQUEST_SET_CONFIGURATION)
            _set_configuration(s);
        else
            _set_ep_stall(DCI_CTRL, true);
        return;

    case (USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_INTERFACE): // 0x01.
        if (s->bRequest == USB_REQUEST_SET_INTERFACE && !s->wValue)
            _ctrl_status(USB_DIR_IN);       // only alternate setting 0
        else
            _set_ep_stall(DCI_CTRL, true);
        return;

    case (USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_ENDPOINT):  // 0x02.
        if ((s->wValue & 0xFF) == USB_FEATURE_ENDPOINT_HALT &&
            (s->bRequest == USB_REQUEST_CLEAR_FEATURE || s->bRequest == USB_REQUEST_SET_FEATURE))
        {
            int dci = _dci_of(s->wIndex);
            if (dci < 0)
                break;
            _set_ep_stall(dci, s->bRequest == USB_REQUEST_SET_FEATURE);
            _ctrl_status(USB_DIR_IN);
            return;
        }
        break;

    case (USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_DEVICE):    // 0x80.
        switch (s->bRequest)
        {
        case USB_REQUEST_GET_STATUS:
            _ctrl_reply(dev_status, sizeof(dev_status), s->wLength);
            return;
        case USB_REQUEST_GET_DESCRIPTOR:
        {
            u32 len;
            const u8 *d = usbser_descriptor(s->wValue >> 8, s->wValue & 0xFF,
                                            x.port_speed != XUSB_FULL_SPEED, &len);
            if (!d)
                break;
            _ctrl_reply(d, len, s->wLength);
            return;
        }
        case USB_REQUEST_GET_CONFIGURATION:
            reply[0] = x.config_num;
            _ctrl_reply(reply, 1, s->wLength);
            return;
        }
        break;

    case (USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_INTERFACE): // 0x81.
        if (s->bRequest == USB_REQUEST_GET_INTERFACE)
        {
            _ctrl_reply(zero, 1, s->wLength);  // alternate setting 0
            return;
        }
        if (s->bRequest == USB_REQUEST_GET_STATUS)
        {
            _ctrl_reply(zero, 2, s->wLength);
            return;
        }
        break;

    case (USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_ENDPOINT):  // 0x82.
        if (s->bRequest == USB_REQUEST_GET_STATUS)
        {
            int dci = _dci_of(s->wIndex);
            if (dci < 0)
                break;
            reply[0] = (XUSB_DEV_XHCI(XUSB_DEV_XHCI_EP_HALT) & BIT(dci)) ? USB_STATUS_EP_HALTED : USB_STATUS_EP_OK;
            _ctrl_reply(reply, 2, s->wLength);
            return;
        }
        break;
    }

    _set_ep_stall(DCI_CTRL, true);
}

/* Whatever events the controller posted since the last call. */
static void _events(void)
{
    if (_mask_wait(XUSB_DEV_XHCI_ST, XHCI_ST_IP, XHCI_ST_IP, 1))
        return;

    // Clear interrupt status.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ST) |= XHCI_ST_IP;

    event_trb_t *enq = (event_trb_t *)(XUSB_DEV_XHCI(XUSB_DEV_XHCI_EREPLO) & 0xFFFFFFF0);
    volatile event_trb_t *event_trb = x.event_deq;

    while ((event_trb->cycle & 1) == x.event_ccs)
    {
        switch (event_trb->trb_type)
        {
        case XUSB_TRB_TRANSFER:
            _transfer_event((transfer_event_trb_t *)event_trb);
            break;
        case XUSB_TRB_PORT_CHANGE:
            _port_change();
            break;
        case XUSB_TRB_SETUP:
        {
            setup_event_trb_t *setup_trb = (setup_event_trb_t *)event_trb;
            usb_ctrl_setup_t setup;
            memcpy(&setup, &setup_trb->ctrl_setup_data, sizeof(usb_ctrl_setup_t));
            x.ctrl_seq_num = setup_trb->ctrl_seq_num;
            _ctrl_setup(&setup);
            break;
        }
        default:
            break;
        }

        // Next event, wrapping from the second segment's end to the first.
        if (x.event_deq == &s_mem->event_seg1[TRB_SLOTS - 1])
        {
            x.event_deq = s_mem->event_seg0;
            x.event_ccs ^= 1;
        }
        else
            x.event_deq = &x.event_deq[1];

        event_trb = x.event_deq;
        if (x.event_deq == enq)
            break;
    }

    // Clear Event Handler bit if enabled and set Dequeue pointer.
    u32 erdp = XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERDPLO) & 0xF;
    if (erdp & XHCI_ERDPLO_EHB)
        erdp |= XHCI_ERDPLO_EHB;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ERDPLO) = ((u32)x.event_deq & 0xFFFFFFF0) | erdp;
}

/* The controller up, then the port armed for a PC: bdk's xusb_device_init()
 * and the first half of its xusb_device_enumerate(), which then waits. */
static int _init(void)
{
    // Ease the stress to APB.
    bpmp_clk_rate_relaxed(true);

    // Disable USB2 device controller clocks.
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_L_SET) = BIT(CLK_L_USBD);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_L_CLR) = BIT(CLK_L_USBD);

    // Enable XUSB clock and clear Reset to XUSB Pad Control.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_W_SET) = BIT(CLK_W_XUSB);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_PADCTL);
    usleep(2);

    // USB2 Pads to XUSB.
    XUSB_PADCTL(XUSB_PADCTL_USB2_PAD_MUX) =
        (XUSB_PADCTL(XUSB_PADCTL_USB2_PAD_MUX) & ~(PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_MASK | PADCTL_USB2_PAD_MUX_USB2_OTG_PAD_PORT0_MASK)) |
        PADCTL_USB2_PAD_MUX_USB2_BIAS_PAD_XUSB | PADCTL_USB2_PAD_MUX_USB2_OTG_PAD_PORT0_XUSB;

    _init_phy();

    // Set USB2.0 Port 0 to device mode, and Super Speed Port 0 to USB2 Port 0.
    XUSB_PADCTL(XUSB_PADCTL_USB2_PORT_CAP) = (XUSB_PADCTL(XUSB_PADCTL_USB2_PORT_CAP) & ~PADCTL_USB2_PORT_CAP_PORT_0_CAP_MASK) | PADCTL_USB2_PORT_CAP_PORT_0_CAP_DEV;
    XUSB_PADCTL(XUSB_PADCTL_SS_PORT_MAP) &= ~PADCTL_SS_PORT_MAP_PORT0_MASK;

    // Power Up ID Wake up and Vbus Wake Up for UTMIP
    PMC(APBDEV_PMC_USB_AO) &= 0xFFFFFFF3;
    usleep(1);

    _init_device_clocks();

    // Restore OC.
    bpmp_clk_rate_relaxed(false);

    // Enable AHB redirect for access to IRAM for Event/EP ring buffers.
    mc_enable_ahb_redirect(1);

    // Enable XUSB device IPFS.
    XUSB_DEV_DEV(XUSB_DEV_CONFIGURATION) |= DEV_CONFIGURATION_EN_FPCI;

    // Configure PCI and BAR0 address space.
    XUSB_DEV_PCI(XUSB_CFG_1) |= CFG_1_BUS_MASTER | CFG_1_MEMORY_SPACE | CFG_1_IO_SPACE;
    usleep(1);
    XUSB_DEV_PCI(XUSB_CFG_4) = XUSB_DEV_BASE | CFG_4_ADDRESS_TYPE_32_BIT;

    // Mask SATA interrupt to MCORE.
    XUSB_DEV_DEV(XUSB_DEV_INTR_MASK) |= DEV_INTR_MASK_IP_INT_MASK;

    // AHB USB performance cfg.
    AHB_GIZMO(AHB_GIZMO_AHB_MEM) |= AHB_MEM_DONT_SPLIT_AHB_WR | AHB_MEM_ENB_FAST_REARBITRATE;
    AHB_GIZMO(AHB_GIZMO_USB3)    |= AHB_GIZMO_IMMEDIATE;
    AHB_GIZMO(AHB_ARBITRATION_PRIORITY_CTRL) = PRIORITY_CTRL_WEIGHT(7)     | PRIORITY_SELECT_USB3;
    AHB_GIZMO(AHB_AHB_MEM_PREFETCH_CFG1)     = MEM_PREFETCH_ENABLE         | MEM_PREFETCH_USB3_MST_ID |
                                               MEM_PREFETCH_ADDR_BNDRY(12) | 0x1000; // Addr boundary 64KB, Inactivity 4096 cycles.

    memset(&x, 0, sizeof(x));
    _event_ring_init();
    _ep_ctx_init(DCI_CTRL);

    // Enable events and interrupts.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) |= XHCI_CTRL_IE | XHCI_CTRL_LSE;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ECPLO) = (u32)s_mem->ep_ctx & 0xFFFFFFF0;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_ECPHI) = 0;

    // No interrupt moderation: the default delays every event by 4.62 ms.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_RT_IMOD) = 0;

    // Disable Wake events.
    XUSB_PADCTL(XUSB_PADCTL_ELPG_PROGRAM_0) = 0;
    XUSB_PADCTL(XUSB_PADCTL_ELPG_PROGRAM_1) = 0;

    // Enable overrides for VBUS and ID.
    XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) = (XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) & ~(PADCTL_USB2_VBUS_ID_VBUS_OVR_MASK | PADCTL_USB2_VBUS_ID_SRC_MASK)) |
                                            PADCTL_USB2_VBUS_ID_VBUS_OVR_EN | PADCTL_USB2_VBUS_ID_SRC_ID_OVR_EN;

    // Clear halt for LTSSM.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTHALT) &= ~XHCI_PORTHALT_HALT_LTSSM;

    // Enable device mode.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) |= XHCI_CTRL_ENABLE;

    // Override access to High/Full Speed.
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CFG_DEV_FE) = (XUSB_DEV_XHCI(XUSB_DEV_XHCI_CFG_DEV_FE) & ~XHCI_CFG_DEV_FE_PORTREGSEL_MASK) | XHCI_CFG_DEV_FE_PORTREGSEL_HSFS;

    XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC)     = (XUSB_DEV_XHCI(XUSB_DEV_XHCI_PORTSC) & ~XHCI_PORTSC_PLS_MASK) | XHCI_PORTSC_LWS | XHCI_PORTSC_PLS_RXDETECT;
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CFG_DEV_FE) &= ~XHCI_CFG_DEV_FE_PORTREGSEL_MASK;

    // Enable VBUS and set ID to Float.
    XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) = (XUSB_PADCTL(XUSB_PADCTL_USB2_VBUS_ID) & ~PADCTL_USB2_VBUS_ID_OVR_MASK) |
                                            PADCTL_USB2_VBUS_ID_OVR_FLOAT | PADCTL_USB2_VBUS_ID_VBUS_ON;

    x.state = XUSB_DEFAULT;
    return USB_RES_OK;
}

static void _end(void)
{
    _disable_eps();
    XUSB_DEV_XHCI(XUSB_DEV_XHCI_CTRL) = 0;
    _power_down();
}

static void _poll(void)            { _events(); }
static bool _configured(void)      { return x.state == XUSB_CONFIGURED; }
static bool _lost(void)            { return x.lost; }
static u32  _max_packet(void)      { return x.port_speed == XUSB_FULL_SPEED ? 64 : 512; }
static bool _tx_idle(void)         { return !x.ring[DCI_BULK_IN].pending; }

static void _tx(const u8 *buf, u32 len)
{
    u8 *dma = (u8 *)USB_EP_BULK_IN_BUF_ADDR;
    memcpy(dma, buf, len);
    _bulk(DCI_BULK_IN, dma, len);
}

/* What the last read brought, then the next read queued: one packet's
 * worth, as the PC sends no more at a time. */
static u32 _rx(u8 *buf, u32 max)
{
    ring_t *r = &x.ring[DCI_BULK_OUT];
    u32 mps = _max_packet(), n = 0;

    if (x.rx_queued && !r->pending)
    {
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_INVALID_WAY, false);
        n = mps > r->left ? mps - r->left : 0;
        if (n > max)
            n = max;
        memcpy(buf, (u8 *)USB_EP_BULK_OUT_BUF_ADDR, n);
        x.rx_queued = false;
    }
    if (!x.rx_queued)
    {
        _bulk(DCI_BULK_OUT, (u8 *)USB_EP_BULK_OUT_BUF_ADDR, mps);
        x.rx_queued = true;
    }
    return n;
}

const usbser_ctrl_t usbser_xusb = {
    .init       = _init,
    .end        = _end,
    .poll       = _poll,
    .configured = _configured,
    .lost       = _lost,
    .max_packet = _max_packet,
    .tx_idle    = _tx_idle,
    .tx         = _tx,
    .rx         = _rx,
};
