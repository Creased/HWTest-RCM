/*
 * dpout.c - the DisplayPort output to the dock (see dpout.h).
 */
#include "hwtest.h"
#include "dpout.h"
#include <soc/clock.h>
#include <soc/gpio.h>
#include <soc/pinmux.h>
#include <soc/pmc.h>
#include <soc/t210.h>
#include <soc/timer.h>

/* DPAUX1 at 0x54040000; bdk's DPAUX1() takes a byte offset. The map, in
 * words, is L4T's dpaux_regs.h; the TRM's chapter 27.11 agrees. */
#define DPAUX1_INTR_EN_AUX          (0x01 * 4)
#define DPAUX1_INTR_AUX             (0x05 * 4)
#define DPAUX1_DP_AUXDATA_WRITE(i)  ((0x09 + 4 * (i)) * 4)
#define DPAUX1_DP_AUXDATA_READ(i)   ((0x19 + 4 * (i)) * 4)
#define DPAUX1_DP_AUXADDR           (0x29 * 4)
#define DPAUX1_DP_AUXCTL            (0x2D * 4)
#define  AUXCTL_CMDLEN_MASK         0xFF
#define  AUXCTL_CMD_MASK            (0xF << 12)
#define  AUXCTL_ADDRESS_ONLY        BIT(8)
#define  AUXCTL_CMD_I2CRD           (1 << 12)
#define  AUXCTL_CMD_MOTWR           (4 << 12)
#define  AUXCTL_CMD_MOTRD           (5 << 12)
#define  AUXCTL_CMD_AUXWR           (8 << 12)
#define  AUXCTL_CMD_AUXRD           (9 << 12)
#define  AUXCTL_TRANSACTREQ         BIT(16)
#define  AUXCTL_RST                 BIT(31)
#define DPAUX1_DP_AUXSTAT           (0x31 * 4)
#define  AUXSTAT_REPLY_M_MASK       0xFF
#define  AUXSTAT_TIMEOUT_ERROR      BIT(8)
#define  AUXSTAT_RX_ERROR           BIT(9)
#define  AUXSTAT_SINKSTAT_ERROR     BIT(10)
#define  AUXSTAT_NO_STOP_ERROR      BIT(11)
#define  AUXSTAT_REPLY_MASK         (0xF << 16)
#define  AUXSTAT_REPLY_ACK          (0 << 16)
#define  AUXSTAT_REPLY_DEFER        (2 << 16)
#define  AUXSTAT_REPLY_I2CDEFER     (8 << 16)
#define  AUXSTAT_HPD_PLUGGED        BIT(28)
#define DPAUX1_HPD_CONFIG           (0x3D * 4)
#define DPAUX1_HPD_IRQ_CONFIG       (0x41 * 4)
#define DPAUX1_HYBRID_PADCTL        (0x49 * 4)
#define  PADCTL_AUX_CMH_V0_70       (2 << 12)
#define  PADCTL_AUX_DRVZ_OHM_50     (4 << 8)
#define  PADCTL_AUX_DRVI(x)         ((x) << 2)
#define  PADCTL_AUX_INPUT_RCV_ENABLE BIT(1)
#define  PADCTL_MODE_AUX            0
#define DPAUX1_HYBRID_SPARE         (0x4D * 4)
#define  SPARE_PAD_PWR_POWERUP      0

/* The hot plug detect pad, hdmi_int_dp_hpd_pcc1: its DP function feeds
 * DPAUX1, and only in SFIO mode. L4T requests the GPIO and frees it at
 * once, "hotplug pin should be in spio mode". bdk has no name for it. */
#define PINMUX_AUX_HDMI_INT_DP_HPD  0x19C
#define HPD_PLUG_MIN_US             250     /* L4T's */
#define HPD_UNPLUG_MIN_US           2000
#define HPD_IRQ_MIN_US              250

#define AUX_MAX_BYTES     16
#define AUX_TIMEOUT_MS    40      /* the DisplayPort specification's limit */
#define AUX_TIMEOUT_TRIES 2
#define AUX_DEFER_TRIES   7

#ifndef APBDEV_PMC_REMOVE_CLAMPING_CMD
#define APBDEV_PMC_REMOVE_CLAMPING_CMD 0x34
#endif

static bool s_ready;
static u32  s_last_stat;

bool dpout_aux_ready(void)
{
    return s_ready;
}

int dpout_aux_init(void)
{
    if (s_ready)
        return 0;

    /* The SOR partition holds DSI as well as SOR1 and DPAUX1, so it is on
     * whenever the panel runs. Otherwise, as the TRM has it: ungate, the
     * clocks on with the resets held, the clamps off, the resets released. */
    if (!(PMC(APBDEV_PMC_PWRGATE_STATUS) & BIT(POWER_RAIL_SOR))) {
        if (pmc_domain_pwrgate_set(POWER_RAIL_SOR, 1))
            return 1;
        CLOCK(CLK_RST_CONTROLLER_RST_DEV_Y_SET) = BIT(CLK_Y_DPAUX1);
        CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_SET) = BIT(CLK_Y_SOR_SAFE) | BIT(CLK_Y_DPAUX1);
        usleep(10);
        PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) = BIT(POWER_RAIL_SOR);
        for (u32 i = 0; i < 1000 && (PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) & BIT(POWER_RAIL_SOR)); i++)
            usleep(1);
    }

    /* host1x must be clocked for any DPAUX access (the TRM); the panel's
     * bring-up did that, and it is not touched again while it runs. */
    if (!(CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_L) & BIT(CLK_L_HOST1X)))
        clock_enable_host1x();

    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_SET) = BIT(CLK_Y_SOR_SAFE) | BIT(CLK_Y_DPAUX1);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_Y_CLR) = BIT(CLK_Y_DPAUX1);
    usleep(2);

    /* The pads, as coreboot sets them for DisplayPort: AUX mode, 50 ohm,
     * 0.70 V common mode, the receiver on; then powered up, which the TRM
     * wants before any transaction. */
    DPAUX1(DPAUX1_INTR_AUX)    = 0xFFFFFFFF;
    DPAUX1(DPAUX1_INTR_EN_AUX) = 0;
    DPAUX1(DPAUX1_HYBRID_PADCTL) = PADCTL_AUX_DRVZ_OHM_50 | PADCTL_AUX_CMH_V0_70 |
                                   PADCTL_AUX_DRVI(0x18) | PADCTL_AUX_INPUT_RCV_ENABLE |
                                   PADCTL_MODE_AUX;
    DPAUX1(DPAUX1_HYBRID_SPARE) = SPARE_PAD_PWR_POWERUP;
    DPAUX1(DPAUX1_HPD_CONFIG) = HPD_PLUG_MIN_US | (HPD_UNPLUG_MIN_US << 16);
    DPAUX1(DPAUX1_HPD_IRQ_CONFIG) = HPD_IRQ_MIN_US;

    /* The HPD pad to DPAUX1: DP function, input, tristate, SFIO mode. In
     * GPIO mode the block does not see the line (both docks: HPD high by
     * the GPIO, unplugged by DPAUX1). */
    PINMUX_AUX(PINMUX_AUX_HDMI_INT_DP_HPD) = PINMUX_INPUT_ENABLE | PINMUX_TRISTATE;
    gpio_config(GPIO_PORT_CC, GPIO_PIN_1, GPIO_MODE_SPIO);
    usleep(HPD_PLUG_MIN_US * 2 + 100);
    s_ready = true;
    return 0;
}

bool dpout_hpd(void)
{
    if (!s_ready)
        return false;
    return (DPAUX1(DPAUX1_DP_AUXSTAT) & AUXSTAT_HPD_PLUGGED) != 0;
}

static int _aux_wait(void)
{
    u32 t = get_tmr_ms() + AUX_TIMEOUT_MS;
    while (DPAUX1(DPAUX1_DP_AUXCTL) & AUXCTL_TRANSACTREQ)
        if (get_tmr_ms() > t)
            return 1;
    return 0;
}

/* One AUX transaction of up to 16 bytes, a native read or write, with
 * coreboot's retries: twice on an error, seven times on a defer. 0 ok;
 * on a read, *n is what came back. */
static int _aux_xfer(u32 cmd, u32 addr, u8 *buf, u32 *n)
{
    u32 timeouts = AUX_TIMEOUT_TRIES, defers = AUX_DEFER_TRIES;
    u32 w[4], ctl;

    if (*n == 0 || *n > AUX_MAX_BYTES)
        return 1;
    s_last_stat = DPAUX1(DPAUX1_DP_AUXSTAT);
    if (!(s_last_stat & AUXSTAT_HPD_PLUGGED))
        return 2;
    DPAUX1(DPAUX1_DP_AUXADDR) = addr;
    if (cmd == AUXCTL_CMD_AUXWR) {
        memset(w, 0, sizeof(w));
        memcpy(w, buf, *n);
        for (u32 i = 0; i < 4; i++)
            DPAUX1(DPAUX1_DP_AUXDATA_WRITE(i)) = w[i];
    }
    /* ADDRESS_ONLY too: the I2C stop that ends an EDID read leaves it set,
     * and a native write with it set carries no data. */
    ctl = DPAUX1(DPAUX1_DP_AUXCTL) & ~(AUXCTL_CMD_MASK | AUXCTL_CMDLEN_MASK | AUXCTL_RST | AUXCTL_ADDRESS_ONLY);
    ctl |= cmd | (*n - 1);
    while (timeouts && defers) {
        u32 st;
        DPAUX1(DPAUX1_DP_AUXCTL) = ctl | AUXCTL_TRANSACTREQ;
        if (_aux_wait()) {
            s_last_stat = DPAUX1(DPAUX1_DP_AUXSTAT);
            return 3;
        }
        st = DPAUX1(DPAUX1_DP_AUXSTAT);
        s_last_stat = st;
        if (st & (AUXSTAT_TIMEOUT_ERROR | AUXSTAT_RX_ERROR | AUXSTAT_SINKSTAT_ERROR |
                  AUXSTAT_NO_STOP_ERROR)) {
            DPAUX1(DPAUX1_DP_AUXSTAT) = st;        /* the error bits clear on write */
            timeouts--;
            usleep(2);
            continue;
        }
        if ((st & AUXSTAT_REPLY_MASK) == AUXSTAT_REPLY_DEFER) {
            DPAUX1(DPAUX1_DP_AUXSTAT) = st;
            defers--;
            usleep(2);
            continue;
        }
        if ((st & AUXSTAT_REPLY_MASK) != AUXSTAT_REPLY_ACK)
            return 4;
        if (cmd == AUXCTL_CMD_AUXRD) {
            u32 got = st & AUXSTAT_REPLY_M_MASK;
            for (u32 i = 0; i < 4; i++)
                w[i] = DPAUX1(DPAUX1_DP_AUXDATA_READ(i));
            if (got > *n)
                got = *n;
            memcpy(buf, w, got);
            *n = got;
        }
        return 0;
    }
    return timeouts ? 5 : 6;
}

/* One I2C-over-AUX transaction, as L4T's dpaux.c: MOT (middle of
 * transaction) writes and reads keep the I2C transaction open, a plain
 * I2C read of no bytes (address only) ends it. Native and I2C defers are
 * both retried. 0 ok; on a read, *n is what came back. */
static int _aux_i2c(u32 cmd, u32 addr, u8 *buf, u32 *n)
{
    u32 tries = 32, w[4], ctl;

    if (*n > AUX_MAX_BYTES)
        return 1;
    if (!(DPAUX1(DPAUX1_DP_AUXSTAT) & AUXSTAT_HPD_PLUGGED))
        return 2;
    DPAUX1(DPAUX1_DP_AUXADDR) = addr;
    if (*n && cmd == AUXCTL_CMD_MOTWR) {
        memset(w, 0, sizeof(w));
        memcpy(w, buf, *n);
        for (u32 i = 0; i < 4; i++)
            DPAUX1(DPAUX1_DP_AUXDATA_WRITE(i)) = w[i];
    }
    ctl = DPAUX1(DPAUX1_DP_AUXCTL) &
          ~(AUXCTL_CMD_MASK | AUXCTL_CMDLEN_MASK | AUXCTL_RST | AUXCTL_ADDRESS_ONLY);
    ctl |= cmd | (*n ? *n - 1 : AUXCTL_ADDRESS_ONLY);
    while (tries--) {
        u32 st, reply;
        DPAUX1(DPAUX1_DP_AUXCTL) = ctl | AUXCTL_TRANSACTREQ;
        if (_aux_wait())
            return 3;
        st = DPAUX1(DPAUX1_DP_AUXSTAT);
        s_last_stat = st;
        if (st & (AUXSTAT_TIMEOUT_ERROR | AUXSTAT_RX_ERROR | AUXSTAT_SINKSTAT_ERROR |
                  AUXSTAT_NO_STOP_ERROR)) {
            DPAUX1(DPAUX1_DP_AUXSTAT) = st;
            usleep(10);
            continue;
        }
        reply = st & AUXSTAT_REPLY_MASK;
        if (reply == AUXSTAT_REPLY_DEFER || reply == AUXSTAT_REPLY_I2CDEFER) {
            DPAUX1(DPAUX1_DP_AUXSTAT) = st;
            usleep(500);
            continue;
        }
        if (reply != AUXSTAT_REPLY_ACK)
            return 4;
        if (*n && (cmd == AUXCTL_CMD_MOTRD || cmd == AUXCTL_CMD_I2CRD)) {
            u32 got = st & AUXSTAT_REPLY_M_MASK;
            for (u32 i = 0; i < 4; i++)
                w[i] = DPAUX1(DPAUX1_DP_AUXDATA_READ(i));
            if (got > *n)
                got = *n;
            memcpy(buf, w, got);
            *n = got;
        }
        return 0;
    }
    return 5;
}

