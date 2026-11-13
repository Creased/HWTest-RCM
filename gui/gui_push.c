/*
 * hwtest GUI - commands from the PC on the debug console.
 *
 * Besides R (reboot, as the text payload has always taken it), a line that
 * starts with ! is a command, so a PC on the USB-C port (or a bench UART)
 * can put a new build on the SD card and start it, with nobody at the
 * console (host_tools/hwtest_push.py drives it):
 *
 *   !PING                        +PONG hwtest GUI <version>, console on <port>
 *   !LS <folder>                 +OK <folder>: name size, sub/, ...
 *   !GET <path>                  +DATA <size> <crc32>, =<hex> lines, +END
 *                                (backup/ and hwtest/ only, read only)
 *   !RMLOGS                      +OK removed <n>: every hwtest/tvlink_*.txt
 *                                (the Dock Test's logs) deleted, nothing else
 *   !PUT <path> <size> <crc32>   +READY <size>, then exactly <size> raw
 *                                bytes; +OK <path> <size> <crc32> once
 *                                it is on the card and read back
 *   !RUN                         +OK, then bootloader/payloads/hwtest.bin
 *                                is started from the card, as the RAM test
 *                                is: it brings up the gui.bin now there
 *   !BT OPEN|SCAN|LIST|CONNECT <n>|DISCONNECT|STATE|CLOSE|RAILPAIR
 *                                the Bluetooth test, driven from the PC
 *
 * Anything that goes wrong answers -ERR and a reason. Only the four files
 * a release puts on the card, and hwtest/hwtest.ini (the console setting),
 * can be written, each through a temporary file
 * that replaces it once it has been checked, so a transfer that dies half
 * way leaves the old one in place.
 */
#include "gui.h"
#include "gui_bt.h"
#include "../ui_menu.h"

#define PUT_MAX       (8 * 1024 * 1024)
#define PUT_IDLE_MS   5000      /* a transfer that stops this long fails */
#define RUN_MAX       0x2FF00   /* what the chain loader can stage        */
#define GET_MAX       (4 * 1024 * 1024)
#define GET_LINE      48        /* file bytes per =<hex> line            */

static const char *const s_files[] = {
    "hwtest/gui.bin",
    "hwtest/libsys_minerva.bso",
    "bootloader/payloads/hwtest.bin",
    "bootloader/payloads/hwtest_ram.bin",
    "hwtest/hwtest.ini",                 /* the console setting, gui_dbg.c */
};

static char s_line[160];
static u32 s_line_n;
static bool s_in_line;

static void _reply(const char *s)
{
    dbg_send((const u8 *)s, strlen(s));
}

static bool _allowed(const char *path)
{
    for (u32 i = 0; i < ARRAY_SIZE(s_files); i++)
        if (!strcmp(path, s_files[i]))
            return true;
    return false;
}

/* Read back only where hwtest itself writes: backup/ (the reports) and
 * hwtest/ (screenshots, Dock Test logs, the settings). */
static bool _readable(const char *path)
{
    if (strstr(path, ".."))
        return false;
    return !strcmp(path, "backup") || !strcmp(path, "hwtest") ||
           !strncmp(path, "backup/", 7) || !strncmp(path, "hwtest/", 7);
}

static u32 _parse_u32(const char **p, u32 base)
{
    while (**p == ' ')
        (*p)++;
    u32 v = 0;
    for (;; (*p)++) {
        char c = **p;
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0')
              : base == 16 && c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10)
              : base == 16 && c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 99;
        if (d >= base)
            return v;
        v = v * base + d;
    }
}

