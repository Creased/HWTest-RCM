/*
 * hwtest GUI - Wi-Fi Scan: the CYW4356 running its real firmware, asked to
 * scan, and the networks it heard.
 *
 * The health check stops at enumeration: the chip's PCIe front end answers.
 * A scan exercises what an orange screen points at - the firmware download
 * over PCIe, the chip's ARM core, its DMA into console memory, the radio -
 * so a chip that only fails under load has a chance to fail here.
 *
 * Three stages, all reported on the left as they go:
 *
 *   1. the firmware and NVRAM out of HOS on the eMMC (gui_wlanfw.c), once
 *      per session: needs sd:/switch/prod.keys from Lockpick_RCM;
 *   2. the health check's PCIe bring-up on the BPMP, then CPU0 released
 *      into the scan image (cpu_wifi/, wifi_mbox.h) with the firmware and
 *      the NVRAM staged in DRAM;
 *   3. the BPMP watching the mailbox until CPU0 says done, stalls, or
 *      faults, then the cluster powergated again.
 *
 * Nothing is written to the eMMC or the SD card. WL_REG_ON is cold-cycled
 * by the bring-up, as on the Wi-Fi pages, and the radio is left off: the
 * cluster is powergated and the next bring-up resets the chip.
 */
#include "gui.h"
#include "gui_wlanfw.h"
#include "../wifi_mbox.h"

#ifdef HAVE_CPU_WIFI
extern const unsigned char cpu_wifi_bin[];
extern const unsigned int cpu_wifi_bin_size;
#endif

#define WIFI_TICK_MS     100
#define WIFI_STALL_MS    6000      /* neither heartbeat moved              */
#define WIFI_CEILING_MS  90000     /* the whole CPU0 run                   */
#define WIFI_SCAN_MS     12000
#define ROWS_MAX         WIFI_BSS_MAX
#define ROW_H            69
#define LIST_Y           (GUI_TOP + 52)

static wlanfw_t s_fw;
static bool     s_fw_ok;
static lv_obj_t *s_btn, *s_log, *s_title, *s_info, *s_page;
static lv_obj_t *s_row[ROWS_MAX], *s_row_name[ROWS_MAX], *s_row_sub[ROWS_MAX];
static lv_task_t *s_task;
static bool     s_running, s_clock_slow;
static u32      s_t0, s_last_move, s_hb, s_stub_hb, s_last_step;
static char     s_logbuf[2048];

static volatile wifi_mbox_t *const W = (volatile wifi_mbox_t *)WIFI_MBOX_ADDR;

static const char *s_step_name[] = {
    "", "starting", "PCIe enumeration (the health check's)",
    "BAR0 and BAR2 assigned", "ChipCommon chip ID", "backplane cores",
    "ARM CR4 halted", "chip watchdog reset", "RAM size",
    "firmware and NVRAM into the chip", "firmware starting",
    "shared RAM", "msgbuf rings", "first ioctls", "radio up", "scanning",
    "done"
};

static const char *_step_name(u32 s)
{
    return s < sizeof(s_step_name) / sizeof(s_step_name[0]) ? s_step_name[s] : "?";
}

/* ---- the left column's running log --------------------------------- */

static void _log(const char *col_hex, const char *icon, const char *text)
{
    char line[192];
    if (col_hex)
        s_printf(line, "#%s %s#  %s\n", col_hex, icon, text);
    else
        s_printf(line, "%s\n", text);
    u32 have = strlen(s_logbuf), add = strlen(line);
    if (have + add >= sizeof(s_logbuf)) {
        /* Keep the newest: drop the older half, from a line start. */
        char *p = s_logbuf + have / 2;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
        memmove(s_logbuf, p, strlen(p) + 1);
        if (strlen(s_logbuf) + add >= sizeof(s_logbuf))
            s_logbuf[0] = 0;
    }
    strcat(s_logbuf, line);
    lv_label_set_text(s_log, s_logbuf);
    char u[200];
    s_printf(u, "gui: wifi: %s\n", text);
    gui_uart_puts(u);
}

static void _extract_step(const char *line)
{
    _log(GUI_HEX_HINT, ICON_INFO, line);
    gui_refresh_now();
}

/* ---- the results ------------------------------------------------------ */