/* The TV's EDID through the dock's converter, at I2C address 50h: the
 * offset, then 16-byte reads, then the stop. 0 ok. */
int dpout_edid_read(u8 *edid, u32 size)
{
    u8 off = 0;
    u32 n = 1, got = 0;
    int r = _aux_i2c(AUXCTL_CMD_MOTWR, 0x50, &off, &n);
    while (!r && got < size) {
        n = size - got > AUX_MAX_BYTES ? AUX_MAX_BYTES : size - got;
        r = _aux_i2c(AUXCTL_CMD_MOTRD, 0x50, edid + got, &n);
        if (!r && !n)
            r = 6;
        got += n;
    }
    n = 0;
    _aux_i2c(AUXCTL_CMD_I2CRD, 0x50, NULL, &n);
    return r;
}

const char *dpout_aux_error(int r)
{
    static const char *n[] = {
        "ok", "bad size", "no HPD at DPAUX1", "the controller never finished",
        "the sink said no", "too many defers", "errors on the line",
    };
    return (r >= 0 && r < 7) ? n[r] : "?";
}

int dpout_dpcd_read(u32 addr, u8 *buf, u32 n)
{
    while (n) {
        u32 cur = n > AUX_MAX_BYTES ? AUX_MAX_BYTES : n;
        int r = _aux_xfer(AUXCTL_CMD_AUXRD, addr, buf, &cur);
        if (r)
            return r;
        if (!cur)
            return 6;
        addr += cur;
        buf  += cur;
        n    -= cur;
    }
    return 0;
}

int dpout_dpcd_write(u32 addr, const u8 *buf, u32 n)
{
    while (n) {
        u32 cur = n > AUX_MAX_BYTES ? AUX_MAX_BYTES : n;
        int r = _aux_xfer(AUXCTL_CMD_AUXWR, addr, (u8 *)buf, &cur);
        if (r)
            return r;
        addr += cur;
        buf  += cur;
        n    -= cur;
    }
    return 0;
}

const char *dpout_rate_name(u8 rate)
{
    switch (rate) {
    case 0x06: return "1.62 Gbps (RBR)";
    case 0x0A: return "2.7 Gbps (HBR)";
    case 0x14: return "5.4 Gbps (HBR2)";
    default:   return "an unknown rate";
    }
}