static void _put(const char *args)
{
    char path[64], tmp[72], buf[160];
    u32 n = 0;
    while (*args == ' ')
        args++;
    while (*args && *args != ' ' && n < sizeof(path) - 1)
        path[n++] = *args++;
    path[n] = 0;
    u32 size = _parse_u32(&args, 10);
    u32 crc = _parse_u32(&args, 16);

    if (!_allowed(path)) {
        _reply("-ERR not a file a release writes\n");
        return;
    }
    if (!size || size > PUT_MAX) {
        _reply("-ERR bad size\n");
        return;
    }
    if (!g_sd_ok || sd_mount()) {
        _reply("-ERR no SD card\n");
        return;
    }
    u8 *data = malloc(size);
    if (!data) {
        _reply("-ERR out of memory\n");
        return;
    }

    s_printf(buf, "+READY %d\n", size);
    _reply(buf);
    /* Nothing is drawn meanwhile: at 115200 a frame's worth of bytes
     * would overrun the UART's FIFO. Over USB the controller holds the
     * PC off whenever this falls behind. */
    u32 got = 0, last = get_tmr_ms();
    while (got < size) {
        u32 k = gui_dbg_read(data + got, size - got);
        if (k) {
            got += k;
            last = get_tmr_ms();
        } else if (get_tmr_ms() - last > PUT_IDLE_MS) {
            free(data);
            s_printf(buf, "-ERR timed out after %d of %d bytes\n", got, size);
            _reply(buf);
            return;
        }
    }

    u32 have = crc32_calc(0, data, size);
    if (have != crc) {
        free(data);
        s_printf(buf, "-ERR crc %08X, expected %08X\n", have, crc);
        _reply(buf);
        return;
    }

    f_mkdir("hwtest");
    f_mkdir("bootloader");
    f_mkdir("bootloader/payloads");
    s_printf(tmp, "%s.new", path);
    int err = sd_save_to_file(data, size, tmp);
    free(data);
    if (err) {
        _reply("-ERR could not write the card\n");
        return;
    }
    /* What landed on the card, read back, before it replaces the old one. */
    u32 back_n = 0;
    u8 *back = sd_file_read(tmp, &back_n);
    u32 back_crc = back ? crc32_calc(0, back, back_n) : 0;
    free(back);
    if (back_n != size || back_crc != crc) {
        f_unlink(tmp);
        _reply("-ERR the card did not read back what was written\n");
        return;
    }
    f_unlink(path);
    if (f_rename(tmp, path) != FR_OK) {
        _reply("-ERR could not rename the new file into place\n");
        return;
    }
    s_printf(buf, "+OK %s %d %08X\n", path, size, crc);
    _reply(buf);
    s_printf(buf, "gui: push wrote %s, %d bytes\n", path, size);
    gui_uart_puts(buf);
}

static void _ls(const char *path)
{
    static char buf[1024];
    if (!_readable(path)) {
        _reply("-ERR only backup/ and hwtest/ can be read\n");
        return;
    }
    if (!g_sd_ok || sd_mount()) {
        _reply("-ERR no SD card\n");
        return;
    }
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, path) != FR_OK) {
        _reply("-ERR no such folder\n");
        return;
    }
    s_printf(buf, "+OK %s:", path);
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (strlen(buf) + strlen(fi.fname) + 16 > sizeof(buf) - 8) {
            strcat(buf, " ...");
            break;
        }
        if (fi.fattrib & AM_DIR)
            s_printf(buf + strlen(buf), " %s/", fi.fname);
        else
            s_printf(buf + strlen(buf), " %s %d", fi.fname, (u32)fi.fsize);
    }
    f_closedir(&dir);
    strcat(buf, "\n");
    _reply(buf);
}

/* The Dock Test's logs, hwtest/tvlink_*.txt, and nothing else: a clean
 * folder before a new round of dock tests (!LS shows 1 KB of it at most).
 * The names are gathered a batch at a time with the folder closed, then
 * deleted. */
