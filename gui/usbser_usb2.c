/*
 * hwtest GUI - the USB2 device controller (Erista), for the USB serial port.
 *
 * A fork of bdk's Enhanced USB Device driver (bdk/usb/usbd.c), reduced to
 * the one device usbser.c describes and changed in three ways:
 *  - it never blocks waiting for a PC: init() brings the controller up and
 *    attaches, and poll() handles a bus reset, a speed change or a SETUP
 *    that came in since. A PC plugged in again resets the bus, and the
 *    port is enumerated afresh from there;
 *  - the control endpoint takes a data stage from the PC (a serial port's
 *    SET_LINE_CODING), and class requests go to usbser.c;
 *  - the configuration has a third endpoint, the serial port's interrupt
 *    IN (endpoint 2): enabled and never primed, so a PC polling it gets
 *    NAKs.
 * Each endpoint has one transfer descriptor: nothing here moves more than
 * a packet or a descriptor at a time. The control stages are synchronous,
 * as in bdk: a PC completes them within microseconds. The PHY sequences are
 * bdk's as they are.
 *
 * Copyright (c) 2019-2025 CTCaer (bdk/usb/usbd.c)
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

#include <soc/bpmp.h>
#include <soc/clock.h>
#include <soc/fuse.h>
#include <soc/pmc.h>
#include <soc/timer.h>
#include <soc/t210.h>

#include <memory_map.h>

#include "usbser.h"

/* Queue heads are numbered 2n for endpoint n's OUT half and 2n + 1 for its
 * IN half. */
#define QH_CTRL_OUT 0
#define QH_CTRL_IN  1
#define QH_BULK_OUT 2
#define QH_BULK_IN  3
#define QH_NOTIF    5
#define QH_N        6

#define SYNC_TRIES  USB_XFER_SYNCED_ENUM

typedef enum
{
    USB_EP_STATUS_IDLE      = 0,
    USB_EP_STATUS_ACTIVE    = 1,
    USB_EP_STATUS_ERROR     = 2,
    USB_EP_STATUS_NO_CONFIG = 3,
    USB_EP_STATUS_STALLED   = 4,
    USB_EP_STATUS_DISABLED  = 5
} usb_ep_status_t;

/* HOSTPC1_DEVLC's port speed: 0 full, 1 low, 2 high. */
#define USB_HIGH_SPEED 2

typedef struct _dTD_t
{
    vu32 next_dTD;
    vu32 info;
    vu32 pages[5];
    vu32 reserved;
} dTD_t;

typedef struct _dQH_t
{
    vu32 ep_capabilities;
    vu32 curr_dTD_ptr;
    vu32 next_dTD_ptr;
    vu32 token;
    vu32 buffers[5];
    vu32 reserved;
    vu32 setup[2];
    vu32 gap[4];
} dQH_t;

/* Queue heads in the controller's own memory, as bdk keeps them; one dTD
 * per queue head in DRAM, where bdk keeps its. */
static volatile dQH_t *const s_qh  = (volatile dQH_t *)USB2_QH_USB2D_QH_EP_BASE;
static volatile dTD_t *const s_dtd = (volatile dTD_t *)USBD_ADDR;

static struct {
    t210_usb2d_t *regs;
    u32 port_speed;
    usb_ctrl_setup_t setup;
    u32 config_num;
    bool configured;
    bool primed[QH_N];      /* a transfer was primed on the queue head */
    u32 requested[QH_N];    /* and how many bytes it asked for         */
    bool rx_queued;
} u;

static int _wait_clear(vu32 *reg, u32 mask)
{
    u32 retries = 100000; // 200ms timeout.
    while (*reg & mask)
    {
        retries--;
        if (!retries)
            return USB_ERROR_TIMEOUT;
        usleep(1);
    }
    return USB_RES_OK;
}

static int _wait_phy_clk(void)
{
    u32 retries = 100000; // 200ms timeout.
    while (!(USB(USB1_IF_USB_SUSP_CTRL) & SUSP_CTRL_USB_PHY_CLK_VALID))
    {
        retries--;
        if (!retries)
            return USB_ERROR_INIT;
        usleep(1);
    }
    return USB_RES_OK;
}