static void _sanitize(char *dst, const char *src, u32 max)
{
    u32 i;
    for (i = 0; i < max && src[i]; i++) {
        u8 c = (u8)src[i];
        dst[i] = (c < 0x20 || c >= 0x7F || c == '#') ? '?' : (char)c;
    }
    dst[i] = 0;
}

static void _show_results(void)
{
    u32 n = W->nbss < ROWS_MAX ? W->nbss : ROWS_MAX;
    u8 order[ROWS_MAX];
    for (u32 i = 0; i < n; i++)
        order[i] = i;
    for (u32 i = 1; i < n; i++)                    /* strongest first */
        for (u32 j = i; j > 0 && W->bss[order[j]].rssi > W->bss[order[j - 1]].rssi; j--) {
            u8 t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }

    char buf[160];
    for (u32 r = 0; r < ROWS_MAX; r++) {
        if (r >= n) {
            gui_hide(s_row[r], true);
            continue;
        }
        volatile wifi_bss_t *b = &W->bss[order[r]];
        char ssid[34];
        _sanitize(ssid, (const char *)b->ssid, b->ssid_len);
        s_printf(buf, "%s%s", ssid[0] ? ssid : "#" GUI_HEX_HINT " (hidden)#",
                 (b->flags & 1) ? "  #" GUI_HEX_HINT " " ICON_SHIELD "#" : "");
        lv_label_set_text(s_row_name[r], buf);
        const char *col = b->rssi >= -60 ? GUI_HEX_OK
                        : b->rssi >= -75 ? GUI_HEX_WARN : GUI_HEX_FAIL;
        s_printf(buf, "#%s %d dBm#   channel %d (%s GHz)   %02X:%02X:%02X:%02X:%02X:%02X",
                 col, b->rssi, b->channel, (b->flags & 2) ? "5" : "2.4",
                 b->bssid[0], b->bssid[1], b->bssid[2],
                 b->bssid[3], b->bssid[4], b->bssid[5]);
        lv_label_set_text(s_row_sub[r], buf);
        gui_hide(s_row[r], false);
        s_printf(buf, "gui: wifi: bss %d dBm ch %d %s\n", b->rssi, b->channel, ssid);
        gui_uart_puts(buf);
    }
}

static void _show_info(void)
{
    char buf[512];
    u32 cid = W->chipid;
    s_printf(buf,
        "Chip ID     %X rev %d (ChipCommon %08X)\n"
        "Firmware    %s\n"
        "MAC         %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Booted in   %d ms, scan %d ms, %d events",
        cid & 0xFFFF, (cid >> 16) & 0xF, cid,
        W->fw_ver[0] ? (const char *)W->fw_ver : s_fw.fw_tail,
        W->mac[0], W->mac[1], W->mac[2], W->mac[3], W->mac[4], W->mac[5],
        W->fw_up_ms, W->scan_ms, W->events);
    lv_label_set_text(s_info, buf);
}

/* The firmware's own log, last lines, when something went wrong. */
static void _show_console(void)
{
    u32 len = W->console_len < WIFI_CONSOLE_MAX ? W->console_len : 0;
    if (!len)
        return;
    const char *c = (const char *)W->console;
    u32 start = len, lines = 0;
    while (start > 0 && lines < 8)
        if (c[--start] == '\n' && start + 1 < len)
            lines++;
    char buf[640];
    u32 o = 0;
    for (u32 i = start; i < len && o < sizeof(buf) - 1; i++)
        buf[o++] = (c[i] == '#') ? '?' : c[i];
    buf[o] = 0;
    lv_label_set_text(s_info, buf);
    gui_uart_puts("gui: wifi: firmware console tail follows\n");
    gui_uart_puts(buf);
    gui_uart_puts("\n");
}

/* ---- stopping ------------------------------------------------------- */

static void _stop(void)
{
    if (s_running) {
        wifi_cpu0_stop();
        s_running = false;
    }
    if (s_clock_slow) {
        gui_clock_fast(true);
        s_clock_slow = false;
    }
    if (s_task) {
        lv_task_del(s_task);
        s_task = NULL;
    }
    gui_hide(s_btn, false);
}

