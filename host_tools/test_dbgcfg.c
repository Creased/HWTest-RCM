/*
 * hwtest/hwtest.ini on the host: the parser in dbgcfg.c against files it
 * will meet, including the list of networks the Wi-Fi screen reads
 * passphrases out of. CI runs it, so a card that has been edited by hand
 * is read the same way the build was tested with.
 *
 *   cc -o test_dbgcfg host_tools/test_dbgcfg.c dbgcfg.c \
 *      -I. -Ithird_party/hekate/bdk ...   (see .github/workflows/ci.yml)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <soc/uart.h>
#include "../dbgcfg.h"

static int fails;
#define CHECK(c, what) do {                                              \
        if (c) printf("ok   %s\n", what);                                \
        else { printf("FAIL %s\n", what); fails++; }                     \
    } while (0)

/* What the card holds, for this run. dbg_cfg_read() frees what this
 * returns, so it comes out of the same allocator. */
static const char *s_file;

void *sd_file_read(const char *path, u32 *fsize)
{
    (void)path;
    if (!s_file)
        return NULL;
    u32 n = (u32)strlen(s_file);
    char *buf = malloc(n + 1);
    memcpy(buf, s_file, n + 1);
    *fsize = n;
    return buf;
}

/* The rest of dbgcfg.c's world, which this test does not reach. */
void pinmux_config_uart(u32 idx) { (void)idx; }
void clock_enable_uart(u32 idx) { (void)idx; }
void regulator_5v_enable(u8 dev) { (void)dev; }
void gpio_direction_output(u32 port, u32 pins, int high)
{ (void)port; (void)pins; (void)high; }
void gpio_config(u32 port, u32 pins, int mode)
{ (void)port; (void)pins; (void)mode; }
void uart_init(u32 idx, u32 baud, u32 mode) { (void)idx; (void)baud; (void)mode; }
void uart_invert(u32 idx, bool enable, u32 invert_mask)
{ (void)idx; (void)enable; (void)invert_mask; }
u32  uart_recv(u32 idx, u8 *buf, u32 len) { (void)idx; (void)buf; (void)len; return 0; }
void usleep(u32 us) { (void)us; }

static dbg_cfg_t read(const char *text)
{
    dbg_cfg_t c;
    s_file = text;
    dbg_cfg_read(&c);
    return c;
}

int main(void)
{
    dbg_cfg_t c;

    c = read(NULL);
    CHECK(c.uart == -1 && c.gui_usb == -1 && c.wifi_n == 0,
          "no file at all: nothing is set");

    c = read("console = UART_A\r\ngui_usb=yes\n; a comment\ndp_swap=1\n");
    CHECK(c.uart == UART_A, "console=UART_A, case and _ ignored");
    CHECK(c.gui_usb == 1 && c.dp_swap == 1, "yes and 1 both read as on");
    CHECK(c.bad[0] == 0, "and nothing was left unread");

    c = read("console=uart-q\n");
    CHECK(c.uart == -1 && !strcmp(c.bad, "uart-q"),
          "a console it does not know is reported, not guessed");

    /* One network: what the file has always held, and what a scan joins
     * on its own. */
    c = read("wifi_ssid=Livebox-1A2B\nwifi_psk=a b#c;d  \n");
    CHECK(c.wifi_n == 1, "one network");
    CHECK(!strcmp(c.wifi[0].ssid, "Livebox-1A2B"), "its name");
    CHECK(!strcmp(c.wifi[0].psk, "a b#c;d"),
          "a passphrase runs to the end of its line, # and ; included, "
          "trailing blanks trimmed");

    /* The list: each name starts another network, and the passphrase
     * under it is that one's. */
    c = read("wifi_ssid=one\nwifi_psk=1111\n"
             "\n[somewhere]\n"
             "wifi_ssid=two\nwifi_psk=2222\n"
             "wifi_ssid=open one\n"
             "wifi_ssid=four\nwifi_psk=4444\n");
    CHECK(c.wifi_n == 4, "four networks");
    CHECK(!strcmp(c.wifi[1].ssid, "two") && !strcmp(c.wifi[1].psk, "2222"),
          "the second keeps its own passphrase");
    CHECK(c.wifi[2].psk[0] == 0, "one with no passphrase is an open network");
    CHECK(!strcmp(c.wifi[3].psk, "4444"),
          "and the one after it is not given the open one's");

    CHECK(!strcmp(dbg_cfg_psk(&c, "two"), "2222"), "a name finds its own");
    CHECK(dbg_cfg_psk(&c, "three") == NULL, "a name not in the file finds none");
    CHECK(dbg_cfg_psk(&c, "Two") == NULL, "names are bytes, not folded case");
    CHECK(dbg_cfg_psk(&c, "open one") != NULL &&
          dbg_cfg_psk(&c, "open one")[0] == 0,
          "and an open network answers an empty passphrase, not none");
    CHECK(dbg_cfg_psk(&c, "") == NULL,
          "a network with no name of its own finds none");

    /* An entry with no name - how a file keeps the list but asks for no
     * automatic join - is not the passphrase of every hidden network. */
    c = read("wifi_ssid=\nwifi_psk=nameless\nwifi_ssid=named\nwifi_psk=1234\n");
    CHECK(c.wifi_n == 2, "a nameless entry still takes its place");
    CHECK(dbg_cfg_psk(&c, "") == NULL, "and no hidden network matches it");
    CHECK(!strcmp(dbg_cfg_psk(&c, "named"), "1234"),
          "while the one under it is found as usual");

    /* A passphrase with no name above it belongs to nothing. */
    c = read("wifi_psk=orphan\nwifi_ssid=late\n");
    CHECK(c.wifi_n == 1 && c.wifi[0].psk[0] == 0,
          "a passphrase before any name is dropped");

    /* Past the end of the table the rest go, rather than over something. */
    {
        char many[1024];
        u32 at = 0;
        for (u32 i = 0; i < DBG_WIFI_MAX + 3; i++)
            at += (u32)sprintf(many + at, "wifi_ssid=net%d\nwifi_psk=p%d\n", i, i);
        c = read(many);
        CHECK(c.wifi_n == DBG_WIFI_MAX, "the table fills and stops");
        CHECK(!strcmp(c.wifi[DBG_WIFI_MAX - 1].psk, "p7"),
              "with the last one it took intact");
    }

    /* Longer than the fields hold: cut, not written past. */
    c = read("wifi_ssid=0123456789012345678901234567890123456789\n");
    CHECK(strlen(c.wifi[0].ssid) == 32, "a name longer than 32 bytes is cut");

    printf(fails ? "%d FAILED\n" : "all OK\n", fails);
    return fails ? 1 : 0;
}
