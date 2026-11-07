/*
 * hwtest GUI - a Bluetooth host for Nintendo controllers. See gui_bt.h.
 *
 * The radio comes up exactly as the health check's Bluetooth probe brings
 * it up (probe_bt.c, its first arm): straps parked and both REG_ON pins
 * low for a cold power-on reset, then HCI_Reset at 115200. That probe is
 * not called, because it puts everything back when it is done, while this
 * keeps the radio on until the screen is left.
 *
 * After the reset it is plain Bluetooth. No vendor command is ever sent, so
 * the radio runs its ROM firmware as it is (HOS loads a firmware patch over
 * HCI first; that patch is not ours to ship), keeps the address it comes up
 * with, and stays at 115200. One Joy-Con's full report is about 60 bytes on
 * the wire 60 times a second, a third of the line.
 *
 * The GUI draws a frame for up to a tenth of a second without reading
 * anything, which at 115200 is more than a kilobyte. So once HCI answers,
 * the UART runs with hardware flow control both ways: our RTS goes up when
 * the receive FIFO fills, and the radio holds what it has until we read.
 *
 * Security is asked for only when the controller asks for it: the HID
 * channels are opened first, and only a "security block" answer leads to
 * pairing. Pairing starts without bonding, which a controller may accept
 * without storing anything, and falls back to bonding if it refuses.
 */
#include "gui.h"
#include "gui_bt.h"

/* ---- the radio's pins and UART (as probe_bt.c) ------------------------- */

#define BT_UART      UART_D
#define BT_UART_REGS ((volatile uart_t *)(UART_BASE + 0x300))

#define PMX_PH3_DEV_WAKE   0x1C0
#define PMX_PH4_BT_REG_ON  0x1C4
#define PMX_PH5_HOST_WAKE  0x1C8
#define PMX_PH7_BT_GPIO5   PINMUX_AUX_AP_WAKE_NFC      /* 0x1CC, NOT PH6 */
#define PMX_PK0_BT_GPIO2   (PINMUX_AUX_GPIO_PK3 - 0xC) /* 0x254 */
#define PMX_PK1_BT_GPIO3   (PINMUX_AUX_GPIO_PK3 - 0x8) /* 0x258 */
#define PMX_PK2_BT_GPIO4   (PINMUX_AUX_GPIO_PK3 - 0x4) /* 0x25C */

#define LSR_OVRF BIT(1)
#define LSR_PERR BIT(2)
#define LSR_FERR BIT(3)
#define LSR_BRK  BIT(4)
#define MSR_CTS  BIT(4)
#define FCR_TRIG16 (3u << 6)
#define FCR_TRIG8  (2u << 6)

#define BT_POR_MS     200   /* the probe's port-H silence after the edge   */
#define BT_RESET_MS   700   /* per HCI_Reset                               */

/* ---- timings of the protocol ------------------------------------------- */

#define SCAN_LEN      0x08  /* inquiry length, x 1.28 s                    */
#define SCAN_GUARD_MS 13000
#define NAME_GUARD_MS 7000
#define CONN_GUARD_MS 12000 /* paging gives up by itself after 5.12 s      */
#define SEC_GUARD_MS  30000
#define CHAN_GUARD_MS 8000
#define SUB_RETRY_MS  300
#define SUB_TRIES     4

/* HCI opcodes. */
#define OP_INQUIRY          0x0401
#define OP_INQUIRY_CANCEL   0x0402
#define OP_CREATE_CONN      0x0405
#define OP_DISCONNECT       0x0406
#define OP_CREATE_CANCEL    0x0408
#define OP_REJECT_CONN      0x040A
#define OP_LINK_KEY_REPLY   0x040B
#define OP_LINK_KEY_NEG     0x040C
#define OP_PIN_NEG          0x040E
#define OP_AUTH_REQ         0x0411
#define OP_SET_ENCRYPT      0x0413
#define OP_NAME_REQ         0x0419
#define OP_READ_FLUSH_TO    0x0C27
#define OP_LINK_POLICY      0x080F
#define OP_LINK_POLICY_CONN 0x080D
#define OP_EXIT_SNIFF       0x0804
#define OP_QOS_SETUP        0x0807
#define OP_PKT_TYPE         0x040F
#define PKT_TYPES           0xFF1E   /* basic rate only, see _connect_now */
#define OP_READ_REMOTE_FEAT 0x041B
#define OP_READ_REMOTE_EXT  0x041C
#define OP_IO_CAP_REPLY     0x042B
#define OP_USER_CONF_REPLY  0x042C
#define OP_PASSKEY_NEG      0x042F
#define OP_SET_EVENT_MASK   0x0C01
#define OP_RESET            0x0C03
#define OP_INQUIRY_MODE     0x0C45
#define OP_SSP_MODE         0x0C56
#define OP_PIN_REPLY        0x040D
#define OP_READ_VERSION     0x1001
#define OP_READ_FEATURES    0x1003
#define OP_READ_BUFFER      0x1005
#define OP_READ_BDADDR      0x1009

/* L2CAP. */
#define PSM_HID_CTRL  0x0011
#define PSM_HID_INTR  0x0013
#define CID_SIGNAL    0x0001
#define CID_CTRL      0x0040   /* ours */
#define CID_INTR      0x0041

/* The controllers' own protocol, as on the rail (gui_jcwire.c): output
 * report 0x01 carries a subcommand after the rumble bytes; the answer is
 * input report 0x21. 0x30 is the full input report. */
#define SUB_DEVICE_INFO 0x02
#define SUB_DATA_FORMAT 0x03
#define SUB_LEDS        0x30
static const u8 s_rumble_off[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };

/* ---- state -------------------------------------------------------------- */

typedef struct {
    u16 psm, scid, dcid;
    bool conn, ours_ok, theirs_ok;
} chan_t;

static gui_bt_state_t s_state = BT_OFF;
static char s_why[96], s_step[64];
static u8 s_local[6];

static gui_bt_dev_t s_dev[BT_DEV_MAX];
static u32 s_dev_n;
static int s_cur = -1;           /* the device being connected or connected */
static int s_pending = -1;       /* to connect once the scan has stopped    */
static u32 s_deadline;

static u16 s_handle = 0xFFFF;
static bool s_secure, s_paired, s_retried, s_redial;
static int s_blocked = -1;       /* the channel a security block stopped    */
static u8 s_key[16];
static u8 s_key_addr[6];
static bool s_key_ok;

static chan_t s_ch[2];
static u8 s_sig_id = 1;

static gui_bt_pad_state_t s_pad;
static u32 s_rate_at, s_rate_base;
static u8 s_hid_pkt;
static u32 s_setup_step, s_setup_tries, s_setup_at;

static u32 s_line_err;
static u32 s_data_traced;        /* HID frames traced on this connection */
static u32 s_acl_sent, s_acl_done;  /* ACL packets handed over, and sent */

/* What gui_bt_open changed, to put back. */
static const struct { u32 id; u8 cfg; } s_rails[] = {
    { REGULATOR_LDO1, MAX77620_REG_LDO1_CFG },
    { REGULATOR_LDO7, MAX77620_REG_LDO7_CFG },
};
static u8 s_rail_saved[ARRAY_SIZE(s_rails)];
static u8 s_ame_saved, s_32k_saved;
static bool s_powered;

/* HCI command credits and a small queue for when there are none. */
#define CMDQ_N 8
static struct { u16 op; u8 len; u8 p[24]; } s_cmdq[CMDQ_N];
static u32 s_cmdq_head, s_cmdq_n;
static u8 s_ncmd = 1;

/* The last Command Complete, for the synchronous set-up commands. */
static struct { u16 op; u8 status; u8 ret[16]; u32 seq; } s_cc;

/* ACL credits and a queue of L2CAP frames waiting for one. */
#define ACLQ_N 4
static struct { u16 cid; u8 len; u8 d[64]; } s_aclq[ACLQ_N];
static u32 s_aclq_head, s_aclq_n;
static u32 s_acl_credits = 1;

/* The H4 reader. */
static struct {
    u8 type;          /* 0: waiting for a packet type                    */
    u32 hdr, need, n; /* header length, header+payload length, have      */
    u32 skip;         /* bytes of a vendor diagnostic packet to drop      */
    u8 buf[1100];
} s_rx;
static u8 s_l2[1100];
static u32 s_l2_n, s_l2_need;

static void _event(u8 code, const u8 *p, u32 n);
static void _acl(const u8 *h, const u8 *d, u32 n);

