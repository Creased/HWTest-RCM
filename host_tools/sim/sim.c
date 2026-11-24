/*
 * The GUI's screens on the host: built from the same sources the console
 * build compiles, drawn by the same LVGL, and checked and photographed
 * here instead of over USB.
 *
 * What it is for: the layout. A screen that draws its text through its own
 * list, a block that grows past the status bar, a keypad whose selection
 * lands off the end of its map - all of that is in the screen code, not in
 * the hardware, and finding it needed a console, an SD card and a photo.
 * This builds the same widget tree in this process, renders it to a PNG,
 * and asserts the things a photograph is read for: what lies inside what,
 * what overlaps, where the focus goes.
 *
 * It is not the console. The hardware is stubbed
 * (host_tools/sim/sim_stubs.c): no touch panel, no Joy-Con, no chip, and
 * the VIC that rotates the finished frame onto the panel does nothing.
 * Presses are injected the way the USB console injects them, and the Wi-Fi
 * mailbox is filled in by hand. Everything above that is the console's own:
 * LVGL's pool and the drawing buffer are mapped at the addresses the
 * console gives them, the display is set up by gui_main.c's own
 * _display_init(), and the screens draw into the framebuffer in place, as
 * they do on the panel. The PNG is that framebuffer.
 *
 *   host_tools/sim/build.sh && build-sim/sim [out-dir]
 *
 * Scenarios are the screens; each writes <out-dir>/<name>.png and prints
 * its checks. Exit status is non-zero if any check failed.
 */
/* The screen code itself, statics and all: the harness drives the same
 * functions the screen's own buttons do, and reads the objects they made.
 * Both files in one unit, so nothing has to be exported for the test. */
#include "../../gui/gui_main.c"
#include "../../gui/gui_wifi.c"

_Static_assert(sizeof(wifi_mbox_t) <= 0x10000, "the mailbox fits sim_mbox_base");

static wifi_mbox_t *const MB = (wifi_mbox_t *)sim_mbox_base;

/* ---- the picture ---------------------------------------------------- */

/* The picture: the panel's framebuffer, drawn into in place. */
static const u32 *const s_fb = (const u32 *)NYX_FB2_ADDRESS;

/* ---- the loop ------------------------------------------------------- */

/* The console's main loop, with the clock moved on by hand: one pass is one
 * frame, which is also one LVGL input read. */
static void _frames(u32 n)
{
    while (n--) {
        sim_us += LV_REFR_PERIOD * 1000;
        lv_task_handler();
        _present();
    }
}

static void _key(u32 k)      { gui_inject_key(k, 3); _frames(5); }
static void _tap(u32 x, u32 y) { gui_inject_tap(x, y, 3); _frames(5); }

/* ---- checks --------------------------------------------------------- */

static u32 s_fails, s_checks;
static const char *s_scene = "";

/* A check is a line on stdout and a bit of the exit status. A macro, not a
 * function: the host's vsnprintf() and this file's headers do not mix, so
 * the format arguments go straight to sim_say(). */
#define _chk(cond, ...) do {                        \
        s_checks++;                                 \
        if (cond) {                                 \
            sim_say("ok    %s: ", s_scene);         \
        } else {                                    \
            s_fails++;                              \
            sim_say("FAIL  %s: ", s_scene);         \
        }                                           \
        sim_say(__VA_ARGS__);                       \
        sim_say("\n");                              \
    } while (0)

static lv_area_t _box(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a;
}

/* Two objects that must not share a pixel. */
static void _chk_apart(lv_obj_t *a, lv_obj_t *b, const char *what)
{
    lv_area_t x = _box(a), y = _box(b);
    bool over = x.x1 <= y.x2 && y.x1 <= x.x2 && x.y1 <= y.y2 && y.y1 <= x.y2;
    _chk(!over, "%s do not overlap (%d,%d..%d,%d and %d,%d..%d,%d)", what,
         x.x1, x.y1, x.x2, x.y2, y.x1, y.y1, y.x2, y.y2);
}

