/*
 * usbpd.c - the BM92T36 and the dock handshake (see usbpd.h).
 */
#include "hwtest.h"
#include "usbpd.h"
#include "dpout.h"
#include <soc/gpio.h>
#include <soc/pinmux.h>
#include <soc/timer.h>
#include <soc/t210.h>

#define PD_ADDR  0x18

/* Registers */
#define ALERT_STATUS_REG    0x02
#define STATUS1_REG         0x03
#define STATUS2_REG         0x04
#define COMMAND_REG         0x05
#define CONFIG1_REG         0x06
#define DEV_CAPS_REG        0x07
#define READ_PDOS_SRC_REG   0x08
#define CONFIG2_REG         0x17
#define DP_STATUS_REG       0x18
#define DP_ALERT_EN_REG     0x19
#define VENDOR_CONFIG_REG   0x1A
#define UNKNOWN_1C_REG      0x1C
#define UNKNOWN_1D_REG      0x1D
#define SYS_CONFIG1_REG     0x26
#define SYS_CONFIG2_REG     0x27
#define CURRENT_PDO_REG     0x28
#define CURRENT_RDO_REG     0x2B
#define SET_RDO_REG         0x30
#define FW_TYPE_REG         0x4B
#define FW_REVISION_REG     0x4C
#define MAN_ID_REG          0x4D
#define DEV_ID_REG          0x4E
#define INCOMING_VDM_REG    0x50
#define OUTGOING_VDM_REG    0x60

/* Commands */
#define PS_RDY_CMD                0x0505
#define SEND_RDO_CMD              0x0707
#define SYS_RESET_CMD             0x0D0D
#define SEND_VDM_CMD              0x1111
#define ACCEPT_VDM_CMD            0x1616
#define DR_SWAP_CMD               0x1818
#define DP_ENTER_MODE_CMD         0x3131
#define DP_CFG_AND_START_HPD_CMD  0x3636
#define STOP_HPD_EXIT_DP_CMD      0x3B3B

/* VDM */
#define VDM_CMD_DISC_ID     0x01
#define VDM_CMD_DISC_SVID   0x02
#define VDM_CMD_DISC_MODE   0x03
#define VDM_CMD_ENTER_MODE  0x04
#define VDM_ACK             0x40
#define VDM_STRUCTURED      0x80
#define VDO_DP_UFP_D        BIT(0)
#define VDO_DP_U_UFP_D      BIT(1)   /* the configuration: UFP_U as a DisplayPort sink */
#define VDO_DP_SUPPORT      BIT(2)
#define VDO_DP_RECEPTACLE   BIT(6)
#define VDO_DP_PIN_C        BIT(2)
#define VDO_DP_PIN_D        BIT(3)
#define VDO_TYPE_ALTERNATE  5

#define VID_NINTENDO        0x057E
#define PID_NIN_DOCK        0x2003
#define PID_NIN_CHARGER     0x2004

/* The Nintendo dock's own messages: unstructured VDMs on its SVID. */
#define ND_REQUEST          0x00
#define ND_READ             0
#define ND_WRITE            1
#define ND_HOST             1
#define ND_DOCK             2
#define ND_BUSY             1
#define ND_CMD_LED          0x01
#define ND_CMD_STATE        0x16
#define ND_CMD_0B           0x0B    /* read by HOS after the state; not in L4T */
#define ND_CMD_18           0x18    /* the same                                */
#define ND_CMD_HUB          0x20

/* What is asked for, as L4T asks: the dock's 15 V at 1.2 A, which is the
 * BQ24193's limit at that voltage; other sources the best of 5/9/12/15 V. */
#define DOCK_ID_MV          5000
#define DOCK_ID_MA          500
#define DOCK_MV             15000
#define DOCK_MA_MIN         2600
#define DOCK_MA_MAX         3000

/* The hot plug detect line from the dock, through the PD controller: pad
 * HDMI_INT_DP_HPD, GPIO CC1 (bdk has no name for it). */
#define PINMUX_AUX_HDMI_INT_DP_HPD  0x19C

#define STEP_TIMEOUT_MS     2500
#define HOS_READ_TIMEOUT_MS 5000    /* HOS's wait for each dock read's reply   */
#define HPD_WAIT_MS         1500    /* the dock's HDMI side raises HPD after the lanes */
#define HPD_WAIT_OTHER_MS   5000    /* longer for anything else: a VIA Labs adapter
                                     * (2109:0103) raised it after 1.5 s */

/* VENDOR_CONFIG */
#define VENDOR_CONFIG_OCP_DISABLE  BIT(2)

/* The controller's own pins, as HOS's gpio service names them: PV5
 * PdRstN, PK4 CradleIrq, PK5 PdVconnEn. bdk names none of them. */
#define PINMUX_AUX_AP_READY         0x210   /* PV5 */
#define PINMUX_AUX_GPIO_PK4         0x264
#define PINMUX_AUX_GPIO_PK5         0x268

/* HOS's timings, from the usb sysmodule's constructor. */
#define PDR_PULSE_MS        30
#define PDR_SETTLE_MS       100
#define PDR_IRQ_MS          2000    /* HOS waits for ever; a line that never falls is told */
#define PDR_ID_MS           10000
#define PDR_CMD_MS          100     /* L4T's wait after SYS_RESET */

static const char *s_lastcmd[8] = {
    "complete", "?", "aborted", "?", "invalid", "?", "rejected", "terminated"
};

/* ---- the chip ----------------------------------------------------------- */

/* bdk reads this chip in packet mode (one transaction, repeated start);
 * the fuel gauge's reads go in normal mode (two). The snapshot finds out
 * which one the chip answers here and the rest follows it; the profile
 * and message buffers are longer than normal mode carries anyway. */
