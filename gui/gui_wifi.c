/*
 * hwtest GUI - Wi-Fi Scan: the CYW4356 running its real firmware, asked to
 * scan, and the networks it heard; picked from the list, joined.
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
 *      the NVRAM staged in DRAM, and the network to join if one was picked
 *      (or is first in hwtest/hwtest.ini);
 *   3. the BPMP watching the mailbox until CPU0 says done, stalls, or
 *      faults, then the cluster powergated again.
 *
 * Nothing is written to the eMMC or the SD card. WL_REG_ON is cold-cycled
 * by the bring-up, as on the Wi-Fi pages. Radio Off, in the header while
 * the radio is up, puts the chip, CPU0 and the PCIe root port back
 * (wifi_chip_stop, about 40 mA), and so does leaving the screen after a
 * run. Bluetooth is never touched.
 */
#include "gui.h"
#include "../dbgcfg.h"
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
static lv_obj_t *s_btn, *s_off, *s_log, *s_title, *s_info, *s_page;
static bool      s_chip_up;             /* powered since last put back   */
static lv_obj_t *s_row[ROWS_MAX], *s_row_name[ROWS_MAX], *s_row_sub[ROWS_MAX];
/* The networks as the rows show them. A copy, not the mailbox: the next
 * run clears that, and a join leaves the list on screen without scanning. */
static wifi_bss_t s_bss[ROWS_MAX];
/* The network picked on screen, and the passphrase typed for it. They
 * win over hwtest.ini's first network for the next run. */
static lv_obj_t *s_pw_bg, *s_pw_ta, *s_pw_kb;
static u8        s_pw_map;              /* 0 lower, 1 upper, 2 symbols   */
static char      s_pick_ssid[33], s_pick_psk[65];
static bool      s_pick_ready;
/* The connection card, over the page while a join runs and after it,
 * showing where it stopped. */
static lv_obj_t *s_cx_bg, *s_cx_card, *s_cx_bar, *s_cx_pct, *s_cx_note;
static lv_obj_t *s_cx_done;
#define CX_N 5
static lv_obj_t *s_cx_label[CX_N], *s_cx_detail[CX_N];
static bool      s_cx_up, s_cx_psk, s_cx_ran;
static lv_task_t *s_task;
static bool     s_running, s_clock_slow;
static u32      s_t0, s_last_move, s_hb, s_stub_hb, s_last_step;
static char     s_logbuf[2048];

static volatile wifi_mbox_t *const W = (volatile wifi_mbox_t *)WIFI_MBOX_ADDR;

static const char *s_step_name[] = {
    "", "starting", "PCIe enumeration",
    "BAR0 and BAR2 assigned", "ChipCommon chip ID", "backplane cores",
    "ARM CR4 halted", "chip watchdog reset", "RAM size",
    "firmware into the chip", "firmware starting",
    "shared RAM", "msgbuf rings", "first ioctls", "radio up", "scanning",
    "joining", "linked", "four-way handshake", "the network", "done"
};

static const char *_step_name(u32 s)
{
    return s < sizeof(s_step_name) / sizeof(s_step_name[0]) ? s_step_name[s] : "?";
}

/* ---- the left column's running log --------------------------------- */

/* The oldest line, gone. False when there is nothing left to drop. */
static bool _log_drop(void)
{
    char *nl = strchr(s_logbuf, '\n');
    if (!nl || !nl[1])
        return false;
    memmove(s_logbuf, nl + 1, strlen(nl + 1) + 1);
    return true;
}