/* ---- small helpers ------------------------------------------------------ */

/* The last few steps, kept for the screen as well as the debug UART: on
 * the console a photo of the screen is often all there is. */
#define LOG_N   10
#define LOG_LEN 72
static char s_log[LOG_N][LOG_LEN];
static u32 s_log_seq;

static void _logs(const char *s)
{
    char buf[160];
    s_printf(buf, "gui: bt %s\n", s);
    gui_uart_puts(buf);
    char *d = s_log[s_log_seq % LOG_N];
    u32 i = 0;
    for (; s[i] && i < LOG_LEN - 1; i++)
        d[i] = s[i];
    d[i] = 0;
    s_log_seq++;
}

/* The wire, for the debug console only (not the screen's list): every
 * command, event and L2CAP signal, with its first bytes. */
static void _trace(const char *what, u32 code, const u8 *p, u32 n)
{
    char buf[160];
    s_printf(buf, "gui: bt> %s %04X len %d:", what, code, n);
    for (u32 i = 0; i < n && i < 20; i++)
        s_printf(buf + strlen(buf), " %02X", p[i]);
    strcat(buf, n > 20 ? " ...\n" : "\n");
    gui_uart_puts(buf);
}

static void _step(const char *s)
{
    strcpy(s_step, s);
    _logs(s);
}

static void _fail(const char *why)
{
    strcpy(s_why, why);
    _logs(why);
}

static void _addr_str(char *out, const u8 *a)
{
    s_printf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[5], a[4], a[3], a[2], a[1], a[0]);
}

static const char *_status_text(u8 st)
{
    switch (st) {
    case 0x04: return "it did not answer";
    case 0x05: return "pairing failed";
    case 0x06: return "it has no key for us";
    case 0x08: return "the link timed out";
    case 0x0B: return "already connected";
    case 0x0C: return "the radio refused";
    case 0x0D: case 0x0E: case 0x0F: return "it refused";
    case 0x13: return "it disconnected";
    case 0x16: return "we disconnected";
    case 0x18: return "it does not allow pairing";
    case 0x22: return "it stopped answering";
    case 0x29: return "it wants a unit key";
    default:   return NULL;
    }
}

static void _fail_status(const char *what, u8 st)
{
    char buf[96];
    const char *t = _status_text(st);
    if (t)
        s_printf(buf, "%s: %s (0x%02X)", what, t, st);
    else
        s_printf(buf, "%s: error 0x%02X", what, st);
    _fail(buf);
}

/* ---- the pins and the power-up (probe_bt.c's first arm) ------------------ */

static void _uart_cfg(u32 baud, u32 mcr)
{
    volatile uart_t *u = BT_UART_REGS;
    u32 div = clock_uart_use_src_div(BT_UART, baud)
                ? ((8 * baud + 408000000) / (16 * baud)) : 1;

    u->UART_IER_DLAB = 0;
    u->UART_LCR      = UART_LCR_DLAB | UART_LCR_WORD_LENGTH_8;
    u->UART_THR_DLAB = (u8)div;
    u->UART_IER_DLAB = (u8)(div >> 8);
    u->UART_LCR      = UART_LCR_WORD_LENGTH_8;   /* 8N1, as H4 wants */
    (void)u->UART_SPR;

    u->UART_IIR_FCR = UART_IIR_FCR_EN_FIFO;
    (void)u->UART_SPR;
    usleep(20);
    u->UART_MCR = 0;
    usleep(96);
    u->UART_IIR_FCR = FCR_TRIG16 | UART_IIR_FCR_EN_FIFO |
                      UART_IIR_FCR_TX_CLR | UART_IIR_FCR_RX_CLR;
    u->UART_MCR = mcr;
    (void)u->UART_SPR;
    usleep(200);
}

/* Clear the FIFOs, with the receive trigger set in the same write: a
 * change of trigger resets the FIFO anyway. */
static void _uart_flush(u32 trig)
{
    volatile uart_t *u = BT_UART_REGS;
    u->UART_IIR_FCR = trig | UART_IIR_FCR_EN_FIFO |
                      UART_IIR_FCR_RX_CLR | UART_IIR_FCR_TX_CLR;
    (void)u->UART_SPR;
    usleep(100);
    for (u32 n = 0; (u->UART_LSR & UART_LSR_RDR) && n < 512; n++)
        (void)u->UART_THR_DLAB;
    (void)u->UART_LSR;
}

/* The straps parked for a UART transport and both REG_ON pins low: see
 * bt_pads_park() in probe_bt.c for why each pin is set the way it is. */
static void _pads_park(void)
{
    PINMUX_AUX(PMX_PH5_HOST_WAKE) =
        PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | PINMUX_PULL_NONE;
    GP_MWR(GPH_MCNF, GPIO_PIN_5, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_5, 0);

    PINMUX_AUX(PMX_PH3_DEV_WAKE) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_3, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_3, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_3, 1);

    PINMUX_AUX(PMX_PH7_BT_GPIO5) = PINMUX_INPUT_ENABLE | PINMUX_PULL_NONE;
    GP_MWR(GPH_MOUT, GPIO_PIN_7, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_7, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_7, 1);

    PINMUX_AUX(PMX_PK0_BT_GPIO2) = 2 | PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    PINMUX_AUX(PMX_PK1_BT_GPIO3) = 2 | PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    PINMUX_AUX(PMX_PK2_BT_GPIO4) = 2 | PINMUX_INPUT_ENABLE | PINMUX_PULL_NONE;
    GP_MWR(GPK_MCNF, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, 1);
    GP_MWR(GPK_MOE,  GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, 0);

    PINMUX_AUX(PMX_PH4_BT_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_4, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_4, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_4, 1);

    PINMUX_AUX(PMX_PH1_WL_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_1, 0);
    GP_MWR(GPH_MCNF, GPIO_PIN_1, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_1, 1);
}

static void _uart_pads(u32 rx_pull)
{
    PINMUX_AUX(PINMUX_AUX_UARTX_TX(BT_UART))  = PINMUX_PULL_NONE;
    PINMUX_AUX(PINMUX_AUX_UARTX_RX(BT_UART))  =
        PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | rx_pull;
    PINMUX_AUX(PINMUX_AUX_UARTX_RTS(BT_UART)) = PINMUX_PULL_NONE;
    PINMUX_AUX(PINMUX_AUX_UARTX_CTS(BT_UART)) =
        PINMUX_INPUT_ENABLE | PINMUX_TRISTATE | PINMUX_PULL_UP;
    GPIO(GPI_CNF) &= ~(GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);
    (void)GPIO(GPI_CNF);
}

/* WL_REG_ON back high, where the vendor GPIO hog leaves it: Wi-Fi is not
 * left dark while the screen is open. BT_REG_ON keeps the shared
 * regulators up on its own. */
static void _wl_reg_on_restore(void)
{
    PINMUX_AUX(PMX_PH1_WL_REG_ON) = PINMUX_INPUT_ENABLE | PINMUX_PULL_DOWN;
    GP_MWR(GPH_MOUT, GPIO_PIN_1, 1);
    GP_MWR(GPH_MCNF, GPIO_PIN_1, 1);
    GP_MWR(GPH_MOE,  GPIO_PIN_1, 1);
}

static void _power_up(void)
{
    for (u32 i = 0; i < ARRAY_SIZE(s_rails); i++) {
        s_rail_saved[i] = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, s_rails[i].cfg);
        max7762x_regulator_enable(s_rails[i].id, true);
        msleep(5);
    }
    /* The radio's 32 kHz sleep clock, as the probe sets it. */
    s_ame_saved = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_AME_GPIO);
    if (!(s_ame_saved & BIT(4)))
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_AME_GPIO,
                      s_ame_saved | BIT(4));
    s_32k_saved = i2c_recv_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CNFG1_32K);
    if (!(s_32k_saved & MAX77620_CNFG1_32K_OUT0_EN))
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CNFG1_32K,
                      s_32k_saved | MAX77620_CNFG1_32K_OUT0_EN);
    msleep(10);
    s_powered = true;

    clock_enable_uart(BT_UART);

    /* The cold power-on reset. */
    _pads_park();
    _uart_pads(PINMUX_PULL_DOWN);
    _uart_cfg(115200, 0);
    msleep(20);
    GP_MWR(GPH_MOUT, GPIO_PIN_4, 1);
    (void)GPIO(GPH_OUT);
    msleep(BT_POR_MS);
    _wl_reg_on_restore();

    _uart_pads(PINMUX_PULL_UP);
    usleep(2000);
    _uart_flush(FCR_TRIG16);
    uart_set_mode(BT_UART, UART_MCR_RTS);
    usleep(200);
    volatile uart_t *u = BT_UART_REGS;
    for (u32 i = 0; i < 500 && !(u->UART_MSR & MSR_CTS); i++)
        usleep(1000);
}