static bool s_normal_mode;

static int _rd(u8 reg, void *buf, u32 n)
{
    memset(buf, 0, n);
    if (s_normal_mode && n <= 8)
        return i2c_recv_buf_small(buf, n, I2C_1, PD_ADDR, reg);
    return i2c_recv_buf_big(buf, n, I2C_1, PD_ADDR, reg);
}

static u16 _rd16(u8 reg)
{
    u8 b[2];
    _rd(reg, b, 2);
    return b[0] | (b[1] << 8);
}

/* A write that ends with a STOP. bdk's packet mode write ends with none:
 * its header asks the controller to carry on (CONT_XFER), and nothing else
 * in hekate uses it. The PD controller took the RDO block that way once
 * and read zero in every register from then on (a photo of 2026-11-04),
 * the state HOS's reset pulse is for. So: normal mode up to seven bytes,
 * and for the longer messages a packet write of our own whose header asks
 * for the STOP, as HOS's Start|Stop option does. */
#define I2C1(off)  MMIO_REG32(I2C_BASE, off)

static int _wr_packet_stop(const u8 *buf, u32 size)
{
    int res = 0;
    if (size > 32)
        return 1;
    I2C1(0x04) = PD_ADDR << 1;                           /* CMD_ADDR0, a write */
    I2C1(0x00) = (2 << 12) | BIT(11);                    /* CNFG: debounce 4T, new master FSM */
    I2C1(0x5C) = BIT(1) | BIT(0);                        /* flush both FIFOs */
    I2C1(0x68) = I2C1(0x68);                             /* stale status cleared */
    I2C1(0x8C) = BIT(5) | BIT(2) | BIT(0);               /* load the configuration */
    for (u32 i = 0; i < 20 && (I2C1(0x8C) & BIT(0)); i++)
        usleep(1);
    I2C1(0x00) = (I2C1(0x00) & ~BIT(9)) | BIT(10);       /* packet mode, go */
    I2C1(0x50) = BIT(4);                                 /* header: the I2C protocol */
    I2C1(0x50) = size - 1;                               /* payload size, less one */
    I2C1(0x50) = BIT(17) | (PD_ADDR << 1);               /* IE on; no CONT_XFER: a STOP follows */
    for (u32 rem = size; rem; ) {
        u32 len = rem < 4 ? rem : 4;
        u32 word = 0;
        memcpy(&word, buf, len);
        I2C1(0x50) = word;
        buf += len;
        rem -= len;
    }
    /* Until the packet is complete: the last byte and the STOP out. The
     * byte counter bdk polls reaches the last byte while it is still on
     * the wire, and switching packet mode off then cuts the STOP; the PD
     * controller went quiet on the first message written that way. Linux
     * waits for this status. */
    u32 timeout = get_tmr_ms() + 200;
    u32 st;
    for (;;) {
        st = I2C1(0x68);                                 /* INT_STATUS */
        if (st & (BIT(7) | BIT(3) | BIT(2)))             /* packet complete, no ack, arbitration lost */
            break;
        if (get_tmr_ms() > timeout) {
            res = 1;
            break;
        }
    }
    if (st & (BIT(3) | BIT(2)))
        res = 1;
    I2C1(0x68) = st;                                     /* clear */
    I2C1(0x00) &= ~(BIT(10) | BIT(9));
    return res;
}

static int _wr(u8 reg, const void *data, u32 n)
{
    u8 buf[32];
    if (n <= 7)
        return i2c_send_buf_small(I2C_1, PD_ADDR, reg, data, n);
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    return _wr_packet_stop(buf, n + 1);
}

static int _cmd(u16 cmd)
{
    u8 b[2] = { cmd & 0xFF, cmd >> 8 };
    return _wr(COMMAND_REG, b, 2);
}

static void _pdo_decode(u32 pdo, u32 *mv, u32 *ma)
{
    *ma = (pdo & 0x3FF) * 10;
    *mv = ((pdo >> 10) & 0x3FF) * 50;
}

static u32 _read_pdos(u32 *mv, u32 *ma, u32 *raw)
{
    u8 b[29];
    if (_rd(READ_PDOS_SRC_REG, b, sizeof(b)))
        return 0;
    u32 n = b[0] / 4;
    if (n > 7)
        n = 7;
    for (u32 i = 0; i < n; i++) {
        u32 pdo = b[1 + 4 * i] | b[2 + 4 * i] << 8 | b[3 + 4 * i] << 16 | b[4 + 4 * i] << 24;
        if (raw)
            raw[i] = pdo;
        _pdo_decode(pdo, &mv[i], &ma[i]);
    }
    return n;
}

/* The hot plug detect line: read as a GPIO until the AUX stage takes the
 * pad for DPAUX1, which has to own it (SFIO mode) to see HPD itself. */
static bool _hpd(void)
{
    if (dpout_aux_ready())
        return dpout_hpd();
    PINMUX_AUX(PINMUX_AUX_HDMI_INT_DP_HPD) = PINMUX_INPUT_ENABLE;
    gpio_config(GPIO_PORT_CC, GPIO_PIN_1, GPIO_MODE_GPIO);
    gpio_direction_input(GPIO_PORT_CC, GPIO_PIN_1);
    return gpio_read(GPIO_PORT_CC, GPIO_PIN_1) != 0;
}

const char *pd_lastcmd_name(u16 status1)
{
    return s_lastcmd[(status1 & PD_S1_LASTCMD_MASK) >> PD_S1_LASTCMD_SHIFT];
}