static void _log(const char *col_hex, const char *icon, const char *text)
{
    char line[192];
    if (col_hex)
        s_printf(line, "#%s %s#  %s\n", col_hex, icon, text);
    else
        s_printf(line, "%s\n", text);
    u32 add = strlen(line);
    if (add >= sizeof(s_logbuf))
        return;
    while (strlen(s_logbuf) + add >= sizeof(s_logbuf) && _log_drop())
        ;
    strcat(s_logbuf, line);
    lv_label_set_text(s_log, s_logbuf);
    /* Lines wrap, so the height is only known once the text is set: drop
     * the oldest until the block ends above the status bar. */
    while (LIST_Y + lv_obj_get_height(s_log) > GUI_BOTTOM && _log_drop())
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

/* One UTF-8 character at src[i]: its code point in *cp, its length as the
 * return value. A byte that is not valid UTF-8 is one character, U+FFFD. */
static u32 _decode(const char *src, u32 max, u32 i, u32 *cp)
{
    u8 c = (u8)src[i];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    u32 n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
            (c & 0xF8) == 0xF0 ? 4 : 1;
    u32 v = n == 1 ? 0xFFFDu : (u32)(c & (0xFFu >> (n + 1)));
    for (u32 k = 1; k < n && i + k < max; k++) {
        u8 cc = (u8)src[i + k];
        if ((cc & 0xC0) != 0x80) {
            n = k;
            v = 0xFFFDu;
            break;
        }
        v = (v << 6) | (cc & 0x3F);
    }
    *cp = v;
    return n;
}

/* The SSID as it is, for the debug console: printable ASCII stands, and
 * everything else becomes \uXXXX (\UXXXXXXXX past the BMP), so a name the
 * screen can only fold - an emoji above all - is still identifiable in
 * the log, exactly, and can be matched against a phone's hotspot name. */
static void _escape(char *dst, const char *src, u32 max)
{
    static const char hex[] = "0123456789ABCDEF";
    u32 o = 0;
    for (u32 i = 0; i < max && src[i]; ) {
        u32 cp;
        i += _decode(src, max, i, &cp);
        if (cp >= 0x20 && cp < 0x7F && cp != '\\') {
            dst[o++] = (char)cp;
            continue;
        }
        dst[o++] = '\\';
        u32 digits = cp > 0xFFFF ? 8 : 4;
        dst[o++] = digits == 8 ? 'U' : 'u';
        for (u32 d = digits; d-- > 0; )
            dst[o++] = hex[(cp >> (4 * d)) & 0xF];
    }
    dst[o] = 0;
}

/* The SSID for a label. The fonts hold printable ASCII only
 * (tools/mkfont.py), and an SSID is arbitrary bytes, UTF-8 in practice:
 * each character becomes one - Latin letters their base letter, the
 * quotes, dashes and spaces a phone puts in a hotspot name their ASCII
 * form, anything else '?'. '#' would start a recolour command, so it goes
 * too. */
static void _sanitize(char *dst, const char *src, u32 max)
{
    u32 o = 0;
    for (u32 i = 0; i < max && src[i]; ) {
        u8 c = (u8)src[i];
        if (c < 0x20 || c == '#') {
            dst[o++] = '?';
            i++;
            continue;
        }
        if (c < 0x7F) {
            dst[o++] = (char)c;
            i++;
            continue;
        }
        u32 cp;
        u32 n = _decode(src, max, i, &cp);
        static const char lat1[] =            /* U+00C0 .. U+00FF */
            "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
            "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
        static const char lata[] =            /* U+0100 .. U+017F */
            "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh"
            "IiIiIiIiIi" "Ii" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnnNn"
            "OoOoOo" "Oo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu"
            "Ww" "YyY" "ZzZzZz" "s";
        char a = '?';
        if (cp >= 0x00C0 && cp <= 0x00FF)
            a = lat1[cp - 0x00C0];
        else if (cp >= 0x0100 && cp <= 0x017F)
            a = lata[cp - 0x0100];
        else switch (cp) {
        case 0x2018: case 0x2019: case 0x02BC: a = '\''; break;
        case 0x201C: case 0x201D:              a = '"';  break;
        case 0x2013: case 0x2014: case 0x2212: a = '-';  break;
        case 0x00A0: case 0x2007: case 0x202F: a = ' ';  break;
        case 0x00DF:                           a = 's';  break;
        default: break;
        }
        dst[o++] = a;
        i += n;
    }
    dst[o] = 0;
}

/* ---- the password keypad ---------------------------------------------
 *
 * LVGL's own keyboard (lv_kb) is compiled out here and its default map
 * wants the symbol font this GUI does not carry, so the keypad is a plain
 * button matrix over a one-line text area. "\202" and friends are the
 * matrix's width codes; LVGL hands the action the label without them. */
static const char *KB_LOWER[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","Del","\n",
    "ABC","z","x","c","v","b","n","m",".","-","\n",
    "#+=","\204Space","\202Cancel","\203Connect","" };
static const char *KB_UPPER[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","Del","\n",
    "abc","Z","X","C","V","B","N","M","_","+","\n",
    "#+=","\204Space","\202Cancel","\203Connect","" };
/* The corner key is the way back, where "#+=" is on the letter maps. */
static const char *KB_SYM[] = {
    "!","@","#","$","%","^","&","*","(",")","\n",
    "-","_","=","+","[","]","{","}",";",":","\n",
    "'","\"",",",".","<",">","/","?","~","`","\n",
    "ABC","\\","|","Del","\n",
    "abc","\204Space","\202Cancel","\203Connect","" };

/* The card the keypad sits on: the screen's width less the margins, tall
 * enough for a title, the one line being typed and five key rows, and
 * centred so neither console bar is covered. */
#define PW_W       GUI_CONTENT_W
#define PW_H       520
#define PW_X       ((GUI_W - PW_W) / 2)
#define PW_Y       ((GUI_H - PW_H) / 2)
#define PW_PAD     24
#define PW_KB_Y    (PW_PAD + 118)

static void _run(void);     /* the scan, which the keypad also starts */

/* The list as it was when a card went up, put back when it closes:
 * deleting the card hands the focus to some other row, and the page
 * scrolls after it. A touched row never takes the focus (the card freezes
 * the group first), so the row is passed in rather than read back. */
static lv_obj_t  *s_view_obj;
static lv_coord_t s_view_y;

static void _view_keep(lv_obj_t *row)
{
    s_view_obj = row ? row : lv_group_get_focused(gui_group());
    s_view_y = lv_obj_get_y(lv_page_get_scrl(s_page));
}

/* New results are a new list, already at the top: nothing to go back to. */
static void _view_forget(void)
{
    s_view_obj = NULL;
    s_view_y = 0;
}

static void _view_back(void)
{
    lv_obj_t *o = s_view_obj ? s_view_obj : s_btn;
    lv_coord_t y = s_view_y;
    _view_forget();
    if (o)
        lv_group_focus_obj(o);
    /* Focusing starts a 150 ms scroll, which would undo this one. */
    lv_obj_t *scrl = lv_page_get_scrl(s_page);
    lv_anim_del(scrl, (lv_anim_fp_t)lv_obj_set_y);
    lv_obj_set_y(scrl, y);
}

static void _pw_close(void)
{
    if (!s_pw_bg)
        return;
    lv_group_focus_freeze(gui_group(), false);
    gui_keypad_grab(false);
    lv_obj_del(s_pw_bg);
    s_pw_bg = s_pw_ta = s_pw_kb = NULL;
    _view_back();
}

/* LVGL keeps the selection as an index into the map, and the symbol map
 * is six keys shorter: switched, the index can point past its end and no
 * direction moves it. So a switch selects the mode key that switches
 * back, in the same place on every map - the first of the fourth row
 * (after three rows of ten), or the corner, first of the last row of
 * four. */
#define KB_ROW4   30

static void _pw_select(lv_obj_t *kb, bool corner)
{
    lv_btnm_ext_t *ext = lv_obj_get_ext_attr(kb);
    if (ext->btn_id_pr == LV_BTNM_PR_NONE)
        return;                 /* nothing was selected: leave it that way */
    ext->btn_id_pr = corner ? (u16)(ext->btn_cnt - 4) : KB_ROW4;
    lv_obj_invalidate(kb);
}

static lv_res_t _pw_key(lv_obj_t *kb, const char *txt)
{
    if (!strcmp(txt, "Cancel")) {
        _pw_close();
        return LV_RES_INV;
    }
    if (!strcmp(txt, "Connect")) {
        const char *t = lv_ta_get_text(s_pw_ta);
        u32 n = strlen(t);
        if (n > sizeof(s_pick_psk) - 1)
            n = sizeof(s_pick_psk) - 1;
        memcpy(s_pick_psk, t, n);
        s_pick_psk[n] = 0;
        s_pick_ready = true;
        _pw_close();
        if (!s_running)
            _run();
        return LV_RES_INV;
    }
    if (!strcmp(txt, "Del")) {
        lv_ta_del_char(s_pw_ta);
        return LV_RES_OK;
    }
    if (!strcmp(txt, "Space")) {
        lv_ta_add_char(s_pw_ta, ' ');
        return LV_RES_OK;
    }
    if (!strcmp(txt, "abc")) {
        bool corner = s_pw_map == 2;    /* the symbol map's "abc" is there */
        s_pw_map = 0;
        lv_btnm_set_map(kb, KB_LOWER);
        _pw_select(kb, corner);
        return LV_RES_OK;
    }
    if (!strcmp(txt, "ABC")) {
        s_pw_map = 1;
        lv_btnm_set_map(kb, KB_UPPER);
        _pw_select(kb, false);
        return LV_RES_OK;
    }
    if (!strcmp(txt, "#+=")) {
        s_pw_map = 2;
        lv_btnm_set_map(kb, KB_SYM);
        _pw_select(kb, true);
        return LV_RES_OK;
    }
    for (const char *c = txt; *c; c++)
        lv_ta_add_char(s_pw_ta, (u32)(u8)*c);
    return LV_RES_OK;
}

/* The chosen network's name is kept as the bytes the chip reported, not
 * as the screen's folded form: it is what goes back to the firmware. */
static void _pw_open(lv_obj_t *row, u32 r)
{
    const wifi_bss_t *b = &s_bss[r];
    u32 n = b->ssid_len > 32 ? 32 : b->ssid_len;
    memcpy(s_pick_ssid, (const void *)b->ssid, n);
    s_pick_ssid[n] = 0;

    _view_keep(row);
    s_pw_bg = gui_dim();
    lv_obj_t *card = gui_panel(s_pw_bg, PW_X, PW_Y, PW_W, PW_H);

    char name[40], line[80];
    _sanitize(name, (const char *)b->ssid, b->ssid_len);
    s_printf(line, "Passphrase for %s", name[0] ? name : "(hidden)");
    lv_obj_t *t = gui_label(card, &gui_font_30, GUI_COL_TEXT, line);
    lv_obj_set_pos(t, PW_PAD, PW_PAD);

    /* A passphrase saved in hwtest/hwtest.ini goes into the field. */
    dbg_cfg_t cfg;
    dbg_cfg_read(&cfg);
    const char *saved = dbg_cfg_psk(&cfg, s_pick_ssid);

    s_pw_ta = lv_ta_create(card, NULL);
    lv_ta_set_one_line(s_pw_ta, true);
    lv_ta_set_text(s_pw_ta, saved ? saved : "");
    lv_obj_set_width(s_pw_ta, PW_W - 2 * PW_PAD);
    lv_obj_set_pos(s_pw_ta, PW_PAD, PW_PAD + 50);

    s_pw_map = 0;
    s_pw_kb = lv_btnm_create(card, NULL);
    lv_btnm_set_map(s_pw_kb, KB_LOWER);
    lv_btnm_set_action(s_pw_kb, _pw_key);
    lv_obj_set_size(s_pw_kb, PW_W - 2 * PW_PAD, PW_H - PW_KB_Y - PW_PAD);
    lv_obj_set_pos(s_pw_kb, PW_PAD, PW_KB_Y);

    /* The matrix takes the directions itself (it walks its own keys), so
     * the row move in gui_main.c stands aside while it is open. */
    lv_group_add_obj(gui_group(), s_pw_kb);
    lv_group_focus_obj(s_pw_kb);
    lv_group_focus_freeze(gui_group(), true);
    gui_keypad_grab(true);

    /* With it there, Connect (the last key) is selected: one press joins. */
    if (saved) {
        lv_btnm_ext_t *ext = lv_obj_get_ext_attr(s_pw_kb);
        ext->btn_id_pr = (u16)(ext->btn_cnt - 1);
        _log(GUI_HEX_HINT, ICON_INFO, "passphrase from hwtest.ini");
    }
}

static lv_res_t _act_row(lv_obj_t *row)
{
    for (u32 r = 0; r < ROWS_MAX; r++)
        if (s_row[r] == row) {
            if (!s_running && !s_pw_bg)
                _pw_open(row, r);
            break;
        }
    return LV_RES_OK;
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
        memcpy(&s_bss[r], (const void *)&W->bss[order[r]], sizeof(s_bss[r]));
        const wifi_bss_t *b = &s_bss[r];
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
        char raw[200];
        _escape(raw, (const char *)b->ssid, b->ssid_len);
        s_printf(buf, "gui: wifi: bss %d dBm ch %d %s\n", b->rssi, b->channel,
                 raw);
        gui_uart_puts(buf);
    }

    /* The scrollable ends at the last result, so scrolling does not run
     * on into the rows that are hidden. */
    lv_obj_set_height(lv_page_get_scrl(s_page), (n ? n : 1) * ROW_H);

    /* Back to the top: the focus may have scrolled it among hidden rows. */
    lv_obj_set_y(lv_page_get_scrl(s_page), 0);
    _view_forget();
}