static void _power_down(void)
{
    if (!s_powered)
        return;
    /* BT_REG_ON first, then the UART: dropping RTS while the radio is
     * still powered could leave it holding a packet mid-send. */
    GP_MWR(GPH_MOUT, GPIO_PIN_4, 0);
    (void)GPIO(GPH_OUT);
    BT_UART_REGS->UART_MCR = 0;
    for (u32 i = 0; i < ARRAY_SIZE(s_rails); i++)
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, s_rails[i].cfg, s_rail_saved[i]);
    i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_AME_GPIO, s_ame_saved);
    i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_CNFG1_32K, s_32k_saved);
    s_powered = false;
}

/* ---- H4 out --------------------------------------------------------------- */

static bool _tx(const u8 *b, u32 n)
{
    volatile uart_t *u = BT_UART_REGS;
    for (u32 i = 0; i < n; i++) {
        u32 spin = 20000;   /* 20 ms a byte: the radio held its RTS up */
        while (!(u->UART_LSR & UART_LSR_THRE)) {
            if (!--spin) {
                s_line_err++;
                return false;
            }
            usleep(1);
        }
        u->UART_THR_DLAB = b[i];
    }
    return true;
}

static void _cmd_now(u16 op, const u8 *p, u32 len)
{
    _trace("cmd", op, p, len);
    u8 h[4] = { 0x01, op & 0xFF, op >> 8, len };
    _tx(h, 4);
    if (len)
        _tx(p, len);
    if (s_ncmd)
        s_ncmd--;
}

static void _cmd(u16 op, const void *p, u32 len)
{
    if (s_ncmd && !s_cmdq_n) {
        _cmd_now(op, p, len);
        return;
    }
    if (s_cmdq_n == CMDQ_N || len > sizeof(s_cmdq[0].p)) {
        _logs("command queue full, one dropped");
        return;
    }
    u32 i = (s_cmdq_head + s_cmdq_n++) % CMDQ_N;
    s_cmdq[i].op = op;
    s_cmdq[i].len = len;
    memcpy(s_cmdq[i].p, p, len);
}

static void _cmd_kick(void)
{
    while (s_ncmd && s_cmdq_n) {
        u32 i = s_cmdq_head;
        s_cmdq_head = (s_cmdq_head + 1) % CMDQ_N;
        s_cmdq_n--;
        _cmd_now(s_cmdq[i].op, s_cmdq[i].p, s_cmdq[i].len);
    }
}

static void _cmd_addr(u16 op, const u8 *addr)
{
    _cmd(op, addr, 6);
}

static void _cmd_handle(u16 op, u8 extra, bool with_extra)
{
    u8 p[3] = { s_handle & 0xFF, s_handle >> 8, extra };
    _cmd(op, p, with_extra ? 3 : 2);
}

static void _l2cap_now(u16 cid, const u8 *d, u32 n)
{
    u16 hf = (s_handle & 0x0FFF) | 0x2000;   /* first, flushable */
    u8 h[9] = { 0x02, hf & 0xFF, hf >> 8, (n + 4) & 0xFF, (n + 4) >> 8,
                n & 0xFF, n >> 8, cid & 0xFF, cid >> 8 };
    _tx(h, sizeof(h));
    _tx(d, n);
    s_acl_sent++;
    if (s_acl_credits)
        s_acl_credits--;
}

static void _l2cap_send(u16 cid, const u8 *d, u32 n)
{
    if (s_handle == 0xFFFF)
        return;
    if (s_acl_credits && !s_aclq_n) {
        _l2cap_now(cid, d, n);
        return;
    }
    if (s_aclq_n == ACLQ_N || n > sizeof(s_aclq[0].d))
        return;   /* a report: the next one replaces it */
    u32 i = (s_aclq_head + s_aclq_n++) % ACLQ_N;
    s_aclq[i].cid = cid;
    s_aclq[i].len = n;
    memcpy(s_aclq[i].d, d, n);
}

static void _acl_kick(void)
{
    while (s_acl_credits && s_aclq_n) {
        u32 i = s_aclq_head;
        s_aclq_head = (s_aclq_head + 1) % ACLQ_N;
        s_aclq_n--;
        _l2cap_now(s_aclq[i].cid, s_aclq[i].d, s_aclq[i].len);
    }
}

/* ---- H4 in ------------------------------------------------------------------ */

/* Length-driven, as probe_bt.c's reader: Broadcom parts send packet types
 * H4 does not define, and the LM diagnostic one (0x07) is 63 bytes with no
 * length field, which can hold anything, so nothing is ever found by
 * scanning for a type byte. */
static void _rx_byte(u8 b)
{
    if (s_rx.skip) {
        s_rx.skip--;
        return;
    }
    if (!s_rx.type) {
        switch (b) {
        case 0x04: s_rx.hdr = 2; break;   /* event: code, length        */
        case 0x02: s_rx.hdr = 4; break;   /* ACL: handle, length (16)   */
        case 0x03: s_rx.hdr = 3; break;   /* SCO: handle, length (8)    */
        case 0x07: s_rx.skip = 63; return;
        case 0x00: case 0x31: case 0x34: return;
        default:   s_line_err++; return;  /* lost sync: wait for a type */
        }
        s_rx.type = b;
        s_rx.n = 0;
        s_rx.need = 0;
        return;
    }
    if (s_rx.n < sizeof(s_rx.buf))
        s_rx.buf[s_rx.n] = b;
    s_rx.n++;
    if (!s_rx.need && s_rx.n == s_rx.hdr) {
        u32 plen = s_rx.type == 0x04 ? s_rx.buf[1]
                 : s_rx.type == 0x02 ? (u32)(s_rx.buf[2] | s_rx.buf[3] << 8)
                 : s_rx.buf[2];
        s_rx.need = s_rx.hdr + plen;
    }
    if (s_rx.need && s_rx.n >= s_rx.need) {
        u8 type = s_rx.type;
        s_rx.type = 0;
        if (s_rx.n > sizeof(s_rx.buf)) {
            s_line_err++;
            return;
        }
        if (type == 0x04)
            _event(s_rx.buf[0], s_rx.buf + 2, s_rx.buf[1]);
        else if (type == 0x02)
            _acl(s_rx.buf, s_rx.buf + 4, s_rx.n - 4);
    }
}

/* Read while the radio is sending, until the line has been quiet for a
 * few byte times (it has nothing more), or for at most max_us. Stopping as
 * soon as the FIFO was empty took one FIFO's worth a call, 1.6 KB/s at a
 * call every 10 ms, while a Joy-Con's full reports are about 4 KB/s: the
 * radio held the rest, and on the console what the screen showed fell
 * further and further behind. */
/* How long the line may be quiet before the radio is taken to have nothing
 * more. Not a few byte times: with flow control the radio stops when the
 * FIFO fills and takes its time to start again once we have read it, and at
 * 300 us the loop gave up after about a FIFO a call, 13 Joy-Con reports a
 * second on the console. */
#define RX_IDLE_US 2000

static u32 s_rx_bytes, s_rx_polls;

static void _rx_pump(u32 max_us)
{
    volatile uart_t *u = BT_UART_REGS;
    u32 t0 = get_tmr_us(), last = t0;
    s_rx_polls++;
    for (;;) {
        u32 lsr = u->UART_LSR;
        if (lsr & (LSR_OVRF | LSR_FERR | LSR_PERR | LSR_BRK))
            s_line_err++;
        u32 now = get_tmr_us();
        if (lsr & UART_LSR_RDR) {
            _rx_byte(u->UART_THR_DLAB & 0xFF);
            s_rx_bytes++;
            last = now;
        } else if (now - last >= RX_IDLE_US) {
            break;
        }
        if (now - t0 >= max_us)
            break;
    }
}

void gui_bt_note(const char *s)
{
    _logs(s);
}

void gui_bt_rx_stats(u32 *bytes, u32 *polls)
{
    *bytes = s_rx_bytes;
    *polls = s_rx_polls;
}