static void _text(const u8 *in, char *out, u32 n)
{
    for (u32 i = 0; i < n; i++)
        out[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? (char)in[i] : '.';
    out[n] = 0;
}

int dpout_probe_sink(dpout_sink_t *s)
{
    u8 b[9];
    int r;

    memset(s, 0, sizeof(*s));
    if (dpout_aux_init()) {
        s_printf(s->why, "the SOR partition did not power up");
        return 1;
    }
    s->hpd = dpout_hpd();
    if (!s->hpd) {
        s_printf(s->why, "DPAUX1 does not see HPD: no DisplayPort sink on the dock, or the lanes are not routed to it");
        return 2;
    }
    r = dpout_dpcd_read(0x000, s->dpcd, 16);
    s->aux_stat = s_last_stat;
    if (r) {
        s_printf(s->why, "the sink does not answer the AUX channel (%s, AUXSTAT %08X)",
                 dpout_aux_error(r), s_last_stat);
        return 3;
    }
    s->answered = true;
    dpout_dpcd_read(0x200, &s->sink_count, 1);
    dpout_dpcd_read(0x202, s->lane_status, 3);
    if (!dpout_dpcd_read(0x400, b, 9)) {
        memcpy(s->sink_oui, b, 3);
        _text(b + 3, (char *)s->sink_id, 6);
    }
    if (!dpout_dpcd_read(0x500, b, 9)) {
        memcpy(s->branch_oui, b, 3);
        _text(b + 3, (char *)s->branch_id, 6);
    }
    s->sink_count &= 0x3F;

    /* The TV behind the converter: its EDID, the maker and the name. */
    if (!dpout_edid_read(s->edid, 128) && s->edid[0] == 0x00 && s->edid[1] == 0xFF &&
        s->edid[7] == 0x00) {
        u16 m = (s->edid[8] << 8) | s->edid[9];
        s->edid_ok = true;
        s->tv_maker[0] = '@' + ((m >> 10) & 0x1F);
        s->tv_maker[1] = '@' + ((m >> 5) & 0x1F);
        s->tv_maker[2] = '@' + (m & 0x1F);
        s->tv_maker[3] = 0;
        for (u32 d = 54; d <= 108; d += 18) {
            if (s->edid[d] || s->edid[d + 1] || s->edid[d + 3] != 0xFC)
                continue;
            u32 j = 0;
            for (; j < 13 && s->edid[d + 5 + j] != 0x0A; j++)
                s->tv_name[j] = (s->edid[d + 5 + j] >= 0x20 && s->edid[d + 5 + j] < 0x7F) ?
                                s->edid[d + 5 + j] : '.';
            s->tv_name[j] = 0;
        }
    }
    return 0;
}

/* ======================================================================== */
/* Stages two and three: SOR1, the link, the second head, the picture        */
/* ======================================================================== */

#include <display/di.h>
#include <memory_map.h>
#include <power/max7762x.h>
#include <power/max77620.h>
#include <soc/i2c.h>
#include <soc/hw_init.h>

/* ---- CAR: the PLLs and clocks the picture needs -------------------------- */

#define CLK_SOURCE_DISP2_REG      0x13C
#define  DISP_SRC_PLLD2_OUT0      (5u << 29)
#define CLK_SOURCE_SOR1_REG       0x410
#define  SOR1_CLK_SRC_PLLD2       (5u << 29)
#define  SOR1_CLK_SEL0            BIT(14)   /* 1: the macro's feedback clock */
#define PLLD2_BASE_REG            0x4B8
#define PLLD2_MISC_REG            0x4BC
#define PLLD2_SS_CFG_REG          0x570
#define PLLD2_SS_CTRL2_REG        0x578
#define PLLDP_BASE_REG            0x590
#define PLLDP_MISC_REG            0x594
#define PLLDP_SS_CFG_REG          0x598
#define PLLDP_SS_CTRL2_REG        0x5A0
#define  PLLSS_BASE_LOCK          BIT(27)
#define  PLLSS_BASE_ENABLE        BIT(30)
#define  PLLSS_MISC_EN_LCKDET     BIT(30)
#define  PLLSS_CFG_EN_SDM         BIT(31)
#define  PLLSS_CFG_EN_DITHER      BIT(28)
#define  PLLSS_CFG_BIT24          BIT(24)   /* coreboot: the vendor code sets it */
#define  PLLSS_CFG_CLAMP          BIT(22)
#ifndef CLK_RST_CONTROLLER_CLK_ENB_X_CLR
#define CLK_RST_CONTROLLER_CLK_ENB_X_CLR 0x288
#endif
#ifndef CLK_RST_CONTROLLER_RST_DEV_L_SET
#define CLK_RST_CONTROLLER_RST_DEV_L_SET 0x300
#endif

/* ---- SOR1 at 0x54580000, word offsets as coreboot's sor.h has them ------- */

#define SOR(off)                  MMIO_REG32(SOR1_BASE, (off) << 2)
#define SOR_SUPER_STATE0          0x01
#define SOR_SUPER_STATE1          0x02
#define  SS1_HEAD_OP_SLEEP        0
#define  SS1_HEAD_OP_AWAKE        2
#define  SS1_ORMODE_NORMAL        BIT(2)
#define  SS1_ATTACHED             BIT(3)
#define SOR_STATE0                0x03
#define SOR_STATE1                0x04
#define  S1_OWNER_HEAD1           2
#define  S1_CRC_COMPLETE_RASTER   (1 << 6)
#define  S1_PROTOCOL_DP_A         (8 << 8)
#define  S1_BPP_24_444            (5 << 17)
#define SOR_HEAD_STATE0(h)        (0x05 + (h))
#define SOR_HEAD_STATE1(h)        (0x07 + (h))
#define SOR_HEAD_STATE2(h)        (0x09 + (h))
#define SOR_HEAD_STATE3(h)        (0x0B + (h))
#define SOR_HEAD_STATE4(h)        (0x0D + (h))
#define SOR_HEAD_STATE5(h)        (0x0F + (h))
#define SOR_CRC_CNTRL             0x11
#define  CRC_ARM                  BIT(0)
#define SOR_CRCA                  0x1D       /* bit 0 valid, write 1 to clear */
#define SOR_CRCB                  0x1E
#define SOR_DP_DEBUG_MVID         0x12       /* undocumented in the TRM     */
#define SOR_CLK_CNTRL             0x13
#define  CLK_DP_CLK_SEL_MASK      3
#define  CLK_DP_CLK_SEL_DIFF_DPCLK 3      /* what L4T selects on both chips */
#define  CLK_DP_LINK_SPEED_MASK   (0x1F << 2)
#define  CLK_DP_LINK_SPEED(bw)    ((bw) << 2)   /* 6 = 1.62, 10 = 2.7 Gbps */
#define SOR_PWR                   0x15
#define SOR_TEST                  0x16
#define  TEST_ATTACHED            BIT(10)
#define  TEST_HEAD_OPMODE_MASK    (3 << 8)
#define  TEST_HEAD_OPMODE_AWAKE   (2 << 8)
#define  TEST_HEAD_OPMODE_SLEEP   (0 << 8)
#define SOR_PLL0                  0x17
#define  PLL0_PWR_OFF             BIT(0)
#define  PLL0_VCOPD_ASSERT        BIT(2)
#define SOR_PLL1                  0x18
#define  PLL1_TMDS_TERM_ENABLE    BIT(8)
#define  PLL1_TMDS_TERMADJ_MASK   (0xF << 9)
#define  PLL1_TMDS_TERMADJ(x)     ((x) << 9)
#define  PLL1_TERM_COMPOUT_HIGH   BIT(15)
#define SOR_PLL2                  0x19
#define  PLL2_AUX1_SEQ_PLLCAPPD_OVERRIDE BIT(17)
#define  PLL2_AUX2_ALLOW_POWERDOWN BIT(18)      /* 0: override */
#define  PLL2_AUX6_BANDGAP_POWERDOWN BIT(22)
#define  PLL2_AUX7_PORT_POWERDOWN BIT(23)
#define  PLL2_AUX8_SEQ_PLLCAPPD_ENFORCE BIT(24)
#define SOR_CSTM                  0x1B
#define  CSTM_ROTCLK_MASK         (0xF << 24)
#define  CSTM_ROTCLK(x)           ((x) << 24)
#define  CSTM_LVDS_EN             BIT(16)
#define SOR_LANE_SEQ_CTL          0x21
#define  LSEQ_SETTING_NEW         BIT(31)
#define  LSEQ_SEQUENCE_DOWN       BIT(20)
#define  LSEQ_POWER_STATE_PD      BIT(16)
#define  LSEQ_DELAY(x)            ((x) << 12)
#define SOR_XBAR_CTRL             0x4A
#define  XBAR_BYPASS              BIT(0)
#define  XBAR_LINK_SWAP           BIT(1)
#define SOR_XBAR_POL              0x4B
#define SOR_DP_LINKCTL0           0x4C
#define  LINKCTL_ENABLE           BIT(0)
#define  LINKCTL_TUSIZE_MASK      (0x7F << 2)
#define  LINKCTL_TUSIZE(x)        ((x) << 2)
#define  LINKCTL_ENHANCEDFRAME    BIT(14)
#define  LINKCTL_LANECOUNT_MASK   (0x1F << 16)
#define  LINKCTL_LANECOUNT(n)     (((n) == 4 ? 15 : (n) == 2 ? 3 : (n) == 1 ? 1 : 0) << 16)
#define SOR_DP_DC0                0x4E       /* lane drive current          */
#define SOR_DP_PR0                0x52       /* pre-emphasis                */
#define SOR_DP_PC0                0x56       /* post cursor                 */
#define SOR_DP_CONFIG0            0x58
#define  DPCFG_WATERMARK_MASK     0x3F
#define  DPCFG_ACTIVESYM_COUNT(x) ((x) << 8)
#define  DPCFG_ACTIVESYM_FRAC(x)  ((x) << 16)
#define  DPCFG_ACTIVESYM_POLARITY_POSITIVE BIT(24)
#define  DPCFG_ACTIVESYM_CNTL     BIT(26)
#define  DPCFG_IDLE_BEFORE_ATTACH BIT(28)
#define  DPCFG_RD_RESET_VAL_NEGATIVE BIT(31)
#define SOR_DP_MN0                0x5A
#define SOR_DP_PADCTL0            0x5C
#define  PADCTL_PD_TXD_2_ON       BIT(0)     /* 1: the pad is up            */
#define  PADCTL_PD_TXD_1_ON       BIT(1)
#define  PADCTL_PD_TXD_0_ON       BIT(2)
#define  PADCTL_PD_TXD_3_ON       BIT(3)
#define  PADCTL_COMODE_MASK       (0xF << 4)
#define  PADCTL_TX_PU_VALUE_MASK  (0xFF << 8)
#define  PADCTL_TX_PU             BIT(22)
#define  PADCTL_PAD_CAL_PD        BIT(23)    /* 1: calibration powered down */
#define SOR_DP_SPARE0             0x60
#define  SPARE_PANEL_INTERNAL     BIT(1)
#define  SPARE_SOR_CLK_SEL_MACRO  BIT(2)
#define SOR_DP_AUDIO_HBLANK_SYMBOLS 0x63
#define SOR_DP_AUDIO_VBLANK_SYMBOLS 0x64
#define SOR_DP_TPG                0x6D
#define  TPG_NONE                 0x50505050 /* channel coding, scrambler on */
#define  TPG_TPS1                 0x41414141
#define  TPG_TPS2                 0x42424242
#define  TPG_TPS3                 0x43434343

/* The crossbar between the SOR's lanes and its pads, as the Tegra210 SoC
 * device tree sets it for sor1 (and sor0): nvidia,xbar-ctrl = <2 1 0 3 4>
 * in tegra210-soc-base.dtsi; the Switch's own trees do not override it.
 * DP lane i goes out on pad s_xbar[i], and L4T takes both the lane's power
 * bit and its byte in the drive registers from that pad number (dp_lt.c
 * set_lt_config, sor.c tegra_sor_get_pd_tx_bitmap). coreboot drives SOR0
 * with the crossbar left in bypass, which is a different mapping. */
static u8 s_xbar[5] = { 2, 1, 0, 3, 4 };    /* dpout_lane_test may exchange [0] and [1] */
static bool s_pol_invert;                   /* dpout_lane_test: SOR_XBAR_POL on lanes 0-2  */
/* By pad: its byte in DC/PR/POSTCURSOR, and its PD_TXD power bit. */
static const u8 s_pad_shift[4] = { 16, 8, 0, 24 };
static const u8 s_pad_pd[4] = { BIT(2), BIT(1), BIT(0), BIT(3) };

static u32 _lane_shift(u32 lane)
{
    return s_pad_shift[s_xbar[lane]];
}

/* The PD_TXD bits that power the first n lanes (also the precharge set). */
static u32 _lane_pads(u32 n)
{
    u32 v = 0;
    for (u32 i = 0; i < n; i++)
        v |= s_pad_pd[s_xbar[i]];
    return v;
}

/* ---- the DPCD, as the DisplayPort specification numbers it -------------- */

#define DPCD_LINK_BW_SET          0x100
#define DPCD_LANE_COUNT_SET       0x101
#define  LANE_COUNT_ENHANCED      BIT(7)
#define DPCD_TRAINING_PATTERN_SET 0x102
#define  TPS_NONE                 0x00
#define  TPS_1                    0x01
#define  TPS_2                    0x02
#define  TPS_3                    0x03
#define  TPS_SCRAMBLING_DISABLED  BIT(5)
#define DPCD_TRAINING_LANE0_SET   0x103
#define DPCD_LANE0_1_STATUS       0x202
#define  ALIGN_DONE               BIT(0)
#define DPCD_SINK_STATUS          0x205
#define  SINK_PORT0_IN_SYNC       BIT(0)
#define DPCD_LANE0_1_ADJUST       0x206
#define DPCD_DOWNSPREAD_CTRL      0x107
#define DPCD_CODING_SET           0x108
#define  CODING_ANSI_8B10B        1
#define DPCD_SYMBOL_ERRORS        0x210      /* 2 bytes a lane, bit 15 valid */
#define DPCD_TEST_CRC             0x240      /* R, G, B: 16 bits each       */
#define DPCD_TEST_SINK_MISC       0x246
#define  TEST_CRC_SUPPORTED       BIT(5)
#define  TEST_CRC_COUNT_MASK      0x0F
#define DPCD_TEST_SINK            0x270
#define  TEST_SINK_START          BIT(0)
#define DPCD_EDP_CONFIG_SET       0x10A      /* bit 0: alternate scrambler reset */
#define DPCD_SET_POWER            0x600
#define  POWER_D0                 1
#define  POWER_D3                 2

/* The drive levels, as the Switch's device tree has them ("Default T210
 * lt-data" in tegra210-nx-common-disp.dtsi; coreboot's Tegra210 tables
 * agree except for the pre-emphasis, off by one step in places): indexed
 * by post cursor, swing, pre-emphasis. */
static const u8 s_vs_regs[4][4][4] = {
    { { 0x13, 0x19, 0x1e, 0x28 }, { 0x1e, 0x25, 0x2d }, { 0x28, 0x32 }, { 0x3c } },
    { { 0x12, 0x17, 0x1b, 0x25 }, { 0x1c, 0x23, 0x2a }, { 0x25, 0x2f }, { 0x39 } },
    { { 0x12, 0x16, 0x1a, 0x22 }, { 0x1b, 0x20, 0x27 }, { 0x24, 0x2d }, { 0x36 } },
    { { 0x11, 0x14, 0x17, 0x1f }, { 0x19, 0x1e, 0x24 }, { 0x22, 0x2a }, { 0x32 } },
};
static const u8 s_pe_regs[4][4][4] = {
    { { 0x00, 0x08, 0x12, 0x24 }, { 0x01, 0x0e, 0x1d }, { 0x01, 0x13 }, { 0x00 } },
    { { 0x00, 0x08, 0x12, 0x24 }, { 0x00, 0x0e, 0x1d }, { 0x00, 0x13 }, { 0x00 } },
    { { 0x00, 0x08, 0x12, 0x24 }, { 0x00, 0x0e, 0x1d }, { 0x00, 0x13 }, { 0x00 } },
    { { 0x00, 0x08, 0x12, 0x24 }, { 0x00, 0x0e, 0x1d }, { 0x00, 0x13 }, { 0x00 } },
};
/* The Mariko tree (tegra210b01-odin.dts) overrides the swing L3 entries of
 * post cursor tables L0 to L2: 0x39, 0x37, 0x35. */
static u8 _vs_reg(u32 pc, u32 vs, u32 pe)
{
    static const u8 b01_l3[3] = { 0x39, 0x37, 0x35 };
    if (vs == 3 && pe == 0 && pc < 3 && hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01)
        return b01_l3[pc];
    return s_vs_regs[pc][vs][pe];
}

/* The TX pull-up: L4T enables it with the largest value over the lanes. */
static const u8 s_tx_pu[4][4][4] = {
    { { 0x20, 0x30, 0x40, 0x60 }, { 0x30, 0x40, 0x60 }, { 0x40, 0x60 }, { 0x60 } },
    { { 0x20, 0x20, 0x30, 0x50 }, { 0x30, 0x40, 0x50 }, { 0x40, 0x50 }, { 0x60 } },
    { { 0x20, 0x20, 0x30, 0x40 }, { 0x30, 0x30, 0x40 }, { 0x40, 0x50 }, { 0x60 } },
    { { 0x20, 0x20, 0x20, 0x40 }, { 0x30, 0x30, 0x40 }, { 0x40, 0x40 }, { 0x60 } },
};
static const u8 s_pc_regs[4][4][4] = {
    { { 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00 }, { 0x00 } },
    { { 0x02, 0x02, 0x04, 0x05 }, { 0x02, 0x04, 0x05 }, { 0x04, 0x05 }, { 0x05 } },
    { { 0x04, 0x05, 0x08, 0x0b }, { 0x05, 0x09, 0x0b }, { 0x08, 0x0a }, { 0x0b } },
    { { 0x05, 0x09, 0x0b, 0x12 }, { 0x09, 0x0d, 0x12 }, { 0x0b, 0x0f }, { 0x12 } },
};

/* ---- the mode: 1280x720 at 60 Hz, CEA timing, the GUI's own size --------- */

typedef struct {
    u32 pclk_khz, xres, yres;
    u32 hfp, hsw, hbp, vfp, vsw, vbp;
    u32 href, vref;
} dpout_mode_t;

static const dpout_mode_t s_mode = { 74250, 1280, 720, 110, 40, 220, 5, 5, 20, 1, 1 };

/* The link, as computed and as trained. */
typedef struct {
    u8  lanes, bw;            /* bw: 6 or 10, the DPCD code               */
    bool enhanced, tps3;
    u8  aux_rd_interval;      /* DPCD 00E, units of 4 ms                  */
    u32 tu, active_count, active_frac, watermark;
    bool active_polarity;
    u32 hblank_sym, vblank_sym;
    u8  vs[4], pe[4], pc[4];
    u8  vs_floor;             /* the swing training starts from (margin retries) */
} dpout_link_t;

/* How far the last start got: 0 nothing, 1 the clocks and the SOR1 block,
 * 2 the macro and the lanes, 3 attached with the picture on. The stop
 * undoes whatever was reached. */
static u8 s_stage;
static bool s_assr;               /* DPCD 10A was set: cleared again at stop */
static dpout_link_t s_link;
static void (*s_log)(const char *);

static void _vlog(dpout_video_t *v, const char *text)
{
    s_printf(v->log, "gui: tv: %s\n", text);
    if (s_log)
        s_log(v->log);
}

bool dpout_video_on(void)
{
    return s_stage >= 3;
}

/* The DisplayPort pads' regulators on the MAX77620: LDO8 "vdd-hdmi-dp"
 * (1.05 V, software controlled: hekate turns it on only on the Lite, where
 * it is the I2C1 pad supply at 2.8 V) and LDO7 "vdd-gen-pll-edp" (1.05 V,
 * normally up from the power-up sequence). One found on is left exactly as
 * it is; one found off is set to 1.05 V and enabled. */
static void _rails_on(dpout_video_t *v)
{
    static const struct { u32 id; u8 cfg; const char *name; } rails[2] = {
        { REGULATOR_LDO7, MAX77620_REG_LDO7_CFG, "LDO7 vdd-gen-pll-edp" },
        { REGULATOR_LDO8, MAX77620_REG_LDO8_CFG, "LDO8 vdd-hdmi-dp" },
    };
    char line[120];
    for (u32 i = 0; i < 2; i++) {
        u8 was = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].cfg);
        u8 now = was;
        if (!(was >> 6)) {
            max7762x_regulator_set_voltage(rails[i].id, 1050000);
            max7762x_regulator_enable(rails[i].id, true);
            msleep(5);
            now = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, rails[i].cfg);
        }
        if (i == 0)
            v->ldo7_on = (now >> 6) != 0;
        else
            v->ldo8_on = (now >> 6) != 0;
        s_printf(line, "%s: %s at %d mV%s", rails[i].name, (now >> 6) ? "on" : "OFF",
                 800 + (now & 0x3F) * 50, (was >> 6) ? "" : " (was off, enabled now)");
        _vlog(v, line);
    }
}

/* ---- the PLLs --------------------------------------------------------------- */

/* A PLLD2/PLLDP class PLL: Fvco = Fref / M * (N + 0.5 + din / 8192), out =
 * Fvco / P. P is coded with the "quasi-linear" table of switchroot's
 * clk-tegra210.c (pll_qlin_pdiv_to_hw): codes 0..16 divide by 1 2 3 4 5 6
 * 8 9 10 12 15 16 18 20 24 30 32, so /4 is 3 and /12 is 9 (8 is /10, which
 * ran the head at 89.1 MHz on the first hardware runs). 0 ok. */
static int _pllss_on(u32 base, u32 misc, u32 cfg, u32 ctrl2, u32 m, u32 n, u32 pcode, u32 din)
{
    u32 want = m | (n << 8) | (pcode << 19);
    u32 v = CLOCK(base);
    if ((v & (PLLSS_BASE_ENABLE | PLLSS_BASE_LOCK)) == (PLLSS_BASE_ENABLE | PLLSS_BASE_LOCK) &&
        (v & 0x00F8FFFF) == want && (CLOCK(ctrl2) & 0xFFFF) == din)
        return 0;                                    /* already there */
    CLOCK(base) = want;                              /* IDDQ off, disabled */
    usleep(5);
    CLOCK(misc) = PLLSS_MISC_EN_LCKDET | 0x20;
    CLOCK(ctrl2) = din & 0xFFFF;
    /* Clamped while it starts, released once locked, as coreboot does it. */
    CLOCK(cfg) = PLLSS_CFG_EN_SDM | PLLSS_CFG_EN_DITHER | PLLSS_CFG_BIT24 | PLLSS_CFG_CLAMP;
    CLOCK(base) = want | PLLSS_BASE_ENABLE;
    for (u32 i = 0; i < 2000; i++) {
        if (CLOCK(base) & PLLSS_BASE_LOCK) {
            CLOCK(cfg) = PLLSS_CFG_EN_SDM | PLLSS_CFG_EN_DITHER | PLLSS_CFG_BIT24;
            return 0;
        }
        usleep(1);
    }
    return 1;
}

/* PLLDP: 270 MHz, the SOR macro's reference, from the 38.4 MHz oscillator:
 * 19.2 * (55 + 0.5 + 6144/8192) = 1080 MHz, divided by 4. */
static int _plldp_on(void)
{
    return _pllss_on(PLLDP_BASE_REG, PLLDP_MISC_REG, PLLDP_SS_CFG_REG, PLLDP_SS_CTRL2_REG,
                     2, 55, 3, 6144);
}