static void _rmlogs(void)
{
    static char names[16][40];
    char buf[64];
    u32 removed = 0;
    bool stuck = false;
    if (!g_sd_ok || sd_mount()) {
        _reply("-ERR no SD card\n");
        return;
    }
    for (;;) {
        DIR dir;
        FILINFO fi;
        u32 n = 0;
        if (f_opendir(&dir, "hwtest") != FR_OK)
            break;
        while (n < ARRAY_SIZE(names) && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            u32 len = strlen(fi.fname);
            if (!(fi.fattrib & AM_DIR) && !strncmp(fi.fname, "tvlink_", 7) &&
                len > 11 && len < sizeof(names[0]) - 8 && !strcmp(fi.fname + len - 4, ".txt"))
                s_printf(names[n++], "hwtest/%s", fi.fname);
        }
        f_closedir(&dir);
        if (!n)
            break;
        u32 before = removed;
        for (u32 i = 0; i < n; i++)
            if (f_unlink(names[i]) == FR_OK)
                removed++;
        if (removed == before) {
            stuck = true;       /* none of them went: stop rather than loop */
            break;
        }
    }
    if (stuck)
        s_printf(buf, "-ERR removed %d, the rest would not go\n", removed);
    else
        s_printf(buf, "+OK removed %d\n", removed);
    _reply(buf);
    s_printf(buf, "gui: push removed %d Dock Test logs\n", removed);
    gui_uart_puts(buf);
}

/* +DATA <size> <crc32>, then the file as =<hex> lines, then +END. Hex
 * keeps it plain text, so nothing on the way turns a byte into another. */
static void _get(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[2 * GET_LINE + 4];
    if (!_readable(path)) {
        _reply("-ERR only backup/ and hwtest/ can be read\n");
        return;
    }
    if (!g_sd_ok || sd_mount()) {
        _reply("-ERR no SD card\n");
        return;
    }
    u32 size = 0;
    u8 *data = sd_file_read(path, &size);
    if (!data) {
        _reply("-ERR no such file\n");
        return;
    }
    if (size > GET_MAX) {
        free(data);
        _reply("-ERR too large\n");
        return;
    }
    s_printf(buf, "+DATA %d %08X\n", size, crc32_calc(0, data, size));
    _reply(buf);
    for (u32 i = 0; i < size; i += GET_LINE) {
        u32 n = size - i < GET_LINE ? size - i : GET_LINE, k = 0;
        buf[k++] = '=';
        for (u32 j = 0; j < n; j++) {
            buf[k++] = hex[data[i + j] >> 4];
            buf[k++] = hex[data[i + j] & 15];
        }
        buf[k++] = '\n';
        if (!gui_dbg_bulk((const u8 *)buf, k))
            break;
    }
    free(data);
    _reply("+END\n");
}

static void _run(void)
{
    char buf[96];
    if (!g_sd_ok || sd_mount()) {
        _reply("-ERR no SD card\n");
        return;
    }
    u32 size = 0;
    void *payload = sd_file_read("bootloader/payloads/hwtest.bin", &size);
    if (!payload || size < 0x100 || ALIGN(size, 0x10) > RUN_MAX) {
        free(payload);
        _reply("-ERR bootloader/payloads/hwtest.bin is missing or the wrong size\n");
        return;
    }
    s_printf(buf, "+OK starting hwtest.bin, %d bytes\n", size);
    _reply(buf);
    /* As the RAM test is handed over to (gui_ram.c): the USB controller
     * stopped, the boot clocks back, then the payload from IRAM. */
    gui_dbg_end();
    gui_clock_fast(false);
    ram_payload_run(payload, size);   /* does not return */
}

/* The Bluetooth test from the PC (gui_btscreen.c, gui_bt.c): the same
 * steps as on the screen, without the confirmation box. */