/* A set-up command and its Command Complete, waited for. */
static bool _cmd_sync(u16 op, const void *p, u32 len, u32 ms)
{
    u32 seq = s_cc.seq;
    s_ncmd = 1;
    _cmd_now(op, p, len);
    u32 end = get_tmr_ms() + ms;
    while ((s32)(end - get_tmr_ms()) > 0) {
        _rx_pump(2000);
        if (s_cc.seq != seq && s_cc.op == op)
            return s_cc.status == 0;
    }
    return false;
}

/* ---- devices ---------------------------------------------------------------- */

static int _dev_find(const u8 *addr)
{
    for (u32 i = 0; i < s_dev_n; i++)
        if (!memcmp(s_dev[i].addr, addr, 6))
            return i;
    return -1;
}

static gui_bt_pad_t _type_from_name(const char *n)
{
    if (strstr(n, "(L)"))
        return BT_PAD_JOYCON_L;
    if (strstr(n, "(R)"))
        return BT_PAD_JOYCON_R;
    if (strstr(n, "Pro Controller"))
        return BT_PAD_PRO;
    return BT_PAD_UNKNOWN;
}

static void _name_set(gui_bt_dev_t *d, const u8 *s, u32 max)
{
    u32 n = 0;
    for (; n < max && n < sizeof(d->name) - 1 && s[n]; n++)
        d->name[n] = (s[n] >= 0x20 && s[n] < 0x7F) ? s[n] : '?';
    d->name[n] = 0;
}

/* Class of device major Peripheral (0x05) that is not a keyboard or a
 * mouse (bits 6-7): Joy-Cons and the Pro Controller are 0x002508 (gamepad),
 * and a licensed pad may say joystick or nothing more. */
static bool _cod_is_pad(u32 cod)
{
    return ((cod >> 8) & 0x1F) == 0x05 && !(cod & 0xC0);
}

static void _dev_add(const u8 *addr, u8 psrm, u32 cod, u16 clk, s8 rssi,
                     const u8 *eir, u32 eir_n)
{
    if (!_cod_is_pad(cod)) {
        /* Said once per search, so a pad this misses can be told apart. */
        if (((cod >> 8) & 0x1F) == 0x05 && _dev_find(addr) < 0) {
            char a[20], buf[64];
            _addr_str(a, addr);
            s_printf(buf, "skipped %s, class %06X (keyboard or mouse)", a, cod);
            _logs(buf);
        }
        return;
    }
    int i = _dev_find(addr);
    if (i < 0) {
        if (s_dev_n == BT_DEV_MAX)
            return;
        i = s_dev_n++;
        memset(&s_dev[i], 0, sizeof(s_dev[i]));
        memcpy(s_dev[i].addr, addr, 6);
        char a[20], buf[64];
        _addr_str(a, addr);
        s_printf(buf, "found %s, class %06X", a, cod);
        _logs(buf);
    }
    gui_bt_dev_t *d = &s_dev[i];
    d->psrm = psrm;
    d->cod = cod;
    d->clock_off = clk;
    d->rssi = rssi;
    /* An extended inquiry result may carry the name already. */
    for (u32 o = 0; eir && o + 1 < eir_n && eir[o]; o += eir[o] + 1) {
        u8 l = eir[o], t = eir[o + 1];
        if ((t == 0x09 || t == 0x08) && l > 1 && o + 1 + l <= eir_n) {
            _name_set(d, eir + o + 2, l - 1);
            d->named = true;
        }
    }
}

/* Inquiry results come in parallel arrays: every address, then every
 * repetition mode, and so on (Core 5.x Vol 4 Part E 7.7.2 and 7.7.33). */
static void _inquiry_result(const u8 *p, u32 n, bool with_rssi)
{
    if (n < 1)
        return;
    /* 14 bytes a response either way: address 6, repetition mode 1,
     * reserved 2 (or 1, then RSSI 1 at the end), class 3, clock offset 2. */
    u32 k = p[0];
    if (n < 1 + k * 14)
        return;
    const u8 *addr = p + 1;
    const u8 *psrm = addr + 6 * k;
    const u8 *res  = psrm + k;
    const u8 *cod  = res + (with_rssi ? 1 : 2) * k;
    const u8 *clk  = cod + 3 * k;
    const u8 *rssi = clk + 2 * k;
    for (u32 i = 0; i < k; i++)
        _dev_add(addr + 6 * i, psrm[i],
                 cod[3 * i] | cod[3 * i + 1] << 8 | cod[3 * i + 2] << 16,
                 clk[2 * i] | clk[2 * i + 1] << 8,
                 with_rssi ? (s8)rssi[i] : 0, NULL, 0);
}

static void _name_next(void);
static void _connect_now(u32 idx);

static void _go_idle(void)
{
    s_state = BT_IDLE;
    s_step[0] = 0;
    if (s_pending >= 0) {
        u32 idx = s_pending;
        s_pending = -1;
        _connect_now(idx);
    }
}

static void _name_next(void)
{
    if (s_pending >= 0) {
        _go_idle();
        return;
    }
    for (u32 i = 0; i < s_dev_n; i++) {
        gui_bt_dev_t *d = &s_dev[i];
        if (d->named)
            continue;
        d->named = true;
        u8 p[10];
        memcpy(p, d->addr, 6);
        p[6] = d->psrm;
        p[7] = 0;
        p[8] = d->clock_off & 0xFF;
        p[9] = (d->clock_off >> 8) | 0x80;   /* the offset is valid */
        _cmd(OP_NAME_REQ, p, sizeof(p));
        s_state = BT_NAMING;
        s_deadline = get_tmr_ms() + NAME_GUARD_MS;
        _step("Asking for its name");
        return;
    }
    _go_idle();
}

/* ---- L2CAP --------------------------------------------------------------------- */

static void _sig(u8 code, u8 id, const u8 *d, u16 n)
{
    u8 b[64];
    if (n > sizeof(b) - 4)
        return;
    b[0] = code;
    b[1] = id;
    b[2] = n & 0xFF;
    b[3] = n >> 8;
    memcpy(b + 4, d, n);
    _trace("l2 out", code, b, 4 + n);
    _l2cap_send(CID_SIGNAL, b, 4 + n);
}

static void _ch_open(int i)
{
    chan_t *c = &s_ch[i];
    c->conn = c->ours_ok = c->theirs_ok = false;
    u8 d[4] = { c->psm & 0xFF, c->psm >> 8, c->scid & 0xFF, c->scid >> 8 };
    _sig(0x02, s_sig_id++, d, sizeof(d));
    s_state = BT_CHANNELS;
    s_deadline = get_tmr_ms() + CHAN_GUARD_MS;
    _step(i ? "Opening the input channel" : "Opening the control channel");
}

static void _ch_config(int i)
{
    /* No options: the default MTU, 672, holds any report. (With the MTU
     * said out loud, as BlueZ says it, a Joy-Con on the console answered
     * nothing more, where it had configured the channel without it.) */
    u8 d[4] = { s_ch[i].dcid & 0xFF, s_ch[i].dcid >> 8, 0, 0 };
    _sig(0x04, s_sig_id++, d, sizeof(d));
}

/* An L2CAP echo: the controller's L2CAP answers it whatever its HID layer
 * makes of us, so it tells a link that does not carry a frame of this size
 * from a controller that ignores what reaches it. */
static void _echo(u32 n)
{
    u8 d[52];
    if (n > sizeof(d))
        n = sizeof(d);
    for (u32 i = 0; i < n; i++)
        d[i] = (u8)(0x30 + i);
    char buf[48];
    s_printf(buf, "echo sent, %d bytes", n);
    _logs(buf);
    _sig(0x08, s_sig_id++, d, n);
}

/* Output report 0x01 at the full length its report descriptor gives it,
 * 48 bytes after the id: a HID device may drop a report of any other
 * length (on the console, short ones were never answered). */
#define OUT_REPORT_LEN 49

/* On the input channel as HID DATA, as HOS sends it, or on the control
 * channel as SET_REPORT, which the controller has to answer at once with a
 * HANDSHAKE: proof, either way, that what is sent reaches it. */