/* PLLD2: the pixel clock, 74.25 MHz: 19.2 * (45 + 0.5 + 7424/8192) = 891 MHz,
 * divided by 12 (code 9). L4T hangs the DisplayPort head on pll_d2_out0
 * too, which is PLLD2 itself on Tegra210. */
static int _plld2_on(void)
{
    return _pllss_on(PLLD2_BASE_REG, PLLD2_MISC_REG, PLLD2_SS_CFG_REG, PLLD2_SS_CTRL2_REG,
                     2, 45, 9, 7424);
}

/* ---- the second head's partition and clock -------------------------------- */

static int _disb_on(void)
{
    if (!(PMC(APBDEV_PMC_PWRGATE_STATUS) & BIT(POWER_RAIL_DISB))) {
        if (pmc_domain_pwrgate_set(POWER_RAIL_DISB, 1))
            return 1;
        CLOCK(CLK_RST_CONTROLLER_RST_DEV_L_SET) = BIT(CLK_L_DISP2);
        CLOCK(CLK_RST_CONTROLLER_CLK_ENB_L_SET) = BIT(CLK_L_DISP2);
        usleep(10);
        PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) = BIT(POWER_RAIL_DISB);
        for (u32 i = 0; i < 1000 && (PMC(APBDEV_PMC_REMOVE_CLAMPING_CMD) & BIT(POWER_RAIL_DISB)); i++)
            usleep(1);
    }
    CLOCK(CLK_SOURCE_DISP2_REG) = DISP_SRC_PLLD2_OUT0;
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_L_SET) = BIT(CLK_L_DISP2);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_L_CLR) = BIT(CLK_L_DISP2);
    usleep(5);
    return 0;
}

/* ---- SOR1's clock and block ------------------------------------------------ */

/* The SOR clock is switched with the clock off (TRM 27.6.6): the safe clock
 * until the macro runs, then the macro's feedback clock. */
static void _sor1_clock(bool macro)
{
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_X_CLR) = BIT(CLK_X_SOR1);
    usleep(2);
    CLOCK(CLK_SOURCE_SOR1_REG) = SOR1_CLK_SRC_PLLD2 | (macro ? SOR1_CLK_SEL0 : 0);
    usleep(2);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_X_SET) = BIT(CLK_X_SOR1);
    usleep(2);
}

static void _sor1_block_on(void)
{
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_Y_SET) = BIT(CLK_Y_SOR_SAFE);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_X_SET) = BIT(CLK_X_SOR1);
    _sor1_clock(false);
    usleep(10);
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_X_CLR) = BIT(CLK_X_SOR1);
    usleep(10);
}

static int _sor_poll(u32 reg, u32 mask, u32 want, u32 timeout_us)
{
    for (u32 t = 0; t < timeout_us; t += 20) {
        if ((SOR(reg) & mask) == want)
            return 0;
        usleep(20);
    }
    return 1;
}

static void _sor_field(u32 reg, u32 mask, u32 val)
{
    SOR(reg) = (SOR(reg) & ~mask) | val;
}

static void _sor_update(void)
{
    SOR(SOR_STATE0) = 0;
    SOR(SOR_STATE0) = 1;
    SOR(SOR_STATE0) = 0;
}

static void _sor_super_update(void)
{
    SOR(SOR_SUPER_STATE0) = 0;
    SOR(SOR_SUPER_STATE0) = 1;
    SOR(SOR_SUPER_STATE0) = 0;
}

/* The lane sequencer: lanes up or down, one every 15 us (the TRM's delay). */
static int _lane_sequencer(bool up)
{
    /* Power up walks the lanes down the list, power down walks them up
     * (coreboot); the 1 us step is DisplayPort's, 15 us is for LVDS. */
    SOR(SOR_LANE_SEQ_CTL) = LSEQ_SETTING_NEW | LSEQ_DELAY(1) |
                            (up ? LSEQ_SEQUENCE_DOWN : LSEQ_POWER_STATE_PD);
    return _sor_poll(SOR_LANE_SEQ_CTL, LSEQ_SETTING_NEW, 0, 50000);
}

static u32 _sor_termination_cal(void);

/* SOR1's DisplayPort prods, as switchroot's SoC device trees give them
 * (CTCaer/l4t-hardware-soc-t210: tegra210-prods.dtsi for Erista,
 * tegra210b01-soc-prods.dtsi for Mariko; the Switch's own trees add none
 * for DP). Byte offsets as the trees write them; the mask picks the bits
 * written. L4T applies prod_c_dp, then the one for the link rate. */
typedef struct { u16 off; u32 mask, val; } sor_prod_t;

static const sor_prod_t s_prod_dp_t210[] = {
    { 0x05C, 0x0F000F10, 0x01000310 },    /* PLL0: ICHPMP 1, VCOCAP 3, RESISTORSEL 1   */
    { 0x060, 0x03F00100, 0x00400100 },    /* PLL1: LVDSCM 0, LOADADJ 4, TMDS_TERM      */
    { 0x068, 0x00002000, 0x00002000 },    /* PLL3: PLLVDD_MODE                          */
    { 0x070, 0xFFFFFFFF, 0x00000000 },    /* LVDS                                       */
    { 0x180, 0x00000001, 0x00000001 },    /* DP_SPARE0: SEQ_ENABLE                      */
};
static const sor_prod_t s_prod_dp_t210b01[] = {
    { 0x04C, 0x00000003, 0x00000003 },    /* CLK_CNTRL: DIFF_DPCLK                      */
    { 0x05C, 0x000F0000, 0x00050000 },    /* PLL0 19:16 = 5                             */
    { 0x060, 0x00F00100, 0x00400100 },    /* PLL1: LOADADJ 4, TMDS_TERM                 */
    { 0x070, 0xFFFFFFFF, 0x00000000 },    /* LVDS                                       */
    { 0x170, 0x00002000, 0x00002000 },    /* DP_PADCTL0: a TX_PU_VALUE bit              */
    { 0x180, 0x00000001, 0x00000001 },    /* DP_SPARE0: SEQ_ENABLE                      */
    { 0x1C8, 0x00400000, 0x00000000 },    /* PLL4: ENABLE_LCKDET off                    */
    { 0x1CC, 0xFF000000, 0x34000000 },    /* PADCTL2: SPAREPLL 0x34                     */
};
/* Mariko's prod_c_rbr and prod_c_hbr (the same), and prod_c_hbr2; Erista
 * has none. */
static const sor_prod_t s_prod_hbr_t210b01[] = {
    { 0x04C, 0x00000003, 0x00000003 },    /* CLK_CNTRL: DIFF_DPCLK                      */
    { 0x170, 0x00001000, 0x00001000 },    /* DP_PADCTL0: a TX_PU_VALUE bit              */
};
static const sor_prod_t s_prod_hbr2_t210b01[] = {
    { 0x04C, 0x00000003, 0x00000003 },    /* CLK_CNTRL: DIFF_DPCLK                      */
    { 0x170, 0x00002000, 0x00002000 },    /* DP_PADCTL0: a TX_PU_VALUE bit              */
};

static void _sor_prods(const sor_prod_t *p, u32 n)
{
    for (u32 i = 0; i < n; i++)
        _sor_field(p[i].off >> 2, p[i].mask, p[i].val & p[i].mask);
}

static bool _mariko(void)
{
    return hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01;
}

/* tegra_dp_prepare_pad: the differential DP clock, the link rate, then the
 * prods (common, then the link rate's). */
/* tegra_dp_link_cal: the link rate's prods. */
static void _sor_rate_prods(u8 bw)
{
    if (!_mariko())
        return;
    if (bw == 0x14)
        _sor_prods(s_prod_hbr2_t210b01, ARRAY_SIZE(s_prod_hbr2_t210b01));
    else
        _sor_prods(s_prod_hbr_t210b01, ARRAY_SIZE(s_prod_hbr_t210b01));
}

static void _sor_prepare_pad(u8 bw)
{
    _sor_field(SOR_CLK_CNTRL, CLK_DP_CLK_SEL_MASK, CLK_DP_CLK_SEL_DIFF_DPCLK);
    _sor_field(SOR_CLK_CNTRL, CLK_DP_LINK_SPEED_MASK, CLK_DP_LINK_SPEED(bw));
    if (_mariko()) {
        _sor_prods(s_prod_dp_t210b01, ARRAY_SIZE(s_prod_dp_t210b01));
        _sor_rate_prods(bw);
    } else {
        _sor_prods(s_prod_dp_t210, ARRAY_SIZE(s_prod_dp_t210));
    }
}

/* tegra_sor_dp_pad_power_up, step by step: only the power fields move;
 * the PLL's own settings are the reset values and the prods. The
 * calibration pad stays powered down. */
static void _sor_pad_power_up(void)
{
    _sor_field(SOR_PLL2, PLL2_AUX2_ALLOW_POWERDOWN, 0);                       /* OVERRIDE_POWERDOWN */
    _sor_field(SOR_PLL2, PLL2_AUX1_SEQ_PLLCAPPD_OVERRIDE, PLL2_AUX1_SEQ_PLLCAPPD_OVERRIDE);
    /* 1: port, bandgap, PLL caps, PLL and VCO down; calibration pad down. */
    _sor_field(SOR_PLL2, PLL2_AUX7_PORT_POWERDOWN | PLL2_AUX6_BANDGAP_POWERDOWN |
               PLL2_AUX8_SEQ_PLLCAPPD_ENFORCE,
               PLL2_AUX7_PORT_POWERDOWN | PLL2_AUX6_BANDGAP_POWERDOWN | PLL2_AUX8_SEQ_PLLCAPPD_ENFORCE);
    _sor_field(SOR_PLL0, PLL0_PWR_OFF | PLL0_VCOPD_ASSERT, PLL0_PWR_OFF | PLL0_VCOPD_ASSERT);
    _sor_field(SOR_DP_PADCTL0, PADCTL_PAD_CAL_PD, PADCTL_PAD_CAL_PD);
    /* 2: the pads' deep power down is off already (L4T's pinctrl step). */
    usleep(20);
    /* 3: bandgap. */
    _sor_field(SOR_PLL2, PLL2_AUX6_BANDGAP_POWERDOWN, 0);
    usleep(150);
    /* 4: PLL and VCO, the PLL caps. */
    _sor_field(SOR_PLL0, PLL0_PWR_OFF | PLL0_VCOPD_ASSERT, 0);
    _sor_field(SOR_PLL2, PLL2_AUX8_SEQ_PLLCAPPD_ENFORCE, 0);
    usleep(1000);
    /* 5: the port; the termination the calibration switched off. */
    _sor_field(SOR_PLL2, PLL2_AUX7_PORT_POWERDOWN, 0);
    _sor_field(SOR_PLL1, PLL1_TMDS_TERM_ENABLE, PLL1_TMDS_TERM_ENABLE);
    usleep(20);
}

/* The macro up, as L4T's DP enable has it: prepare the pad, calibrate the
 * termination, power the pad. */
static int _sor_macro_up(u8 bw, u32 *termadj)
{
    _sor_prepare_pad(bw);
    *termadj = _sor_termination_cal();
    _sor_pad_power_up();
    return 0;
}

/* The link's dividers: how many link symbols carry a pixel, and the FIFO
 * watermark, exactly as coreboot computes them (the 100000 is its
 * precision factor). 0 ok. */
static int _link_calc(dpout_link_t *l, const dpout_mode_t *m)
{
    const u64 f = 100000;
    const u32 link_rate = 27 * l->bw * 1000 * 1000;
    const u32 bpp = 24;
    u64 ratio_f, lowest_err = 64 * f, watermark_f;
    u32 num_linkclk_line, best_tu = 64, best_count = 0, best_frac = 0, best_pol = 0;

    if ((u64)m->pclk_khz * 1000 * bpp >= (u64)link_rate * 8 * l->lanes)
        return 1;
    num_linkclk_line = (u32)(((u64)link_rate * m->xres) / (m->pclk_khz * 1000));
    ratio_f = (u64)m->pclk_khz * 1000 * bpp * f / 8;
    ratio_f = ratio_f / (link_rate * l->lanes);

    for (int i = 64; i >= 32; i--) {
        u64 activesym_f = ratio_f * i;
        u64 activecount_f = (activesym_f / f) * f;
        u64 frac_f = activesym_f - activecount_f;
        u32 activecount = (u32)(activecount_f / f);
        u32 polarity, activefrac = 0;
        u64 approx_f, err_f;
        bool neg;

        if (frac_f < f / 2) {
            polarity = 0;
        } else {
            polarity = 1;
            frac_f = f - frac_f;
        }
        if (frac_f != 0) {
            frac_f = (f * f) / frac_f;
            if (frac_f > 15 * f)
                activefrac = polarity ? 1 : 15;
            else
                activefrac = polarity ? (u32)(frac_f / f) + 1 : (u32)(frac_f / f);
        }
        if (activefrac == 1)
            polarity = 0;
        if (polarity == 1)
            approx_f = activefrac ? (activecount_f + (activefrac * f - f) * f) / (activefrac * f)
                                  : activecount_f + f;
        else
            approx_f = activefrac ? activecount_f + f / activefrac : activecount_f;
        if (activesym_f < approx_f) {
            err_f = num_linkclk_line * ((approx_f - activesym_f) / i);
            neg = true;
        } else {
            err_f = num_linkclk_line * ((activesym_f - approx_f) / i);
            neg = false;
        }
        if ((neg && lowest_err > err_f) || err_f == 0) {
            lowest_err = err_f;
            best_tu = i;
            best_count = activecount;
            best_pol = polarity;
            best_frac = activefrac;
            if (err_f == 0)
                break;
        }
    }
    if (best_frac == 0) {
        l->active_polarity = false;
        l->active_count = best_pol ? best_count : best_count - 1;
        l->active_frac = 1;
    } else {
        l->active_polarity = best_pol != 0;
        l->active_count = best_count;
        l->active_frac = best_frac;
    }
    l->tu = best_tu;
    watermark_f = (ratio_f * l->tu * (f - ratio_f)) / f;
    l->watermark = (u32)((watermark_f + lowest_err) / f) + bpp / 4 - 1;
    if (l->watermark > 30)
        l->watermark = 30;
    {
        u32 per_line = (m->xres * bpp) / (8 * l->lanes);
        if (l->watermark > per_line)
            l->watermark = per_line;
    }
    {
        int h = (int)(((u64)(m->hbp + m->hfp + m->hsw - 7) * link_rate) / (m->pclk_khz * 1000))
                - 3 * (l->enhanced ? 1 : 0) - (12 / l->lanes);
        int vb = (int)(((u64)(m->xres - 25) * link_rate) / (m->pclk_khz * 1000)) - (36 / l->lanes) - 4;
        l->hblank_sym = h < 0 ? 0 : (u32)h;
        l->vblank_sym = vb < 0 ? 0 : (u32)vb;
    }
    return 0;
}