/* Inside the content grid: the margins on both sides, the bars above and
 * below. Everything a screen puts on itself lives here. */
static void _chk_grid(lv_obj_t *o, const char *what)
{
    lv_area_t a = _box(o);
    _chk(a.x1 >= GUI_MARGIN && a.x2 <= GUI_RIGHT,
         "%s stays between the margins (%d..%d)", what, a.x1, a.x2);
    _chk(a.y1 >= GUI_BAR_H && a.y2 <= GUI_H - GUI_BAR_H,
         "%s stays clear of the bars (%d..%d)", what, a.y1, a.y2);
}

/* A label must not be wider than the box that holds it: that is the shape
 * of a long free-form string running off its column. */
static void _chk_in_parent(lv_obj_t *o, const char *what)
{
    lv_obj_t *par = lv_obj_get_parent(o);
    lv_area_t a = _box(o), p = _box(par);
    _chk(a.x1 >= p.x1 && a.x2 <= p.x2,
         "%s stays inside its box across (%d..%d in %d..%d)", what,
         a.x1, a.x2, p.x1, p.x2);
}

/* ---- a mailbox a screen can be drawn from --------------------------- */

static void _mb_scan(u32 nbss)
{
    static const char *names[] = {
        "Livebox-1A2B", "test", "FreeWifi_secure", "SFR_1A2B",
        "Bbox-7788", "iPhone de Camille", "eduroam", "Pixel_4a2f",
    };
    memset(MB, 0, sizeof(*MB));
    MB->magic = WIFI_MBOX_MAGIC;
    MB->result = WIFI_OK;
    MB->step = WIFI_ST_DONE;
    MB->chipid = 0x4356A204;
    MB->fw_up_ms = 412;
    MB->scan_ms = 2104;
    MB->events = 37;
    strcpy(MB->fw_ver, "wl0: Nov 20 2026 11:02:43 version 13.10.197.84 "
                       "(r853893 CY) FWID 01-4b4e4f54");
    static const u8 mac[6] = { 0x98, 0xB6, 0xE9, 0x11, 0x22, 0x33 };
    memcpy(MB->mac, mac, 6);
    MB->nbss = nbss;
    for (u32 i = 0; i < nbss && i < WIFI_BSS_MAX; i++) {
        wifi_bss_t *b = &MB->bss[i];
        const char *n = names[i % (sizeof(names) / sizeof(names[0]))];
        b->ssid_len = (u8)strlen(n);
        memcpy(b->ssid, n, b->ssid_len);
        b->rssi = (short)(-38 - (int)i * 7);
        b->channel = (u8)(1 + (i * 5) % 11);
        b->flags = (u8)((i % 3 ? 1 : 0) | (i % 2 ? 2 : 0));
        for (u32 k = 0; k < 6; k++)
            b->bssid[k] = (u8)(0x5E + i * 0x11 + k);
    }
}

/* The same, with the join the screen reports when a network was picked and
 * a passphrase typed: this is the state that drew its details through the
 * list. */
static void _mb_join(void)
{
    _mb_scan(8);
    MB->join_req = 1;
    MB->link_up = 1;
    MB->keyed = 1;
    MB->join_ms = 1840;
    MB->sw_sup = 1;
    MB->rx_frames = 24;
    MB->rx_eapol = 2;
    MB->tx_eapol = 2;
    MB->rx_posted = 32;
    strcpy(MB->join_ssid, "Livebox-1A2B");
    memcpy((void *)MB->join_bssid, MB->bss[0].bssid, 6);
}

/* ---- scenarios ------------------------------------------------------ */

static const char *s_out = "build-sim";

static void _shot(const char *name, const char *suffix)
{
    lv_obj_invalidate(lv_scr_act());
    _frames(3);
    sim_png_write(s_out, name, suffix, s_fb, GUI_W, GUI_H);
}