static void _subcmd(u8 cmd, const u8 *arg, u32 n, bool control)
{
    u8 b[1 + OUT_REPORT_LEN];
    if (n > 8)
        n = 8;
    memset(b, 0, sizeof(b));
    b[0] = control ? 0x52 : 0xA2;   /* SET_REPORT / DATA, type output */
    b[1] = 0x01;                 /* output report: rumble + subcommand */
    b[2] = s_hid_pkt++ & 0x0F;
    memcpy(b + 3, s_rumble_off, 8);
    b[11] = cmd;
    if (n)
        memcpy(b + 12, arg, n);
    _trace(control ? "hid set" : "hid out", cmd, b, 12 + n);
    _l2cap_send(s_ch[control ? 0 : 1].dcid, b, sizeof(b));
}

static void _setup_send(void)
{
    static const u8 full = 0x30, led1 = 0x01;
    bool control = s_setup_tries >= 2;   /* the last two tries */
    switch (s_setup_step) {
    case 0: _subcmd(SUB_DATA_FORMAT, &full, 1, control); break;
    case 1: _subcmd(SUB_DEVICE_INFO, NULL, 0, control); break;
    case 2: _subcmd(SUB_LEDS, &led1, 1, control); break;
    }
    s_setup_at = get_tmr_ms();
}

static void _setup_next(void)
{
    static const char *names[] = {
        "Asking for full reports", "Reading what it is", "Lighting its first LED",
    };
    s_setup_tries = 0;
    if (++s_setup_step >= 3) {
        s_state = BT_READY;
        _step("Connected");
        _echo(4);
        return;
    }
    _step(names[s_setup_step]);
    _setup_send();
}

static void _ch_check(int i)
{
    chan_t *c = &s_ch[i];
    if (!c->conn || !c->ours_ok || !c->theirs_ok)
        return;
    if (i == 0) {
        _ch_open(1);
        return;
    }
    s_state = BT_SETUP;
    s_setup_step = 0;
    s_setup_tries = 0;
    _echo(4);
    _echo(50);
    _step("Asking for full reports");
    _setup_send();
}

static int _ch_by_scid(u16 cid)
{
    for (int i = 0; i < 2; i++)
        if (s_ch[i].scid == cid)
            return i;
    return -1;
}

static void _secure(void);

static void _signal(const u8 *d, u32 n)
{
    /* One frame may carry several commands. */
    while (n >= 4) {
        u8 code = d[0], id = d[1];
        u32 len = d[2] | d[3] << 8;
        const u8 *p = d + 4;
        if (len > n - 4)
            return;
        _trace("l2 in", code, d, 4 + len);
        switch (code) {
        case 0x02: { /* Connection Request: the controller opens one */
            if (len < 4)
                break;
            u16 psm = p[0] | p[1] << 8, rcid = p[2] | p[3] << 8;
            int i = psm == PSM_HID_CTRL ? 0 : psm == PSM_HID_INTR ? 1 : -1;
            u8 r[8] = { 0, 0, rcid & 0xFF, rcid >> 8, 0, 0, 0, 0 };
            if (i < 0) {
                r[4] = 0x02;   /* PSM not supported */
                _sig(0x03, id, r, sizeof(r));
                break;
            }
            s_ch[i].dcid = rcid;
            s_ch[i].conn = true;
            s_ch[i].ours_ok = s_ch[i].theirs_ok = false;
            r[0] = s_ch[i].scid & 0xFF;
            r[1] = s_ch[i].scid >> 8;
            _sig(0x03, id, r, sizeof(r));
            _ch_config(i);
            break;
        }
        case 0x03: { /* Connection Response */
            if (len < 8)
                break;
            u16 dcid = p[0] | p[1] << 8, scid = p[2] | p[3] << 8;
            u16 res = p[4] | p[5] << 8;
            int i = _ch_by_scid(scid);
            if (i < 0)
                break;
            if (res == 0) {
                s_ch[i].dcid = dcid;
                s_ch[i].conn = true;
                _ch_config(i);
            } else if (res == 1) {
                /* pending: the answer comes later */
            } else if (res == 3 && !s_secure) {
                s_blocked = i;
                _secure();
            } else {
                char buf[64];
                s_printf(buf, "The %s channel was refused (%d)",
                         i ? "input" : "control", res);
                _fail(buf);
                gui_bt_disconnect();
            }
            break;
        }
        case 0x04: { /* Configuration Request: accept theirs */
            if (len < 4)
                break;
            u16 dcid = p[0] | p[1] << 8;
            int i = _ch_by_scid(dcid);
            if (i < 0)
                break;
            /* Accepted, with their MTU echoed when they gave one, as BlueZ
             * answers. */
            u8 r[10] = { s_ch[i].dcid & 0xFF, s_ch[i].dcid >> 8, 0, 0, 0, 0 };
            u32 rn = 6;
            for (u32 o = 4; o + 1 < len; o += 2 + p[o + 1]) {
                if ((p[o] & 0x7F) == 0x01 && p[o + 1] == 2 && o + 3 < len) {
                    r[6] = 0x01;
                    r[7] = 0x02;
                    r[8] = p[o + 2];
                    r[9] = p[o + 3];
                    rn = 10;
                }
            }
            _sig(0x05, id, r, rn);
            s_ch[i].theirs_ok = true;
            _ch_check(i);
            break;
        }
        case 0x05: { /* Configuration Response */
            if (len < 6)
                break;
            u16 scid = p[0] | p[1] << 8, res = p[4] | p[5] << 8;
            int i = _ch_by_scid(scid);
            if (i < 0)
                break;
            if (res == 0) {
                s_ch[i].ours_ok = true;
                _ch_check(i);
            } else {
                _fail("A channel's settings were refused");
                gui_bt_disconnect();
            }
            break;
        }
        case 0x06: { /* Disconnection Request */
            if (len < 4)
                break;
            _sig(0x07, id, p, 4);
            int i = _ch_by_scid(p[0] | p[1] << 8);
            if (i >= 0) {
                s_ch[i].conn = false;
                _fail("It closed its HID channel");
                gui_bt_disconnect();
            }
            break;
        }
        case 0x08: /* Echo Request */
            _sig(0x09, id, p, len);
            break;
        case 0x09: { /* Echo Response: the probes of _echo */
            char buf[48];
            s_printf(buf, "echo answered, %d bytes", len);
            _logs(buf);
            break;
        }
        case 0x0A: { /* Information Request: nothing extended */
            if (len < 2)
                break;
            u16 type = p[0] | p[1] << 8;
            u8 r[12] = { p[0], p[1], 0, 0 };
            if (type == 2) {          /* extended features: none */
                _sig(0x0B, id, r, 8);
            } else if (type == 3) {   /* fixed channels: signalling */
                r[4] = 0x02;
                _sig(0x0B, id, r, 12);
            } else {
                r[2] = 1;             /* not supported */
                _sig(0x0B, id, r, 4);
            }
            break;
        }
        case 0x01: { /* Command Reject */
            char buf[48];
            s_printf(buf, "l2cap command rejected (%d)", len >= 2 ? p[0] : -1);
            _logs(buf);
            break;
        }
        default:
            break;
        }
        d += 4 + len;
        n -= 4 + len;
    }
}

/* ---- the controller's reports ------------------------------------------------- */

static void _report(const u8 *r, u32 n)
{
    if (n < 12)
        return;
    u8 id = r[0];
    if (id == 0x3F)
        s_pad.simple++;       /* the plain HID report, before 0x30 is asked */
    if (id != 0x30 && id != 0x21)
        return;
    s_pad.buttons = r[3] | r[4] << 8 | r[5] << 16;
    s_pad.stick[0] = r[6] | (r[7] & 0x0F) << 8;
    s_pad.stick[1] = (r[7] >> 4) | r[8] << 4;
    s_pad.stick[2] = r[9] | (r[10] & 0x0F) << 8;
    s_pad.stick[3] = (r[10] >> 4) | r[11] << 4;
    s_pad.batt = (r[2] >> 4) & 0x0E;
    s_pad.charging = (r[2] >> 4) & 0x01;
    if (id == 0x30)
        s_pad.reports++;

    /* What it is, whenever it says: a first answer can come after the
     * set-up has moved on, and the view follows the type. */
    if (id == 0x21 && n >= 18 && r[14] == SUB_DEVICE_INFO &&
        r[17] >= 1 && r[17] <= 3)
        s_pad.type = (gui_bt_pad_t)r[17];
    if (id == 0x21 && n >= 15 && s_state == BT_SETUP) {
        u8 sub = r[14];
        static const u8 want[] = { SUB_DATA_FORMAT, SUB_DEVICE_INFO, SUB_LEDS };
        if (s_setup_step < 3 && sub == want[s_setup_step])
            _setup_next();
    }
}