void pd_snapshot(pd_snapshot_t *s, bool read_alert)
{
    u8 b[5];
    memset(s, 0, sizeof(*s));
    /* The firmware type first, as bdk reads it: a chip that has dozed off
     * may let the first transfer go by, so the ids get a few goes, in
     * each of the two transfer modes. */
    for (u32 tries = 0; tries < 6 && !s->ok; tries++) {
        if (tries)
            msleep(20);
        s_normal_mode = tries >= 3;
        s->rc  = _rd(FW_TYPE_REG, b, 2);
        s->fw_type = b[0] | b[1] << 8;
        s->rc |= _rd(MAN_ID_REG, b, 2) << 1;
        s->man_id = b[0] | b[1] << 8;
        s->rc |= _rd(DEV_ID_REG, b, 2) << 2;
        s->dev_id = b[0] | b[1] << 8;
        s->ok = s->man_id == 0x4B5 && s->dev_id == 0x3B0;
    }
    s->normal_mode = s_normal_mode;
    if (!s->ok)
        return;
    s->fw_rev    = _rd16(FW_REVISION_REG);
    s->caps      = _rd16(DEV_CAPS_REG);
    if (read_alert)
        s->alert = _rd16(ALERT_STATUS_REG);
    s->status1   = _rd16(STATUS1_REG);
    s->status2   = _rd16(STATUS2_REG);
    s->dp_status = _rd16(DP_STATUS_REG);
    s->dp_unk1c  = _rd16(UNKNOWN_1C_REG);
    s->dp_unk1d  = _rd16(UNKNOWN_1D_REG);
    s->config1   = _rd16(CONFIG1_REG);
    s->config2   = _rd16(CONFIG2_REG);
    s->sys1      = _rd16(SYS_CONFIG1_REG);
    s->sys2      = _rd16(SYS_CONFIG2_REG);
    s->pdo_n     = _read_pdos(s->pdo_mv, s->pdo_ma, NULL);
    if (!_rd(CURRENT_PDO_REG, b, 5) && b[0] == 4)
        s->cur_pdo = b[1] | b[2] << 8 | b[3] << 16 | b[4] << 24;
    if (!_rd(CURRENT_RDO_REG, b, 5) && b[0] == 4)
        s->cur_rdo = b[1] | b[2] << 8 | b[3] << 16 | b[4] << 24;
    s->hpd = _hpd();
}

/* ---- the pins ------------------------------------------------------------ */

static void _pad(u32 port, u32 pin, u32 mux, pd_pad_t *p)
{
    /* The bank's CNF; OE +0x10, OUT +0x20, IN +0x30. */
    u32 base = ((port >> 2) << 8) + ((port & 3) << 2);
    p->mux  = PINMUX_AUX(mux);
    p->gpio = (GPIO(base + 0x00) & pin) != 0;
    p->oe   = (GPIO(base + 0x10) & pin) != 0;
    p->out  = (GPIO(base + 0x20) & pin) != 0;
    p->in   = (GPIO(base + 0x30) & pin) != 0;
}

void pd_pads(pd_pads_t *p)
{
    _pad(GPIO_PORT_V, GPIO_PIN_5, PINMUX_AUX_AP_READY, &p->reset);
    _pad(GPIO_PORT_K, GPIO_PIN_4, PINMUX_AUX_GPIO_PK4, &p->irq);
    _pad(GPIO_PORT_K, GPIO_PIN_5, PINMUX_AUX_GPIO_PK5, &p->vconn);
}

void pd_pad_text(const pd_pad_t *p, char *out)
{
    if (!p->gpio)
        s_printf(out, "sfio");
    else if (p->oe)
        s_printf(out, "out %d", p->out);
    else
        s_printf(out, "in %d", p->in);
}

/* ---- HOS's start-up ------------------------------------------------------ */

static const char *s_reset_name[] = {
    "idle", "pulsing the reset line", "settling", "waiting for the interrupt line",
    "waiting for the device id", "sys reset", "configuring", "done", "failed",
};

const char *pd_reset_state_name(pd_reset_state_t s)
{
    return s_reset_name[s];
}

static void _rgo(pd_reset_t *r, pd_reset_state_t s)
{
    r->state = s;
    r->since = get_tmr_ms();
}

static void _rlog(pd_reset_t *r, const char *text)
{
    s_printf(r->log, "gui: pd reset: %s\n", text);
    r->log_new = true;
}

static void _rfail(pd_reset_t *r, const char *why)
{
    char line[80];
    s_printf(r->why, "%s", why);
    r->failed_at = r->state;
    _rgo(r, PDR_FAILED);
    s_printf(line, "failed: %s", why);
    _rlog(r, line);
}

void pd_reset_start(pd_reset_t *r)
{
    char a[12], b[12], c[12], line[96];
    memset(r, 0, sizeof(*r));
    pd_pads(&r->before);
    pd_pad_text(&r->before.reset, a);
    pd_pad_text(&r->before.irq, b);
    pd_pad_text(&r->before.vconn, c);
    s_printf(line, "pins before: PV5 %s (mux %03X), PK4 %s (mux %03X), PK5 %s",
             a, r->before.reset.mux, b, r->before.irq.mux, c);
    _rlog(r, line);

    /* The interrupt line, to watch: pulled up and tristated, as L4T has it. */
    PINMUX_AUX(PINMUX_AUX_GPIO_PK4) = PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | PINMUX_PULL_UP;
    gpio_config(GPIO_PORT_K, GPIO_PIN_4, GPIO_MODE_GPIO);
    gpio_direction_input(GPIO_PORT_K, GPIO_PIN_4);
    /* The reset line, driven: high for the pulse, low at rest, as HOS
     * holds it and L4T boots it (output low, pulled down, as its device
     * tree has the pad). */
    PINMUX_AUX(PINMUX_AUX_AP_READY) = PINMUX_PULL_DOWN;
    gpio_config(GPIO_PORT_V, GPIO_PIN_5, GPIO_MODE_GPIO);
    gpio_direction_output(GPIO_PORT_V, GPIO_PIN_5, GPIO_HIGH);
    r->started = get_tmr_ms();
    _rgo(r, PDR_PULSE);
}