/* A screen is left the way gui_main.c leaves it, so the next one starts
 * from an empty screen and an empty group. */
static void _reset(void)
{
    if (s_leave) {
        void (*leave)(void) = s_leave;
        s_leave = NULL;
        leave();
    }
    lv_group_focus_freeze(gui_group(), false);
    gui_keypad_grab(false);
    s_chip_up = false;
    sim_ini = NULL;                 /* no card, unless a scenario puts one */

    /* The last scene's screen is left for the next gui_win_new() to tear
     * down, as on the console: deleting it here too would free it twice.
     * The leave hook above has closed the cards. */
    s_logbuf[0] = 0;

    /* The focus group starts empty, as on a console that has just started.
     * Scenes are not screen changes, and the group would otherwise keep
     * the last one's entries; the console's own is checked for exactly
     * that by gui_group_ghosts(). */
    lv_ll_init(&gui_group()->obj_ll, sizeof(lv_obj_t *));
    gui_group()->obj_focus = NULL;
}

/* The page's rows and the details block below it: the list is cut to whole
 * rows and ends above the block, whatever the block grew to. */
static void _chk_list_and_info(void)
{
    lv_area_t pg = _box(s_page), in = _box(s_info);
    _chk_apart(s_page, s_info, "the list and the details");
    _chk(pg.y2 < in.y1, "the list ends above the details (%d < %d)",
         pg.y2, in.y1);
    _chk(lv_obj_get_height(s_page) % ROW_H == 0,
         "the list is a whole number of rows (%d of %d)",
         lv_obj_get_height(s_page), ROW_H);
    _chk(lv_obj_get_height(s_page) >= ROW_H, "the list has room for a row (%d)",
         lv_obj_get_height(s_page));
    _chk_grid(s_page, "the list");
    _chk_grid(s_info, "the details");
    _chk_in_parent(s_info, "the details");
}

/* The console's own check (gui_theme.c, the BOUNDS command), run here on
 * every scene: nothing drawn outside what holds it, nothing under the
 * status bar. */
static void _chk_bounds(void)
{
    /* The layout half; the focus-group half is the console's (see _reset). */
    u32 bad = gui_bounds_report();
    _chk(bad == 0, "nothing is drawn outside its lines (%d)", bad);
}

static lv_coord_t _list_y(void)
{
    return lv_obj_get_y(lv_page_get_scrl(s_page));
}

static void _scene_scan_idle(void)
{
    s_scene = "wifi-idle";
    _reset();
    gui_wifi_scan();
    _frames(2);
    _chk_grid(s_log, "the step log");
    _chk_grid(s_title, "the list's heading");
    _chk_in_parent(s_log, "the step log");
    _chk_grid(s_page, "the list");
    _shot("wifi-idle", NULL);
}

static void _scene_scan_done(void)
{
    s_scene = "wifi-scan";
    _reset();
    _mb_scan(8);
    gui_wifi_scan();
    _frames(2);
    _finish();
    _chk_list_and_info();
    _chk(_list_y() == 0, "a scan's results start at the top (%d)", _list_y());
    for (u32 r = 0; r < 8; r++) {
        _chk(!lv_obj_get_hidden(s_row[r]), "row %d is shown", r);
        _chk_in_parent(s_row_name[r], "row's name");
        _chk_in_parent(s_row_sub[r], "row's details");
    }
    _chk(lv_obj_get_hidden(s_row[8]), "the ninth row stays hidden");
    _chk_bounds();
    _shot("wifi-scan", NULL);
}