static void _l2cap(u16 cid, const u8 *d, u32 n)
{
    if (cid == CID_SIGNAL) {
        _signal(d, n);
        return;
    }
    /* The first frames of each connection, whatever channel, in the trace:
     * which channel the controller talks on, and what it says first. */
    if (s_data_traced < 40) {
        s_data_traced++;
        _trace("data in", cid, d, n);
    }
    /* HID DATA on the input channel: 0xA1, then the report. */
    if (cid == s_ch[1].scid && n >= 2 && d[0] == 0xA1)
        _report(d + 1, n - 1);
}

static void _acl(const u8 *h, const u8 *d, u32 n)
{
    u16 hf = h[0] | h[1] << 8;
    if ((hf & 0x0FFF) != s_handle)
        return;
    u8 pb = (hf >> 12) & 3;
    if (pb != 1) {                /* a start: the L2CAP header is here */
        if (n < 4)
            return;
        s_l2_need = 4 + (d[0] | d[1] << 8);
        s_l2_n = 0;
        if (s_l2_need > sizeof(s_l2)) {
            s_l2_need = 0;
            return;
        }
    }
    if (!s_l2_need)
        return;
    u32 take = n;
    if (s_l2_n + take > s_l2_need)
        take = s_l2_need - s_l2_n;
    memcpy(s_l2 + s_l2_n, d, take);
    s_l2_n += take;
    if (s_l2_n == s_l2_need) {
        s_l2_need = 0;
        _l2cap(s_l2[2] | s_l2[3] << 8, s_l2 + 4, s_l2_n - 4);
    }
}

/* ---- HCI events ------------------------------------------------------------ */

static void _secure(void)
{
    s_state = BT_SECURING;
    s_deadline = get_tmr_ms() + SEC_GUARD_MS;
    _step("Pairing");
    _cmd_handle(OP_AUTH_REQ, 0, false);
}

static void _link_down(void)
{
    s_handle = 0xFFFF;
    s_ch[0].conn = s_ch[1].conn = false;
    s_cmdq_n = 0;
    s_aclq_n = 0;
    s_l2_need = 0;
}

static void _event(u8 code, const u8 *p, u32 n)
{
    char buf[96];
    /* All but the steady ones: packets done, and the search's results
     * (each device found is logged once already). */
    if ((code != 0x13 || s_handle != 0xFFFF) &&
        code != 0x02 && code != 0x22 && code != 0x2F)
        _trace("ev", code, p, n);
    switch (code) {
    case 0x0E: /* Command Complete: ncmd, opcode, status, return */
        if (n < 3)
            break;
        s_ncmd = p[0];
        s_cc.op = p[1] | p[2] << 8;
        s_cc.status = n > 3 ? p[3] : 0;
        memset(s_cc.ret, 0, sizeof(s_cc.ret));
        if (n > 4)
            memcpy(s_cc.ret, p + 4, MIN(n - 4, sizeof(s_cc.ret)));
        s_cc.seq++;
        if (s_cc.op == OP_INQUIRY_CANCEL && s_state == BT_SCANNING)
            _name_next();
        _cmd_kick();
        break;

    case 0x0F: { /* Command Status: status, ncmd, opcode */
        if (n < 4)
            break;
        u8 st = p[0];
        u16 op = p[2] | p[3] << 8;
        s_ncmd = p[1];
        if (st) {
            if (op == OP_INQUIRY) {
                _fail_status("The search did not start", st);
                s_state = BT_IDLE;
            } else if (op == OP_CREATE_CONN) {
                _fail_status("The call did not start", st);
                s_state = BT_IDLE;
                s_cur = -1;
            } else if (op == OP_NAME_REQ) {
                _name_next();
            } else if (op == OP_AUTH_REQ) {
                _fail_status("Pairing did not start", st);
                gui_bt_disconnect();
            } else {
                s_printf(buf, "command %04X refused (0x%02X)", op, st);
                _logs(buf);
            }
        }
        _cmd_kick();
        break;
    }

    case 0x13: { /* Number Of Completed Packets: k, k handles, k counts */
        u32 k = n ? p[0] : 0;
        for (u32 i = 0; i < k && 1 + 2 * k + 2 * i + 1 < n; i++) {
            u32 done = p[1 + 2 * k + 2 * i] | p[1 + 2 * k + 2 * i + 1] << 8;
            s_acl_credits += done;
            s_acl_done += done;
        }
        _acl_kick();
        break;
    }

    case 0x02: /* Inquiry Result */
        _inquiry_result(p, n, false);
        break;
    case 0x22: /* Inquiry Result with RSSI */
        _inquiry_result(p, n, true);
        break;
    case 0x2F: /* Extended Inquiry Result: one, then 240 bytes of EIR */
        if (n >= 15)
            _dev_add(p + 1, p[7], p[9] | p[10] << 8 | p[11] << 16,
                     p[12] | p[13] << 8, (s8)p[14], p + 15, n - 15);
        break;
    case 0x01: /* Inquiry Complete */
        if (s_state == BT_SCANNING)
            _name_next();
        break;

    case 0x07: { /* Remote Name Request Complete: status, addr, name */
        if (n >= 7) {
            int i = _dev_find(p + 1);
            if (i >= 0 && !p[0])
                _name_set(&s_dev[i], p + 7, n - 7);
            if (i >= 0) {
                s_printf(buf, "name of device %d: %s", i,
                         p[0] ? "(no answer)" : s_dev[i].name);
                _logs(buf);
            }
        }
        if (s_state == BT_NAMING)
            _name_next();
        break;
    }

    case 0x03: { /* Connection Complete: status, handle, addr, link, enc */
        if (n < 11 || s_state != BT_CONNECTING)
            break;
        if (p[0]) {
            _fail_status("No connection", p[0]);
            s_state = BT_IDLE;
            s_cur = -1;
            break;
        }
        s_handle = (p[1] | p[2] << 8) & 0x0FFF;
        s_l2_need = 0;
        s_aclq_n = 0;
        s_printf(buf, "connected, handle %03X", s_handle);
        _logs(buf);
        /* What it supports (in the trace), then pairing before anything
         * is opened, as BlueZ pairs a controller first: on the console a
         * Switch controller hung up on an HID channel asked for before. */
        _cmd_handle(OP_READ_REMOTE_FEAT, 0, false);
        _cmd_handle(OP_READ_REMOTE_EXT, 1, true);
        /* Whether a packet the radio counts as done may have been dropped
         * (a flush timeout other than 0, which is never). */
        _cmd_handle(OP_READ_FLUSH_TO, 0, false);
        /* And said again on the link itself, in case the connection's
         * types did not carry over to the controller's own use. */
        {
            u8 t[4] = { s_handle & 0xFF, s_handle >> 8, PKT_TYPES & 0xFF, PKT_TYPES >> 8 };
            _cmd(OP_PKT_TYPE, t, sizeof(t));
        }
        /* Poll it often. A slave only sends when polled, every 40 slots
         * (25 ms) by default, and with basic-rate packets a full report
         * takes two or three: on the console that held a Joy-Con to 13
         * reports a second of its 60, with the rest queued in it and the
         * screen seconds behind. Guaranteed service with 2.5 ms latency
         * brings the poll down to a few slots. */
        {
            u8 q[20] = { s_handle & 0xFF, s_handle >> 8, 0, 0x02,
                         0x40, 0x1F, 0, 0,          /* token rate 8000 B/s */
                         0, 0, 0, 0,                /* peak: unspecified   */
                         0xC4, 0x09, 0, 0,          /* latency 2500 us     */
                         0xFF, 0xFF, 0xFF, 0xFF };  /* delay variation any */
            _cmd(OP_QOS_SETUP, q, sizeof(q));
        }
        _secure();
        break;
    }

    case 0x04: /* Connection Request: not taking calls, page scan is off */
        if (n >= 6) {
            u8 r[7];
            memcpy(r, p, 6);
            r[6] = 0x0F;   /* unacceptable BD_ADDR */
            _cmd(OP_REJECT_CONN, r, 7);
        }
        break;

    case 0x05: { /* Disconnection Complete: status, handle, reason */
        if (n < 4)
            break;
        u16 h = (p[1] | p[2] << 8) & 0x0FFF;
        if (h != s_handle)
            break;
        u8 reason = p[3];
        _link_down();
        if (s_redial && s_cur >= 0) {
            /* A pairing failed on the first try: a fresh link for the
             * second, as a controller may refuse to start again on one
             * that just failed. */
            s_redial = false;
            _connect_now(s_cur);
            break;
        }
        if (s_state == BT_SECURING) {
            s_printf(buf, "disconnected while pairing, reason %02X", reason);
            _logs(buf);
        }
        if (s_state != BT_IDLE && reason != 0x16)
            _fail_status("Disconnected", reason);
        s_state = BT_IDLE;
        s_cur = -1;
        s_step[0] = 0;
        break;
    }

    case 0x17: /* Link Key Request: addr */
        if (n < 6)
            break;
        if (s_key_ok && !memcmp(p, s_key_addr, 6)) {
            u8 r[22];
            memcpy(r, p, 6);
            memcpy(r + 6, s_key, 16);
            _cmd(OP_LINK_KEY_REPLY, r, sizeof(r));
            _logs("link key asked: gave the one from before");
        } else {
            _cmd_addr(OP_LINK_KEY_NEG, p);
            _logs("link key asked: none, so it pairs");
        }
        break;

    case 0x31: { /* IO Capability Request: addr */
        if (n < 6)
            break;
        u8 r[9];
        memcpy(r, p, 6);
        r[6] = 0x03;               /* NoInputNoOutput: "just works" */
        r[7] = 0x00;               /* no OOB data */
        r[8] = 0x04;               /* general bonding, no MITM, as BlueZ */
        _cmd(OP_IO_CAP_REPLY, r, sizeof(r));
        _logs("io capability asked: no input, no output, bonding");
        break;
    }
    case 0x32: /* IO Capability Response: addr, io, oob, auth */
        if (n >= 9) {
            s_printf(buf, "its io capability %d, oob %d, auth %02X", p[6], p[7], p[8]);
            _logs(buf);
        }
        break;
    case 0x33: /* User Confirmation Request: addr, value */
        if (n >= 6) {
            _cmd_addr(OP_USER_CONF_REPLY, p);
            _logs("confirmation asked: yes");
        }
        break;
    case 0x34: /* User Passkey Request: we have no keyboard */
        if (n >= 6) {
            _cmd_addr(OP_PASSKEY_NEG, p);
            _logs("passkey asked: refused, no keyboard");
        }
        break;
    case 0x16: /* PIN Code Request: legacy pairing */
        if (n >= 6) {
            /* Not Simple Pairing: the old way, with a PIN. 0000 is the
             * one a device without a keypad is most often built with. */
            u8 r[23] = { 0 };
            memcpy(r, p, 6);
            r[6] = 4;
            memcpy(r + 7, "0000", 4);
            _cmd(OP_PIN_REPLY, r, sizeof(r));
            _logs("PIN asked (legacy pairing): answered 0000");
        }
        break;
    case 0x36: /* Simple Pairing Complete: status, addr */
        if (n >= 1) {
            s_printf(buf, "simple pairing complete, status %02X", p[0]);
            _logs(buf);
        }
        break;
    case 0x18: /* Link Key Notification: addr, key, type */
        if (n >= 22) {
            memcpy(s_key_addr, p, 6);
            memcpy(s_key, p + 6, 16);
            s_key_ok = true;
            s_paired = true;
            s_printf(buf, "link key received, type %d", n > 22 ? p[22] : -1);
            _logs(buf);
        }
        break;

    case 0x06: /* Authentication Complete: status, handle */
        if (n < 3 || s_state != BT_SECURING)
            break;
        s_printf(buf, "authentication complete, status %02X", p[0]);
        _logs(buf);
        if (!p[0]) {
            _step("Turning encryption on");
            _cmd_handle(OP_SET_ENCRYPT, 0x01, true);
        } else if (!s_retried) {
            s_retried = s_redial = true;
            _logs("calling again for a second try");
            gui_bt_disconnect();
        } else {
            _fail_status("Pairing failed", p[0]);
            gui_bt_disconnect();
        }
        break;

    case 0x08: /* Encryption Change: status, handle, on */
        if (n < 4 || s_state != BT_SECURING)
            break;
        if (!p[0] && p[3]) {
            s_secure = true;
            _ch_open(s_blocked >= 0 ? s_blocked : 0);
            s_blocked = -1;
        } else {
            _fail_status("Encryption failed", p[0]);
            gui_bt_disconnect();
        }
        break;

    case 0x14: /* Mode Change: status, handle, mode, interval */
        if (n >= 6 && !p[0] && ((p[1] | p[2] << 8) & 0x0FFF) == s_handle &&
            p[3] == 2) {
            /* Sniff, as a Joy-Con asks for: it wakes every interval, and
             * with basic-rate packets only a full report takes several
             * wake-ups. On the console that carried 13 reports a second
             * of the 60 it makes, the rest queued in it, and the screen
             * ran 3-4 s behind. Back to active, and no sniff on this link
             * from here. */
            s_printf(buf, "it went to sniff mode (every %d slots): back to active",
                     p[4] | p[5] << 8);
            _logs(buf);
            u8 pol[4] = { s_handle & 0xFF, s_handle >> 8, 0x01, 0x00 };
            _cmd(OP_LINK_POLICY_CONN, pol, sizeof(pol));
            _cmd_handle(OP_EXIT_SNIFF, 0, false);
        }
        break;

    case 0x10: /* Hardware Error */
        s_printf(buf, "The radio reported a hardware error (0x%02X)",
                 n ? p[0] : 0);
        _fail(buf);
        _link_down();
        s_state = BT_IDLE;
        break;

    default:
        break;
    }
}