/* The block grows a line when a handshake ran and two when the firmware
 * log is shown, so it hangs from the bottom of the content rather than
 * starting at a fixed line and running into the bar below. */
static void _info_set(const char *text)
{
    lv_label_set_text(s_info, text);
    lv_coord_t y = GUI_BOTTOM - lv_obj_get_height(s_info);
    lv_obj_set_y(s_info, y);
    /* And the list above gives way to it, in whole rows. */
    lv_coord_t h = y - 10 - LIST_Y;
    h -= h % ROW_H;
    if (h < ROW_H)
        h = ROW_H;
    lv_obj_set_height(s_page, h);
}

static void _show_info(void)
{
    char buf[512], keys[80];
    u32 cid = W->chipid;
    /* What the handshake saw, when one was asked for: a firmware that
     * keeps the AP's key frames to itself shows frames but no EAPOL. */
    keys[0] = 0;
    if (W->sw_sup)
        s_printf(keys, "; keys %d in, %d out, %d frames up, %d posted",
                 W->rx_eapol, W->tx_eapol, W->rx_frames, W->rx_posted);
    s_printf(buf,
        "Chip ID     %X rev %d (ChipCommon %08X)\n"
        "Firmware    %s\n"
        "MAC         %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Booted in   %d ms, scan %d ms, %d events%s",
        cid & 0xFFFF, (cid >> 16) & 0xF, cid,
        W->fw_ver[0] ? (const char *)W->fw_ver : s_fw.fw_tail,
        W->mac[0], W->mac[1], W->mac[2], W->mac[3], W->mac[4], W->mac[5],
        W->fw_up_ms, W->scan_ms, W->events,
        W->link_up ? (W->keyed ? "\nLinked      associated and keyed"
                               : "\nLinked      associated (open)") : "");
    if (keys[0] && W->link_up)
        strcat(buf, keys);
    else if (keys[0])
        strcat(buf, "\nKey frames  none: the link never came up");
    _info_set(buf);
}

