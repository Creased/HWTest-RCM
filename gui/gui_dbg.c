/*
 * hwtest GUI - where the debug console goes.
 *
 * The text payload's console is UART-B, on the right Joy-Con rail. In the
 * GUI it can be moved from the Custom Check screen: UART-B on the right
 * rail, UART-C on the left rail (not on a Lite, whose built-in controls are
 * on it) or UART-A on the board's test pads, all at 115200 8N1 with 1.8 V
 * levels, or a serial port on a PC over the USB-C port (usbser.c).
 * Everything the console carries follows: the check's report as it runs,
 * the GUI's own lines, and the R that reboots the console.
 *
 * A rail's port becomes the Joy-Con's once one is attached there, as UART-B
 * always did: nothing more is sent on it, and nothing read that the
 * Joy-Con driver needs. R is also taken on UART-B while it is free,
 * whichever port is picked, so a bench that reboots the console over it
 * keeps working.
 *
 * The USB serial port also runs beside whichever port is picked, from the
 * GUI's start: a PC on the USB-C port that opens it gets the same lines,
 * and can send the same commands (gui_push.c), with no adapter and no
 * choice made on the console first.
 */
#include "gui.h"
#include "usbser.h"
#include "../dbgcfg.h"
#include "../dpout.h"

static gui_dbg_port_t s_port = DBG_UART_B;
static bool s_taken[2];          /* UART-B, UART-C: a Joy-Con (a Lite's controls) owns it */
static bool s_lite;
static bool s_usb;               /* the USB serial port is up, as console or beside it */
static bool s_src_usb;           /* where the last byte dbg_recv returned came from */
static u32 s_src_uart;

static const u32 s_uart[DBG_USB] = { UART_B, UART_C, UART_A };
static const char *const s_name[DBG_PORTS] = {
    "UART-B, right rail", "UART-C, left rail", "UART-A, test pads", "USB serial",
};

const char *gui_dbg_name(gui_dbg_port_t p) { return s_name[p]; }
gui_dbg_port_t gui_dbg_port(void)          { return s_port; }

bool gui_dbg_taken(gui_dbg_port_t p)
{
    return p <= DBG_UART_C && s_taken[p];
}

/* A rail's IsAttached line shares its contact with that UART's TX, so it is
 * read with TX released, as bdk's Joy-Con driver reads it. A Joy-Con pulls
 * it low; a bench UART adapter does not. */
static bool _rail_attached(gui_dbg_port_t p)
{
    bool left = p == DBG_UART_C;
    u32 tx  = left ? PINMUX_AUX_UART3_TX : PINMUX_AUX_UART2_TX;
    u32 tx_port = left ? GPIO_PORT_D : GPIO_PORT_G;
    u32 tx_pin  = left ? GPIO_PIN_1 : GPIO_PIN_0;
    u32 det = left ? GPIO_PORT_E : GPIO_PORT_H;

    PINMUX_AUX(tx) = PINMUX_INPUT_ENABLE;
    gpio_direction_input(tx_port, tx_pin);
    usleep(20);
    (void)gpio_read(det, GPIO_PIN_6);       /* unlatches the input buffer */
    bool on = !gpio_read(det, GPIO_PIN_6);
    PINMUX_AUX(tx) = 0;
    gpio_config(tx_port, tx_pin, GPIO_MODE_SPIO);
    usleep(20);
    return on;
}

/* The port set up as main.c sets UART-B up for the console (dbgcfg.c,
 * shared with hwtest.bin): 115200 with no line inversion (the Joy-Con
 * driver inverts a rail's TX), RX pulled up, and on the left rail the 5 V
 * and the charge-enable that bridge its pads to the SoC. */
static void _uart_up(gui_dbg_port_t p)
{
    dbg_uart_up(s_uart[p]);
}

/* After jc_init_hw(), which re-muxed both rails for Joy-Cons: with nothing
 * on the right rail, UART-B is brought back up (for R, and as the console
 * when it is picked). A Lite's controls are on UART-C, and bdk leaves its
 * UART-B alone. Then hwtest/hwtest.ini, as hwtest.bin reads it (dbgcfg.h):
 * console= picks the UART, the right rail when the file does not say, and
 * gui_usb=0, the GUI's own key, turns off the USB serial port that runs
 * beside it. */
void gui_dbg_init(bool lite)
{
    s_lite = lite;
    s_taken[DBG_UART_C] = lite;
    if (!lite) {
        if (_rail_attached(DBG_UART_B))
            s_taken[DBG_UART_B] = true;
        else
            _uart_up(DBG_UART_B);
    }

    dbg_cfg_t cfg = { -1, -1, -1, -1, "" };
    if (g_sd_ok && !sd_mount())
        dbg_cfg_read(&cfg);
    char line[96];
    if (cfg.bad[0]) {
        s_printf(line, "gui: " DBG_CFG_PATH ": console=%s unknown\n", cfg.bad);
        dbg_send((u8 *)line, strlen(line));
    }
    if (cfg.dp_pol > 0 || cfg.dp_swap > 0) {  /* bench tests of the DisplayPort lanes */
        dpout_lane_test(cfg.dp_pol > 0, cfg.dp_swap > 0);
        s_printf(line, "gui: " DBG_CFG_PATH ": DP lanes %s%s%s for the Dock Test\n",
                 cfg.dp_pol > 0 ? "inverted" : "", cfg.dp_pol > 0 && cfg.dp_swap > 0 ? " and " : "",
                 cfg.dp_swap > 0 ? "exchanged" : "");
        dbg_send((u8 *)line, strlen(line));
    }
    if (cfg.gui_usb == 0 && s_usb) {
        dbg_send((const u8 *)"gui: gui_usb=0, USB serial off\n", 31);
        usbser_stop();
        s_usb = false;
    }
    gui_dbg_port_t want = cfg.uart == UART_A ? DBG_UART_A :
                          cfg.uart == UART_C ? DBG_UART_C : DBG_UART_B;
    if (want != s_port && !gui_dbg_set(want)) {
        s_printf(line, "gui: %s is not there, the console stays on %s\n",
                 s_name[want], s_name[s_port]);
        dbg_send((u8 *)line, strlen(line));
    }
}

