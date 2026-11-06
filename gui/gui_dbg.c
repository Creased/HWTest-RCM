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
 */
#include "gui.h"
#include "usbser.h"

static gui_dbg_port_t s_port = DBG_UART_B;
static bool s_taken[2];          /* UART-B, UART-C: a Joy-Con (a Lite's controls) owns it */
static bool s_lite;

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

/* The port set up as main.c sets UART-B up for the console: the controller
 * at 115200 with no line inversion (the Joy-Con driver inverts a rail's TX),
 * its RX pin owned by the UART and pulled up, as a bench adapter leaves the
 * line floating between bytes, and on the left rail the 5 V and the
 * charge-enable that bridge the rail's pads to the SoC (main.c does the
 * right rail's). Then whatever the line caught while it settled is
 * drained. */
static void _uart_up(gui_dbg_port_t p)
{
    u32 u = s_uart[p];

    if (p == DBG_UART_A) {
        pinmux_config_uart(UART_A);
        clock_enable_uart(UART_A);
    } else if (p == DBG_UART_C) {
        regulator_5v_enable(REGULATOR_5V_JC_L);
        PINMUX_AUX(PINMUX_AUX_SPDIF_IN) = PINMUX_PULL_DOWN | 1;
        gpio_direction_output(GPIO_PORT_CC, GPIO_PIN_3, GPIO_HIGH);
        gpio_config(GPIO_PORT_D, GPIO_PIN_1, GPIO_MODE_SPIO);
        gpio_config(GPIO_PORT_D, GPIO_PIN_0, GPIO_MODE_SPIO);
        pinmux_config_uart(UART_C);
        clock_enable_uart(UART_C);
    } else {
        gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_SPIO);
    }
    uart_init(u, 115200, UART_AO_TX_AO_RX);
    uart_invert(u, false, UART_INVERT_TXD | UART_INVERT_RTS);
    PINMUX_AUX(PINMUX_AUX_UARTX_RX(u)) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP;

    usleep(2000);
    u8 c;
    while (uart_recv(u, &c, 1) == 1)
        ;
}

/* After jc_init_hw(), which re-muxed both rails for Joy-Cons: with nothing
 * on the right rail, UART-B goes back to being the console. A Lite's
 * controls are on UART-C, and bdk leaves its UART-B alone. */
void gui_dbg_init(bool lite)
{
    s_lite = lite;
    s_taken[DBG_UART_C] = lite;
    if (lite)
        return;
    if (_rail_attached(DBG_UART_B))
        s_taken[DBG_UART_B] = true;
    else
        _uart_up(DBG_UART_B);
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

void gui_dbg_poll(void)
{
    if (s_port == DBG_USB)
        usbser_poll();
}

bool gui_dbg_set(gui_dbg_port_t p)
{
    if (p == s_port)
        return true;
    if (p >= DBG_PORTS || (p == DBG_UART_C && s_lite))
        return false;
    if (p == DBG_USB && !usbser_start())
        return false;

    char line[80];
    s_printf(line, "gui: debug console moves to %s\n", s_name[p]);
    dbg_send((u8 *)line, strlen(line));
    if (s_port == DBG_USB)
        usbser_stop();
    else if (!gui_dbg_taken(s_port))
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
    if (s_port == DBG_USB)
        usbser_send(buf, n);
    else if (!gui_dbg_taken(s_port))
        uart_send_crlf(s_uart[s_port], buf, n);
}

int dbg_recv(u8 *c)
{
    if (s_port == DBG_USB) {
        if (usbser_getc(c))
            return 1;
    } else if (!gui_dbg_taken(s_port) && uart_recv(s_uart[s_port], c, 1) == 1) {
        return 1;
    }
    /* UART-B while it is free, whichever port is the console. */
    return s_port != DBG_UART_B && !s_taken[DBG_UART_B] &&
           uart_recv(UART_B, c, 1) == 1;
}