static int _reset_phy_device_mode(void)
{
    // Clear UTMIP reset, and wait for PHY clock to get validated.
    USB(USB1_IF_USB_SUSP_CTRL) &= ~SUSP_CTRL_UTMIP_RESET;
    if (_wait_phy_clk())
        return USB_ERROR_INIT;

    // Clear all device addresses, enabled setup requests and transmit events.
    u.regs->periodiclistbase = 0;
    u.regs->endptsetupstat   = u.regs->endptsetupstat;
    u.regs->endptcomplete    = u.regs->endptcomplete;

    // Stop device controller, set it to idle and reset it.
    u.regs->usbcmd  &= ~USB2D_USBCMD_RUN;
    u.regs->usbmode &= ~USB2D_USBMODE_CM_MASK;
    u.regs->usbcmd |= USB2D_USBCMD_RESET;
    if (_wait_clear(&u.regs->usbcmd, USB2D_USBCMD_RESET))
        return USB_ERROR_INIT;

    // Wait for PHY clock to get validated after reset.
    if (_wait_phy_clk())
        return USB_ERROR_INIT;

    // Set controller to Device mode, and wait for the mode to be enabled.
    u.regs->usbmode = (u.regs->usbmode & ~USB2D_USBMODE_CM_MASK) | USB2D_USBMODE_CM_DEVICE;
    u32 retries = 100000; // 200ms timeout.
    while ((u.regs->usbmode & USB2D_USBMODE_CM_MASK) != USB2D_USBMODE_CM_DEVICE)
    {
        retries--;
        if (!retries)
            return USB_ERROR_INIT;
        usleep(1);
    }

    // Disable all interrupts.
    u.regs->usbintr = 0;

    // Set the ID pullup and disable all OTGSC interrupts.
    u.regs->otgsc  = USB2D_OTGSC_USB_ID_PULLUP;

    // Clear all relevant interrupt statuses.
    u.regs->usbsts = USB2D_USBSTS_UI  | USB2D_USBSTS_UEI | USB2D_USBSTS_PCI |
                     USB2D_USBSTS_FRI | USB2D_USBSTS_SEI | USB2D_USBSTS_AAI |
                     USB2D_USBSTS_URI | USB2D_USBSTS_SRI | USB2D_USBSTS_SLI;

    // Disable and clear all OTGSC interrupts.
    u.regs->otgsc  = USB2D_OTGSC_USB_IRQ_STS_MASK;

    // Clear EP0, EP1, EP2 setup requests.
    u.regs->endptsetupstat = 7;

    // Set all interrupts to immediate.
    u.regs->usbcmd &= ~USB2D_USBCMD_ITC_MASK;

    return USB_RES_OK;
}