/* Before each Joy-Con poll. The driver releases both rails' TX pins to look
 * for Joy-Cons, so the console's last bytes go out first, and a rail the
 * console needs is checked before the driver can take its UART over:
 * UART-B always (for the reboot), UART-C while it is the console. */
void gui_dbg_rails(void)
{
    if (s_lite)
        return;
    for (u32 p = DBG_UART_B; p <= DBG_UART_C; p++) {
        if (s_taken[p] || (p == DBG_UART_C && s_port != DBG_UART_C))
            continue;
        uart_wait_xfer(s_uart[p], UART_TX_IDLE);
        if (_rail_attached(p))
            s_taken[p] = true;
    }
}

/* A Joy-Con on a rail, by its detect line, for a sensor screen: it stops
 * polling the driver while it owns a rail (gui_jcwire.h). What the rail's
 * UART is sending goes out first, since its TX pin is let go for the read. */
bool gui_rail_attached(int side)
{
    if (s_lite)
        return false;
    gui_dbg_port_t p = side ? DBG_UART_B : DBG_UART_C;
    uart_wait_xfer(s_uart[p], UART_TX_IDLE);
    bool on = _rail_attached(p);
    if (on)
        s_taken[p] = true;          /* the console keeps off a Joy-Con's UART */
    return on;
}

void gui_dbg_poll(void)
{
    if (s_usb)
        usbser_poll();
}

/* The USB serial port beside the console, from the GUI's start. */
/* A long answer to a PC's command (a file, gui_push.c): over USB alone
 * when it is up, since at 115200 it would hold a bench UART for minutes,
 * and paced by the PC, whose queue drops what it cannot hold. False when
 * the PC stops taking it. */
bool gui_dbg_bulk(const u8 *buf, u32 n)
{
    if (!s_usb) {
        dbg_send(buf, n);
        return true;
    }
    usbser_send(buf, n);
    u32 t0 = get_tmr_ms();
    while (!usbser_tx_empty())
        if (get_tmr_ms() - t0 > 2000)
            return false;
    return true;
}

void gui_dbg_usb_start(void)
{
    s_usb = usbser_start();
}

/* Before handing over to another payload: the USB controller keeps a read
 * queued, which it would go on filling by DMA in memory the next payload
 * owns (the RAM test tests it). */
void gui_dbg_end(void)
{
    if (s_usb) {
        for (u32 t0 = get_tmr_ms(); !usbser_tx_empty() && get_tmr_ms() - t0 < 300; )
            ;
        usbser_stop();
        s_usb = false;
    }
}

bool gui_dbg_set(gui_dbg_port_t p)
{
    if (p == s_port)
        return true;
    if (p >= DBG_PORTS || (p == DBG_UART_C && s_lite))
        return false;
    if (p == DBG_USB && !s_usb) {
        if (!usbser_start())
            return false;
        s_usb = true;
    }

    char line[80];
    s_printf(line, "gui: debug console moves to %s\n", s_name[p]);
    dbg_send((u8 *)line, strlen(line));
    /* The USB port stays up beside the console when it is left. */
    if (s_port != DBG_USB && !gui_dbg_taken(s_port))
        uart_wait_xfer(s_uart[s_port], UART_TX_IDLE);

    /* A left Joy-Con the driver already drives keeps its port. */
    if (p == DBG_UART_C && !s_taken[p] && _rail_attached(p))
        s_taken[p] = true;
    if (p != DBG_USB && !gui_dbg_taken(p))
        _uart_up(p);
    s_port = p;

    s_printf(line, "\n=== hwtest GUI === debug console on %s\n", s_name[p]);
    dbg_send((u8 *)line, strlen(line));
    return true;
}

void dbg_send(const u8 *buf, u32 n)
{
    if (s_usb)
        usbser_send(buf, n);
    if (s_port != DBG_USB && !gui_dbg_taken(s_port))
        uart_send_crlf(s_uart[s_port], buf, n);
}

int dbg_recv(u8 *c)
{
    if (s_usb && usbser_getc(c)) {
        s_src_usb = true;
        return 1;
    }
    s_src_usb = false;
    if (s_port != DBG_USB && !gui_dbg_taken(s_port) &&
        uart_recv(s_uart[s_port], c, 1) == 1) {
        s_src_uart = s_uart[s_port];
        return 1;
    }
    /* UART-B while it is free, whichever port is the console. */
    if (s_port != DBG_UART_B && !s_taken[DBG_UART_B] &&
        uart_recv(UART_B, c, 1) == 1) {
        s_src_uart = UART_B;
        return 1;
    }
    return 0;
}

/* More bytes from where the last one dbg_recv returned came from: a file
 * sent after a command (gui_push.c) is read from that port only. */
u32 gui_dbg_read(u8 *buf, u32 max)
{
    if (s_src_usb)
        return usbser_read(buf, max);
    u32 n = 0;
    while (n < max && uart_recv(s_src_uart, buf + n, 1) == 1)
        n++;
    return n;
}