static void _sor_set_dp_mode(const dpout_link_t *l)
{
    u32 v;
    _sor_field(SOR_CLK_CNTRL, CLK_DP_LINK_SPEED_MASK, CLK_DP_LINK_SPEED(l->bw));
    v = SOR(SOR_DP_LINKCTL0);
    v &= ~LINKCTL_TUSIZE_MASK;
    v |= LINKCTL_ENABLE | LINKCTL_TUSIZE(l->tu) | (l->enhanced ? LINKCTL_ENHANCEDFRAME : 0);
    SOR(SOR_DP_LINKCTL0) = v;
    SOR(SOR_DP_TPG) = TPG_NONE;
    v = SOR(SOR_DP_CONFIG0);
    v &= ~(DPCFG_WATERMARK_MASK | DPCFG_ACTIVESYM_COUNT(0x7F) | DPCFG_ACTIVESYM_FRAC(0xF) |
           DPCFG_ACTIVESYM_POLARITY_POSITIVE);
    v |= l->watermark | DPCFG_ACTIVESYM_COUNT(l->active_count) | DPCFG_ACTIVESYM_FRAC(l->active_frac) |
         (l->active_polarity ? DPCFG_ACTIVESYM_POLARITY_POSITIVE : 0) |
         DPCFG_ACTIVESYM_CNTL | DPCFG_RD_RESET_VAL_NEGATIVE | DPCFG_IDLE_BEFORE_ATTACH;
    SOR(SOR_DP_CONFIG0) = v;
    _sor_field(SOR_DP_AUDIO_HBLANK_SYMBOLS, 0x1FFFF, l->hblank_sym);
    _sor_field(SOR_DP_AUDIO_VBLANK_SYMBOLS, 0x1FFFF, l->vblank_sym);
}

/* The crossbar between the SOR and its pads, as L4T's
 * tegra_sor_config_xbar sets it: no bypass, no link swap, link 0's
 * channels from s_xbar (the reset value has bypass and swap set). */
static void _sor_xbar(void)
{
    u32 mask = XBAR_BYPASS | XBAR_LINK_SWAP, val = 0;
    for (u32 i = 0; i < 5; i++) {
        mask |= 7u << (2 + 3 * i);
        val |= (u32)s_xbar[i] << (2 + 3 * i);
    }
    _sor_field(SOR_XBAR_CTRL, mask, val);
    /* XBAR_POL: one polarity bit per link 0 channel in bits 0-4 (link 1 in
     * 5-9). The test inverts channels 0-2, which covers the two DP lanes
     * whether the register counts channels before or after the crossbar
     * (lanes 0 and 1 sit on pads 2 and 1); channel 2 carries nothing on a
     * two-lane link. */
    SOR(SOR_XBAR_POL) = s_pol_invert ? 0x07 : 0;
}

void dpout_lane_test(bool invert, bool swap)
{
    s_pol_invert = invert;
    static const u8 normal[5] = { 2, 1, 0, 3, 4 };
    memcpy(s_xbar, normal, sizeof(s_xbar));
    if (swap) {
        s_xbar[0] = normal[1];
        s_xbar[1] = normal[0];
    }
}

/* The termination calibration, as L4T does before the pads come up: the
 * bandgap and the termination on, the calibration pad powered, a binary
 * search on TMDS_TERMADJ against the comparator, everything off again;
 * the value stays in PLL1. */
static u32 _sor_termination_cal(void)
{
    u32 termadj = 8, cur = 8;
    _sor_field(SOR_PLL2, PLL2_AUX6_BANDGAP_POWERDOWN, 0);
    _sor_field(SOR_PLL1, PLL1_TMDS_TERM_ENABLE, PLL1_TMDS_TERM_ENABLE);
    usleep(20);
    _sor_field(SOR_DP_PADCTL0, PADCTL_PAD_CAL_PD, 0);
    usleep(10);
    _sor_field(SOR_PLL1, PLL1_TMDS_TERMADJ_MASK, PLL1_TMDS_TERMADJ(termadj));
    while (cur) {
        usleep(100);
        if (SOR(SOR_PLL1) & PLL1_TERM_COMPOUT_HIGH)
            termadj -= cur;
        cur >>= 1;
        termadj += cur;
        _sor_field(SOR_PLL1, PLL1_TMDS_TERMADJ_MASK, PLL1_TMDS_TERMADJ(termadj));
    }
    _sor_field(SOR_DP_PADCTL0, PADCTL_PAD_CAL_PD, PADCTL_PAD_CAL_PD);
    usleep(10);
    _sor_field(SOR_PLL2, PLL2_AUX6_BANDGAP_POWERDOWN, PLL2_AUX6_BANDGAP_POWERDOWN);
    _sor_field(SOR_PLL1, PLL1_TMDS_TERM_ENABLE, 0);
    usleep(20);
    return termadj;
}

static void _sor_lanes_on(u32 lanes)
{
    u32 v = SOR(SOR_DP_PADCTL0);
    v &= ~(PADCTL_PD_TXD_0_ON | PADCTL_PD_TXD_1_ON | PADCTL_PD_TXD_2_ON | PADCTL_PD_TXD_3_ON);
    v |= _lane_pads(lanes);
    SOR(SOR_DP_PADCTL0) = v;
    _sor_field(SOR_DP_LINKCTL0, LINKCTL_LANECOUNT_MASK, LINKCTL_LANECOUNT(lanes));
}

/* ---- link training ---------------------------------------------------------- */

static void _lt_wait(const dpout_link_t *l, bool cr)
{
    if (!l->aux_rd_interval)
        usleep(cr ? 200 : 500);
    else
        msleep(l->aux_rd_interval * 4);
}

/* The drive levels into the SOR and the DPCD. */
static void _lt_config(dpout_link_t *l)
{
    u8 lane_set[4];
    for (u32 i = 0; i < l->lanes; i++) {
        u32 sh = _lane_shift(i), mask = 0xFFu << sh;
        _sor_field(SOR_DP_PR0, mask, (u32)s_pe_regs[l->pc[i]][l->vs[i]][l->pe[i]] << sh);
        _sor_field(SOR_DP_DC0, mask, (u32)_vs_reg(l->pc[i], l->vs[i], l->pe[i]) << sh);
        if (l->tps3)
            _sor_field(SOR_DP_PC0, mask, (u32)s_pc_regs[l->pc[i]][l->vs[i]][l->pe[i]] << sh);
    }
    {
        u32 pu = 0;
        for (u32 i = 0; i < l->lanes; i++)
            if (s_tx_pu[l->pc[i]][l->vs[i]][l->pe[i]] > pu)
                pu = s_tx_pu[l->pc[i]][l->vs[i]][l->pe[i]];
        _sor_field(SOR_DP_PADCTL0, PADCTL_TX_PU_VALUE_MASK | PADCTL_TX_PU, (pu << 8) | PADCTL_TX_PU);
    }
    usleep(20);
    for (u32 i = 0; i < l->lanes; i++) {
        bool max_vs = l->vs[i] >= 3 - l->pe[i];
        bool max_pe = l->pe[i] >= 3 - l->vs[i];
        lane_set[i] = l->vs[i] | (max_vs ? BIT(2) : 0) | (l->pe[i] << 3) | (max_pe ? BIT(5) : 0);
    }
    dpout_dpcd_write(DPCD_TRAINING_LANE0_SET, lane_set, l->lanes);
    if (l->tps3) {
        u8 pc2 = l->pc[0] | (l->pc[0] >= 3 ? BIT(2) : 0) | (l->pc[1] << 4) | (l->pc[1] >= 3 ? BIT(6) : 0);
        dpout_dpcd_write(0x10F, &pc2, 1);
    }
}

static void _lt_adjust(dpout_link_t *l)
{
    u8 adj[2] = { 0, 0 };
    dpout_dpcd_read(DPCD_LANE0_1_ADJUST, adj, 2);
    for (u32 i = 0; i < l->lanes; i++) {
        u8 b = adj[i / 2];
        u32 sh = (i & 1) ? 4 : 0;
        l->vs[i] = (b >> sh) & 3;
        l->pe[i] = (b >> (sh + 2)) & 3;
        /* Never below the margin floor; swing plus pre-emphasis stays <= 3. */
        if (l->vs[i] < l->vs_floor)
            l->vs[i] = l->vs_floor;
        if (l->vs[i] + l->pe[i] > 3)
            l->pe[i] = 3 - l->vs[i];
    }
    if (l->tps3) {
        u8 pc = 0;
        dpout_dpcd_read(0x20C, &pc, 1);
        for (u32 i = 0; i < l->lanes; i++)
            l->pc[i] = (pc >> (2 * i)) & 3;
    }
}

static void _tpg(dpout_link_t *l, u32 pattern, u8 dpcd)
{
    SOR(SOR_DP_TPG) = pattern;
    dpout_dpcd_write(DPCD_TRAINING_PATTERN_SET, &dpcd, 1);
    (void)l;
}

static bool _cr_done(const dpout_link_t *l, u8 *st)
{
    dpout_dpcd_read(DPCD_LANE0_1_STATUS, st, 3);
    for (u32 i = 0; i < l->lanes; i++)
        if (!((st[i / 2] >> ((i & 1) ? 4 : 0)) & BIT(0)))
            return false;
    return true;
}

static bool _eq_done(const dpout_link_t *l, u8 *st)
{
    dpout_dpcd_read(DPCD_LANE0_1_STATUS, st, 3);
    for (u32 i = 0; i < l->lanes; i++) {
        u8 b = (st[i / 2] >> ((i & 1) ? 4 : 0)) & 7;
        if ((b & 6) != 6)                 /* channel eq done, symbol locked */
            return false;
    }
    return (st[2] & ALIGN_DONE) != 0;
}