static void _scene_join_done(void)
{
    s_scene = "wifi-join";
    _reset();
    _mb_join();
    gui_wifi_scan();
    _frames(2);
    _show_results();                 /* the list the network was picked from */
    lv_obj_set_y(lv_page_get_scrl(s_page), -3 * ROW_H);   /* scrolled to it  */
    /* The join's own run clears the mailbox, and does not scan. */
    memset((void *)MB->bss, 0, sizeof(MB->bss));
    MB->nbss = 0;
    _cx_open("Livebox-1A2B", true);
    _finish();
    _frames(2);
    _shot("wifi-join-card", NULL);
    _chk_grid(s_cx_card, "the connection card");
    _cx_close();                     /* Done: the card goes, the page stays */
    _frames(2);
    _chk_list_and_info();
    _chk(_list_y() < 0, "the list is still where the network was picked"
         " from (%d)", _list_y());
    _chk_bounds();
    _shot("wifi-join", NULL);

    _act_row(s_row[0]);
    _chk(s_pw_bg && !strcmp(s_pick_ssid, "Livebox-1A2B"),
         "after a join, a row still opens its own network (\"%s\")",
         s_pick_ssid);
    _pw_close();
}

/* The keypad: its three maps, and what each of them leaves selected. The
 * maps are not the same length, and the selection is an index into the one
 * the matrix was built for. */
/* The step log. Lines keep coming - a run reports every stage, and what
 * CPU0 has to say is a sentence, not a word - and the column they go in
 * ends above the status bar. */
static void _scene_log(void)
{
    s_scene = "wifi-log";
    _reset();
    gui_wifi_scan();
    _frames(2);
    for (u32 i = 0; i < 40; i++)
        _log(GUI_HEX_HINT, ICON_INFO, (i % 4) ? "a step of the run" :
             "a step whose name runs on and on, the way a reason from the"
             " chip's own firmware does");
    _frames(2);
    _chk(LIST_Y + lv_obj_get_height(s_log) <= GUI_BOTTOM,
         "40 lines of log stop above the status bar (%d of %d)",
         LIST_Y + lv_obj_get_height(s_log), GUI_BOTTOM);
    _chk_in_parent(s_log, "the step log");
    _chk_grid(s_log, "the step log");
    _chk(s_logbuf[0] != 0, "and it still says something");
    _chk_bounds();
    _shot("wifi-log", NULL);
}

/* A message box, which every screen can put up: the longest one asked
 * here, to see it wrap inside its own width rather than over the screen. */
static lv_res_t _mbox_act(lv_obj_t *b, const char *txt)
{
    (void)txt;
    gui_mbox_close(b);
    return LV_RES_INV;
}

static void _scene_mbox(void)
{
    s_scene = "wifi-mbox";
    _reset();
    gui_wifi_scan();
    _frames(2);
    static const char *map[] = { "\221Start", "\221Cancel", "" };
    gui_mbox("#FF8000 Do you really want to do the thing?#\n\n"
             "It takes a paragraph to say what happens next, which is what "
             "the longest of these boxes does, and the box is half the "
             "screen wide.", map, _mbox_act);
    _frames(2);
    _chk_bounds();
    _shot("wifi-mbox", NULL);
    gui_mbox_close(NULL);           /* as its buttons do; the theme holds it */
}