void pd_reset_step(pd_reset_t *r)
{
    u32 now = get_tmr_ms();
    u32 el = now - r->since;
    u8 b[2];
    char line[96];

    switch (r->state) {
    case PDR_PULSE:
        if (el < PDR_PULSE_MS)
            break;
        gpio_write(GPIO_PORT_V, GPIO_PIN_5, GPIO_LOW);
        _rgo(r, PDR_SETTLE);
        break;

    case PDR_SETTLE:
        if (el < PDR_SETTLE_MS)
            break;
        _rgo(r, PDR_WAIT_IRQ);
        break;

    case PDR_WAIT_IRQ:
        if (!gpio_read(GPIO_PORT_K, GPIO_PIN_4)) {
            r->irq_ms = now - r->started;
            s_printf(line, "interrupt line low %d ms after the pulse", r->irq_ms);
            _rlog(r, line);
            _rgo(r, PDR_WAIT_ID);
        } else if (el >= PDR_IRQ_MS) {
            r->irq_ms = (u32)-1;
            _rlog(r, "interrupt line still high after 2 s, going on without it");
            _rgo(r, PDR_WAIT_ID);
        }
        break;

    case PDR_WAIT_ID:
        s_normal_mode = false;
        if (_rd(DEV_ID_REG, b, 2))
            r->id_errors = true;
        r->dev_id = b[0] | b[1] << 8;
        if (r->dev_id) {
            r->id_ms = now - r->started;
            s_printf(line, "device id %04X %d ms after the pulse%s", r->dev_id, r->id_ms,
                     r->id_errors ? ", reads reported errors before" : "");
            _rlog(r, line);
            r->alert = _rd16(ALERT_STATUS_REG);     /* HOS reads it here */
            _cmd(SYS_RESET_CMD);
            _rgo(r, PDR_SYS_RESET);
        } else if (el >= PDR_ID_MS) {
            _rfail(r, r->id_errors ? "no device id within 10 s, the reads reporting errors"
                                   : "no device id within 10 s, the reads reporting none");
        }
        break;

    case PDR_SYS_RESET:
        if (el < PDR_CMD_MS)
            break;
        r->alert   = _rd16(ALERT_STATUS_REG);
        r->status1 = _rd16(STATUS1_REG);
        r->status2 = _rd16(STATUS2_REG);
        s_printf(line, "sys reset sent, alert %04X s1 %04X s2 %04X", r->alert, r->status1,
                 r->status2);
        _rlog(r, line);
        _rgo(r, PDR_CONFIG);
        break;

    case PDR_CONFIG: {
        u8 on[2] = { 0xFF, 0x00 };
        u16 vc;
        r->man_id  = _rd16(MAN_ID_REG);
        r->fw_type = _rd16(FW_TYPE_REG);
        r->fw_rev  = _rd16(FW_REVISION_REG);
        /* The DisplayPort alerts on, the value HOS writes. */
        if (_wr(DP_ALERT_EN_REG, on, 2)) {
            _rfail(r, "the DP alert enable register did not take the write");
            break;
        }
        r->dp_alert_en = _rd16(DP_ALERT_EN_REG);
        /* The over-current protection on: HOS clears the bit that turns
         * it off, when it finds it set. */
        vc = _rd16(VENDOR_CONFIG_REG);
        if (vc & VENDOR_CONFIG_OCP_DISABLE) {
            u8 w[2] = { (vc & ~VENDOR_CONFIG_OCP_DISABLE) & 0xFF, vc >> 8 };
            r->ocp_was_off = true;
            if (_wr(VENDOR_CONFIG_REG, w, 2)) {
                _rfail(r, "the vendor config register did not take the write");
                break;
            }
        }
        r->vendor_cfg = _rd16(VENDOR_CONFIG_REG);
        s_printf(line, "ids %04X/%04X fw %04X rev %04X, dp alerts %04X, vendor cfg %04X%s",
                 r->man_id, r->dev_id, r->fw_type, r->fw_rev, r->dp_alert_en, r->vendor_cfg,
                 r->ocp_was_off ? " (ocp was off)" : "");
        _rlog(r, line);
        _rgo(r, PDR_DONE);
        break;
    }

    default:
        break;
    }
}

/* ---- the handshake ----------------------------------------------------- */

static const char *s_state_name[] = {
    "idle", "waiting for a cable", "asking for power", "sink ready",
    "asking to be the data host", "asking who it is", "reading who it is",
    "entering the dock's mode", "the dock's mode", "asking for DisplayPort",
    "reading DisplayPort support", "asking for DisplayPort mode",
    "reading DisplayPort mode", "entering DisplayPort", "configuring DisplayPort",
    "waiting for the TV's hot plug detect",
    "asking the dock's state", "reading the dock's state",
    "asking the dock's 0B value", "reading the dock's 0B value",
    "asking the dock's 18 value", "reading the dock's 18 value", "lighting the dock",
    "the dock's light", "powering the dock's hub", "the dock's hub",
    "done", "failed",
};

const char *dock_state_name(dock_state_t s)
{
    return s_state_name[s];
}

static void _go(dock_t *d, dock_state_t s)
{
    d->state = s;
    d->since = get_tmr_ms();
    d->tries = 0;
    s_printf(d->log, "gui: dock %s: alert %04X s1 %04X s2 %04X dp %04X\n",
             dock_state_name(s), d->alerts, d->s1, d->s2, d->dp);
    d->log_new = true;
}