static void _finish(void)
{
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);
    char line[200];
    if (W->result == WIFI_OK) {
        s_printf(line, "%d networks", W->nbss);
        _log(GUI_HEX_OK, ICON_OK, line);
        lv_label_set_text(s_title, "#" GUI_HEX_OK " " ICON_WIFI "#  The chip scanned");
        _show_results();
        _show_info();
        if (W->why[0])
            _log(GUI_HEX_WARN, ICON_WARN, (const char *)W->why);
    } else {
        s_printf(line, "stopped at: %s", _step_name(W->step));
        _log(GUI_HEX_FAIL, ICON_FAIL, line);
        _log(GUI_HEX_FAIL, ICON_FAIL, (const char *)W->why);
        lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No scan");
        _show_console();
        if (W->step >= WIFI_ST_CHIPID && W->chipid) {
            s_printf(line, "chip ID read %08X", W->chipid);
            _log(GUI_HEX_HINT, ICON_INFO, line);
        }
    }
    _stop();
}

static void _tick(void *param)
{
    (void)param;
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);
    volatile cpu_mbox_t *mb = (volatile cpu_mbox_t *)CPU_MBOX_ADDR;
    u32 now = get_tmr_ms();

    if (W->heartbeat != s_hb || mb->heartbeat != s_stub_hb) {
        s_hb = W->heartbeat;
        s_stub_hb = mb->heartbeat;
        s_last_move = now;
    }
    if (W->magic == WIFI_MBOX_MAGIC && W->step != s_last_step) {
        s_last_step = W->step;
        if (s_last_step > WIFI_ST_ENUM)
            _log(GUI_HEX_HINT, ICON_INFO, _step_name(s_last_step));
    }
    if (W->done == WIFI_MBOX_DONE) {
        _finish();
        return;
    }

    char line[160];
    if (mb->exc_taken == CPU_MBOX_EXC) {
        s_printf(line, "CPU0 fault at %s: ESR %08X FAR %08X",
                 W->magic == WIFI_MBOX_MAGIC ? _step_name(W->step) : "bring-up",
                 mb->exc_esr, mb->exc_far);
        _log(GUI_HEX_FAIL, ICON_FAIL, line);
        lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No scan");
        _stop();
        return;
    }
    if (now - s_last_move > WIFI_STALL_MS || now - s_t0 > WIFI_CEILING_MS) {
        if (W->magic == WIFI_MBOX_MAGIC)
            s_printf(line, "CPU0 stalled at: %s", _step_name(W->step));
        else
            s_printf(line, "CPU0 stalled in the bring-up, stub stage %d",
                     mb->stage);
        _log(GUI_HEX_FAIL, ICON_FAIL, line);
        lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No scan");
        _stop();
    }
}

/* ---- starting ------------------------------------------------------- */