static void _scene_keypad(void)
{
    s_scene = "wifi-keypad";
    _reset();
    _mb_scan(12);                    /* more rows than the list can show */
    gui_wifi_scan();
    _frames(2);
    _show_results();
    /* Scrolled down, as a finger would: what the list shows when the card
     * closes is what the next tap picks from. */
    lv_obj_set_y(lv_page_get_scrl(s_page), -2 * ROW_H);
    const lv_coord_t was = _list_y();
    /* Opened the way a finger opens it: a tap on the first row. */
    _tap(700, LIST_Y + ROW_H / 2);
    _frames(2);
    _chk(s_pw_bg != NULL, "a tap on a row opens the passphrase card");
    _chk_grid(s_pw_kb, "the keypad");
    _chk_in_parent(s_pw_kb, "the keypad");
    _chk(gui_keypad_grabbed(), "the keypad holds the directions");
    _shot("wifi-keypad", "-lower");

    lv_btnm_ext_t *ext = lv_obj_get_ext_attr(s_pw_kb);
    /* Opened by a finger, no key is selected, and switching maps leaves it
     * that way: a highlight nobody asked for would sit under the next tap. */
    _chk(ext->btn_id_pr == LV_BTNM_PR_NONE, "a finger leaves no key selected");
    _pw_key(s_pw_kb, "#+=");
    _chk(ext->btn_id_pr == LV_BTNM_PR_NONE,
         "and the symbol map opens with none selected either");
    _pw_key(s_pw_kb, "abc");

    /* From here it is the Joy-Con's: the first direction takes a key, and
     * every switch after that has to leave a key of the new map selected -
     * the maps are not the same length, and the selection is an index. */
    _key(LV_GROUP_KEY_DOWN);
    _chk(ext->btn_id_pr < ext->btn_cnt, "a direction takes a key (%d of %d)",
         ext->btn_id_pr, ext->btn_cnt);
    struct { const char *to, *dash, *key; u32 shot; } step[] = {
        { "upper",   "-upper",   "ABC", 1 },
        { "symbols", "-symbols", "#+=", 1 },
        { "lower",   "-lower-again", "abc", 1 },
    };
    for (u32 i = 0; i < 3; i++) {
        _pw_key(s_pw_kb, step[i].key);
        _frames(2);
        _chk(ext->btn_id_pr < ext->btn_cnt,
             "%s: the selection is a key of this map (%d of %d)",
             step[i].to, ext->btn_id_pr, ext->btn_cnt);
        /* And it is on the key that switches back, which is where the
         * thumb already is. */
        const char *sel = "";
        u32 idx = 0;
        for (const char **m = ext->map_p; m[0][0] != '\0'; m++) {
            if (m[0][0] == '\n')         /* the line breaks are not keys */
                continue;
            if (idx++ == ext->btn_id_pr) {
                sel = m[0];
                break;
            }
        }
        _chk(!strcmp(sel, i == 2 ? "#+=" : "abc"),
             "%s: the way back is selected (\"%s\")", step[i].to, sel);
        if (step[i].shot)
            _shot("wifi-keypad", step[i].dash);
        /* And the directions move it from there. Up, not down: the two
         * mode keys are both on the bottom row, where down has nowhere to
         * go. What the stale index did was leave every direction dead. */
        u16 before = ext->btn_id_pr;
        _key(LV_GROUP_KEY_UP);
        _chk(ext->btn_id_pr != before && ext->btn_id_pr < ext->btn_cnt,
             "%s: up moves the selection (%d -> %d)", step[i].to,
             before, ext->btn_id_pr);
        before = ext->btn_id_pr;
        _key(LV_GROUP_KEY_NEXT);     /* VOL+, turned into a right by the grab */
        _chk(ext->btn_id_pr != before && ext->btn_id_pr < ext->btn_cnt,
             "%s: VOL+ moves it across (%d -> %d)", step[i].to,
             before, ext->btn_id_pr);
        /* Back onto the mode key for the next switch. */
        ext->btn_id_pr = (u16)(i == 1 ? ext->btn_cnt - 4 : KB_ROW4);
    }
    _pw_close();
    _frames(2);
    _chk(_list_y() == was, "cancelling leaves the list where it was"
         " (%d, was %d)", _list_y(), was);
}

/* The header: its buttons, and the title they are laid out towards. A
 * button's box reaches GUI_TEXT_PAD past its text, which is why the one on
 * the right sits that far past the margin on purpose; what must not happen
 * is two of them sharing a pixel, or the leftmost reaching the title. A
 * third button is how a header runs out of room.
 *
 * The buttons are what the focus group holds inside the top bar; the title
 * is the other child of the header they all hang from. */