static int _link_train(dpout_video_t *v, dpout_link_t *l)
{
    u8 b, st[3];
    char line[120];

    /* Precharge the lanes in use: common mode on for 100 us. */
    _sor_field(SOR_DP_PADCTL0, PADCTL_COMODE_MASK, _lane_pads(l->lanes) << 4);
    usleep(100);
    _sor_field(SOR_DP_PADCTL0, PADCTL_COMODE_MASK, 0);

    /* The link's rate and width, both sides. */
    _sor_field(SOR_CLK_CNTRL, CLK_DP_LINK_SPEED_MASK, CLK_DP_LINK_SPEED(l->bw));
    /* 8b/10b coding, and no spread: PLLDP runs without spread spectrum. */
    b = CODING_ANSI_8B10B;
    dpout_dpcd_write(DPCD_CODING_SET, &b, 1);
    b = 0;
    dpout_dpcd_write(DPCD_DOWNSPREAD_CTRL, &b, 1);
    b = l->bw;
    if (dpout_dpcd_write(DPCD_LINK_BW_SET, &b, 1)) {
        s_printf(v->why, "the sink did not take the link rate");
        return 1;
    }
    b = l->lanes | (l->enhanced ? LANE_COUNT_ENHANCED : 0);
    if (dpout_dpcd_write(DPCD_LANE_COUNT_SET, &b, 1)) {
        s_printf(v->why, "the sink did not take the lane count");
        return 1;
    }
    _sor_field(SOR_DP_LINKCTL0, LINKCTL_LANECOUNT_MASK, LINKCTL_LANECOUNT(l->lanes));
    SOR(SOR_DP_TPG) = TPG_NONE;

    memset(l->vs, l->vs_floor, sizeof(l->vs));
    memset(l->pe, 0, sizeof(l->pe));
    memset(l->pc, 0, sizeof(l->pc));

    /* Clock recovery: pattern 1 until every lane locks, the drive levels
     * following the sink's requests; five unchanged requests end it. */
    _tpg(l, TPG_TPS1, TPS_1 | TPS_SCRAMBLING_DISABLED);
    {
        u32 same = 0, tries = 0;
        bool ok = false;
        while (same < 5 && tries < 20) {
            u8 vs_before[4];
            tries++;
            _lt_config(l);
            _lt_wait(l, true);
            if (_cr_done(l, st)) {
                ok = true;
                break;
            }
            memcpy(vs_before, l->vs, sizeof(vs_before));
            _lt_adjust(l);
            {
                char t[100];
                s_printf(t, "cr try %d: lanes %02X %02X, sink asks swing %d/%d pre-emphasis %d/%d",
                         tries, st[0], st[1], l->vs[0], l->vs[1], l->pe[0], l->pe[1]);
                _vlog(v, t);
            }
            same = memcmp(vs_before, l->vs, sizeof(vs_before)) ? 0 : same + 1;
        }
        v->cr_tries = tries;
        if (!ok) {
            _tpg(l, TPG_NONE, TPS_NONE);
            memcpy(v->lane_status, st, 3);
            s_printf(v->why, "clock recovery failed after %d tries (lanes %02X %02X)", tries, st[0], st[1]);
            return 2;
        }
    }
    s_printf(line, "clock recovery in %d tries, swing %d/%d pre-emphasis %d/%d", v->cr_tries,
             l->vs[0], l->vs[1], l->pe[0], l->pe[1]);
    _vlog(v, line);

    /* Channel equalization: pattern 3 when the sink has it, with
     * post-cursor2, as L4T (link_cfg.tps, is_pc2_supported); else pattern 2. */
    if (l->tps3)
        _tpg(l, TPG_TPS3, TPS_3 | TPS_SCRAMBLING_DISABLED);
    else
        _tpg(l, TPG_TPS2, TPS_2 | TPS_SCRAMBLING_DISABLED);
    {
        bool ok = false;
        u32 tries;
        for (tries = 0; tries < 4; tries++) {
            if (tries) {
                _lt_adjust(l);
                _lt_config(l);
            }
            _lt_wait(l, false);
            if (!_cr_done(l, st)) {
                s_printf(v->why, "clock recovery lost during equalization (lanes %02X %02X)", st[0], st[1]);
                break;
            }
            if (_eq_done(l, st)) {
                ok = true;
                break;
            }
        }
        _tpg(l, TPG_NONE, TPS_NONE);
        memcpy(v->lane_status, st, 3);
        if (!ok) {
            if (!v->why[0])
                s_printf(v->why, "equalization failed after %d tries (lanes %02X %02X align %02X)",
                         tries, st[0], st[1], st[2]);
            return 3;
        }
    }
    memcpy(v->vs, l->vs, 4);
    memcpy(v->pe, l->pe, 4);
    return 0;
}

/* ---- the second head and the attach ---------------------------------------- */

#define DC1(off) MMIO_REG32(DISPLAY_B_BASE, (off) << 2)
#define DPOUT_SYNCPT 11           /* a host1x syncpoint nothing else uses */

static void _dc1_init(const dpout_mode_t *m, u32 fb)
{
    DC1(DC_CMD_STATE_ACCESS) = READ_MUX_ASSEMBLY | WRITE_MUX_ASSEMBLY;
    DC1(DC_CMD_INT_MASK) = 0;
    DC1(DC_CMD_INT_ENABLE) = 0;
    DC1(DC_CMD_REG_ACT_CONTROL) = WIN_A_ACT_HCNTR_SEL | WIN_B_ACT_HCNTR_SEL | WIN_C_ACT_HCNTR_SEL |
                                  WIN_D_ACT_HCNTR_SEL;
    DC1(DC_DISP_DC_MCCIF_FIFOCTRL) = 0;
    DC1(DC_DISP_DISP_MEM_HIGH_PRIORITY) = 0;
    DC1(DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER) = 0;
    DC1(DC_CMD_DISPLAY_POWER_CONTROL) = PW0_ENABLE | PW1_ENABLE | PW2_ENABLE | PW3_ENABLE |
                                        PW4_ENABLE | PM0_ENABLE | PM1_ENABLE;
    DC1(DC_CMD_GENERAL_INCR_SYNCPT_CNTRL) = SYNCPT_CNTRL_NO_STALL;
    DC1(DC_CMD_DISPLAY_WINDOW_HEADER) = WINDOW_A_SELECT | WINDOW_B_SELECT | WINDOW_C_SELECT |
                                        WINDOW_D_SELECT;
    DC1(DC_WIN_WIN_OPTIONS) = 0;
    DC1(DC_WIN_DV_CONTROL) = 0;
    DC1(DC_WINBUF_BLEND_LAYER_CONTROL) = WIN_BLEND_BYPASS | WIN_BLEND_DEPTH(255);
    DC1(DC_DISP_DISP_COLOR_CONTROL) = BASE_COLOR_SIZE_888;
    DC1(DC_DISP_DISP_INTERFACE_CONTROL) = DISP_DATA_FORMAT_DF1P1C;
    DC1(DC_DISP_BLEND_BACKGROUND_COLOR) = 0;
    DC1(DC_COM_CRC_CONTROL) = 0;
    for (u32 i = 0; i < 4; i++)
        DC1(DC_COM_PIN_OUTPUT_ENABLE(i)) = 0;
    DC1(DC_CMD_DISPLAY_COMMAND_OPTION0) = 0;
    DC1(DC_DISP_DISP_WIN_OPTIONS) = 0;
    DC1(DC_CMD_DISPLAY_COMMAND) = DISP_CTRL_MODE_STOP;
    DC1(DC_CMD_STATE_CONTROL) = GENERAL_UPDATE | WIN_A_UPDATE | WIN_B_UPDATE | WIN_C_UPDATE | WIN_D_UPDATE;
    DC1(DC_CMD_STATE_CONTROL) = GENERAL_ACT_REQ | WIN_A_ACT_REQ | WIN_B_ACT_REQ | WIN_C_ACT_REQ |
                                WIN_D_ACT_REQ;

    /* The raster. */
    DC1(DC_DISP_DISP_TIMING_OPTIONS) = VSYNC_H_POSITION(0);
    DC1(DC_DISP_REF_TO_SYNC) = V_REF_TO_SYNC(m->vref) | H_REF_TO_SYNC(m->href);
    DC1(DC_DISP_SYNC_WIDTH)  = V_SYNC_WIDTH(m->vsw) | H_SYNC_WIDTH(m->hsw);
    /* For DP, L4T's tegra_dc_program_mode moves the ref-to-sync lines from
     * the back porch to the front porch of the head (the SOR keeps the
     * mode's own numbers). */
    DC1(DC_DISP_BACK_PORCH)  = V_BACK_PORCH(m->vbp - m->vref) | H_BACK_PORCH(m->hbp);
    DC1(DC_DISP_FRONT_PORCH) = V_FRONT_PORCH(m->vfp + m->vref) | H_FRONT_PORCH(m->hfp);
    DC1(DC_DISP_ACTIVE)      = V_DISP_ACTIVE(m->yres) | H_DISP_ACTIVE(m->xres);
    /* The head's clock is PLLD2 at the pixel clock itself, so the shift
     * clock divides by nothing: L4T's (rate * 2 / pclk) - 2 = 0 (the panel
     * runs DISP1 at three times its pixel clock and uses 4 here). */
    DC1(DC_DISP_DISP_CLOCK_CONTROL) = PIXEL_CLK_DIVIDER_PCD1 | SHIFT_CLK_DIVIDER(0);
    DC1(DC_DISP_SHIFT_CLOCK_OPTIONS) = SC1_H_QUALIFIER_NONE | SC0_H_QUALIFIER_NONE;
    DC1(DC_DISP_DATA_ENABLE_OPTIONS) = DE_SELECT_ACTIVE | DE_CONTROL_NORMAL;

    /* Window A: the frame as it lies in memory, landscape, 32 bits a pixel. */
    DC1(DC_CMD_DISPLAY_WINDOW_HEADER) = WINDOW_A_SELECT;
    DC1(DC_WIN_WIN_OPTIONS) = 0;
    DC1(DC_WIN_COLOR_DEPTH) = WIN_COLOR_DEPTH_B8G8R8A8;
    DC1(DC_WIN_POSITION) = 0;
    DC1(DC_WIN_H_INITIAL_DDA) = 0;
    DC1(DC_WIN_V_INITIAL_DDA) = 0;
    DC1(DC_WIN_PRESCALED_SIZE) = V_PRESCALED_SIZE(m->yres) | H_PRESCALED_SIZE(m->xres * 4);
    DC1(DC_WIN_DDA_INC) = V_DDA_INC(0x1000) | H_DDA_INC(0x1000);
    DC1(DC_WIN_SIZE) = V_SIZE(m->yres) | H_SIZE(m->xres);
    DC1(DC_WIN_LINE_STRIDE) = LINE_STRIDE(m->xres * 4);
    DC1(DC_WIN_BUFFER_CONTROL) = BUFFER_CONTROL_HOST;
    DC1(DC_WINBUF_SURFACE_KIND) = PITCH;
    DC1(DC_WINBUF_START_ADDR) = fb;
    DC1(DC_WINBUF_ADDR_H_OFFSET) = 0;
    DC1(DC_WINBUF_ADDR_V_OFFSET) = 0;
    DC1(DC_WIN_WIN_OPTIONS) = WIN_ENABLE;
    DC1(DC_CMD_STATE_CONTROL) = GENERAL_UPDATE | WIN_A_UPDATE;
    DC1(DC_CMD_STATE_CONTROL) = GENERAL_ACT_REQ | WIN_A_ACT_REQ;
}

static void _vlog(dpout_video_t *v, const char *text);

/* The attach, in the order of L4T's tegra_dc_sor_attach: the SOR told the
 * raster, attached asleep and safe, then normal; the head started and its
 * clock to the SOR enabled; then the SOR awake. */
static int _sor_attach(dpout_video_t *v, const dpout_mode_t *m)
{
    u32 access, htotal, vtotal, hse, vse, hbe, vbe;
    char line[120];

    access = DC1(DC_CMD_STATE_ACCESS);
    DC1(DC_CMD_STATE_ACCESS) = access | WRITE_MUX_ACTIVE;

    /* The SOR's view of the raster: head 1, DisplayPort link A, 24 bits,
     * positive syncs as the CEA mode has them. */
    SOR(SOR_STATE1) = S1_OWNER_HEAD1 | S1_PROTOCOL_DP_A | S1_CRC_COMPLETE_RASTER | S1_BPP_24_444;
    SOR(SOR_HEAD_STATE0(1)) = 0;                       /* progressive, VESA range, RGB */
    htotal = m->hsw + m->hbp + m->xres + m->hfp;
    vtotal = m->vsw + m->vbp + m->yres + m->vfp;
    SOR(SOR_HEAD_STATE1(1)) = (vtotal << 16) | htotal;
    hse = m->hsw - 1;
    vse = m->vsw - 1;
    SOR(SOR_HEAD_STATE2(1)) = (vse << 16) | hse;
    hbe = hse + m->hbp;
    vbe = vse + m->vbp;
    SOR(SOR_HEAD_STATE3(1)) = (vbe << 16) | hbe;
    SOR(SOR_HEAD_STATE4(1)) = ((vbe + m->yres) << 16) | (hbe + m->xres);
    SOR(SOR_HEAD_STATE5(1)) = 1;
    _sor_field(SOR_CSTM, CSTM_ROTCLK_MASK | CSTM_LVDS_EN, CSTM_ROTCLK(2));
    _sor_update();

    /* Attached, asleep, safe; then normal. */
    SOR(SOR_SUPER_STATE1) = SS1_ATTACHED | SS1_HEAD_OP_SLEEP;
    _sor_super_update();
    if (_sor_poll(SOR_TEST, TEST_ATTACHED, TEST_ATTACHED, 50000))
        _vlog(v, "attach: TEST never reported ATTACHED (L4T only logs this)");
    SOR(SOR_SUPER_STATE1) = SS1_ATTACHED | SS1_HEAD_OP_SLEEP | SS1_ORMODE_NORMAL;
    _sor_super_update();

    /* The head: running, then its pixel clock to SOR1. */
    DC1(DC_DISP_DISP_TIMING_OPTIONS) = VSYNC_H_POSITION(1);
    DC1(DC_CMD_DISPLAY_COMMAND) = DISP_CTRL_MODE_C_DISPLAY;
    DC1(DC_DISP_DISP_WIN_OPTIONS) = DC1(DC_DISP_DISP_WIN_OPTIONS) | SOR1_ENABLE;

    /* Awake. */
    SOR(SOR_SUPER_STATE1) = SS1_ATTACHED | SS1_HEAD_OP_AWAKE | SS1_ORMODE_NORMAL;
    _sor_super_update();
    DC1(DC_CMD_STATE_ACCESS) = access;
    /* The same values in the assembly copies, so a later general update
     * cannot take the head or its SOR clock away again. */
    DC1(DC_DISP_DISP_TIMING_OPTIONS) = VSYNC_H_POSITION(1);
    DC1(DC_CMD_DISPLAY_COMMAND) = DISP_CTRL_MODE_C_DISPLAY;
    DC1(DC_DISP_DISP_WIN_OPTIONS) = SOR1_ENABLE;

    {
        int r = _sor_poll(SOR_TEST, TEST_HEAD_OPMODE_MASK, TEST_HEAD_OPMODE_AWAKE, 200000);
        s_printf(line, "attach: test %08X super1 %08X state1 %08X spare %08X winopt %08X cmd %08X",
                 SOR(SOR_TEST), SOR(SOR_SUPER_STATE1), SOR(SOR_STATE1), SOR(SOR_DP_SPARE0),
                 DC1(DC_DISP_DISP_WIN_OPTIONS), DC1(DC_CMD_DISPLAY_COMMAND));
        _vlog(v, line);
        return r;
    }
}