static void _init_phy(void)
{
    // Configure and enable PLLU.
    clock_enable_pllu();

    // Enable USBD clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_L_SET) = BIT(CLK_L_USBD);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_L_SET) = BIT(CLK_L_USBD);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_L_CLR) = BIT(CLK_L_USBD);
    usleep(2);

    // Clear XUSB_PADCTL reset
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_CLR) = BIT(CLK_W_XUSB_PADCTL);

    // Enable USB PHY and reset for programming.
    u32 usb_susp_ctrl = USB(USB1_IF_USB_SUSP_CTRL);
    USB(USB1_IF_USB_SUSP_CTRL) = usb_susp_ctrl | SUSP_CTRL_UTMIP_RESET;
    USB(USB1_IF_USB_SUSP_CTRL) = usb_susp_ctrl | SUSP_CTRL_UTMIP_PHY_ENB | SUSP_CTRL_UTMIP_RESET;

    // Enable IDDQ control by software and disable UTMIPLL IDDQ.
    CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) = (CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) & 0xFFFFFFFC) | 1;
    usleep(10);

    // Disable crystal clock.
    USB(USB1_UTMIP_MISC_CFG1) &= 0xBFFFFFFF;
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) &= 0xBFFFFFFF;

    // Set B_SESS_VLD.
    USB(USB1_IF_USB_PHY_VBUS_SENSORS) |= 0x1000;
    USB(USB1_IF_USB_PHY_VBUS_SENSORS) |= 0x800;

    // Set UTMIPLL dividers and config based on OSC and enable it to 960 MHz.
    clock_enable_utmipll();

    // Configure UTMIP Transceiver Cells.
    u32 fuse_usb_calib = FUSE(FUSE_USB_CALIB);
    USB(USB1_UTMIP_XCVR_CFG0) = (((USB(USB1_UTMIP_XCVR_CFG0) & 0xFFFFFFF0) | (fuse_usb_calib & 0xF)) & 0xFE3FFFFF) | ((fuse_usb_calib & 0x3F) << 25 >> 29 << 22);
    USB(USB1_UTMIP_XCVR_CFG1) = (USB(USB1_UTMIP_XCVR_CFG1) & 0xFFC3FFFF) | ((fuse_usb_calib << 21) >> 28 << 18);
    USB(USB1_UTMIP_XCVR_CFG3) = (USB(USB1_UTMIP_XCVR_CFG3) & 0xFFFFC1FF) | ((FUSE(FUSE_USB_CALIB_EXT) & 0x1F) << 9);
    USB(USB1_UTMIP_XCVR_CFG0) &= 0xFFDFFFFF;
    USB(USB1_UTMIP_XCVR_CFG2) = (USB(USB1_UTMIP_XCVR_CFG2) & 0xFFFFF1FF) | 0x400;
    usleep(1);

    // Configure misc UTMIP.
    USB(USB1_UTMIP_DEBOUNCE_CFG0) = (USB(USB1_UTMIP_DEBOUNCE_CFG0) & 0xFFFF0000) | 0xBB80;
    USB(USB1_UTMIP_BIAS_CFG1)     = (USB(USB1_UTMIP_BIAS_CFG1) & 0xFFFFC0FF)     | 0x100; // when osc is 38.4KHz

    USB(USB1_UTMIP_BIAS_CFG2)  |= 2;          // UTMIP_HSSQUELCH_LEVEL_NEW: 2.
    USB(USB1_UTMIP_SPARE_CFG0) &= 0xFFFFFE67; // FUSE_HS_IREF_CAP_CFG
    USB(USB1_UTMIP_TX_CFG0)    |= 0x80000;

    USB(USB1_UTMIP_HSRX_CFG0) = (USB(USB1_UTMIP_HSRX_CFG0) & 0xF0F003FF) | 0x88000 | 0x4000; // UTMIP_PCOUNT_UPDN_DIV: From 1 to 0.
    USB(USB1_UTMIP_BIAS_CFG2) &= 0xFFFFFFF8; // UTMIP_HSSQUELCH_LEVEL_NEW: 0

    USB(USB1_UTMIP_HSRX_CFG1) = (USB(USB1_UTMIP_HSRX_CFG1) & 0xFFFFFFC1) | 0x12;
    USB(USB1_UTMIP_MISC_CFG1) |= 0x40000000;

    // Enable crystal clock.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) |= 0x40000000;

    // Enable USB2 tracking clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_SET)            = BIT(CLK_Y_USB2_TRK);
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_USB2_HSIC_TRK) = (CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_USB2_HSIC_TRK) & 0xFFFFFF00) | 6; // Set trank divisor to 4.

    USB(USB1_UTMIP_BIAS_CFG1) = (USB(USB1_UTMIP_BIAS_CFG1) & 0xFFC03F07) | 0x78000 | 0x50; // Set delays.
    USB(USB1_UTMIP_BIAS_CFG0) &= 0xFFFFFBFF; // Disable Power down bias circuit.
    usleep(1);

    // Force PDTRK input into power up, then down when the TRK cycle is done; twice.
    USB(USB1_UTMIP_BIAS_CFG1) = (USB(USB1_UTMIP_BIAS_CFG1) & 0xFFFFFFFE) | 2;
    usleep(100);
    USB(USB1_UTMIP_BIAS_CFG1) = (USB(USB1_UTMIP_BIAS_CFG1) & 0xFF7FFFFF) | 1;
    usleep(3);
    USB(USB1_UTMIP_BIAS_CFG1) = USB(USB1_UTMIP_BIAS_CFG1) & 0xFFFFFFFE;
    usleep(100);
    USB(USB1_UTMIP_BIAS_CFG1) = (USB(USB1_UTMIP_BIAS_CFG1) & 0xFF7FFFFF) | 1;

    // Disable USB2 tracking clock and configure UTMIP misc.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_CLR) = BIT(CLK_Y_USB2_TRK);
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) = (CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) & 0xFEFFFFEA) | 0x2000000 | 0x28 | 2;
    usleep(1);

    USB(USB1_UTMIP_BIAS_CFG0) &= 0xFF3FF7FF;
    usleep(1);

    // Clear power downs on UTMIP ID and VBUS wake up, PD, PD2, PDZI, PDCHRP, PDDR.
    PMC(APBDEV_PMC_USB_AO)    &= 0xFFFFFFF3; // UTMIP ID and VBUS wake up.
    usleep(1);
    USB(USB1_UTMIP_XCVR_CFG0) &= 0xFFFFBFFF; // UTMIP_FORCE_PD_POWERDOWN.
    usleep(1);
    USB(USB1_UTMIP_XCVR_CFG0) &= 0xFFFEFFFF; // UTMIP_FORCE_PD2_POWERDOWN.
    usleep(1);
    USB(USB1_UTMIP_XCVR_CFG0) &= 0xFFFBFFFF; // UTMIP_FORCE_PDZI_POWERDOWN.
    usleep(1);
    USB(USB1_UTMIP_XCVR_CFG1) &= 0xFFFFFFFB; // UTMIP_FORCE_PDCHRP_POWERDOWN.
    usleep(1);
    USB(USB1_UTMIP_XCVR_CFG1) &= 0xFFFFFFEF; // UTMIP_FORCE_PDDR_POWERDOWN.
    usleep(1);
}