static void _bt(const char *a)
{
    static const char *const states[] = {
        "off", "failed", "idle", "scanning", "naming", "connecting",
        "securing", "channels", "setup", "ready",
    };
    static char buf[640];

    if (!strcmp(a, "OPEN")) {
        gui_show(gui_bt_test);
        _reply("+OK opening the Bluetooth screen\n");
        return;
    }
    if (!strcmp(a, "CLOSE")) {
        gui_show(gui_home);
        _reply("+OK\n");
        return;
    }
    gui_bt_state_t st = gui_bt_state();
    if (!strcmp(a, "STATE")) {
        const gui_bt_pad_state_t *pad = gui_bt_pad();
        u32 sent, done, rx, polls;
        gui_bt_acl_stats(&sent, &done);
        gui_bt_rx_stats(&rx, &polls);
        s_printf(buf, "+OK %s, step '%s', why '%s', device %d, buttons %06X, "
                      "reports %d (%d/s), simple %d, line errors %d, acl sent %d "
                      "done %d, rx %d bytes in %d polls, ms %d\n",
                 states[st], gui_bt_step(), gui_bt_why(), gui_bt_connected(),
                 pad->buttons, pad->reports, pad->rate, pad->simple,
                 gui_bt_line_errors(), sent, done, rx, polls, get_tmr_ms());
        _reply(buf);
        return;
    }
    if (st == BT_OFF || st == BT_FAILED) {
        _reply("-ERR the radio is off: BT OPEN first\n");
        return;
    }
    if (!strcmp(a, "SCAN")) {
        gui_bt_scan();
        _reply(gui_bt_state() == BT_SCANNING ? "+OK searching\n" : "-ERR busy\n");
    } else if (!strncmp(a, "CONNECT ", 8)) {
        const char *q = a + 8;
        _reply(gui_bt_connect(_parse_u32(&q, 10)) ? "+OK connecting\n"
                                                  : "-ERR no such device, or busy\n");
    } else if (!strcmp(a, "DISCONNECT")) {
        gui_bt_disconnect();
        _reply("+OK\n");
    } else if (!strcmp(a, "RAILPAIR")) {
        int n = gui_bt_rail_pair();
        if (n < 0)
            _reply("-ERR no Joy-Con on the rails\n");
        else {
            s_printf(buf, "+OK pairing mode asked of %d Joy-Con(s): slide them off\n", n);
            _reply(buf);
        }
    } else if (!strcmp(a, "LIST")) {
        s_printf(buf, "+OK %d found", gui_bt_dev_count());
        for (u32 i = 0; i < gui_bt_dev_count() && strlen(buf) < sizeof(buf) - 80; i++) {
            const gui_bt_dev_t *d = gui_bt_dev(i);
            s_printf(buf + strlen(buf), " | %d %02X:%02X:%02X:%02X:%02X:%02X %06X %d '%s'",
                     i, d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1],
                     d->addr[0], d->cod, d->rssi, d->name);
        }
        strcat(buf, "\n");
        _reply(buf);
    } else {
        _reply("-ERR unknown BT command\n");
    }
}

static void _command(const char *line)
{
    char buf[96];
    if (!strcmp(line, "PING")) {
        s_printf(buf, "+PONG hwtest GUI %s, console on %s\n", GUI_VERSION,
                 gui_dbg_name(gui_dbg_port()));
        _reply(buf);
    } else if (!strncmp(line, "PUT ", 4)) {
        _put(line + 4);
    } else if (!strncmp(line, "LS ", 3)) {
        _ls(line + 3);
    } else if (!strncmp(line, "GET ", 4)) {
        _get(line + 4);
    } else if (!strcmp(line, "RMLOGS")) {
        _rmlogs();
    } else if (!strcmp(line, "RUN")) {
        _run();
    } else if (!strncmp(line, "BT ", 3)) {
        _bt(line + 3);
    } else {
        _reply("-ERR unknown command\n");
    }
}

void gui_cmd_poll(void)
{
    u8 c;
    while (dbg_recv(&c)) {
        if (!s_in_line) {
            if (c == 'R')
                do_reboot();
            if (c == '!') {
                s_in_line = true;
                s_line_n = 0;
            }
            continue;     /* anything else is not for us */
        }
        if (c == '\r')
            continue;
        if (c == '\n') {
            s_line[s_line_n] = 0;
            s_in_line = false;
            _command(s_line);
            continue;
        }
        if (s_line_n < sizeof(s_line) - 1)
            s_line[s_line_n++] = c;
    }
}