/* ---- the interface -------------------------------------------------------- */

bool gui_bt_open(void)
{
    if (s_state != BT_OFF && s_state != BT_FAILED)
        return true;
    memset(&s_rx, 0, sizeof(s_rx));
    memset(&s_pad, 0, sizeof(s_pad));
    s_dev_n = 0;
    s_cur = s_pending = -1;
    s_why[0] = s_step[0] = 0;
    s_cmdq_n = s_aclq_n = 0;
    s_ncmd = 1;
    s_acl_credits = 1;
    s_line_err = 0;
    s_ch[0] = (chan_t){ PSM_HID_CTRL, CID_CTRL };
    s_ch[1] = (chan_t){ PSM_HID_INTR, CID_INTR };
    _link_down();

    _logs("powering the radio up");
    _power_up();

    bool ok = false;
    for (u32 t = 0; t < 3 && !ok; t++)
        ok = _cmd_sync(OP_RESET, NULL, 0, BT_RESET_MS);
    if (!ok) {
        _fail("The radio did not answer");
        _power_down();
        s_state = BT_FAILED;
        return false;
    }
    /* "100 msec delay for module to complete reset process" (btbcm.c). */
    msleep(100);
    /* From here, hardware flow control both ways. Our RTS goes up at the
     * receive trigger: at 8 bytes rather than 16, so what the radio still
     * sends after seeing it fits in the rest of the FIFO. One flush sets
     * it; then the flow control. */
    _uart_flush(FCR_TRIG8);
    uart_set_mode(BT_UART, UART_MCR_RTS_EN | UART_MCR_CTS_EN);

    if (_cmd_sync(OP_READ_BDADDR, NULL, 0, 500))
        memcpy(s_local, s_cc.ret, 6);
    if (_cmd_sync(OP_READ_BUFFER, NULL, 0, 500)) {
        s_acl_credits = s_cc.ret[3] | s_cc.ret[4] << 8;
        if (!s_acl_credits)
            s_acl_credits = 1;
    }
    char a[20], buf[96];
    /* What firmware this is, and whether it does Simple Pairing (LMP
     * features byte 6 bit 3): the radio runs its ROM, unpatched. */
    if (_cmd_sync(OP_READ_VERSION, NULL, 0, 500)) {
        s_printf(buf, "firmware: HCI %d rev %04X, LMP %d sub %04X, maker %04X",
                 s_cc.ret[0], s_cc.ret[1] | s_cc.ret[2] << 8, s_cc.ret[3],
                 s_cc.ret[6] | s_cc.ret[7] << 8, s_cc.ret[4] | s_cc.ret[5] << 8);
        _logs(buf);
    }
    if (_cmd_sync(OP_READ_FEATURES, NULL, 0, 500)) {
        s_printf(buf, "simple pairing in the radio: %s",
                 (s_cc.ret[6] & 0x08) ? "yes" : "NO");
        _logs(buf);
    }
    /* The default mask leaves out the Simple Pairing events (bits 48-55).
     * Only defined bits, as BlueZ sets them: a reserved bit may get the
     * whole mask refused, and then no pairing event ever reaches us. */
    static const u8 mask[8] = { 0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x1D };
    static const u8 one = 1;
    /* Role switch and sniff mode allowed on every link, as BlueZ sets them:
     * after a reset the policy allows neither, so the radio refuses, with
     * no event to the host, the sniff a HID controller asks for to save
     * power, and on the console a Joy-Con dropped the link seconds after
     * its channels opened. */
    static const u8 policy[2] = { 0x05, 0x00 };
    static const struct { u16 op; const u8 *p; u8 n; const char *what; } init[] = {
        { OP_SET_EVENT_MASK, mask, 8, "the event mask" },
        { OP_SSP_MODE,       &one, 1, "simple pairing" },
        { OP_INQUIRY_MODE,   &one, 1, "inquiry with RSSI" },
        { OP_LINK_POLICY,    policy, 2, "the default link policy" },
    };
    for (u32 i = 0; i < ARRAY_SIZE(init); i++) {
        if (!_cmd_sync(init[i].op, init[i].p, init[i].n, 500)) {
            s_printf(buf, "the radio refused %s (status %02X)", init[i].what,
                     s_cc.op == init[i].op ? s_cc.status : 0xFF);
            _logs(buf);
        }
    }
    s_ncmd = 1;

    _addr_str(a, s_local);
    s_printf(buf, "radio up, address %s, %d ACL buffers", a, s_acl_credits);
    _logs(buf);
    /* HOS gives the radio the console's address from PRODINFO with a
     * vendor command, which is not sent here: the ROM's own is a filler. */
    if (s_local[0] == s_local[1] && s_local[1] == s_local[2] &&
        s_local[2] == s_local[3] && s_local[3] == s_local[4] &&
        s_local[4] == s_local[5])
        _logs("that address is the radio's filler, not the console's");
    s_state = BT_IDLE;
    s_rate_at = get_tmr_ms();
    return true;
}