static void _power_down(void)
{
    // Enable PHY low power suspend. No controller register after this.
    u.regs->hostpc1_devlc |= USB2D_HOSTPC1_DEVLC_PHCD;

    // Power down OTG and Bias circuits, and the ID detectors.
    USB(USB1_UTMIP_BIAS_CFG0) |= BIT(11) | BIT(10); // UTMIP_OTGPD, UTMIP_BIASPD.
    USB(USB1_UTMIP_BIAS_CFG0) |= BIT(23) | BIT(22); // UTMIP_IDPD_SEL, UTMIP_IDPD_VAL.

    // Power down the UTMIP transceivers.
    // UTMIP_FORCE_PDZI_POWERDOWN, UTMIP_FORCE_PD2_POWERDOWN, UTMIP_FORCE_PD_POWERDOWN.
    USB(USB1_UTMIP_XCVR_CFG0) |= BIT(18) | BIT(16) |BIT(14);
    // UTMIP_FORCE_PDDR_POWERDOWN, UTMIP_FORCE_PDCHRP_POWERDOWN, UTMIP_FORCE_PDDISC_POWERDOWN.
    USB(USB1_UTMIP_XCVR_CFG1) |= BIT(4) | BIT(2) | BIT(0);

    // Keep UTMIP in reset.
    USB(USB1_IF_USB_SUSP_CTRL) |= SUSP_CTRL_UTMIP_RESET;

    // Power down PD trunk.
    USB(USB1_UTMIP_BIAS_CFG1) |= BIT(0); //UTMIP_FORCE_PDTRK_POWERDOWN.

    // Force UTMIP_PLL power down.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG1) |= BIT(14);           // UTMIP_FORCE_PLL_ENABLE_POWERDOWN.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG1) |= BIT(12);           // UTMIP_FORCE_PLL_ACTIVE_POWERDOWN.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG2) |= BIT(4) | BIT(0); // UTMIP_FORCE_PD_SAMP_A/C_POWERDOWN.
    CLOCK(CLK_RST_CONTROLLER_UTMIP_PLL_CFG1) |= BIT(16);           // UTMIP_FORCE_PLLU_POWERDOWN.

    // Disable crystal clock.
    USB(USB1_UTMIP_MISC_CFG1) &= 0xBFFFFFFF;

    // Force enable UTMIPLL IDDQ.
    CLOCK(CLK_RST_CONTROLLER_UTMIPLL_HW_PWRDN_CFG0) |= 3;

    // Set XUSB_PADCTL reset
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_W_SET) = BIT(CLK_W_XUSB_PADCTL);

    // Disable USBD clock.
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_L_CLR) = BIT(CLK_L_USBD);

    // Disable PLLU.
    clock_disable_pllu();
}

static u32 _bit(u32 qh)
{
    return (qh & 1 ? USB2D_ENDPT_STATUS_TX_OFFSET : USB2D_ENDPT_STATUS_RX_OFFSET) << (qh >> 1);
}

static u32 _max_packet_of(u32 qh)
{
    switch (qh)
    {
    case QH_BULK_OUT:
    case QH_BULK_IN:
        return u.port_speed == USB_HIGH_SPEED ? 512 : 64;
    case QH_NOTIF:
        return USBSER_NOTIF_MPS;
    default:
        return 64;
    }
}

/* An endpoint half's queue head, type and enable: control on endpoint 0,
 * bulk on 1, interrupt on 2 (IN only). Its data toggle is reset. */