/* How far the picture gets: frames out of the head, the raster into the
 * SOR, the video into the dock's converter. Read only, apart from arming
 * the CRCs and the sink's CRC test, which are stopped again. */
static void _video_check(dpout_video_t *v)
{
    u8 b = 0, d[6];
    char line[160];

    /* The head: frame-end and underflow events over three frames, and
     * the CRC of its active picture. */
    /* The status bits latch only for enabled sources; the BPMP's interrupt
     * controller has the display B line off, so nothing is delivered. */
    DC1(DC_CMD_INT_ENABLE) = BIT(1) | BIT(2) | BIT(8) | BIT(14);  /* frame end, vblank, A under/overflow */
    DC1(DC_CMD_INT_MASK) = 0;
    DC1(DC_CMD_INT_STATUS) = 0xFFFFFFFF;
    DC1(DC_COM_CRC_CONTROL) = BIT(3) | BIT(2) | BIT(0);           /* always, active data, on */
    /* A frame counter that does not depend on interrupt logic: the head
     * increments a host1x syncpoint at every vsync (hekate's panel uses 9). */
    DC1(DC_CMD_CONT_SYNCPT_VSYNC) = SYNCPT_VSYNC_ENABLE | SYNCPT_VSYNC_INDX(DPOUT_SYNCPT);
    {
        u32 c0 = HOST1X(HOST1X_CH0_SYNC_SYNCPT_BASE + 4 * DPOUT_SYNCPT);
        msleep(100);
        v->dc_frames = HOST1X(HOST1X_CH0_SYNC_SYNCPT_BASE + 4 * DPOUT_SYNCPT) - c0;
    }
    /* The pixel clock as the SOR sees it: DP_DEBUG_MVID (0x12, mainline
     * Linux's name) is Mvid against N_VAL (DP_MN0) of the link symbol
     * clock, 270 MHz at 2.7 Gbps, 162 MHz at 1.62. */
    {
        u32 mvid = SOR(SOR_DP_DEBUG_MVID) & 0xFFFFFF;
        u32 nval = SOR(SOR_DP_MN0) & 0xFFFFFF;
        u32 ls_khz = (u32)s_link.bw * 27000;     /* the link symbol clock */
        v->pclk_khz = nval ? (u32)(((u64)mvid * ls_khz) / nval) : 0;
    }
    DC1(DC_CMD_CONT_SYNCPT_VSYNC) = 0;
    v->dc_int = DC1(DC_CMD_INT_STATUS);
    v->dc_crc = DC1(DC_COM_CRC_CONTROL + 1);                       /* DC_COM_CRC_CHECKSUM */
    DC1(DC_COM_CRC_CONTROL) = 0;
    DC1(DC_CMD_INT_ENABLE) = 0;
    DC1(DC_CMD_INT_STATUS) = 0xFFFFFFFF;

    /* The SOR: the CRC of the raster it receives. */
    SOR(SOR_CRCA) = 1;
    SOR(SOR_CRC_CNTRL) = CRC_ARM;
    msleep(50);
    v->sor_crc_valid = (SOR(SOR_CRCA) & 1) != 0;
    v->sor_crc = SOR(SOR_CRCB);
    SOR(SOR_CRC_CNTRL) = 0;

    /* The sink: its status, symbol errors, and its own CRC of the video
     * it receives, when it has the test. */
    dpout_dpcd_read(0x200, v->dpcd_200, 6);
    /* Some converters drop sync for a moment when video starts (the Ugreen
     * dock did, every time) and lock again: the later read counts too. */
    if (v->dpcd_200[5] & SINK_PORT0_IN_SYNC)
        v->in_sync = true;
    dpout_dpcd_read(DPCD_SYMBOL_ERRORS, d, 4);
    v->sym_err[0] = d[0] | (d[1] << 8);
    v->sym_err[1] = d[2] | (d[3] << 8);
    if (!dpout_dpcd_read(DPCD_TEST_SINK_MISC, &b, 1) && (b & TEST_CRC_SUPPORTED)) {
        u8 c0 = b & TEST_CRC_COUNT_MASK;
        v->sink_crc_test = true;
        b = TEST_SINK_START;
        dpout_dpcd_write(DPCD_TEST_SINK, &b, 1);
        msleep(100);
        dpout_dpcd_read(DPCD_TEST_SINK_MISC, &b, 1);
        v->sink_crc_frames = ((b & TEST_CRC_COUNT_MASK) - c0) & TEST_CRC_COUNT_MASK;
        dpout_dpcd_read(DPCD_TEST_CRC, d, 6);
        v->sink_crc[0] = d[0] | (d[1] << 8);
        v->sink_crc[1] = d[2] | (d[3] << 8);
        v->sink_crc[2] = d[4] | (d[5] << 8);
        b = 0;
        dpout_dpcd_write(DPCD_TEST_SINK, &b, 1);
    }
    s_printf(line, "check: dc frames %d pclk %d kHz int %08X crc %08X, sor crc %08X valid %d, dpcd 200 %02X %02X %02X %02X %02X %02X",
             v->dc_frames, v->pclk_khz, v->dc_int, v->dc_crc, v->sor_crc, v->sor_crc_valid, v->dpcd_200[0], v->dpcd_200[1],
             v->dpcd_200[2], v->dpcd_200[3], v->dpcd_200[4], v->dpcd_200[5]);
    _vlog(v, line);
    s_printf(line, "check: symbol errors %04X %04X, sink crc test %d frames %d crc %04X %04X %04X",
             v->sym_err[0], v->sym_err[1], v->sink_crc_test, v->sink_crc_frames, v->sink_crc[0],
             v->sink_crc[1], v->sink_crc[2]);
    _vlog(v, line);
}

/* ---- the whole of it --------------------------------------------------------- */

/* Everything the picture depends on, for the log: CAR and PMC, every SOR1
 * register, the second head's command, display and window A registers
 * (active copies), and the sink's DPCD blocks. Read only. */
static void _dump_words(dpout_video_t *v, const char *tag, volatile u32 *base, u32 first, u32 count)
{
    char line[200];
    for (u32 i = 0; i < count; i += 8) {
        u32 n = count - i < 8 ? count - i : 8;
        char *p = line;
        s_printf(p, "%s %03X:", tag, first + i);
        p += strlen(p);
        for (u32 j = 0; j < n; j++) {
            s_printf(p, " %08X", base[first + i + j]);
            p += strlen(p);
        }
        _vlog(v, line);
    }
}

static void _dump_dpcd(dpout_video_t *v, u32 addr, u32 n)
{
    u8 d[16];
    char line[120], *p = line;
    memset(d, 0, sizeof(d));
    int r = dpout_dpcd_read(addr, d, n);
    s_printf(p, "dpcd %04X:", addr);
    p += strlen(p);
    for (u32 i = 0; i < n; i++) {
        s_printf(p, " %02X", d[i]);
        p += strlen(p);
    }
    if (r) {
        s_printf(p, " (read failed %d)", r);
    }
    _vlog(v, line);
}

static void _dump(dpout_video_t *v)
{
    char line[200];
    volatile u32 *car = (volatile u32 *)CLOCK_BASE;
    volatile u32 *sor = (volatile u32 *)SOR1_BASE;
    volatile u32 *dc = (volatile u32 *)DISPLAY_B_BASE;
    u32 access, header;

    _vlog(v, "dump: start");
    s_printf(line, "car plld2 %08X %08X ss %08X %08X %08X, plldp %08X %08X ss %08X %08X %08X",
             CLOCK(PLLD2_BASE_REG), CLOCK(PLLD2_MISC_REG), CLOCK(PLLD2_SS_CFG_REG),
             CLOCK(PLLD2_SS_CFG_REG + 4), CLOCK(PLLD2_SS_CTRL2_REG), CLOCK(PLLDP_BASE_REG),
             CLOCK(PLLDP_MISC_REG), CLOCK(PLLDP_SS_CFG_REG), CLOCK(PLLDP_SS_CFG_REG + 4),
             CLOCK(PLLDP_SS_CTRL2_REG));
    _vlog(v, line);
    s_printf(line, "car disp1 %08X disp2 %08X sor1 %08X, enb L %08X X %08X Y %08X, rst L %08X X %08X Y %08X",
             CLOCK(0x138), CLOCK(CLK_SOURCE_DISP2_REG), CLOCK(CLK_SOURCE_SOR1_REG),
             CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_L), CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_X),
             CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_Y), CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_L),
             CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_X), CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_Y));
    _vlog(v, line);
    s_printf(line, "pmc pwrgate %08X clamp %08X", PMC(APBDEV_PMC_PWRGATE_STATUS),
             PMC(0x2C));   /* APBDEV_PMC_CLAMP_STATUS */
    _vlog(v, line);
    (void)car;

    _dump_words(v, "sor", sor, 0x00, 0x78);

    access = dc[DC_CMD_STATE_ACCESS];
    header = dc[DC_CMD_DISPLAY_WINDOW_HEADER];
    dc[DC_CMD_STATE_ACCESS] = READ_MUX_ACTIVE | (access & WRITE_MUX_ACTIVE);
    _dump_words(v, "dc", dc, 0x028, 0x020);           /* syncpt, command, power, interrupts, state */
    _dump_words(v, "dc", dc, 0x300, 0x008);           /* CRC, pin outputs */
    _dump_words(v, "dc", dc, 0x400, 0x040);           /* the display registers */
    dc[DC_CMD_DISPLAY_WINDOW_HEADER] = WINDOW_A_SELECT;
    _dump_words(v, "dcA", dc, 0x700, 0x020);          /* window A */
    _dump_words(v, "dcA", dc, 0x800, 0x010);          /* window A buffer */
    /* What the head scans against what the GUI draws: row checksums of
     * both buffers (identical right after the start, unless the copy or
     * the address is wrong; all zero would be a black screen). */
    {
        volatile u32 *gui = (volatile u32 *)NYX_FB2_ADDRESS;
        volatile u32 *tv = (volatile u32 *)dc[DC_WINBUF_START_ADDR];
        static const u16 rows[3] = { 100, 360, 700 };
        u32 sg[3], st[3];
        for (u32 r = 0; r < 3; r++) {
            sg[r] = st[r] = 0;
            for (u32 x = 0; x < 1280; x++) {
                sg[r] += gui[rows[r] * 1280 + x];
                st[r] += tv[rows[r] * 1280 + x];
            }
        }
        s_printf(line, "fb rows 100/360/700: gui %08X %08X %08X, scanned at %08X %08X %08X %08X",
                 sg[0], sg[1], sg[2], (u32)tv, st[0], st[1], st[2]);
        _vlog(v, line);
    }
    dc[DC_CMD_DISPLAY_WINDOW_HEADER] = header;
    dc[DC_CMD_STATE_ACCESS] = access;

    s_printf(line, "dpaux intr %08X auxstat %08X", DPAUX1(DPAUX1_INTR_AUX), DPAUX1(DPAUX1_DP_AUXSTAT));
    _vlog(v, line);
    _dump_dpcd(v, 0x000, 16);
    _dump_dpcd(v, 0x2200, 16);
    _dump_dpcd(v, 0x300, 12);
    _dump_dpcd(v, 0x080, 16);
    _dump_dpcd(v, 0x100, 16);
    _dump_dpcd(v, 0x200, 16);
    _dump_dpcd(v, 0x210, 8);
    _dump_dpcd(v, 0x240, 8);
    _dump_dpcd(v, 0x600, 1);
    _vlog(v, "dump: end");
}

static int _video_start(dpout_video_t *v, const dpout_sink_t *s, u32 fb);

int dpout_video_start(dpout_video_t *v, const dpout_sink_t *s, u32 fb)
{
    int r = _video_start(v, s, fb);
    if (s_stage)
        _dump(v);
    return r;
}