static void _fail(dock_t *d, const char *why)
{
    d->failed_state = d->state;
    d->failed_at = dock_state_name(d->state);
    s_printf(d->fail, "%s", why);
    _go(d, DOCK_FAILED);
}

/* The HOS read a state belongs to. */
static const char *_hos_read_name(dock_state_t s)
{
    return s <= DOCK_QUERY_ACC ? "state" : s <= DOCK_R0B_ACC ? "0B" : "18";
}

/* One of HOS's three reads went unanswered, or was answered with something
 * else: HOS would not take the dock as docked. The run ends here, done, so
 * the picture is still tried. */
static void _hos_reject(dock_t *d)
{
    d->hos_reject = _hos_read_name(d->state);
    _go(d, DOCK_DONE);
    s_printf(d->log + strlen(d->log), "gui: dock: no answer to the dock's %s read; "
             "HOS does not dock without it\n", d->hos_reject);
}

static bool _take(dock_t *d, u16 bit)
{
    if (!(d->alerts & bit))
        return false;
    d->alerts &= ~bit;
    return true;
}

static bool _timed_out(dock_t *d)
{
    return get_tmr_ms() - d->since > STEP_TIMEOUT_MS;
}

/* A command whose completion the next state waits for: an earlier one's
 * done bit must not count for it. */
static void _cmd_go(dock_t *d, u16 cmd, dock_state_t next)
{
    d->alerts &= ~PD_AL_CMD_DONE;
    if (cmd == DP_ENTER_MODE_CMD)
        d->dp_on = true;
    _cmd(cmd);
    _go(d, next);
}

/* A command's completion: the chip says done, is no longer busy, and did
 * not reject it. */
static int _cmd_done(dock_t *d)
{
    if (!_take(d, PD_AL_CMD_DONE))
        return _timed_out(d) ? -1 : 0;
    if (d->s1 & PD_S1_CMD_BUSY)
        return 0;
    if ((d->s1 & PD_S1_LASTCMD_MASK) != 0)
        return -2;
    return 1;
}

/* A command that got no completion, or completed with a status other
 * than complete: the controller's word for it. */
static void _cmd_fail(dock_t *d, const char *what, int r)
{
    char why[96];
    if (r == -1)
        s_printf(why, "no answer to %s", what);
    else
        s_printf(why, "%s was refused (last command %s)", what, pd_lastcmd_name(d->s1));
    _fail(d, why);
}

static void _send_vdm(dock_t *d, const u8 *msg, u32 len, dock_state_t next)
{
    u8 buf[16];
    buf[0] = len;
    memcpy(buf + 1, msg, len);
    if (_wr(OUTGOING_VDM_REG, buf, len + 1)) {
        _fail(d, "the message could not be written to the controller");
        return;
    }
    _cmd(SEND_VDM_CMD);
    _go(d, next);
}

static void _send_vdm_structured(dock_t *d, u8 cmd, u8 hdr, u16 svid, dock_state_t next)
{
    u8 m[4] = { cmd, hdr, svid & 0xFF, svid >> 8 };
    _send_vdm(d, m, 4, next);
}

/* The Nintendo dock's requests: eight bytes, the last four the command. */
static void _send_nd(dock_t *d, u8 rw, u8 cmd, const u8 *extra, dock_state_t next)
{
    u8 m[12] = { ND_REQUEST, 0x00, 0x7E, 0x05, rw, ND_HOST, cmd, 0x00 };
    u32 len = 8;
    if (extra) {
        memcpy(m + 8, extra, 4);
        len = 12;
    }
    _send_vdm(d, m, len, next);
}

/* The reply's bytes: [0] size, [1] command with the ack, [2] header,
 * [3..4] the SVID, [5..] the objects. */
static int _read_vdm(u8 *vdm)
{
    return _rd(INCOMING_VDM_REG, vdm, 29);
}

/* What to ask the source for, as L4T chooses: a Nintendo dock's 15 V, or
 * the most powerful fixed profile up to 15 V and 3 A. */
static bool _choose(dock_t *d)
{
    d->pick = 0;
    if (d->pdo_n == 2 && d->pdo_mv[0] == DOCK_ID_MV && d->pdo_ma[0] == DOCK_ID_MA) {
        if (d->pdo_mv[1] == DOCK_MV && d->pdo_ma[1] >= DOCK_MA_MIN && d->pdo_ma[1] <= DOCK_MA_MAX) {
            d->dock_power = true;
            d->pick = 2;
            return true;
        }
        return false;
    }
    u32 best = 0;
    for (u32 i = 0; i < d->pdo_n; i++) {
        u32 mv = d->pdo_mv[i], ma = d->pdo_ma[i];
        if (mv != 5000 && mv != 9000 && mv != 12000 && mv != 15000)
            continue;
        if (ma > 3000)
            continue;
        if (mv * ma > best || (!d->pick && mv == 5000)) {
            best = mv * ma;
            d->pick = i + 1;
        }
    }
    return d->pick != 0;
}

/* The operating current L4T settles on: the profile's less a reserve for
 * a USB device, capped per voltage, rounded down to the charger's steps. */
static u32 _op_ma(u32 mv, u32 ma)
{
    static const u32 steps[] = { 100, 150, 500, 900, 1200, 1500, 2000, 3000 };
    u32 limit = ma - (mv > 5000 ? 4500000 : 2500000) / mv;
    u32 cap = mv >= 15000 ? 1200 : mv == 12000 ? 1500 : 2000;
    if (limit > cap)
        limit = cap;
    for (int i = 7; i >= 0; i--)
        if (limit >= steps[i])
            return steps[i];
    return 100;
}