static void _ep_ctrl_init(u32 qh)
{
    u32 ep = qh >> 1;
    bool in = qh & 1;

    memset((void *)&s_qh[qh], 0, sizeof(dQH_t));
    if (!qh)
        s_qh[qh].ep_capabilities = USB_QHD_EP_CAP_IOS_ENABLE;
    s_qh[qh].next_dTD_ptr = 1; // TERMINATE_SET
    s_qh[qh].ep_capabilities |= (_max_packet_of(qh) & USB_QHD_EP_CAP_MAX_PKT_LEN_MASK) << 16;
    u.primed[qh] = false;

    if (in)
    {
        u32 type = ep == 0 ? USB2D_ENDPTCTRL_TX_EP_TYPE_CTRL :
                   ep == 1 ? USB2D_ENDPTCTRL_TX_EP_TYPE_BULK : USB2D_ENDPTCTRL_TX_EP_TYPE_INTR;
        u.regs->endptctrl[ep] = (u.regs->endptctrl[ep] & ~USB2D_ENDPTCTRL_TX_EP_TYPE_MASK) | type;
        u.regs->endptctrl[ep] &= ~USB2D_ENDPTCTRL_TX_EP_STALL;
        if (ep)
            u.regs->endptctrl[ep] |= USB2D_ENDPTCTRL_TX_EP_RESET;
        u.regs->endptctrl[ep] |= USB2D_ENDPTCTRL_TX_EP_ENABLE;
    }
    else
    {
        u32 type = ep == 0 ? USB2D_ENDPTCTRL_RX_EP_TYPE_CTRL : USB2D_ENDPTCTRL_RX_EP_TYPE_BULK;
        u.regs->endptctrl[ep] = (u.regs->endptctrl[ep] & ~USB2D_ENDPTCTRL_RX_EP_TYPE_MASK) | type;
        u.regs->endptctrl[ep] &= ~USB2D_ENDPTCTRL_RX_EP_STALL;
        if (ep)
            u.regs->endptctrl[ep] |= USB2D_ENDPTCTRL_RX_EP_RESET;
        u.regs->endptctrl[ep] |= USB2D_ENDPTCTRL_RX_EP_ENABLE;
    }
}

static int _flush(u32 mask)
{
    u.regs->endptflush = mask;
    if (_wait_clear(&u.regs->endptflush, mask))
        return USB_ERROR_TIMEOUT;

    // Wait for the endpoint to finish all transactions, then clear its primed status.
    if (_wait_clear(&u.regs->endptstatus, mask))
        return USB_ERROR_TIMEOUT;
    return _wait_clear(&u.regs->endptprime, mask);
}

static void _mark_complete(u32 qh)
{
    _flush(_bit(qh));
    memset((void *)&s_dtd[qh], 0, sizeof(dTD_t));
    memset((void *)&s_qh[qh], 0, sizeof(dQH_t));
    u.primed[qh] = false;
    u.requested[qh] = 0;
    u.regs->endptcomplete |= _bit(qh);
}

static usb_ep_status_t _ep_status(u32 qh)
{
    u32 ctrl = u.regs->endptctrl[qh >> 1];
    bool in = qh & 1;

    if (ctrl & (in ? USB2D_ENDPTCTRL_TX_EP_STALL : USB2D_ENDPTCTRL_RX_EP_STALL))
        return USB_EP_STATUS_STALLED;
    if (!(ctrl & (in ? USB2D_ENDPTCTRL_TX_EP_ENABLE : USB2D_ENDPTCTRL_RX_EP_ENABLE)))
        return USB_EP_STATUS_DISABLED;

    u32 token_error_mask = USB_QHD_TOKEN_HALTED | USB_QHD_TOKEN_BUFFER_ERROR | USB_QHD_TOKEN_XFER_ERROR;
    if (s_qh[qh].token & token_error_mask)
        return USB_EP_STATUS_ERROR;

    if ((u.regs->endptprime & _bit(qh)) || (u.regs->endptstatus & _bit(qh)))
        return USB_EP_STATUS_ACTIVE;

    return u.primed[qh] ? USB_EP_STATUS_IDLE : USB_EP_STATUS_NO_CONFIG;
}

/* One transfer on a queue head, of at most a buffer page: primed, and with
 * sync waited for. */
static int _ep_op(u32 qh, u8 *buf, u32 len, bool sync)
{
    if (!buf)
        len = 0;

    _mark_complete(qh);

    if (qh == QH_CTRL_OUT)
        s_qh[qh].ep_capabilities = USB_QHD_EP_CAP_IOS_ENABLE;
    s_qh[qh].ep_capabilities |= ((_max_packet_of(qh) & USB_QHD_EP_CAP_MAX_PKT_LEN_MASK) << 16) |
                                USB_QHD_EP_CAP_ZERO_LEN_TERM_DIS;
    s_qh[qh].next_dTD_ptr = 0; // Clear terminate bit.
    u.primed[qh] = true;
    u.requested[qh] = len;

    s_dtd[qh].info = (len << 16) | USB_QHD_TOKEN_ACTIVE;
    for (u32 i = 0; i < 4; i++)
        s_dtd[qh].pages[i] = !buf ? 0 : (u32)&buf[USB_TD_BUFFER_PAGE_SIZE * i];
    s_dtd[qh].next_dTD = 1;
    s_qh[qh].next_dTD_ptr |= (u32)&s_dtd[qh] & 0xFFFFFFE0;

    // Flush AHB prefetcher, and data before priming EP.
    AHB_GIZMO(AHB_AHB_MEM_PREFETCH_CFG1) &= ~MEM_PREFETCH_ENABLE;
    AHB_GIZMO(AHB_AHB_MEM_PREFETCH_CFG1) |=  MEM_PREFETCH_ENABLE;
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_WAY, false);

    u.regs->endptprime |= _bit(qh);

    if (!sync)
        return USB_RES_OK;

    int res = USB_RES_OK;
    usb_ep_status_t ep_status = _ep_status(qh);
    if (ep_status == USB_EP_STATUS_ACTIVE)
    {
        u32 retries = SYNC_TRIES;
        while (retries)
        {
            ep_status = _ep_status(qh);
            if (ep_status != USB_EP_STATUS_ACTIVE)
            {
                if (ep_status == USB_EP_STATUS_DISABLED)
                    res = USB2_ERROR_XFER_EP_DISABLED;
                goto out;
            }
            retries--;
            usleep(1);
        }
        res = USB_ERROR_TIMEOUT;
    }
    else if (ep_status == USB_EP_STATUS_DISABLED)
        res = USB2_ERROR_XFER_EP_DISABLED;