/* The firmware's own log, last lines, when something went wrong. */
static void _show_console(void)
{
    u32 len = W->console_len < WIFI_CONSOLE_MAX ? W->console_len : 0;
    if (!len)
        return;
    const char *c = (const char *)W->console;
    u32 start = len, lines = 0;
    while (start > 0 && lines < 5)      /* the lines wrap */
        if (c[--start] == '\n' && start + 1 < len)
            lines++;
    char buf[640];
    u32 o = 0;
    for (u32 i = start; i < len && o < sizeof(buf) - 1; i++)
        buf[o++] = (c[i] == '#') ? '?' : c[i];
    buf[o] = 0;
    _info_set(buf);
    gui_uart_puts("gui: wifi: firmware console tail follows\n");
    gui_uart_puts(buf);
    gui_uart_puts("\n");
}

/* ---- the connection card ---------------------------------------------
 *
 * A join's five stages under a bar, each pending, running, done, failed
 * or not needed on this network. The last two are the IP stack's - an
 * address, then an echo off the internet - which is what tells a chip
 * that associated from one that carries data. */

#define CX_W      GUI_CONTENT_W
#define CX_H      524
#define CX_X      ((GUI_W - CX_W) / 2)
#define CX_Y      ((GUI_H - CX_H) / 2)
#define CX_PAD    28
#define CX_ROW0   (CX_PAD + 150)
#define CX_ROW_H  56