void gui_bt_close(void)
{
    if (s_state == BT_OFF)
        return;
    if (s_powered && s_state != BT_FAILED) {
        if (s_state == BT_SCANNING)
            _cmd(OP_INQUIRY_CANCEL, NULL, 0);
        if (s_handle != 0xFFFF) {
            u8 r[3] = { s_handle & 0xFF, s_handle >> 8, 0x13 };
            _cmd(OP_DISCONNECT, r, 3);
            u32 end = get_tmr_ms() + 500;
            while (s_handle != 0xFFFF && (s32)(end - get_tmr_ms()) > 0)
                _rx_pump(2000);
        }
    }
    _power_down();
    _link_down();
    s_state = BT_OFF;
    _logs("radio off");
}

void gui_bt_poll(void)
{
    if (s_state == BT_OFF || s_state == BT_FAILED)
        return;
    _rx_pump(8000);     /* at most 8 ms of every poll */

    u32 now = get_tmr_ms();
    if ((s32)(now - s_rate_at) >= 1000) {
        s_pad.rate = s_pad.reports - s_rate_base;
        s_rate_base = s_pad.reports;
        s_rate_at = now;
    }

    switch (s_state) {
    case BT_SCANNING:
        if ((s32)(now - s_deadline) > 0)
            _name_next();
        break;
    case BT_NAMING:
        if ((s32)(now - s_deadline) > 0)
            _name_next();
        break;
    case BT_CONNECTING:
        if ((s32)(now - s_deadline) > 0) {
            if (s_cur >= 0)
                _cmd_addr(OP_CREATE_CANCEL, s_dev[s_cur].addr);
            _fail("It did not answer: press its sync button and try again");
            s_state = BT_IDLE;
            s_cur = -1;
        }
        break;
    case BT_SECURING:
    case BT_CHANNELS:
        if ((s32)(now - s_deadline) > 0) {
            _fail(s_state == BT_SECURING ? "Pairing took too long"
                                         : "The HID channels did not open");
            gui_bt_disconnect();
        }
        break;
    case BT_SETUP:
        if ((s32)(now - s_setup_at) >= SUB_RETRY_MS) {
            if (++s_setup_tries >= SUB_TRIES) {
                /* No answer to this one: go on, reports may come anyway. */
                _setup_next();
            } else {
                _setup_send();
            }
        }
        break;
    default:
        break;
    }
}

void gui_bt_scan(void)
{
    if (s_state != BT_IDLE)
        return;
    s_dev_n = 0;
    s_why[0] = 0;
    u8 p[5] = { 0x33, 0x8B, 0x9E, SCAN_LEN, 0x00 };   /* GIAC, 10.24 s */
    _cmd(OP_INQUIRY, p, sizeof(p));
    s_state = BT_SCANNING;
    s_deadline = get_tmr_ms() + SCAN_GUARD_MS;
    _step("Searching");
}

void gui_bt_scan_stop(void)
{
    if (s_state == BT_SCANNING)
        _cmd(OP_INQUIRY_CANCEL, NULL, 0);
}

static void _connect_now(u32 idx)
{
    gui_bt_dev_t *d = &s_dev[idx];
    s_cur = idx;
    s_data_traced = 0;
    s_acl_sent = s_acl_done = 0;
    s_secure = s_paired = false;
    s_blocked = -1;
    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.type = _type_from_name(d->name);
    s_rate_base = 0;
    u8 p[13];
    memcpy(p, d->addr, 6);
    /* DM1 DH1 DM3 DH3 DM5 DH5, and the six "shall not be used" bits of
     * the EDR types set: basic rate only. On the console, everything a
     * Joy-Con sent that fits a basic-rate DH1 arrived and nothing longer
     * did, while it carried out what it was sent; the radio runs its ROM
     * without the patch HOS loads. */
    p[6] = PKT_TYPES & 0xFF;
    p[7] = PKT_TYPES >> 8;
    p[8] = d->psrm;
    p[9] = 0;
    p[10] = d->clock_off & 0xFF;
    p[11] = (d->clock_off >> 8) | 0x80;
    p[12] = 0x01;                /* it may take the master role */
    _cmd(OP_CREATE_CONN, p, sizeof(p));
    s_state = BT_CONNECTING;
    s_deadline = get_tmr_ms() + CONN_GUARD_MS;
    _step("Calling it");
}

bool gui_bt_connect(u32 idx)
{
    if (idx >= s_dev_n)
        return false;
    s_why[0] = 0;
    s_retried = s_redial = false;
    if (s_state == BT_SCANNING) {
        s_pending = idx;
        gui_bt_scan_stop();
        return true;
    }
    if (s_state == BT_NAMING) {
        s_pending = idx;     /* once the name request in flight answers */
        return true;
    }
    if (s_state != BT_IDLE)
        return false;
    _connect_now(idx);
    return true;
}

void gui_bt_disconnect(void)
{
    if (s_state == BT_CONNECTING && s_cur >= 0) {
        _cmd_addr(OP_CREATE_CANCEL, s_dev[s_cur].addr);
        s_state = BT_IDLE;
        s_cur = -1;
        return;
    }
    if (s_handle != 0xFFFF) {
        u8 r[3] = { s_handle & 0xFF, s_handle >> 8, 0x13 };
        _cmd(OP_DISCONNECT, r, 3);
    }
}

gui_bt_state_t gui_bt_state(void)        { return s_state; }
const char *gui_bt_why(void)             { return s_why; }
const char *gui_bt_step(void)            { return s_step; }
u32 gui_bt_dev_count(void)               { return s_dev_n; }
const gui_bt_dev_t *gui_bt_dev(u32 idx)  { return idx < s_dev_n ? &s_dev[idx] : NULL; }
int gui_bt_connected(void)               { return s_cur; }
const gui_bt_pad_state_t *gui_bt_pad(void) { return &s_pad; }
const u8 *gui_bt_local_addr(void)        { return s_local; }
u32 gui_bt_line_errors(void)             { return s_line_err; }
bool gui_bt_paired(void)                 { return s_paired; }
void gui_bt_acl_stats(u32 *sent, u32 *done) { *sent = s_acl_sent; *done = s_acl_done; }
bool gui_bt_bonded(void)                 { return s_paired; }
u32 gui_bt_log_seq(void)                 { return s_log_seq; }

const char *gui_bt_log(u32 back)
{
    if (back >= LOG_N || back >= s_log_seq)
        return NULL;
    return s_log[(s_log_seq - 1 - back) % LOG_N];
}