static int _video_start(dpout_video_t *v, const dpout_sink_t *s, u32 fb)
{
    dpout_link_t *l = &s_link;
    char line[160];
    u8 b;

    memset(v, 0, sizeof(*v));
    if (s_stage)
        dpout_video_stop();
    if (!s->answered) {
        s_printf(v->why, "no sink on the AUX channel");
        return 1;
    }
    /* tegra_dp_dpcd_init: the source's IEEE OUI (NVIDIA) into DPCD 300h. */
    {
        static const u8 oui[3] = { 0x00, 0x04, 0x4B };
        int r = dpout_dpcd_write(0x300, oui, 3);
        _vlog(v, r ? "source OUI 00044B: the write failed" : "source OUI 00044B written");
    }
    memset(l, 0, sizeof(*l));
    l->lanes = DPCD_MAX_LANES(s) >= 2 ? 2 : 1;            /* the port has two */
    /* The sink's maximum rate, at most this SoC's HBR2, as L4T trains:
     * 1.62, 2.7 or 5.4 Gbps (lower ones follow if training fails). */
    {
        u8 max = DPCD_MAX_RATE(s);
        l->bw = max >= 0x14 ? 0x14 : max >= 0x0A ? 0x0A : 0x06;
    }
    l->enhanced = DPCD_ENHANCED(s);
    l->tps3 = DPCD_TPS3(s);                                 /* L4T: whenever the sink has it */
    l->aux_rd_interval = s->dpcd[0x0E] & 0x7F;
    v->lanes = l->lanes;
    v->rate = l->bw;
    if (_link_calc(l, &s_mode)) {
        s_printf(v->why, "the mode does not fit the link");
        return 2;
    }
    s_printf(line, "link %d lanes at %s: tu %d, active %d.%d%s, watermark %d, blank %d/%d",
             l->lanes, dpout_rate_name(l->bw), l->tu, l->active_count, l->active_frac,
             l->active_polarity ? "+" : "-", l->watermark, l->hblank_sym, l->vblank_sym);
    _vlog(v, line);

    /* The pads' regulators, then the clocks: the macro's reference, the
     * pixel clock, the head, SOR1. */
    _rails_on(v);
    if (_plldp_on()) {
        s_printf(v->why, "PLLDP did not lock");
        return 3;
    }
    if (_plld2_on()) {
        s_printf(v->why, "PLLD2 did not lock");
        return 4;
    }
    if (_disb_on()) {
        s_printf(v->why, "the DISB partition did not power up");
        return 5;
    }
    _sor1_block_on();
    s_stage = 1;
    _vlog(v, "PLLDP 270 MHz, PLLD2 74.25 MHz, DISB and SOR1 up");

    /* The head, with the frame, stopped until the attach. */
    _dc1_init(&s_mode, fb);

    /* The SOR macro: PLL and bandgap up, then its own feedback clock, then
     * the lanes, then the link parameters. */
    {
        u32 termadj = 0;
        int r = _sor_macro_up(l->bw, &termadj);
        s_printf(line, "termination calibrated to %d", termadj);
        _vlog(v, line);
        if (r) {
            s_printf(v->why, "the SOR macro's PLL did not come up");
            return 6;
        }
    }
    s_stage = 2;
    _sor1_clock(true);
    _sor_lanes_on(l->lanes);
    if (_lane_sequencer(true)) {
        s_printf(v->why, "the lane sequencer did not finish powering the lanes");
        return 7;
    }
    _sor_set_dp_mode(l);
    _sor_xbar();
    /* tegra_dc_sor_set_internal_panel(false): an external panel and the
     * macro clock; SEQ_ENABLE stays as the prods set it (the sequencer is
     * never triggered: L4T does not touch SOR_PWR for DP). */
    SOR(SOR_DP_SPARE0) = (SOR(SOR_DP_SPARE0) & ~SPARE_PANEL_INTERNAL) | SPARE_SOR_CLK_SEL_MACRO;
    /* A sink that can use the alternate scrambler reset gets it, on both
     * sides, as L4T does (tegra_dc_dp_set_assr, before training): DPCD 10A
     * bit 0, and the SOR's internal-panel bit, which starts its scrambler at
     * 0xFFFE. Training patterns are not scrambled, so a mismatch here only
     * shows once video flows: a clean link and a converter seeing nothing. */
    if (s->dpcd[0x0D] & 1) {
        b = 1;
        if (!dpout_dpcd_write(DPCD_EDP_CONFIG_SET, &b, 1)) {
            _sor_field(SOR_DP_SPARE0, SPARE_PANEL_INTERNAL, SPARE_PANEL_INTERNAL);
            v->assr = true;
            s_assr = true;
            _vlog(v, "the sink can use the alternate scrambler reset: on, both sides");
        }
    }
    s_printf(line, "xbar %08X pol %08X padctl %08X linkctl %08X clk %08X pll0 %08X pll2 %08X",
             SOR(SOR_XBAR_CTRL), SOR(SOR_XBAR_POL), SOR(SOR_DP_PADCTL0), SOR(SOR_DP_LINKCTL0),
             SOR(SOR_CLK_CNTRL), SOR(SOR_PLL0), SOR(SOR_PLL2));
    _vlog(v, line);
    _vlog(v, _mariko() ? "SOR1 macro up on its feedback clock, lanes powered (Mariko prods)"
                       : "SOR1 macro up on its feedback clock, lanes powered (Erista prods)");
    if (s_pol_invert || s_xbar[0] != 2) {
        s_printf(line, "BENCH TEST from hwtest.ini: lanes %s%s%s (xbar lane 0 -> pad %d, lane 1 -> pad %d)",
                 s_pol_invert ? "inverted" : "", s_pol_invert && s_xbar[0] != 2 ? " and " : "",
                 s_xbar[0] != 2 ? "exchanged" : "", s_xbar[0], s_xbar[1]);
        _vlog(v, line);
    }

    /* The sink awake, then the link. */
    b = POWER_D0;
    for (u32 i = 0; i < 3 && dpout_dpcd_write(DPCD_SET_POWER, &b, 1); i++)
        usleep(1000);
    if (!dpout_hpd()) {
        s_printf(v->why, "HPD went away");
        return 8;
    }
    for (;;) {
        if (!_link_train(v, l)) {
            /* The margin: symbol errors over 20 ms of idle pattern (the
             * counters clear when read). The Nintendo dock showed a picture
             * only on runs without errors on DP lane 0. */
            u8 e0[4], e4[4];
            u32 errs;
            dpout_dpcd_read(DPCD_SYMBOL_ERRORS, e0, 4);
            msleep(20);
            dpout_dpcd_read(DPCD_SYMBOL_ERRORS, e4, 4);
            errs = ((e4[0] | (e4[1] << 8)) & 0x7FFF) + ((e4[2] | (e4[3] << 8)) & 0x7FFF);
            s_printf(line, "margin at swing %d/%d: %d symbol errors in 20 ms (%02X%02X %02X%02X; "
                     "first read %02X%02X %02X%02X)", l->vs[0], l->vs[1], errs, e4[1], e4[0],
                     e4[3], e4[2], e0[1], e0[0], e0[3], e0[2]);
            _vlog(v, line);
            v->margin_errors = errs;
            if (!errs || l->vs_floor >= 2)
                break;
            l->vs_floor++;
            v->margin_retries++;
            s_printf(line, "training again from swing %d", l->vs_floor);
            _vlog(v, line);
            continue;
        }
        if (l->bw == 0x06)
            return 9;
        {
            u8 next = l->bw == 0x14 ? 0x0A : 0x06;
            s_printf(line, "training failed at %s: %s; trying %s", dpout_rate_name(l->bw), v->why,
                     dpout_rate_name(next));
            _vlog(v, line);
            l->bw = next;
            l->vs_floor = 0;
        }
        v->rate = l->bw;
        v->why[0] = 0;
        if (_link_calc(l, &s_mode)) {
            s_printf(v->why, "the mode does not fit the link");
            return 2;
        }
        _sor_set_dp_mode(l);
        _sor_rate_prods(l->bw);
    }
    s_printf(line, "link trained: lanes %02X %02X align %02X, swing %d/%d pre-emphasis %d/%d",
             v->lane_status[0], v->lane_status[1], v->lane_status[2], l->vs[0], l->vs[1],
             l->pe[0], l->pe[1]);
    _vlog(v, line);

    /* The SOR attached to the head and awake, the picture on. No SOR_PWR
     * trigger: that runs the power sequencer, which L4T leaves alone for
     * DisplayPort. */
    if (_sor_attach(v, &s_mode)) {
        s_printf(v->why, "the head did not wake up behind the SOR");
        return 11;
    }
    s_stage = 3;
    msleep(85);
    if (!dpout_dpcd_read(DPCD_SINK_STATUS, &b, 1))
        v->in_sync = (b & SINK_PORT0_IN_SYNC) != 0;
    dpout_dpcd_read(DPCD_LANE0_1_STATUS, v->lane_status, 3);
    s_printf(line, "attached, sink %s sync, lanes %02X %02X %02X", v->in_sync ? "in" : "NOT in",
             v->lane_status[0], v->lane_status[1], v->lane_status[2]);
    _vlog(v, line);
    _video_check(v);
    return 0;
}

void dpout_video_stop(void)
{
    u8 b = POWER_D3;
    u8 stage = s_stage;
    if (!stage)
        return;
    s_stage = 0;
    /* The TRM's stop: safe, asleep, detached; the head stopped; the SOR
     * on its safe clock, lanes and PLL down; the block reset. A start
     * that failed part way is undone from the stage it reached. */
    if (stage >= 3) {
        SOR(SOR_SUPER_STATE1) = SS1_ATTACHED | SS1_HEAD_OP_AWAKE;
        _sor_super_update();
        SOR(SOR_SUPER_STATE1) = SS1_ATTACHED | SS1_HEAD_OP_SLEEP;
        _sor_super_update();
        _sor_poll(SOR_TEST, TEST_HEAD_OPMODE_MASK, TEST_HEAD_OPMODE_SLEEP, 100000);
        SOR(SOR_SUPER_STATE1) = SS1_HEAD_OP_SLEEP;
        _sor_super_update();
        _sor_poll(SOR_TEST, TEST_ATTACHED, 0, 100000);
        SOR(SOR_STATE1) = 0;
        _sor_update();
    }
    /* The head stopped in its assembly copies first, window A with it: the
     * attach mirrored C_DISPLAY and SOR1_ENABLE there, and a general update
     * would bring them back (it did: the head kept scanning the TV buffer at
     * 60 Hz after leaving, and the GUI felt it). Then the active copies, at
     * once, as the TRM's stop has it. */
    DC1(DC_CMD_STATE_ACCESS) = READ_MUX_ASSEMBLY | WRITE_MUX_ASSEMBLY;
    DC1(DC_CMD_DISPLAY_WINDOW_HEADER) = WINDOW_A_SELECT;
    DC1(DC_WIN_WIN_OPTIONS) = 0;
    DC1(DC_DISP_DISP_WIN_OPTIONS) = 0;
    DC1(DC_CMD_DISPLAY_COMMAND) = DISP_CTRL_MODE_STOP;
    DC1(DC_CMD_STATE_CONTROL) = GENERAL_UPDATE | WIN_A_UPDATE;
    DC1(DC_CMD_STATE_CONTROL) = GENERAL_ACT_REQ | WIN_A_ACT_REQ;
    DC1(DC_CMD_STATE_ACCESS) = READ_MUX_ACTIVE | WRITE_MUX_ACTIVE;
    DC1(DC_WIN_WIN_OPTIONS) = 0;
    DC1(DC_DISP_DISP_WIN_OPTIONS) = 0;
    DC1(DC_CMD_DISPLAY_COMMAND) = DISP_CTRL_MODE_STOP;
    DC1(DC_CMD_STATE_ACCESS) = READ_MUX_ASSEMBLY | WRITE_MUX_ASSEMBLY;
    msleep(20);                     /* the frame in flight ends */
    DC1(DC_CMD_DISPLAY_POWER_CONTROL) = 0;
    /* And the head off altogether: its clock gated, held in reset (the next
     * start's _disb_on releases both). */
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_L_SET) = BIT(CLK_L_DISP2);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_L_CLR) = BIT(CLK_L_DISP2);
    _sor1_clock(false);
    if (stage >= 2) {
        _sor_field(SOR_DP_PADCTL0, PADCTL_PD_TXD_0_ON | PADCTL_PD_TXD_1_ON | PADCTL_PD_TXD_2_ON |
                   PADCTL_PD_TXD_3_ON, 0);
        _lane_sequencer(false);
        _sor_field(SOR_PLL2, PLL2_AUX7_PORT_POWERDOWN, PLL2_AUX7_PORT_POWERDOWN);
        usleep(20);
        _sor_field(SOR_PLL0, PLL0_PWR_OFF | PLL0_VCOPD_ASSERT, PLL0_PWR_OFF | PLL0_VCOPD_ASSERT);
        _sor_field(SOR_PLL2, PLL2_AUX1_SEQ_PLLCAPPD_OVERRIDE | PLL2_AUX8_SEQ_PLLCAPPD_ENFORCE,
                   PLL2_AUX1_SEQ_PLLCAPPD_OVERRIDE | PLL2_AUX8_SEQ_PLLCAPPD_ENFORCE);
        usleep(20);
        _sor_field(SOR_PLL2, PLL2_AUX6_BANDGAP_POWERDOWN, PLL2_AUX6_BANDGAP_POWERDOWN);
    }
    CLOCK(CLK_RST_CONTROLLER_RST_DEV_X_SET) = BIT(CLK_X_SOR1);
    CLOCK(CLK_RST_CONTROLLER_CLK_ENB_X_CLR) = BIT(CLK_X_SOR1);
    if (stage >= 2) {
        if (s_assr) {
            u8 z = 0;
            dpout_dpcd_write(DPCD_EDP_CONFIG_SET, &z, 1);
        }
        dpout_dpcd_write(DPCD_SET_POWER, &b, 1);
    }
    s_assr = false;
    /* The pixel clock and the macro's reference off too (the next start's
     * _pllss_on locks them again). */
    CLOCK(PLLD2_BASE_REG) &= ~PLLSS_BASE_ENABLE;
    CLOCK(PLLDP_BASE_REG) &= ~PLLSS_BASE_ENABLE;
}

void dpout_set_logger(void (*fn)(const char *))
{
    s_log = fn;
}