out:
    if (res)
        _mark_complete(qh);
    else if (_ep_status(qh) != USB_EP_STATUS_IDLE)
        res = USB_ERROR_XFER_ERROR;

    // Invalidate data after OP is done.
    if (!(qh & 1))
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_INVALID_WAY, false);

    return res;
}

static int _ack(u32 qh)
{
    return _ep_op(qh, NULL, 0, true);
}

static void _ep0_stall(void)
{
    // EP Control endpoints must be always stalled together.
    u.regs->endptctrl[0] = USB2D_ENDPTCTRL_TX_EP_ENABLE | USB2D_ENDPTCTRL_TX_EP_STALL |
                           USB2D_ENDPTCTRL_RX_EP_ENABLE | USB2D_ENDPTCTRL_RX_EP_STALL;
}

/* A halt set or cleared on an endpoint half; a cleared one restarts at
 * DATA0. */
static void _set_stall(u32 qh, bool stall)
{
    u32 ep = qh >> 1;
    u32 stall_bit = qh & 1 ? USB2D_ENDPTCTRL_TX_EP_STALL : USB2D_ENDPTCTRL_RX_EP_STALL;
    u32 reset_bit = qh & 1 ? USB2D_ENDPTCTRL_TX_EP_RESET : USB2D_ENDPTCTRL_RX_EP_RESET;

    if (stall)
        u.regs->endptctrl[ep] |= stall_bit;
    else
    {
        u.regs->endptctrl[ep] &= ~stall_bit;
        if (ep)
            u.regs->endptctrl[ep] |= reset_bit;
    }
}

static void _disable_eps(void)
{
    _flush(USB_EP_ALL);
    for (u32 ep = 1; ep <= 2; ep++)
    {
        _set_stall(ep * 2, false);
        _set_stall(ep * 2 + 1, false);
        u.regs->endptctrl[ep] = 0;
    }

    u.config_num = 0;
    u.configured = false;
    u.rx_queued = false;
}

static int _ep0_init(void)
{
    memset((void *)s_qh,  0, sizeof(dQH_t) * QH_N);
    memset((void *)s_dtd, 0, sizeof(dTD_t) * QH_N);

    u.regs->asynclistaddr = (u32)s_qh;

    _ep_ctrl_init(QH_CTRL_OUT);
    _ep_ctrl_init(QH_CTRL_IN);

    // Disable Auto Low Power.
    u.regs->hostpc1_devlc &= ~USB2D_HOSTPC1_DEVLC_ASUS;

    // Initiate an attach event.
    u.regs->usbcmd |= USB2D_USBCMD_RUN;

    u32 retries = 100000; // 200ms timeout.
    while (!(u.regs->usbcmd & USB2D_USBCMD_RUN))
    {
        retries--;
        if (!retries)
            return USB_ERROR_TIMEOUT;
        usleep(1);
    }

    return USB_RES_OK;
}

/* Answer on the control endpoint from a copy in its buffer, then the
 * status stage from the PC. */
static void _ctrl_reply(const void *data, u32 size)
{
    u8 *buf = (u8 *)USB_EP_CONTROL_BUF_ADDR;
    if (size > u.setup.wLength)
        size = u.setup.wLength;
    memcpy(buf, data, size);
    if (!_ep_op(QH_CTRL_IN, buf, size, true))
        _ack(QH_CTRL_OUT);
}

static int _qh_of(u16 ep_addr)
{
    switch (ep_addr)
    {
    case 0x00:            return QH_CTRL_OUT;
    case 0x80:            return QH_CTRL_IN;
    case USBSER_EP_OUT:   return QH_BULK_OUT;
    case USBSER_EP_IN:    return QH_BULK_IN;
    case USBSER_EP_NOTIF: return QH_NOTIF;
    default:              return -1;
    }
}