static void _run(void)
{
    char why[128];
    s_logbuf[0] = 0;
    lv_label_set_text(s_log, "");
    lv_label_set_text(s_info, "");
    for (u32 r = 0; r < ROWS_MAX; r++)
        gui_hide(s_row[r], true);
    lv_label_set_text(s_title, "#" GUI_HEX_ACCENT " " ICON_WIFI "#  Scanning...");
    gui_hide(s_btn, true);
    gui_refresh_now();

#ifndef HAVE_CPU_WIFI
    _log(GUI_HEX_FAIL, ICON_FAIL, "this build has no CPU0 Wi-Fi image (no aarch64 compiler)");
    gui_hide(s_btn, false);
    return;
#else
    if (!s_fw_ok) {
        gui_input_hold(true);
        int r = wlanfw_extract(&s_fw, why, sizeof(why), _extract_step);
        gui_input_hold(false);
        if (r) {
            _log(GUI_HEX_FAIL, ICON_FAIL, why);
            lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No firmware");
            gui_hide(s_btn, false);
            return;
        }
        s_fw_ok = true;
    }
    char line[160];
    s_printf(line, "firmware from HOS: %d bytes, %s", s_fw.fw_len, s_fw.fw_tail);
    _log(GUI_HEX_OK, ICON_OK, line);

    /* Stage the inputs, the NVRAM in the chip's binary form. */
    u32 nvlen = wlanfw_nvram_binary(s_fw.nvram, s_fw.nvram_len,
                                    (u8 *)WIFI_NVRAM_ADDR, WIFI_NVRAM_MAX);
    if (!nvlen || s_fw.fw_len > WIFI_FW_MAX || cpu_wifi_bin_size > WIFI_IMG_MAX) {
        _log(GUI_HEX_FAIL, ICON_FAIL, "firmware, NVRAM or image too large to stage");
        gui_hide(s_btn, false);
        return;
    }
    memcpy((void *)WIFI_FW_ADDR, s_fw.fw, s_fw.fw_len);
    memset((void *)W, 0, sizeof(wifi_mbox_t));
    W->fw_len = s_fw.fw_len;
    W->nvram_len = nvlen;
    W->scan_time_ms = WIFI_SCAN_MS;
    W->in_magic = WIFI_MBOX_MAGIC;
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);

    _log(GUI_HEX_HINT, ICON_INFO, "PCIe bring-up and CPU0 start");
    gui_refresh_now();
    /* The payload's clocks, as for the health check: the bring-up derives
     * the UPHY management clock from PLLP. */
    gui_clock_fast(false);
    s_clock_slow = true;
    log_silence(true);
    const char *err = wifi_cpu0_launch(cpu_wifi_bin, cpu_wifi_bin_size,
                                       WIFI_IMG_ADDR);
    log_silence(false);
    if (err) {
        _log(GUI_HEX_FAIL, ICON_FAIL, err);
        lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No scan");
        _stop();
        return;
    }
    s_running = true;
    s_t0 = s_last_move = get_tmr_ms();
    s_hb = s_stub_hb = 0;
    s_last_step = 0;
    s_task = lv_task_create(_tick, WIFI_TICK_MS, LV_TASK_PRIO_MID, NULL);
#endif
}

static lv_res_t _act_scan(lv_obj_t *btn)
{
    (void)btn;
    if (!s_running)
        _run();
    return LV_RES_OK;
}

static void _leave(void)
{
    _stop();
}

void gui_wifi_scan(void)
{
    lv_obj_t *win = gui_win_new(ICON_WIFI " Wi-Fi Scan");
    gui_on_leave(_leave);
    s_btn = gui_win_btn(win, ICON_REFRESH " Scan", _act_scan);

    lv_obj_t *t = gui_label(win, &gui_font_30, GUI_COL_TEXT, "Steps");
    lv_obj_set_pos(t, GUI_MARGIN, GUI_TOP + 4);
    s_log = gui_hint(win, GUI_COL1_W, "");
    lv_obj_set_pos(s_log, GUI_MARGIN, LIST_Y);
    lv_label_set_text(s_log,
        "Boots the Wi-Fi chip with the firmware this console's HOS carries "
        "and asks it to scan. Needs sd:/switch/prod.keys (Lockpick_RCM) to "
        "read the firmware from the eMMC; nothing is written.\n\n"
        "Press Scan.");
    gui_divider(win, GUI_DIVIDER_X, GUI_TOP, GUI_CONTENT_H);

    s_title = gui_label(win, &gui_font_30, GUI_COL_TEXT,
                        "#" GUI_HEX_ACCENT " " ICON_WIFI "#  Networks");
    lv_obj_set_pos(s_title, GUI_COL2_X, GUI_TOP + 4);
    s_info = gui_label(win, &gui_font_mono, GUI_COL_HINT, "");
    lv_obj_set_pos(s_info, GUI_COL2_X, GUI_BOTTOM - 110);

    s_page = gui_page(win, GUI_COL2_X, LIST_Y, GUI_COL2_W,
                      GUI_BOTTOM - 120 - LIST_Y);
    lv_coord_t rw = GUI_COL2_W - GUI_PAGE_SB_W;
    for (u32 i = 0; i < ROWS_MAX; i++) {
        lv_obj_t *r = gui_focus_row(s_page, i * ROW_H, rw);
        lv_obj_set_height(r, ROW_H - 6);
        s_row_name[i] = gui_label(r, &gui_font_20, GUI_COL_TEXT, "");
        lv_obj_set_pos(s_row_name[i], 16, 6);
        s_row_sub[i] = gui_label(r, &gui_font_20, GUI_COL_HINT, "");
        lv_obj_set_pos(s_row_sub[i], 16, 34);
        s_row[i] = r;
        gui_hide(r, true);
    }
    s_running = false;
    s_task = NULL;
}
