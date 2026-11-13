/*
 * hwtest GUI - the Joy-Con's wired HID protocol (see gui_jcwire.h).
 */
#include "gui.h"
#include "gui_jcwire.h"
#include <power/regulator_5v.h>
#include <soc/gpio.h>
#include <soc/timer.h>
#include <soc/uart.h>

#define WIRED_SND_MAGIC  "\x19\x01\x03"
#define WIRED_RCV_MAGIC  "\x19\x81\x03"
#define WIRED_HID        0x92

#define SUBCMD_SPI_READ  0x10

/* Where the stick calibration lives in the flash. */
#define SPI_CAL_USER_L   0x8010   /* two magic bytes, then nine of calibration */
#define SPI_CAL_USER_R   0x801B
#define SPI_CAL_FACT_L   0x603D   /* nine bytes */
#define SPI_CAL_FACT_R   0x6046
#define SPI_COLOUR       0x6050   /* the body's RGB, then the buttons' */
#define CAL_USER_MAGIC_0 0xB2
#define CAL_USER_MAGIC_1 0xA1

typedef struct {
    u8  magic[3];
    u16 total_size;        /* everything after this header's first 5 bytes */
    u8  cmd, subcmd;
    u16 payload_size;
    u8  status, crc_payload, crc_hdr;
    u8  payload[];
} __attribute__((packed)) wired_hdr_t;

/* The driver's own request for an input report: a HID packet with one
 * byte of payload whose value does not matter, checksums included. */
static const u8 s_status_req[] = {
    0x19, 0x01, 0x03, 0x08, 0x00, WIRED_HID, 0x00, 0x01, 0x00, 0x00, 0x69, 0x2D, 0x1F
};
static const u8 s_rumble_off[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };
/* Full strength on both bands, as the console plays it when a Joy-Con is
 * attached; hekate sends the same. */
static const u8 s_rumble_on[8]  = { 0xC2, 0xC8, 0x03, 0x72, 0xC2, 0xC8, 0x03, 0x72 };

static u8  s_pkt_id;
/* Bytes received and not yet handed out, per side: a report can arrive in
 * pieces, and more than one at a time. */
static u8  s_rx[2][0x200];
static u32 s_rx_n[2];
static bool s_framed[2];     /* a reply's framing has been logged this time */

static u32 _uart(int side) { return side ? UART_B : UART_C; }