static void _ctrl_setup(void)
{
    static const u8 dev_status[2] = { USB_STATUS_DEV_SELF_POWERED, 0 };
    static const u8 zero[2] = { 0, 0 };
    const usb_ctrl_setup_t *s = &u.setup;
    u8 reply[2] = { 0, 0 };

    if ((s->bmRequestType & 0x60) == USB_SETUP_TYPE_CLASS)
    {
        u8 *buf = (u8 *)USB_EP_CONTROL_BUF_ADDR;
        u32 len = 0;
        switch (usbser_class(s, buf, &len))
        {
        case USBSER_REQ_ACK:
            _ack(QH_CTRL_IN);
            return;
        case USBSER_REQ_IN:
            if (len > s->wLength)
                len = s->wLength;
            if (!_ep_op(QH_CTRL_IN, buf, len, true))
                _ack(QH_CTRL_OUT);
            return;
        case USBSER_REQ_OUT:
            if (_ep_op(QH_CTRL_OUT, buf, s->wLength, true))
                return;
            usbser_out(s, buf, u.requested[QH_CTRL_OUT] - (s_qh[QH_CTRL_OUT].token >> 16));
            _ack(QH_CTRL_IN);
            return;
        default:
            _ep0_stall();
            return;
        }
    }

    switch (s->bmRequestType)
    {
    case (USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_DEVICE):    // 0x00.
        if (s->bRequest == USB_REQUEST_SET_ADDRESS)
        {
            // Set USB address for device mode, after the status stage.
            if (!_ack(QH_CTRL_IN))
                u.regs->periodiclistbase = (u.regs->periodiclistbase & 0x1FFFFFF) | ((s->wValue & 0xFF) << 25);
            return;
        }
        if (s->bRequest == USB_REQUEST_SET_CONFIGURATION)
        {
            if (_ack(QH_CTRL_IN))
                return;
            u.config_num = s->wValue;
            if (!u.config_num)
            {
                _disable_eps();
                return;
            }
            _ep_ctrl_init(QH_BULK_OUT);
            _ep_ctrl_init(QH_BULK_IN);
            _ep_ctrl_init(QH_NOTIF);
            u.rx_queued = false;
            u.configured = true;
            return;
        }
        break;

    case (USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_INTERFACE): // 0x01.
        if (s->bRequest == USB_REQUEST_SET_INTERFACE && !s->wValue)
        {
            _ack(QH_CTRL_IN);               // only alternate setting 0
            return;
        }
        break;

    case (USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_ENDPOINT):  // 0x02.
        if ((s->wValue & 0xFF) == USB_FEATURE_ENDPOINT_HALT &&
            (s->bRequest == USB_REQUEST_CLEAR_FEATURE || s->bRequest == USB_REQUEST_SET_FEATURE))
        {
            int qh = _qh_of(s->wIndex);
            if (qh < 0)
                break;
            _set_stall(qh, s->bRequest == USB_REQUEST_SET_FEATURE);
            _ack(QH_CTRL_IN);
            return;
        }
        break;

    case (USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_DEVICE):    // 0x80.
        switch (s->bRequest)
        {
        case USB_REQUEST_GET_STATUS:
            _ctrl_reply(dev_status, sizeof(dev_status));
            return;
        case USB_REQUEST_GET_DESCRIPTOR:
        {
            u32 len;
            const u8 *d = usbser_descriptor(s->wValue >> 8, s->wValue & 0xFF,
                                            u.port_speed == USB_HIGH_SPEED, &len);
            if (!d)
                break;
            _ctrl_reply(d, len);
            return;
        }
        case USB_REQUEST_GET_CONFIGURATION:
            reply[0] = u.config_num;
            _ctrl_reply(reply, 1);
            return;
        }
        break;

    case (USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_INTERFACE): // 0x81.
        if (s->bRequest == USB_REQUEST_GET_INTERFACE)
        {
            _ctrl_reply(zero, 1);           // alternate setting 0
            return;
        }
        if (s->bRequest == USB_REQUEST_GET_STATUS)
        {
            _ctrl_reply(zero, 2);
            return;
        }
        break;

    case (USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_ENDPOINT):  // 0x82.
        if (s->bRequest == USB_REQUEST_GET_STATUS)
        {
            int qh = _qh_of(s->wIndex);
            if (qh < 0)
                break;
            reply[0] = _ep_status(qh) == USB_EP_STATUS_STALLED ? USB_STATUS_EP_HALTED : USB_STATUS_EP_OK;
            _ctrl_reply(reply, 2);
            return;
        }
        break;
    }

    _ep0_stall();
}