/* Where each stage's detail column starts, so the five read as a table. */
#define CX_DET_X  (CX_PAD + 470)

enum { CX_PEND, CX_RUN, CX_DONE, CX_FAIL, CX_SKIP };

static const char *CX_NAME[CX_N] = {
    "Starting the chip",
    "Associating",
    "Key handshake",
    "Getting an address",
    "Reaching the internet",
};

/* How far the run got, as a stage each. A stage is running when the one
 * before it is done and this one is not, which keeps the five in step
 * with the mailbox without a second state machine to go stale. */
static u32 _cx_reached(void)
{
    if (W->magic != WIFI_MBOX_MAGIC)
        return 0;
    u32 n = 0;
    if (W->step >= WIFI_ST_JOIN || W->link_up)
        n = 1;
    if (W->link_up)
        n = 2;
    if (!s_cx_psk || W->keyed)
        n = (n >= 2) ? 3 : n;
    if (W->net_step >= WIFI_NET_BOUND)
        n = 4;
    if (W->net_step >= WIFI_NET_REACHED)
        n = 5;
    return n;
}

static void _cx_detail(u32 i, char *out)
{
    out[0] = 0;
    switch (i) {
    case 1:
        if (W->link_up)
            s_printf(out, "%02X:%02X:%02X:%02X:%02X:%02X, %d ms",
                     W->join_bssid[0], W->join_bssid[1], W->join_bssid[2],
                     W->join_bssid[3], W->join_bssid[4], W->join_bssid[5],
                     W->join_ms);
        break;
    case 2:
        if (!s_cx_psk)
            s_printf(out, "open network, none needed");
        else if (W->sw_sup)
            s_printf(out, "%d key frames in, %d out", W->rx_eapol,
                     W->tx_eapol);
        break;
    case 3:
        if (W->ip_addr)
            s_printf(out, "%d.%d.%d.%d, %d ms",
                     (W->ip_addr >> 24) & 0xFF, (W->ip_addr >> 16) & 0xFF,
                     (W->ip_addr >> 8) & 0xFF, W->ip_addr & 0xFF,
                     W->dhcp_ms);
        break;
    case 4:
        if (W->ping_sent)
            s_printf(out, "%d.%d.%d.%d: %d of %d, %d ms",
                     (W->ping_target >> 24) & 0xFF,
                     (W->ping_target >> 16) & 0xFF,
                     (W->ping_target >> 8) & 0xFF, W->ping_target & 0xFF,
                     W->ping_recv, W->ping_sent, W->ping_ms);
        break;
    default:
        break;
    }
}