static void _send_rdo(dock_t *d)
{
    u32 mv = d->pdo_mv[d->pick - 1], ma = d->pdo_ma[d->pick - 1];
    d->req_ma = _op_ma(mv, ma);
    u32 rdo = (ma / 10) | ((d->req_ma / 10) << 10) | BIT(26) | (d->pick << 28);
    u8 b[5] = { 4, rdo, rdo >> 8, rdo >> 16, rdo >> 24 };
    _wr(SET_RDO_REG, b, 5);
    _cmd(SEND_RDO_CMD);
    _go(d, DOCK_RDO_SENT);
}

static void _dp_configure(dock_t *d)
{
    u8 msg[5];
    _rd(INCOMING_VDM_REG, msg, 5);
    memcpy(d->dp_caps, msg + 1, 4);
    /* Two lanes (pin D) as L4T runs the dock, which keeps USB 3 on the
     * other pair; four (pin C) if that is all it offers. A dock reports a
     * plug but answers on the receptacle assignments. */
    u8 pins = d->nin_dock ? msg[3] : (msg[1] & VDO_DP_RECEPTACLE) ? msg[3] : msg[2];
    u8 pin = 0;
    if (pins & VDO_DP_PIN_D) {
        pin = VDO_DP_PIN_D;
        d->lanes = 2;
    } else if (pins & VDO_DP_PIN_C) {
        pin = VDO_DP_PIN_C;
        d->lanes = 4;
    }
    if (msg[0] != 4 || !pin) {
        char why[96];
        s_printf(why, "no usable pin assignment (%02X %02X %02X %02X)",
                 msg[0], msg[1], msg[2], msg[3]);
        _fail(d, why);
        return;
    }
    /* The configuration VDO as L4T builds it: byte 0 makes the other end
     * the sink (UFP_U as UFP_D) with DisplayPort signaling; the pin
     * assignment goes in byte 1, except for the Nintendo dock, which takes
     * it in byte 2. A Nintendo dock refuses anything else; a generic
     * adapter let a wrong one through. */
    u8 cfg[4] = { VDO_DP_SUPPORT | VDO_DP_U_UFP_D, 0x00, 0x00, 0x00 };
    if (d->nin_dock)
        cfg[2] = pin;
    else
        cfg[1] = pin;
    u8 out[5] = { 4, cfg[0], cfg[1], cfg[2], cfg[3] };
    _wr(OUTGOING_VDM_REG, out, 5);
    _cmd_go(d, DP_CFG_AND_START_HPD_CMD, DOCK_DP_CFG_SENT);
}

void dock_start(dock_t *d)
{
    memset(d, 0, sizeof(*d));
    (void)_rd16(ALERT_STATUS_REG);      /* whatever is pending is not ours */
    _go(d, DOCK_WAIT_PLUG);
}