/* The controller up, then the port attached for a PC: bdk's
 * usb_device_init() and the start of its enumeration, which then waits. */
static int _init(void)
{
    // Ease the stress to APB.
    bpmp_clk_rate_relaxed(true);
    _init_phy();
    bpmp_clk_rate_relaxed(false);

    // AHB USB performance cfg.
    AHB_GIZMO(AHB_GIZMO_AHB_MEM) |= AHB_MEM_DONT_SPLIT_AHB_WR | AHB_MEM_ENB_FAST_REARBITRATE;
    AHB_GIZMO(AHB_GIZMO_USB)     |= AHB_GIZMO_IMMEDIATE;
    AHB_GIZMO(AHB_ARBITRATION_PRIORITY_CTRL) = PRIORITY_CTRL_WEIGHT(7) | PRIORITY_SELECT_USB;
    AHB_GIZMO(AHB_AHB_MEM_PREFETCH_CFG1)     = MEM_PREFETCH_ENABLE     | MEM_PREFETCH_USB_MST_ID |
                                               MEM_PREFETCH_ADDR_BNDRY(12) | 0x1000; // Addr boundary 64KB, Inactivity 4096 cycles.

    memset(&u, 0, sizeof(u));
    u.regs = (t210_usb2d_t *)USB_OTG_BASE;

    int res = _reset_phy_device_mode();
    if (res)
        return res;
    return _ep0_init();
}

static void _end(void)
{
    _disable_eps();

    // Stop device controller, and enable PHY auto low power suspend.
    u.regs->usbcmd &= ~USB2D_USBCMD_RUN;
    u.regs->hostpc1_devlc |= USB2D_HOSTPC1_DEVLC_ASUS;

    _power_down();
}

/* A bus reset (a PC plugged in, or starting over), the speed, then a SETUP
 * if one is waiting. */
static void _poll(void)
{
    u32 usb_status_irqs = u.regs->usbsts;
    u.regs->usbsts = usb_status_irqs;

    if (usb_status_irqs & USB2D_USBSTS_URI)
    {
        // Clear all device addresses, enabled setup requests, transmit events and flush all endpoints.
        u.regs->periodiclistbase = 0;
        u.regs->endptsetupstat   = u.regs->endptsetupstat;
        u.regs->endptcomplete    = u.regs->endptcomplete;
        _disable_eps();
    }

    if (usb_status_irqs & USB2D_USBSTS_PCI)
        u.port_speed = (u.regs->hostpc1_devlc & USB2D_HOSTPC1_DEVLC_PSPD_MASK) >> 25;

    u32 ep0_setup_req = u.regs->endptsetupstat;
    if (ep0_setup_req & 1)
    {
        u.regs->endptsetupstat = ep0_setup_req;
        memcpy(&u.setup, (void *)s_qh[QH_CTRL_OUT].setup, sizeof(usb_ctrl_setup_t));
        _ctrl_setup();
    }
}

static bool _configured(void) { return u.configured; }
static bool _lost(void)       { return false; }  /* a bus reset starts over in _poll */
static u32  _max_packet(void) { return _max_packet_of(QH_BULK_IN); }
static bool _tx_idle(void)    { return _ep_status(QH_BULK_IN) != USB_EP_STATUS_ACTIVE; }

static void _tx(const u8 *buf, u32 len)
{
    u8 *dma = (u8 *)USB_EP_BULK_IN_BUF_ADDR;
    memcpy(dma, buf, len);
    _ep_op(QH_BULK_IN, dma, len, false);
}

/* What the last read brought, then the next read primed: one packet's
 * worth, as the PC sends no more at a time. */
static u32 _rx(u8 *buf, u32 max)
{
    u32 n = 0;

    if (u.rx_queued && _ep_status(QH_BULK_OUT) == USB_EP_STATUS_IDLE)
    {
        bpmp_mmu_maintenance(BPMP_MMU_MAINT_INVALID_WAY, false);
        n = u.requested[QH_BULK_OUT] - (s_qh[QH_BULK_OUT].token >> 16);
        if (n > max)
            n = max;
        memcpy(buf, (u8 *)USB_EP_BULK_OUT_BUF_ADDR, n);
        u.rx_queued = false;
    }
    if (!u.rx_queued)
    {
        _ep_op(QH_BULK_OUT, (u8 *)USB_EP_BULK_OUT_BUF_ADDR, _max_packet(), false);
        u.rx_queued = true;
    }
    return n;
}

const usbser_ctrl_t usbser_usb2 = {
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