static void _cx_update(void)
{
    if (!s_cx_up)
        return;
    u32 reached = _cx_reached();
    if (s_running)
        s_cx_ran = true;
    /* The card goes up before CPU0 is released, so "not running" only
     * means finished once it has been running. */
    bool over = (W->done == WIFI_MBOX_DONE) || (s_cx_ran && !s_running);

    for (u32 i = 0; i < CX_N; i++) {
        u32 st;
        if (i == 2 && !s_cx_psk)
            st = CX_SKIP;
        else if (i < reached)
            st = CX_DONE;
        else if (i == reached)
            st = over ? CX_FAIL : CX_RUN;
        else
            st = CX_PEND;

        const char *icon, *hex;
        switch (st) {
        case CX_DONE: icon = ICON_OK;      hex = GUI_HEX_OK;     break;
        case CX_FAIL: icon = ICON_FAIL;    hex = GUI_HEX_FAIL;   break;
        case CX_RUN:  icon = ICON_RIGHT;   hex = GUI_HEX_ACCENT; break;
        case CX_SKIP: icon = ICON_INFO;    hex = GUI_HEX_HINT;   break;
        default:      icon = ICON_UNKNOWN; hex = GUI_HEX_HINT;  break;
        }
        char buf[96];
        s_printf(buf, "#%s %s#  #%s %s#", hex, icon,
                 st == CX_PEND ? GUI_HEX_HINT : GUI_HEX_TEXT, CX_NAME[i]);
        lv_label_set_text(s_cx_label[i], buf);

        char det[96], out[128];
        _cx_detail(i, det);
        s_printf(out, "#%s %s#", st == CX_FAIL ? GUI_HEX_FAIL : GUI_HEX_HINT,
                 det);
        lv_label_set_text(s_cx_detail[i], det[0] ? out : "");
    }

    lv_bar_set_value(s_cx_bar, (int)(reached * 100 / CX_N));
    char buf[64];
    s_printf(buf, "%d%%", reached * 100 / CX_N);
    lv_label_set_text(s_cx_pct, buf);

    if (over) {
        if (reached == CX_N)
            s_printf(buf, "#" GUI_HEX_OK " Connected.#");
        else if (W->link_up && W->net_why[0])
            s_printf(buf, "#" GUI_HEX_FAIL " %s#", (const char *)W->net_why);
        else if (W->why[0])
            s_printf(buf, "#" GUI_HEX_FAIL " %s#", (const char *)W->why);
        else
            s_printf(buf, "#" GUI_HEX_FAIL " Stopped at %s.#",
                     _step_name(W->step));
        lv_label_set_text(s_cx_note, buf);
        gui_hide(s_cx_done, false);
    }
}

static void _cx_close(void)
{
    if (!s_cx_up)
        return;
    s_cx_up = false;
    lv_group_focus_freeze(gui_group(), false);
    lv_obj_del(s_cx_bg);
    s_cx_bg = NULL;
    _view_back();
}

static lv_res_t _cx_act_done(lv_obj_t *btn)
{
    (void)btn;
    _cx_close();
    return LV_RES_OK;
}

static void _cx_open(const char *ssid, bool psk)
{
    s_cx_psk = psk;
    _view_keep(NULL);
    s_cx_bg = gui_dim();
    s_cx_card = gui_panel(s_cx_bg, CX_X, CX_Y, CX_W, CX_H);

    char line[80];
    s_printf(line, "Connecting to %s", ssid && ssid[0] ? ssid : "(hidden)");
    lv_obj_t *t = gui_label(s_cx_card, &gui_font_30, GUI_COL_TEXT, line);
    lv_obj_set_pos(t, CX_PAD, CX_PAD);

    s_cx_bar = lv_bar_create(s_cx_card, NULL);
    lv_obj_set_size(s_cx_bar, CX_W - 2 * CX_PAD - 120, 30);
    lv_obj_set_pos(s_cx_bar, CX_PAD, CX_PAD + 54);
    lv_bar_set_range(s_cx_bar, 0, 100);
    lv_bar_set_value(s_cx_bar, 0);

    s_cx_pct = gui_label(s_cx_card, &gui_font_30, GUI_COL_ACCENT, "0%");
    lv_obj_align(s_cx_pct, s_cx_bar, LV_ALIGN_OUT_RIGHT_MID, 24, 0);

    s_cx_note = gui_hint(s_cx_card, CX_W - 2 * CX_PAD,
                         "This takes about half a minute: the chip is reset"
                         " and its firmware reloaded every run.");
    lv_obj_set_pos(s_cx_note, CX_PAD, CX_PAD + 100);

    for (u32 i = 0; i < CX_N; i++) {
        s_cx_label[i] = gui_label(s_cx_card, &gui_font_30, GUI_COL_TEXT, "");
        lv_obj_set_pos(s_cx_label[i], CX_PAD, CX_ROW0 + (lv_coord_t)i * CX_ROW_H);
        s_cx_detail[i] = gui_label(s_cx_card, &gui_font_20, GUI_COL_HINT, "");
        lv_obj_set_pos(s_cx_detail[i], CX_DET_X,
                       CX_ROW0 + (lv_coord_t)i * CX_ROW_H + 5);
    }

    s_cx_done = gui_button(s_cx_card, ICON_LEFT " Back to the list", _cx_act_done);
    lv_obj_align(s_cx_done, NULL, LV_ALIGN_IN_BOTTOM_RIGHT, -CX_PAD, -CX_PAD);
    gui_hide(s_cx_done, true);

    lv_group_focus_freeze(gui_group(), true);
    s_cx_up = true;
    s_cx_ran = false;
    _cx_update();
}