static void _chk_header_clear(void)
{
    /* The header's children: the title, a bare label, and the buttons,
     * each holding one. */
    lv_obj_t *btn[8], *title = NULL;
    u32 n = 0;
    lv_obj_t *hdr = lv_obj_get_parent(s_btn), *c = NULL;
    while ((c = lv_obj_get_child(hdr, c))) {
        if (lv_obj_get_hidden(c))
            continue;
        if (lv_obj_get_child(c, NULL) == NULL)
            title = c;                      /* the window's title */
        else if (n < 8)
            btn[n++] = c;
    }
    _chk(n >= 2, "the header has its buttons (%d)", n);
    if (!n)
        return;

    for (u32 i = 0; i < n; i++) {
        lv_area_t a = _box(btn[i]);
        _chk(a.y1 >= 0 && a.y2 <= GUI_BAR_H,
             "a header button stays in the top bar (%d..%d)", a.y1, a.y2);
        _chk(a.x1 >= GUI_MARGIN - GUI_TEXT_PAD &&
             a.x2 <= GUI_RIGHT + GUI_TEXT_PAD,
             "a header button stays on the grid (%d..%d)", a.x1, a.x2);
        for (u32 j = i + 1; j < n; j++)
            _chk_apart(btn[i], btn[j], "two header buttons");
    }

    if (title)
        _chk_apart(title, btn[n - 1], "the title and the nearest button");
}

/* Stopping the chip: the radio does not stay up behind the next test. */
static void _scene_radio(void)
{
    s_scene = "wifi-radio";
    _reset();
    _mb_scan(4);
    gui_wifi_scan();
    _frames(2);
    /* Nothing has run, so there is no radio to turn off and no button
     * for it: the header is the other two. */
    _chk(lv_obj_get_hidden(s_off),
         "Radio Off is not in the header of a screen that has not run");

    s_chip_up = true;                   /* as a run leaves the chip */
    _off_sync();
    _frames(2);
    _chk(!lv_obj_get_hidden(s_off), "and it is there once the radio is up");
    _chk_header_clear();

    u32 before = sim_chip_stops;
    _act_off(NULL);
    _frames(2);
    _chk(sim_chip_stops == before + 1,
         "Radio off puts the WLAN section back into reset");
    _chk(strstr(s_logbuf, "radio off") != NULL, "and says so in the steps");
    _chk(lv_obj_get_hidden(s_off), "and takes itself out of the header");
    _chk_bounds();
    _shot("wifi-radio-off", NULL);

    /* Leaving the screen: the chip goes back if this visit powered it,
     * and the pins are left alone if it did not. */
    before = sim_chip_stops;
    _leave();
    _chk(sim_chip_stops == before,
         "leaving a screen that ran nothing does not touch the radio");
    s_chip_up = true;                   /* as a run leaves it */
    _leave();
    _chk(sim_chip_stops == before + 1, "leaving after a run stops the chip");
    _chk(!s_chip_up, "and only once");
}

/* A passphrase saved in hwtest/hwtest.ini: picking that network opens the
 * keypad with it already in the field and Connect selected, and picking
 * another one opens an empty field, as it always did. */
static void _scene_saved(void)
{
    static const char *const INI =
        "console=uart-b\n"
        "wifi_ssid=Livebox-1A2B\n"
        "wifi_psk=sim passphrase 42\n"
        "wifi_ssid=somewhere else\n"
        "wifi_psk=not this one\n";

    s_scene = "wifi-saved";
    _reset();
    _mb_scan(4);
    sim_ini = INI;
    gui_wifi_scan();
    _frames(2);
    _show_results();

    _tap(700, LIST_Y + ROW_H / 2);              /* Livebox-1A2B, saved */
    _frames(2);
    _chk(s_pw_bg != NULL, "the keypad opens on a saved network");
    _chk(!strcmp(lv_ta_get_text(s_pw_ta), "sim passphrase 42"),
         "with its passphrase in the field (\"%s\")",
         lv_ta_get_text(s_pw_ta));
    lv_btnm_ext_t *ext = lv_obj_get_ext_attr(s_pw_kb);
    _chk(ext->btn_id_pr == ext->btn_cnt - 1,
         "and Connect selected, so one press joins (%d of %d)",
         ext->btn_id_pr, ext->btn_cnt);
    _chk_grid(s_pw_kb, "the keypad");
    _shot("wifi-keypad-saved", NULL);
    _pw_close();
    _frames(2);

    _tap(700, LIST_Y + ROW_H + ROW_H / 2);      /* "test", not in the file */
    _frames(2);
    _chk(s_pw_bg != NULL, "the keypad opens on a network it has nothing for");
    _chk(lv_ta_get_text(s_pw_ta)[0] == 0, "with an empty field (\"%s\")",
         lv_ta_get_text(s_pw_ta));
    ext = lv_obj_get_ext_attr(s_pw_kb);
    _chk(ext->btn_id_pr == LV_BTNM_PR_NONE, "and no key selected");
    _pw_close();
    _frames(2);
}

