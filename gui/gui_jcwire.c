/*
 * hwtest GUI - the Joy-Con's wired HID protocol (see gui_jcwire.h).
 */
#include "gui.h"
#include "gui_jcwire.h"
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

u32 gui_jc_recv(int side, u8 *out, u32 max, u32 wait_ms)
{
    u8  *buf = s_rx[side];
    u32 *n   = &s_rx_n[side];
    u32 end  = get_tmr_us() + wait_ms * 1000;

    for (;;) {
        /* Whatever precedes a reply magic is noise, or the tail of a
         * report whose start was missed. */
        u32 at = 0;
        while (at + 3 <= *n && memcmp(buf + at, WIRED_RCV_MAGIC, 3))
            at++;
        if (at) {
            *n -= at;
            memmove(buf, buf + at, *n);
        }
        if (*n >= sizeof(wired_hdr_t)) {
            const wired_hdr_t *w = (const wired_hdr_t *)buf;
            u32 len = w->cmd == WIRED_HID ? w->payload_size : 0;
            u32 pkt = len ? sizeof(wired_hdr_t) + len : 5u + w->total_size;
            if (pkt < sizeof(wired_hdr_t) || pkt > sizeof(s_rx[0])) {
                *n = 0;             /* not a length a Joy-Con sends: start over */
            } else if (*n >= pkt) {
                /* The driver never checks a reply's two sizes against each
                 * other, so the first one each time goes to the debug UART
                 * for the bench to confirm the framing. */
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
                if (len > max)
                    len = max;
                if (len)
                    memcpy(out, w->payload, len);
                *n -= pkt;
                memmove(buf, buf + pkt, *n);
                if (len)
                    return len;
                continue;           /* a wired command's reply: not a report */
            }
        }
        if ((s32)(end - get_tmr_us()) <= 0)
            return 0;
        if (*n >= sizeof(s_rx[0]))
            *n = 0;
        *n += uart_recv(_uart(side), buf + *n, sizeof(s_rx[0]) - *n);
    }
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