/* ---- stopping ------------------------------------------------------- */

/* Radio Off is in the header only while there is a radio to turn off. */
static void _off_sync(void)
{
    if (s_off)
        gui_hide(s_off, !s_chip_up);
}

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
        if (W->join_req) {
            lv_label_set_text(s_title, W->link_up
                ? "#" GUI_HEX_OK " " ICON_WIFI "#  Connected"
                : "#" GUI_HEX_FAIL " " ICON_WIFI "#  Not connected");
        } else {
            s_printf(line, "%d networks", W->nbss);
            _log(GUI_HEX_OK, ICON_OK, line);
            lv_label_set_text(s_title,
                "#" GUI_HEX_OK " " ICON_WIFI "#  The chip scanned");
            _show_results();
        }
        _show_info();
        if (W->why[0])
            _log(GUI_HEX_WARN, ICON_WARN, (const char *)W->why);
    } else {
        s_printf(line, "stopped: %s", _step_name(W->step));
        _log(GUI_HEX_FAIL, ICON_FAIL, line);
        _log(GUI_HEX_FAIL, ICON_FAIL, (const char *)W->why);
        lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No scan");
        /* A handshake that stopped: what the rings carried, for whoever is
         * debugging it. The console only - the step column is too narrow. */
        if (W->sw_sup) {
            s_printf(line, "gui: wifi: keys %d in, %d out, %d frames up, "
                     "%d posted; d2h ctrl %d/%d tx %d/%d rx %d/%d\n",
                     W->rx_eapol, W->tx_eapol, W->rx_frames, W->rx_posted,
                     W->ring_idx[0], W->ring_idx[1], W->ring_idx[2],
                     W->ring_idx[3], W->ring_idx[4], W->ring_idx[5]);
            gui_uart_puts(line);
            s_printf(line, "gui: wifi: join rc wsec %d auth %d sup %d pmk %d "
                     "sup0 %d; h2d ctrl %d/%d rxpost %d/%d max %d\n",
                     W->join_rc[0], W->join_rc[1], W->join_rc[2],
                     W->join_rc[3], W->join_rc[4], W->ring_idx[6],
                     W->ring_idx[7], W->ring_idx[8], W->ring_idx[9],
                     W->max_rxbufpost);
            gui_uart_puts(line);
        }
        _show_console();
        if (W->step >= WIFI_ST_CHIPID && W->chipid) {
            s_printf(line, "chip ID read %08X", W->chipid);
            _log(GUI_HEX_HINT, ICON_INFO, line);
        }
    }
    _stop();
    _cx_update();
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
    _cx_update();
    if (W->done == WIFI_MBOX_DONE) {
        _finish();
        return;
    }

    char line[160];
    if (mb->exc_taken == CPU_MBOX_EXC) {
        /* Two lines: the name and the registers do not fit the column
         * side by side. The step name is only meaningful once the image
         * has claimed the mailbox. */
        s_printf(line, "CPU0 fault at %s",
                 W->magic == WIFI_MBOX_MAGIC ? _step_name(W->step) : "bring-up");
        _log(GUI_HEX_FAIL, ICON_FAIL, line);
        s_printf(line, "ESR %08X FAR %08X", mb->exc_esr, mb->exc_far);
        _log(GUI_HEX_FAIL, ICON_FAIL, line);
        lv_label_set_text(s_title, "#" GUI_HEX_FAIL " " ICON_WIFI "#  No scan");
        _stop();
        return;
    }
    if (now - s_last_move > WIFI_STALL_MS || now - s_t0 > WIFI_CEILING_MS) {
        if (W->magic == WIFI_MBOX_MAGIC)
            s_printf(line, "stalled: %s", _step_name(W->step));
        else
            s_printf(line, "bring-up stalled, stage %d",
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
    _cx_close();
    s_logbuf[0] = 0;
    lv_label_set_text(s_log, "");
    _info_set("");
    lv_label_set_text(s_title,
        "#" GUI_HEX_ACCENT " " ICON_WIFI "#  Scanning...");
    gui_hide(s_btn, true);
    gui_refresh_now();

#ifndef HAVE_CPU_WIFI
    _log(GUI_HEX_FAIL, ICON_FAIL, "no CPU0 Wi-Fi image in build");
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
    s_printf(line, "firmware: %d bytes", s_fw.fw_len);
    _log(GUI_HEX_OK, ICON_OK, line);

    /* Stage the inputs, the NVRAM in the chip's binary form. */
    u32 nvlen = wlanfw_nvram_binary(s_fw.nvram, s_fw.nvram_len,
                                    (u8 *)WIFI_NVRAM_ADDR, WIFI_NVRAM_MAX);
    if (!nvlen || s_fw.fw_len > WIFI_FW_MAX || cpu_wifi_bin_size > WIFI_IMG_MAX) {
        _log(GUI_HEX_FAIL, ICON_FAIL, nvlen ? "firmware too large to stage"
                                            : "NVRAM unusable");
        gui_hide(s_btn, false);
        return;
    }
    memcpy((void *)WIFI_FW_ADDR, s_fw.fw, s_fw.fw_len);
    memset((void *)W, 0, sizeof(wifi_mbox_t));
    W->fw_len = s_fw.fw_len;
    W->nvram_len = nvlen;
    W->scan_time_ms = WIFI_SCAN_MS;
    /* A network to join: the one picked on screen, else hwtest.ini's
     * first. The scan proves the receive path; a join proves the rest. */
    if (s_pick_ready) {
        W->join_req = 1;
        memcpy((void *)W->join_ssid, s_pick_ssid, sizeof(W->join_ssid));
        memcpy((void *)W->join_psk, s_pick_psk, sizeof(W->join_psk));
    } else {
        dbg_cfg_t cfg;
        dbg_cfg_read(&cfg);
        W->join_req = cfg.wifi_n && cfg.wifi[0].ssid[0] ? 1 : 0;
        memcpy((void *)W->join_ssid, cfg.wifi[0].ssid, sizeof(W->join_ssid));
        memcpy((void *)W->join_psk, cfg.wifi[0].psk, sizeof(W->join_psk));
    }
    W->in_magic = WIFI_MBOX_MAGIC;
    bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLN_INV_WAY, false);

    /* A join gets the card over the list; a scan redraws the list. */
    if (W->join_req) {
        lv_label_set_text(s_title,
            "#" GUI_HEX_ACCENT " " ICON_WIFI "#  Connecting...");
        _cx_open((const char *)W->join_ssid, W->join_psk[0] != 0);
    } else {
        for (u32 r = 0; r < ROWS_MAX; r++)
            gui_hide(s_row[r], true);
    }

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
    /* Whether or not CPU0 got away, the bring-up powered the chip. */
    s_chip_up = true;
    _off_sync();
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

/* The chip off, once a test is done with it, or a run going nowhere. */
static lv_res_t _act_off(lv_obj_t *btn)
{
    (void)btn;
    _cx_close();
    _stop();
    wifi_chip_stop();
    s_chip_up = false;
    _off_sync();                /* nothing left for it to turn off */
    lv_label_set_text(s_title, "#" GUI_HEX_HINT " " ICON_WIFI "#  Radio off");
    _log(GUI_HEX_OK, ICON_OK, "radio off: WLAN back in reset");
    return LV_RES_OK;
}

static lv_res_t _act_scan(lv_obj_t *btn)
{
    (void)btn;
    /* Scan means scan: a picked network is for the run its card started. */
    s_pick_ready = false;
    if (!s_running)
        _run();
    return LV_RES_OK;
}

static void _leave(void)
{
    s_pick_ready = false;
    _pw_close();
    _cx_close();
    _stop();
    /* What this visit powered, it puts back. */
    if (s_chip_up) {
        /* Before, not after: the USB console restarts across a screen
         * change and can lose a line written in the gap. */
        gui_uart_puts("gui: wifi: radio off, leaving the screen\n");
        wifi_chip_stop();
        s_chip_up = false;
    }
}

void gui_wifi_scan(void)
{
    lv_obj_t *win = gui_win_new(ICON_WIFI " Wi-Fi Scan");
    gui_on_leave(_leave);
    s_btn = gui_win_btn(win, ICON_REFRESH " Scan", _act_scan);
    s_off = gui_win_btn(win, ICON_POWER " Radio Off", _act_off);
    _off_sync();                /* nothing is running yet */

    s_logbuf[0] = 0;        /* the last visit's steps are not this one's */
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
    /* Wrapped: the firmware's version string is long and free-form. */
    s_info = gui_label_wrap(win, &gui_font_mono, GUI_COL_HINT, GUI_COL2_W, "");
    lv_obj_set_pos(s_info, GUI_COL2_X, GUI_BOTTOM - 110);

    s_page = gui_page(win, GUI_COL2_X, LIST_Y, GUI_COL2_W,
                      GUI_BOTTOM - 120 - LIST_Y);
    lv_coord_t rw = GUI_COL2_W - GUI_PAGE_SB_W;
    /* Fit off: LVGL 5 fits a scrollable by shifting every child as each
     * row is added, and 64 rows at fixed y overflow lv_coord_t. Sized
     * here, and again in _show_results(). */
    lv_obj_t *scrl = lv_page_get_scrl(s_page);
    lv_cont_set_fit(scrl, false, false);
    lv_obj_set_size(scrl, rw, ROWS_MAX * ROW_H);
    for (u32 i = 0; i < ROWS_MAX; i++) {
        lv_obj_t *r = gui_list_row(s_page, i * ROW_H, rw, ROW_H - 6, _act_row);
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