u8 gui_jc_crc8(const u8 *p, u32 n)
{
    u8 crc = 0;
    while (n--) {
        crc ^= *p++;
        for (u32 b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
    }
    return crc;
}

void gui_jc_send(int side, u8 *rpt, u32 len)
{
    u8 pkt[sizeof(wired_hdr_t) + JC_RPT_LEN];
    wired_hdr_t *w = (wired_hdr_t *)pkt;
    if (len > JC_RPT_LEN)
        len = JC_RPT_LEN;
    memset(pkt, 0, sizeof(pkt));
    memcpy(w->magic, WIRED_SND_MAGIC, 3);
    w->total_size   = sizeof(wired_hdr_t) - 5 + len;
    w->cmd          = WIRED_HID;
    w->payload_size = len;
    rpt[1] = s_pkt_id++ & 0xF;
    memcpy(w->payload, rpt, len);
    uart_send(_uart(side), pkt, sizeof(wired_hdr_t) + len);
    uart_wait_xfer(_uart(side), UART_TX_IDLE);
}

void gui_jc_poll(int side)
{
    uart_send(_uart(side), s_status_req, sizeof(s_status_req));
    uart_wait_xfer(_uart(side), UART_TX_IDLE);
}

/* The next packet from the Joy-Con, whole, into s_pkt: its length, or 0
 * when none is complete by end_us. Whatever precedes a reply magic is
 * noise, or the tail of a packet whose start was missed. */
static u8 s_pkt[2][0x200];

/* What a start-up got back, for the debug console: the bytes, the first of
 * them, and the last packet's command, subcommand and status. */
static u32 s_seen[2];
static u8  s_seen_head[2][12];
static u8  s_last[2][3];

static u32 _pkt_recv(int side, u32 end_us)
{
    u8  *buf = s_rx[side];
    u32 *n   = &s_rx_n[side];

    for (;;) {
        u32 at = 0;
        while (at + 3 <= *n && memcmp(buf + at, WIRED_RCV_MAGIC, 3))
            at++;
        if (at) {
            *n -= at;
            memmove(buf, buf + at, *n);
        }
        /* A report's length is its HID payload's; any other packet's, the
         * UART header's, and may be shorter than a full header (bdk's
         * driver takes replies from 8 bytes, the handshake's among them). */
        const wired_hdr_t *w = (const wired_hdr_t *)buf;
        bool hid = *n > 5 && w->cmd == WIRED_HID;
        if ((hid && *n >= sizeof(wired_hdr_t)) || (!hid && *n > 5)) {
            u32 pkt = hid ? sizeof(wired_hdr_t) + w->payload_size : 5u + w->total_size;
            if (pkt < 6 || pkt > sizeof(s_rx[0])) {
                *n = 0;             /* not a length a Joy-Con sends: start over */
            } else if (*n >= pkt) {
                memcpy(s_pkt[side], buf, pkt);
                *n -= pkt;
                memmove(buf, buf + pkt, *n);
                return pkt;
            }
        }
        if ((s32)(end_us - get_tmr_us()) <= 0)
            return 0;
        if (*n >= sizeof(s_rx[0]))
            *n = 0;
        u32 got = uart_recv(_uart(side), buf + *n, sizeof(s_rx[0]) - *n);
        for (u32 i = 0; i < got && s_seen[side] + i < sizeof(s_seen_head[0]); i++)
            s_seen_head[side][s_seen[side] + i] = buf[*n + i];
        s_seen[side] += got;
        *n += got;
    }
}

u32 gui_jc_recv(int side, u8 *out, u32 max, u32 wait_ms)
{
    u32 end = get_tmr_us() + wait_ms * 1000;
    while (_pkt_recv(side, end)) {
        const wired_hdr_t *w = (const wired_hdr_t *)s_pkt[side];
        u32 len = w->cmd == WIRED_HID ? w->payload_size : 0;
        /* The driver never checks a reply's two sizes against each other,
         * so the first one each time goes to the debug UART for the bench
         * to confirm the framing. */
        if (!s_framed[side]) {
            s_framed[side] = true;
            char line[96];
            s_printf(line, "gui: jc %c reply total %d payload %d cmd %02X first %02X%s\n",
                     side ? 'R' : 'L', w->total_size, w->payload_size, w->cmd,
                     len ? w->payload[0] : 0,
                     5u + w->total_size == sizeof(wired_hdr_t) + w->payload_size
                         ? "" : " (sizes disagree)");
            gui_uart_puts(line);
        }
        if (!len)
            continue;               /* a wired command's reply: not a report */
        if (len > max)
            len = max;
        memcpy(out, w->payload, len);
        return len;
    }
    return 0;
}

bool gui_jc_subcmd(int side, u8 subcmd, const u8 *args, u32 len, u8 *reply)
{
    u8 rpt[JC_RPT_LEN], in[JC_RPT_LEN];
    if (len > JC_RPT_LEN - 11)
        len = JC_RPT_LEN - 11;
    for (u32 tries = 0; tries < 4; tries++) {
        memset(rpt, 0, sizeof(rpt));
        rpt[0] = JC_OUT_SUBCMD;
        memcpy(rpt + 2, s_rumble_off, 8);
        rpt[10] = subcmd;
        if (len)
            memcpy(rpt + 11, args, len);
        gui_jc_send(side, rpt, JC_RPT_LEN);
        /* Its reply: byte 13 acknowledges (bit 7), byte 14 names the
         * subcommand. Anything else in between is the driver's. */
        u32 end = get_tmr_us() + 64000;
        while ((s32)(end - get_tmr_us()) > 0) {
            u32 n = gui_jc_recv(side, in, sizeof(in), 10);
            if (n >= 15 && in[0] == JC_IN_SUBCMD && in[14] == subcmd && (in[13] & 0x80)) {
                if (reply)
                    memcpy(reply, in, JC_RPT_LEN);
                return true;
            }
        }
    }
    return false;
}

void gui_jc_own(int side)
{
    s_rx_n[side] = 0;
    s_framed[side] = false;
    uart_set_mode(_uart(side), UART_AO_TX_HW_RX);
}

void gui_jc_release(int side)
{
    /* Nothing of ours left in the FIFO for the driver to trip over. */
    u8 junk[64];
    while (gui_jc_recv(side, junk, sizeof(junk), 2))
        ;
    uart_set_mode(_uart(side), UART_AO_TX_MN_RX);
    s_rx_n[side] = 0;
}

/* Whether the Joy-Con on this rail really is up: one status request, and an
 * input report back within 40 ms. The driver's connected flags cannot tell:
 * each report from either Joy-Con copies both from its own per-side state,
 * which only a detach clears, so a Joy-Con the driver is still starting over
 * reads as up as soon as the other one reports (bdk joycon.c,
 * _jc_parse_wired_hid). A sensor screen that took one on that word got no
 * answer from it. */
bool gui_jc_answers(int side)
{
    u8 rpt[JC_RPT_LEN];
    bool ok = false;
    gui_jc_own(side);
    gui_jc_poll(side);
    u32 end = get_tmr_ms() + 40;
    while (!ok && (s32)(end - get_tmr_ms()) > 0) {
        u32 n = gui_jc_recv(side, rpt, sizeof(rpt), 10);
        ok = n && (rpt[0] == JC_IN_STD || rpt[0] == JC_IN_SUBCMD || rpt[0] == JC_IN_MCU);
    }
    gui_jc_release(side);
    return ok;
}

/* ---- bringing a Joy-Con up -------------------------------------------- */

#define WIRED_CMD         0x91
#define WIRED_INIT_REPLY  0x94
#define WIRED_HANDSHAKE   0xA5
#define WCMD_GET_INFO     0x01
#define WCMD_HID_CONN     0x10
#define WCMD_HID_DISC     0x11
#define WCMD_SET_HIDRATE  0x12
#define WCMD_SET_BRATE    0x20
#define JC_TYPE_HORI      0x20

/* The driver's start-up, byte for byte (bdk input/joycon.c). */
static const u8 s_init_wake[] = { 0xA1, 0xA2, 0xA3, 0xA4 };
static const u8 s_init_handshake[] = {
    0x19, 0x01, 0x03, 0x07, 0x00, WIRED_HANDSHAKE, 0x02, 0x01, 0x7E, 0x00, 0x00, 0x00
};
static const u8 s_init_get_info[] = {
    0x19, 0x01, 0x03, 0x07, 0x00, WIRED_CMD, WCMD_GET_INFO, 0x00, 0x00, 0x00, 0x00, 0x24
};
static const u8 s_init_brate[] = {           /* 3 Mbaud */
    0x19, 0x01, 0x03, 0x0F, 0x00, WIRED_CMD, WCMD_SET_BRATE, 0x08, 0x00, 0x00, 0xBD, 0xB1,
    0xC0, 0xC6, 0x2D, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const u8 s_init_hid_disc[] = {
    0x19, 0x01, 0x03, 0x07, 0x00, WIRED_CMD, WCMD_HID_DISC, 0x00, 0x00, 0x00, 0x00, 0x0E
};
static const u8 s_init_hid_conn[] = {
    0x19, 0x01, 0x03, 0x07, 0x00, WIRED_CMD, WCMD_HID_CONN, 0x00, 0x00, 0x00, 0x00, 0x3D
};
static const u8 s_init_hid_rate[] = {        /* a report every 15 ms */
    0x19, 0x01, 0x03, 0x0B, 0x00, WIRED_CMD, WCMD_SET_HIDRATE, 0x04, 0x00, 0x00, 0x12, 0xA6,
    0x0F, 0x00, 0x00, 0x00
};
/* The driver's rate command with 1 Mbaud instead of 3, to give a Joy-Con
 * back as the driver starts one. Its checksums are bdk's CRC-8 (polynomial
 * 0x8D, JC_CRC8_POLY): over the payload, then over the header from the
 * command to the payload's checksum; the same sums give the 3 Mbaud
 * packet's BD and B1. */
static const u8 s_brate_1m[] = {
    0x19, 0x01, 0x03, 0x0F, 0x00, WIRED_CMD, WCMD_SET_BRATE, 0x08, 0x00, 0x00, 0xDD, 0xEB,
    0x40, 0x42, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void _raw(int side, const u8 *b, u32 n)
{
    uart_send(_uart(side), b, n);
    uart_wait_xfer(_uart(side), UART_TX_IDLE);
}

/* A wired reply: cmd, and for an init reply its subcommand. The driver
 * leaves the Joy-Con gap_ms after each command before it reads (with manual
 * RTS the Joy-Con holds its reply until then), and a Joy-Con sent its next
 * command straight after a reply ignored it: on the bench a handshake
 * answered at once and the type request that followed got nothing. So the
 * gap is kept, then the reply is waited for up to wait_ms more. */
static const wired_hdr_t *_reply(int side, u8 cmd, u8 subcmd, u32 gap_ms, u32 wait_ms)
{
    msleep(gap_ms);
    u32 end = get_tmr_us() + wait_ms * 1000;
    while (_pkt_recv(side, end)) {
        const wired_hdr_t *w = (const wired_hdr_t *)s_pkt[side];
        s_last[side][0] = w->cmd;
        s_last[side][1] = w->subcmd;
        s_last[side][2] = w->status;
        if (w->cmd == cmd && (cmd != WIRED_INIT_REPLY || w->subcmd == subcmd))
            return w;
    }
    return NULL;
}

/* The rail's UART at a rate, inverted and with manual RTS, as the driver
 * sets it up. */
static void _uart_at(int side, u32 baud)
{
    uart_init(_uart(side), baud, UART_AO_TX_MN_RX);
    uart_invert(_uart(side), true, UART_INVERT_TXD | UART_INVERT_RTS);
    s_rx_n[side] = 0;
}

/* The steps, for the debug console; 0 is up. */
enum { UP_OK, UP_HANDSHAKE, UP_TYPE, UP_HORI, UP_BRATE, UP_HID_DISC, UP_HID_CONN,
       UP_FIRST_RPT, UP_ALREADY };
static const char *const s_up_step[] = {
    "", "handshake", "type", "Hori pad", "3 Mbaud", "HID disconnect", "HID connect",
    "first report", ""
};

static int _bring_up(int side)
{
    u32 uart = _uart(side);
    /* The rail's 5 V and charge enable, as the driver has them for a start. */
    if (!regulator_5v_get_dev_enabled(1 << uart)) {
        regulator_5v_enable(1 << uart);
        gpio_write(side ? GPIO_PORT_K : GPIO_PORT_CC, GPIO_PIN_3, GPIO_HIGH);
    }
    /* Up already, at 3 Mbaud: one the driver had up that missed the screen's
     * first check, or one a try of ours got as far as the HID connection.
     * Started over at 1 Mbaud, it would not hear the handshake. */
    _uart_at(side, 3000000);
    if (gui_jc_answers(side))
        return UP_ALREADY;
    _uart_at(side, 1000000);
    const wired_hdr_t *w = NULL;
    for (u32 i = 0; i < 10 && !w; i++) {
        _raw(side, s_init_wake, sizeof(s_init_wake));
        u8 junk[32];
        uart_recv(uart, junk, sizeof(junk));    /* what the wake stirred up */
        _raw(side, s_init_handshake, sizeof(s_init_handshake));
        w = _reply(side, WIRED_HANDSHAKE, 0, 4, 4);
    }
    if (!w)
        return UP_HANDSHAKE;
    /* Its type: a Hori pad keeps 1 Mbaud and no HID connection, nothing
     * for the sensor screens. */
    _raw(side, s_init_get_info, sizeof(s_init_get_info));
    w = _reply(side, WIRED_INIT_REPLY, WCMD_GET_INFO, 2, 10);
    if (!w || w->status)
        return UP_TYPE;
    if (w->payload[0] & JC_TYPE_HORI)
        return UP_HORI;
    _raw(side, s_init_brate, sizeof(s_init_brate));
    w = _reply(side, WIRED_INIT_REPLY, WCMD_SET_BRATE, 2, 10);
    if (!w || w->status)
        return UP_BRATE;
    _uart_at(side, 3000000);
    /* No HID connection left over (it answers 0xF), then a new one. */
    w = NULL;
    for (u32 i = 0; i < 10 && !w; i++) {
        _raw(side, s_init_hid_disc, sizeof(s_init_hid_disc));
        w = _reply(side, WIRED_INIT_REPLY, WCMD_HID_DISC, 5, 5);
        if (w && w->status != 0xF)
            w = NULL;
    }
    if (!w)
        return UP_HID_DISC;
    _raw(side, s_init_hid_conn, sizeof(s_init_hid_conn));
    w = _reply(side, WIRED_INIT_REPLY, WCMD_HID_CONN, 2, 10);
    if (!w || w->status)
        return UP_HID_CONN;
    /* The report rate's reply may come later, among the reports: the driver
     * does not wait for it, and on the bench it often missed 10 ms. */
    _raw(side, s_init_hid_rate, sizeof(s_init_hid_rate));
    _reply(side, WIRED_INIT_REPLY, WCMD_SET_HIDRATE, 2, 10);
    /* Up once it answers an input request, as the driver counts a Joy-Con
     * connected at its first report: on the bench the screen's first
     * command, sent straight after the HID connection, went unanswered. */
    for (u32 i = 0; i < 3; i++)
        if (gui_jc_answers(side))
            return UP_OK;
    return UP_FIRST_RPT;
}

bool gui_jc_bring_up(int side)
{
    int step = _bring_up(side);
    return step == UP_OK || step == UP_ALREADY;
}

/* One start-up from a rail, and what it came to on the debug console: the
 * step it stopped at, the bytes the Joy-Con sent and the first of them, and
 * its last reply (command, subcommand, status). The first ten tries are
 * logged, then every tenth. */
static bool _rail_bring_up(int side, u32 attempt)
{
    s_seen[side] = 0;
    s_last[side][0] = s_last[side][1] = s_last[side][2] = 0;
    int step = _bring_up(side);
    bool up = step == UP_OK || step == UP_ALREADY;
    if (!up && attempt > 10 && attempt % 10)
        return false;
    char line[160];
    if (up) {
        s_printf(line, "gui: jc %c %s at try %d\n", side ? 'R' : 'L',
                 step == UP_ALREADY ? "was up already, taken" : "started", attempt);
    } else {
        char head[40] = "";
        u32 k = s_seen[side] < sizeof(s_seen_head[0]) ? s_seen[side] : sizeof(s_seen_head[0]);
        for (u32 i = 0; i < k; i++)
            s_printf(head + 3 * i, " %02X", s_seen_head[side][i]);
        s_printf(line, "gui: jc %c try %d stopped at %s: %d bytes back%s%s, last %02X %02X %02X\n",
                 side ? 'R' : 'L', attempt, s_up_step[step], s_seen[side],
                 k ? ", first" : "", head, s_last[side][0], s_last[side][1], s_last[side][2]);
    }
    gui_uart_puts(line);
    return up;
}

/* ---- a sensor screen's rail ------------------------------------------ */

#define RAIL_CHECK_MS   150     /* the detect line, this often              */
#define RAIL_SEAT_MS    300     /* a Joy-Con just slid on, left to seat      */
#define RAIL_RETRY_MS   500     /* between start-ups that got no answer      */
#define RAIL_TRIES      8       /* then it is reported as not answering      */
#define RAIL_SLOW_MS    1000    /* and tried this often, while on the rail   */
#define RAIL_DROPS      2       /* silent that often in a row: left alone    */
#define RAIL_STEADY_MS  5000    /* running this long clears the drops        */

bool gui_jc_rail_start(jc_rail_t *r, int side)
{
    memset(r, 0, sizeof(*r));
    r->rail_at = get_tmr_ms();
    if (!gui_rail_attached(side)) {
        r->state = JCR_OFF;
        return false;
    }
    /* The driver has it up: it answers at 3 Mbaud, and is taken as it is. */
    if (gui_jc_answers(side)) {
        gui_jc_own(side);
        r->state = JCR_RUN;
        r->run_at = r->rail_at;
        return true;
    }
    r->state = JCR_UP;
    r->next_at = r->rail_at;
    return false;
}

int gui_jc_rail_tick(jc_rail_t *r, int side)
{
    u32 now = get_tmr_ms();
    if (now - r->rail_at >= RAIL_CHECK_MS) {
        r->rail_at = now;
        bool on = gui_rail_attached(side);
        if (!on && r->state != JCR_OFF) {
            gui_jc_release(side);
            r->state = JCR_OFF;
            r->tries = 0;
            r->drops = 0;
            return JCR_EV_GONE;
        }
        if (on && r->state == JCR_OFF) {
            r->state = JCR_UP;
            r->tries = 0;
            r->next_at = now + RAIL_SEAT_MS;
            return JCR_EV_STATE;
        }
    }
    if (r->state == JCR_RUN && r->drops && now - r->run_at >= RAIL_STEADY_MS)
        r->drops = 0;
    /* Started until it answers, as the driver does: a Joy-Con that has not
     * answered yet is still tried, less often, while it is on the rail. One
     * that kept going silent is left alone until it is reseated. */
    bool trying = r->state == JCR_UP || (r->state == JCR_STUCK && !r->drops);
    if (trying && (s32)(now - r->next_at) >= 0) {
        if (r->tries < 255)
            r->tries++;
        if (_rail_bring_up(side, r->tries)) {
            gui_jc_own(side);
            r->state = JCR_RUN;
            r->run_at = now;
            return JCR_EV_UP;
        }
        r->next_at = get_tmr_ms() + (r->state == JCR_STUCK ? RAIL_SLOW_MS : RAIL_RETRY_MS);
        if (r->state == JCR_UP && r->tries >= RAIL_TRIES) {
            r->state = JCR_STUCK;
            return JCR_EV_STATE;
        }
    }
    return JCR_EV_NONE;
}

void gui_jc_rail_lost(jc_rail_t *r)
{
    r->state = ++r->drops >= RAIL_DROPS ? JCR_STUCK : JCR_UP;
    r->tries = 0;
    r->next_at = get_tmr_ms();
}

/* A running Joy-Con back to the driver. Left at 3 Mbaud with its HID
 * connection, it is something the driver cannot restart: the driver finds
 * it silent at once (it was not polled), and its restart's 1 Mbaud
 * handshake goes unheard until the Joy-Con drops the connection, 2 s after
 * the screen's last request. A failed restart waits 1.8 s for the next, so
 * the second missed too and the third got through, 3.6 s on. So the
 * Joy-Con is left as the driver's restart expects it: HID disconnected,
 * then at 1 Mbaud. It answers the rate change at the old rate. */
static void _hand_back(int side)
{
    _raw(side, s_init_hid_disc, sizeof(s_init_hid_disc));
    const wired_hdr_t *w = _reply(side, WIRED_INIT_REPLY, WCMD_HID_DISC, 5, 10);
    int disc = w ? w->status : -1;
    s_seen[side] = 0;
    s_last[side][0] = s_last[side][1] = s_last[side][2] = 0;
    _raw(side, s_brate_1m, sizeof(s_brate_1m));
    w = _reply(side, WIRED_INIT_REPLY, WCMD_SET_BRATE, 2, 20);
    bool ok = w && !w->status;
    /* For the bench: what the Joy-Con made of the two. */
    char head[40] = "";
    u32 k = s_seen[side] < sizeof(s_seen_head[0]) ? s_seen[side] : sizeof(s_seen_head[0]);
    for (u32 i = 0; i < k; i++)
        s_printf(head + 3 * i, " %02X", s_seen_head[side][i]);
    char line[200];
    s_printf(line, "gui: jc %c back to the driver %s (HID disconnect %s%02X, rate: %d bytes%s%s, "
                   "last %02X %02X %02X)\n",
             side ? 'R' : 'L', ok ? "at 1 Mbaud" : "with its rate unchanged",
             disc < 0 ? "no reply " : "", disc < 0 ? 0 : disc, s_seen[side],
             k ? ", first" : "", head, s_last[side][0], s_last[side][1], s_last[side][2]);
    gui_uart_puts(line);
}

bool gui_jc_rail_end(jc_rail_t *r, int side)
{
    bool touched = r->state != JCR_OFF;
    if (r->state == JCR_RUN)
        _hand_back(side);
    if (touched)
        gui_jc_release(side);
    r->state = JCR_OFF;
    return touched;
}

void gui_jc_rumble_enable(int side)
{
    /* Subcommand 0x48, argument 1, as the driver sends it; its reply is
     * the driver's to read. */
    u8 rpt[12] = { JC_OUT_SUBCMD, 0 };
    memcpy(rpt + 2, s_rumble_off, 8);
    rpt[10] = 0x48;
    rpt[11] = 0x01;
    gui_jc_send(side, rpt, sizeof(rpt));
}

void gui_jc_rumble(int side, bool on)
{
    u8 rpt[10] = { JC_OUT_RUMBLE, 0 };
    memcpy(rpt + 2, on ? s_rumble_on : s_rumble_off, 8);
    gui_jc_send(side, rpt, sizeof(rpt));
}

/* ---- the stick calibration -------------------------------------------- */

/* The subcommand's own retries are the only ones: a pad with no flash to
 * read keeps the Joy-Con test waiting while this runs, at every connect. */
static bool _spi_read(int side, u32 addr, u32 len, u8 *out)
{
    u8 args[5] = { addr, addr >> 8, addr >> 16, addr >> 24, len };
    u8 reply[JC_RPT_LEN];
    if (!gui_jc_subcmd(side, SUBCMD_SPI_READ, args, sizeof(args), reply))
        return false;
    /* The reply repeats the address and size, then the data. */
    if (memcmp(reply + 15, args, sizeof(args)))
        return false;
    memcpy(out, reply + 20, len);
    return true;
}

/* Three bytes hold two 12-bit values. */
static void _unpack(const u8 *b, u16 *a, u16 *c)
{
    *a = ((b[1] << 8) & 0xF00) | b[0];
    *c = (b[2] << 4) | (b[1] >> 4);
}

/* The nine bytes: the left stick's are max, centre, min; the right's are
 * centre, min, max. */
static bool _decode(int side, const u8 *d, jc_cal_t *cal)
{
    u16 v[6];
    for (u32 i = 0; i < 3; i++)
        _unpack(d + 3 * i, &v[2 * i], &v[2 * i + 1]);
    if (!side) {
        cal->xmax = v[0]; cal->ymax = v[1];
        cal->cx   = v[2]; cal->cy   = v[3];
        cal->xmin = v[4]; cal->ymin = v[5];
    } else {
        cal->cx   = v[0]; cal->cy   = v[1];
        cal->xmin = v[2]; cal->ymin = v[3];
        cal->xmax = v[4]; cal->ymax = v[5];
    }
    /* Erased flash reads 0xFFF throughout. A real calibration has its
     * centre near the middle and a travel of a plausible size. */
    const u16 travel[4] = { cal->xmax, cal->ymax, cal->xmin, cal->ymin };
    for (u32 i = 0; i < 4; i++)
        if (travel[i] < 0x200 || travel[i] > 0xA00)
            return false;
    return cal->cx > 0x400 && cal->cx < 0xC00 && cal->cy > 0x400 && cal->cy < 0xC00;
}

bool gui_jc_colour_read(int side, u8 *rgb)
{
    u8 d[3];
    gui_jc_own(side);
    bool ok = _spi_read(side, SPI_COLOUR, sizeof(d), d);
    gui_jc_release(side);
    /* A pad that never had colours written reads erased flash, FF. */
    if (!ok || (d[0] & d[1] & d[2]) == 0xFF)
        return false;
    memcpy(rgb, d, sizeof(d));
    return true;
}

bool gui_jc_cal_read(int side, jc_cal_t *out)
{
    u8 d[11];
    bool ok = false;

    memset(out, 0, sizeof(*out));
    /* The replies are longer than the driver's manual flow control lets
     * through at once, so hardware flow control for the exchange, as the
     * driver uses for its own pairing read. */
    gui_jc_own(side);
    if (_spi_read(side, side ? SPI_CAL_USER_R : SPI_CAL_USER_L, 11, d)
        && d[0] == CAL_USER_MAGIC_0 && d[1] == CAL_USER_MAGIC_1 && _decode(side, d + 2, out)) {
        out->user = true;
        ok = true;
    } else if (_spi_read(side, side ? SPI_CAL_FACT_R : SPI_CAL_FACT_L, 9, d)) {
        ok = _decode(side, d, out);
    }
    gui_jc_release(side);
    return ok;
}
