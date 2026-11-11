/*
 * hwtest - hwtest/hwtest.ini, the debug console's settings. See dbgcfg.h.
 *
 * Lines are key=value; # and ; start a comment; [sections] and keys it does
 * not know are ignored. Case, and _ against -, are not told apart, so
 * "Console = UART_A" is "console=uart-a". Kept small: it is in hwtest.bin
 * too, which has to stay under the RCM limit.
 */
#include "hwtest.h"
#include "dbgcfg.h"

static bool _is(const char *s, u32 n, const char *w)
{
    for (u32 i = 0; i < n; i++, w++) {
        char c = s[i] | 0x20;               /* ASCII lower case; - stays - */
        if (c == ('_' | 0x20))
            c = '-';
        if (!*w || c != *w)
            return false;
    }
    return !*w;
}

/* Value words, in order: three UARTs (and their other names), then off and
 * on for gui_usb. */
static const char *const s_words[] = {
    "uart-a", "pads", "test-pads",
    "uart-b", "uart", "right-rail",
    "uart-c", "left-rail",
    "0", "off", "no",
    "1", "on", "yes",
};
static const u8 s_word_val[] = {
    UART_A, UART_A, UART_A,
    UART_B, UART_B, UART_B,
    UART_C, UART_C,
    0, 0, 0,
    1, 1, 1,
};

static int _word(const char *s, u32 n)
{
    for (u32 i = 0; i < ARRAY_SIZE(s_words); i++)
        if (_is(s, n, s_words[i]))
            return i;
    return -1;
}

static bool _blank(char c) { return c == ' ' || c == '\t'; }

void dbg_cfg_read(dbg_cfg_t *c)
{
    c->uart = c->gui_usb = c->dp_pol = c->dp_swap = -1;
    c->bad[0] = 0;
    u32 size = 0;
    char *t = sd_file_read(DBG_CFG_PATH, &size);
    if (!t)
        return;
    for (u32 i = 0; i < size; ) {
        u32 e = i;
        while (e < size && t[e] != '\n' && t[e] != '\r')
            e++;
        u32 k = i;
        while (k < e && _blank(t[k]))
            k++;
        u32 ke = k;
        while (ke < e && t[ke] != '=' && !_blank(t[ke]))
            ke++;
        u32 v = ke;
        while (v < e && (_blank(t[v]) || t[v] == '='))
            v++;
        u32 ve = v;
        while (ve < e && !_blank(t[ve]) && t[ve] != '#' && t[ve] != ';')
            ve++;
        int w = _word(t + v, ve - v);
        if (_is(t + k, ke - k, "console")) {
            if (w >= 0 && w < 8) {
                c->uart = s_word_val[w];
            } else {
                u32 n = ve - v < sizeof(c->bad) - 1 ? ve - v : sizeof(c->bad) - 1;
                memcpy(c->bad, t + v, n);
                c->bad[n] = 0;
            }
        } else if (_is(t + k, ke - k, "gui-usb") && w >= 8) {
            c->gui_usb = s_word_val[w];
        } else if (_is(t + k, ke - k, "dp-pol") && w >= 8) {
            c->dp_pol = s_word_val[w];
        } else if (_is(t + k, ke - k, "dp-swap") && w >= 8) {
            c->dp_swap = s_word_val[w];
        }
        i = e + 1;
    }
    free(t);
}

void dbg_uart_up(u32 u)
{
    if (u == UART_A) {
        pinmux_config_uart(UART_A);
        clock_enable_uart(UART_A);
    } else if (u == UART_C) {
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