/* Stepping a row at a time, from the header into the list: the first step
 * into it goes to the first row, not to whichever row happens to be under
 * the header on the screen. */
static void _scene_focus(void)
{
    s_scene = "wifi-focus";
    _reset();
    _mb_scan(8);
    gui_wifi_scan();
    _frames(2);
    _finish();
    /* Scrolled down the list, the way picking a network leaves it: a row's
     * place on the screen is then not its place in the list, which is what
     * sent the first step down into the middle of it. */
    lv_obj_set_y(lv_page_get_scrl(s_page), -4 * ROW_H);
    lv_group_focus_obj(s_btn);       /* the header's Scan button */
    _frames(2);
    for (u32 i = 0; i < 3; i++) {
        _key(LV_GROUP_KEY_DOWN);
        lv_obj_t *f = lv_group_get_focused(gui_group());
        _chk(f == s_row[i], "the %d%s step down is row %d", i + 1,
             i ? (i == 1 ? "nd" : "rd") : "st", i);
        if (f != s_row[i]) {
            lv_area_t a = _box(f);
            sim_say("      focus is at %d..%d instead\n", a.y1, a.y2);
        }
    }
    _chk(lv_obj_get_y(lv_page_get_scrl(s_page)) == 0,
         "and the list scrolled back to the rows it walked (%d)",
         lv_obj_get_y(lv_page_get_scrl(s_page)));
    _shot("wifi-focus", NULL);
    _key(LV_GROUP_KEY_UP);
    _key(LV_GROUP_KEY_UP);
    _key(LV_GROUP_KEY_UP);
    /* And back out of the list the same way. Which header button it lands
     * on is whichever is nearest across - there are three of them - so
     * what is checked is that the focus left the list for the header. */
    lv_obj_t *up = lv_group_get_focused(gui_group());
    _chk(up != NULL, "the focus is still on something after three steps up");
    if (!up)
        return;
    lv_area_t fa = _box(up);
    _chk(fa.y2 <= GUI_BAR_H, "and three steps up are back on the header (%d..%d)",
         fa.y1, fa.y2);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        s_out = argv[1];

    /* The two blocks of DRAM the GUI is given on the console, at the same
     * addresses: LVGL allocates its objects out of the first and draws
     * into the second. Before lv_init(), which uses both. */
    sim_map(NYX_LV_MEM_ADR, LV_MEM_SIZE, "LVGL's pool");
    sim_map(NYX_FB2_ADDRESS, GUI_W * GUI_H * 4, "the framebuffer");

    lv_init();
    _display_init();
    gui_theme_init();
    gui_statusbar_init();
    _input_init();

    _scene_scan_idle();
    _scene_scan_done();
    _scene_join_done();
    _scene_keypad();
    _scene_saved();
    _scene_log();
    _scene_mbox();
    _scene_radio();
    _scene_focus();

    if (s_fails)
        sim_say("\n%d of %d checks FAILED\n", s_fails, s_checks);
    else
        sim_say("\nall %d OK\n", s_checks);
    return s_fails ? 1 : 0;
}