void dock_step(dock_t *d)
{
    u8 vdm[29];
    int r;

    if (d->state == DOCK_IDLE || d->state == DOCK_FAILED)
        return;
    d->alerts |= _rd16(ALERT_STATUS_REG);
    d->s1 = _rd16(STATUS1_REG);
    d->s2 = _rd16(STATUS2_REG);
    d->dp = _rd16(DP_STATUS_REG);
    d->plugged = (d->s1 & PD_S1_INSERT) != 0;

    if (d->state > DOCK_WAIT_PLUG && !d->plugged) {
        /* A pulled cable, or a controller gone quiet: with the ids zero
         * too, it is the latter, the state HOS resets it from. */
        u8 id[2] = { 0, 0 };
        _rd(DEV_ID_REG, id, 2);
        if (!d->s1 && !d->s2 && !(id[0] | id[1]))
            _fail(d, "the controller went quiet: every register reads zero; Reset Chip is HOS's answer");
        else
            _fail(d, "the cable went away");
        return;
    }
    if (d->s1 & PD_S1_FAULT_MASK) {
        char why[64];
        s_printf(why, "the controller reports an internal fault (%d)", d->s1 & PD_S1_FAULT_MASK);
        _fail(d, why);
        return;
    }

    switch (d->state) {
    case DOCK_WAIT_PLUG:
        if (!d->plugged)
            break;
        if (!d->plug_seen) {
            d->plug_seen = true;
            d->since = get_tmr_ms();    /* the wait starts at the plug  */
        }
        if (get_tmr_ms() - d->since < 300)
            break;                      /* let the source announce itself */
        d->pdo_n = _read_pdos(d->pdo_mv, d->pdo_ma, NULL);
        if (!d->pdo_n) {
            if (_timed_out(d))
                _fail(d, "no power profiles offered: a plain charger or cable, not a PD source");
            break;
        }
        if (!_choose(d)) {
            _fail(d, "it offers power profiles, but none this console takes");
            break;
        }
        d->alerts = 0;
        _send_rdo(d);
        break;

    case DOCK_RDO_SENT:
        if (_take(d, PD_AL_CONTRACT)) {
            u8 b[5];
            if (!_rd(CURRENT_PDO_REG, b, 5) && b[0] == 4) {
                u32 pdo = b[1] | b[2] << 8 | b[3] << 16 | b[4] << 24;
                _pdo_decode(pdo, &d->got_mv, &d->got_ma);
            }
            _cmd_go(d, PS_RDY_CMD, DOCK_PS_RDY_SENT);
        } else if (_timed_out(d)) {
            _fail(d, "no power contract came back");
        }
        break;

    case DOCK_PS_RDY_SENT:
        r = _cmd_done(d);
        if (r < 0) {
            _cmd_fail(d, "sink ready", r);
            break;
        }
        if (!r)
            break;
        d->role = (d->s1 & PD_S1_DR_MASK) >> PD_S1_DR_SHIFT;
        if (d->role == PD_ROLE_DFP) {
            _send_vdm_structured(d, VDM_CMD_DISC_ID, VDM_STRUCTURED, 0xFF00, DOCK_DISC_ID_SENT);
        } else if (d->role == PD_ROLE_UFP) {
            _cmd_go(d, DR_SWAP_CMD, DOCK_DR_SWAP_SENT);
        } else {
            _fail(d, "no data role after the contract");
        }
        break;

    case DOCK_DR_SWAP_SENT:
        r = _cmd_done(d);
        if (r < 0) {
            _cmd_fail(d, "the data role swap", r);
            break;
        }
        if (!r)
            break;
        d->role = (d->s1 & PD_S1_DR_MASK) >> PD_S1_DR_SHIFT;
        if (d->role != PD_ROLE_DFP) {
            _fail(d, "still not the data host after the swap");
            break;
        }
        _send_vdm_structured(d, VDM_CMD_DISC_ID, VDM_STRUCTURED, 0xFF00, DOCK_DISC_ID_SENT);
        break;

    /* Every VDM: the reply arrives, is accepted, then read. */
    case DOCK_DISC_ID_SENT:
    case DOCK_NIN_SENT:
    case DOCK_SVID_SENT:
    case DOCK_MODE_SENT:
    case DOCK_QUERY_SENT:
    case DOCK_R0B_SENT:
    case DOCK_R18_SENT:
    case DOCK_LED_SENT:
    case DOCK_HUB_SENT:
        if (_take(d, PD_AL_VDM_RECEIVED)) {
            _cmd_go(d, ACCEPT_VDM_CMD, d->state + 1);
        } else if (d->state >= DOCK_QUERY_SENT && d->state <= DOCK_R18_SENT) {
            /* HOS's three reads: its usb sysmodule waits 5 s for each and
             * does not dock without all three. */
            if (get_tmr_ms() - d->since > HOS_READ_TIMEOUT_MS)
                _hos_reject(d);
        } else if (_timed_out(d) && d->state >= DOCK_LED_SENT) {
            /* The light and the hub: HOS never needs them to dock (other
             * services ask for them later), and L4T waits for their
             * replies without a limit. A dock that does not answer them
             * (a clone with Nintendo's ids and no such controller) is done
             * here, with DisplayPort up, and the picture follows. */
            d->nd_silent = d->state == DOCK_LED_SENT ? "light" : "hub";
            _go(d, DOCK_DONE);
            s_printf(d->log + strlen(d->log), "gui: dock: no reply to the dock's %s "
                     "message\n", d->nd_silent);
        } else if (_timed_out(d)) {
            _fail(d, "no reply to the message");
        }
        break;

    case DOCK_DISC_ID_ACC:
        r = _cmd_done(d);
        if (r <= 0) {
            if (r < 0) _fail(d, "the reply could not be read");
            break;
        }
        _read_vdm(vdm);
        {
            u32 id = vdm[5] | vdm[6] << 8 | vdm[7] << 16 | vdm[8] << 24;
            d->vid = id & 0xFFFF;
            d->vdo_type = (id >> 27) & 7;
            d->pid = vdm[15] | vdm[16] << 8;
        }
        if (vdm[1] != (VDM_ACK | VDM_CMD_DISC_ID)) {
            _fail(d, "the other end did not say who it is");
            break;
        }
        d->nin_dock = d->vid == VID_NINTENDO && d->pid == PID_NIN_DOCK && d->vdo_type == VDO_TYPE_ALTERNATE;
        d->nin_charger = d->vid == VID_NINTENDO && d->pid == PID_NIN_CHARGER;
        if (d->nin_dock)
            _send_vdm_structured(d, VDM_CMD_ENTER_MODE, VDM_STRUCTURED | 1, VID_NINTENDO, DOCK_NIN_SENT);
        else if (d->nin_charger)
            _go(d, DOCK_DONE);
        else
            _send_vdm_structured(d, VDM_CMD_DISC_SVID, VDM_STRUCTURED, 0xFF00, DOCK_SVID_SENT);
        break;

    case DOCK_NIN_ACC:
        r = _cmd_done(d);
        if (r <= 0) {
            if (r < 0) _fail(d, "the reply could not be read");
            break;
        }
        _read_vdm(vdm);
        if (!(vdm[1] == (VDM_ACK | VDM_CMD_ENTER_MODE) && vdm[2] == (VDM_STRUCTURED | 1)
              && vdm[3] == 0x7E && vdm[4] == 0x05)) {
            _fail(d, "the dock refused its own mode");
            break;
        }
        d->nin_mode = true;
        _cmd_go(d, DP_ENTER_MODE_CMD, DOCK_DP_ENTER_SENT);
        break;

    case DOCK_SVID_ACC:
        r = _cmd_done(d);
        if (r <= 0) {
            if (r < 0) _fail(d, "the reply could not be read");
            break;
        }
        _read_vdm(vdm);
        if (vdm[1] != (VDM_ACK | VDM_CMD_DISC_SVID)) {
            _fail(d, "it does not list any alternate modes");
            break;
        }
        _send_vdm_structured(d, VDM_CMD_DISC_MODE, VDM_STRUCTURED, 0xFF01, DOCK_MODE_SENT);
        break;

    case DOCK_MODE_ACC:
        r = _cmd_done(d);
        if (r <= 0) {
            if (r < 0) _fail(d, "the reply could not be read");
            break;
        }
        _read_vdm(vdm);
        if (!(vdm[0] > 4 && vdm[1] == (VDM_ACK | VDM_CMD_DISC_MODE) && vdm[3] == 0x01
              && vdm[4] == 0xFF && (vdm[5] & VDO_DP_UFP_D) && (vdm[5] & VDO_DP_SUPPORT))) {
            _fail(d, "it does not do DisplayPort");
            break;
        }
        _cmd_go(d, DP_ENTER_MODE_CMD, DOCK_DP_ENTER_SENT);
        break;

    case DOCK_DP_ENTER_SENT:
        r = _cmd_done(d);
        if (r < 0) {
            _cmd_fail(d, "DisplayPort mode", r);
            break;
        }
        if (!r)
            break;
        /* L4T gives the controller 100 ms here before reading its answer. */
        if (get_tmr_ms() - d->since < 100)
            msleep(100 - (get_tmr_ms() - d->since));
        _dp_configure(d);
        break;

    case DOCK_DP_CFG_SENT:
        r = _cmd_done(d);
        if (r < 0) {
            _cmd_fail(d, "the DisplayPort configuration", r);
            break;
        }
        if (!r)
            break;
        d->dp_status = d->dp;
        d->hpd = _hpd();
        _go(d, DOCK_DP_HPD_WAIT);
        break;

    case DOCK_DP_HPD_WAIT:
        /* The dock's HDMI side raises HPD a moment after the lanes are
         * set: a grace period before saying nothing is on it. */
        d->dp_status = d->dp;
        d->hpd = _hpd();
        if (d->hpd || (d->dp & PD_DP_SIGNAL_ON) ||
            get_tmr_ms() - d->since > (d->nin_dock ? HPD_WAIT_MS : HPD_WAIT_OTHER_MS)) {
            if (d->nin_dock)
                _send_nd(d, ND_READ, ND_CMD_STATE, NULL, DOCK_QUERY_SENT);
            else
                _go(d, DOCK_DONE);
        }
        break;

    case DOCK_QUERY_ACC:
    case DOCK_R0B_ACC:
    case DOCK_R18_ACC: {
        /* HOS's reads, in its order (usb sysmodule 22.1.0): the reply must
         * be the dock's, to the same command; HOS takes anything else as
         * no answer. */
        static const u8 cmd_of[3] = { ND_CMD_STATE, ND_CMD_0B, ND_CMD_18 };
        u32 k = (d->state - DOCK_QUERY_ACC) / 2;
        u8 *keep = k == 0 ? d->dock_state : k == 1 ? d->dock_0b : d->dock_18;
        r = _cmd_done(d);
        if (r <= 0) {
            if (r < 0) _fail(d, "the reply could not be read");
            break;
        }
        _read_vdm(vdm);
        if (vdm[6] != ND_DOCK || vdm[7] != cmd_of[k] + 1) {
            _hos_reject(d);
            break;
        }
        memcpy(keep, vdm + 9, 4);
        if (k == 0)
            d->dock_old_fw = (vdm[11] & 0x02) != 0;
        if (k < 2) {
            _send_nd(d, ND_READ, cmd_of[k + 1], NULL, d->state + 1);
        } else {
            /* The light: full brightness, a short fade in. */
            static const u8 led[4] = { 64, 0, 0, 128 };   /* fade, off, on, duty */
            _send_nd(d, ND_WRITE, ND_CMD_LED, led, DOCK_LED_SENT);
        }
        break;
    }

    case DOCK_LED_ACC:
        if (!d->led_lit) {
            r = _cmd_done(d);
            if (r <= 0) {
                if (r < 0) _fail(d, "the reply could not be read");
                break;
            }
            d->led_lit = true;
            d->since = get_tmr_ms();    /* the hub's own power-up from here */
            break;
        }
        if (get_tmr_ms() - d->since < 500)
            break;
        _send_nd(d, ND_WRITE, ND_CMD_HUB, NULL, DOCK_HUB_SENT);
        break;

    case DOCK_HUB_ACC:
        r = _cmd_done(d);
        if (r <= 0) {
            if (r < 0) _fail(d, "the reply could not be read");
            break;
        }
        _read_vdm(vdm);
        if (vdm[6] == ND_DOCK && vdm[7] == ND_CMD_HUB + 1) {
            if ((vdm[5] & ND_BUSY) && d->tries < 10) {
                d->tries++;
                msleep(250);
                _send_nd(d, ND_WRITE, ND_CMD_HUB, NULL, DOCK_HUB_SENT);
                break;
            }
            d->hub_up = !(vdm[5] & ND_BUSY);
        }
        _go(d, DOCK_DONE);
        break;

    case DOCK_DONE:
        d->dp_status = d->dp;
        d->hpd = _hpd();
        break;

    default:
        break;
    }
}

/* A VDM exchange on its own, for the way out: sent, the reply accepted. */
static void _exchange(const u8 *msg, u32 len)
{
    u8 buf[16];
    buf[0] = len;
    memcpy(buf + 1, msg, len);
    _wr(OUTGOING_VDM_REG, buf, len + 1);
    _cmd(SEND_VDM_CMD);
    u32 end = get_tmr_ms() + 600;
    while (get_tmr_ms() < end) {
        if (_rd16(ALERT_STATUS_REG) & PD_AL_VDM_RECEIVED) {
            _cmd(ACCEPT_VDM_CMD);
            msleep(20);
            return;
        }
        msleep(10);
    }
}

void dock_stop(dock_t *d)
{
    if (d->state == DOCK_IDLE)
        return;
    if (d->led_lit) {
        static const u8 led_off[12] = { ND_REQUEST, 0x00, 0x7E, 0x05, ND_WRITE, ND_HOST,
                                        ND_CMD_LED, 0x00, 128, 0, 0, 0 };
        _exchange(led_off, sizeof(led_off));
    }
    if (d->dp_on) {
        _cmd(STOP_HPD_EXIT_DP_CMD);
        msleep(100);
    }
    d->state = DOCK_IDLE;
}
