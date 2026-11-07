/*
 * hwtest GUI - the Bluetooth test: find a Joy-Con or a Pro Controller in
 * pairing mode, connect to it over the console's own radio, and show every
 * button it has, SL and SR included, and its sticks. The rail cannot show
 * SL and SR (they sit inside the rail), and a Pro Controller has no rail.
 *
 * The stack is gui_bt.c. This screen only polls it and draws: the
 * controllers found on the left, the one connected on the right.
 */
#include "gui.h"
#include "gui_bt.h"
#include "gui_jcwire.h"

#define RING        200
#define DOT         26
#define DEAD        52
#define SPAN        0x480   /* raw counts from the middle to the ring */

#define PAD_IN      24
#define PILL_COLS   6
#define PILL_H      50
#define PILL_GAP    12

#define LIST_Y      (GUI_TOP + 172)
#define ROW_H       69
#define UI_MS       50
#define RAIL_WAIT_MS 3000   /* for the rail driver to bring a Joy-Con up */

typedef struct {
    const char *name;
    u8 bit;              /* jc_gamepad_rpt_t.buttons */
} bt_btn_t;

static const bt_btn_t s_btn_l[] = {
    { "Up", 17 }, { "Down", 16 }, { "Left", 19 }, { "Right", 18 },
    { "L", 22 }, { "ZL", 23 }, { "-", 8 }, { "Capture", 13 },
    { "Stick", 11 }, { "SL", 21 }, { "SR", 20 },
};
static const bt_btn_t s_btn_r[] = {
    { "A", 3 }, { "B", 2 }, { "X", 1 }, { "Y", 0 },
    { "R", 6 }, { "ZR", 7 }, { "+", 9 }, { "Home", 12 },
    { "Stick", 10 }, { "SL", 5 }, { "SR", 4 },
};
static const bt_btn_t s_btn_pro[] = {
    { "A", 3 }, { "B", 2 }, { "X", 1 }, { "Y", 0 },
    { "Up", 17 }, { "Down", 16 }, { "Left", 19 }, { "Right", 18 },
    { "L", 22 }, { "R", 6 }, { "ZL", 23 }, { "ZR", 7 },
    { "-", 8 }, { "+", 9 }, { "Home", 12 }, { "Capture", 13 },
    { "L Stick", 11 }, { "R Stick", 10 },
};
#define BTN_MAX ARRAY_SIZE(s_btn_pro)

static lv_task_t *s_task;
static u32 s_ui_at;
static lv_obj_t *s_hint, *s_search, *s_disc, *s_rails;
static lv_obj_t *s_row[BT_DEV_MAX], *s_row_name[BT_DEV_MAX], *s_row_sub[BT_DEV_MAX];
static lv_obj_t *s_panel, *s_title, *s_status, *s_count, *s_info, *s_box;
static lv_obj_t *s_dot[2], *s_pill[BTN_MAX];
static const bt_btn_t *s_btns;
static u32 s_btn_n, s_lit, s_done;
static int s_rings;                 /* sticks drawn: 1 or 2           */
static int s_built = -1;            /* the pad type the view is for   */
static u16 s_centre[4];
static bool s_centred;
static int s_confirm = -1;          /* the device the box asks about  */
static lv_obj_t *s_steps;           /* the last steps, while no view  */
static u32 s_steps_seq;
static lv_style_t st_ring, st_dead, st_dot, st_pill, st_pill_on, st_pill_done;

/* Set a label's text only when it changes: every change is a redraw. */
static void _set(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text))
        lv_label_set_text(l, text);
}

static const char *_pad_name(gui_bt_pad_t t)
{
    switch (t) {
    case BT_PAD_JOYCON_L: return "Left Joy-Con";
    case BT_PAD_JOYCON_R: return "Right Joy-Con";
    case BT_PAD_PRO:      return "Pro Controller";
    default:              return "Controller";
    }
}

/* ---- the controller view ----------------------------------------------- */

static void _count(void)
{
    u32 n = 0;
    for (u32 b = s_done; b; b >>= 1)
        n += b & 1;
    char buf[64];
    if (n == s_btn_n)
        s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  All %d buttons#", s_btn_n);
    else
        s_printf(buf, "%d of %d buttons", n, s_btn_n);
    _set(s_count, buf);
    lv_obj_align(s_count, NULL, LV_ALIGN_IN_TOP_RIGHT, -PAD_IN,
                 26 + (gui_font_30.h_px - gui_font_20.h_px) / 2);
}

static void _view_build(gui_bt_pad_t type)
{
    lv_obj_clean(s_box);
    s_lit = s_done = 0;
    s_centred = false;
    s_built = type;
    s_btns = type == BT_PAD_JOYCON_L ? s_btn_l
           : type == BT_PAD_JOYCON_R ? s_btn_r : s_btn_pro;
    s_btn_n = type == BT_PAD_JOYCON_L ? ARRAY_SIZE(s_btn_l)
            : type == BT_PAD_JOYCON_R ? ARRAY_SIZE(s_btn_r) : ARRAY_SIZE(s_btn_pro);
    s_rings = (type == BT_PAD_JOYCON_L || type == BT_PAD_JOYCON_R) ? 1 : 2;

    for (int i = 0; i < s_rings; i++) {
        lv_coord_t x = i * (RING + PAD_IN);
        lv_obj_t *ring = gui_box(s_box, &st_ring, x, 0, RING, RING);
        gui_box(ring, &st_dead, (RING - DEAD) / 2, (RING - DEAD) / 2, DEAD, DEAD);
        s_dot[i] = gui_box(ring, &st_dot, (RING - DOT) / 2, (RING - DOT) / 2, DOT, DOT);
    }
    lv_coord_t tx = s_rings * (RING + PAD_IN);
    lv_coord_t tw = GUI_COL2_W - 2 * PAD_IN - tx;
    s_info = gui_hint(s_box, tw, "");
    lv_obj_set_pos(s_info, tx, 8);

    lv_coord_t pw = (GUI_COL2_W - 2 * PAD_IN - (PILL_COLS - 1) * PILL_GAP) / PILL_COLS;
    for (u32 i = 0; i < s_btn_n; i++) {
        lv_obj_t *p = lv_cont_create(s_box, NULL);
        lv_cont_set_style(p, &st_pill);
        lv_obj_set_click(p, false);
        lv_obj_set_size(p, pw, PILL_H);
        lv_obj_set_pos(p, (i % PILL_COLS) * (pw + PILL_GAP),
                       RING + 24 + (i / PILL_COLS) * (PILL_H + PILL_GAP));
        lv_obj_t *l = lv_label_create(p, NULL);
        lv_label_set_text(l, s_btns[i].name);
        lv_obj_align(l, NULL, LV_ALIGN_CENTER, 0, 0);
        s_pill[i] = p;
    }
    _count();
}

static lv_coord_t _dot_pos(u16 raw, u16 centre, bool flip)
{
    s32 d = (s32)raw - centre;
    if (flip)
        d = -d;
    s32 r = (RING - DOT) / 2;
    d = d * r / SPAN;
    if (d > r) d = r;
    if (d < -r) d = -r;
    return r + d;
}

static void _view_update(const gui_bt_pad_state_t *pad)
{
    if (!pad->reports)
        return;
    if (!s_centred) {
        memcpy(s_centre, pad->stick, sizeof(s_centre));
        s_centred = true;
    }
    /* A Joy-Con's one stick is the left pair on the left one, the right
     * pair on the right one; a Pro Controller has both. */
    for (int i = 0; i < s_rings; i++) {
        int s = s_rings == 2 ? i : s_built == BT_PAD_JOYCON_R ? 1 : 0;
        lv_coord_t x = _dot_pos(pad->stick[2 * s], s_centre[2 * s], false);
        lv_coord_t y = _dot_pos(pad->stick[2 * s + 1], s_centre[2 * s + 1], true);
        if (lv_obj_get_x(s_dot[i]) != x || lv_obj_get_y(s_dot[i]) != y)
            lv_obj_set_pos(s_dot[i], x, y);
    }
    u32 lit = 0;
    for (u32 i = 0; i < s_btn_n; i++)
        if (pad->buttons & BIT(s_btns[i].bit))
            lit |= BIT(i);
    if (lit != s_lit) {
        u32 was_done = s_done;
        s_done |= lit;
        for (u32 i = 0; i < s_btn_n; i++) {
            if ((lit ^ s_lit) & BIT(i) || (s_done ^ was_done) & BIT(i))
                lv_cont_set_style(s_pill[i], (lit & BIT(i)) ? &st_pill_on
                                  : (s_done & BIT(i)) ? &st_pill_done : &st_pill);
        }
        s_lit = lit;
        if (s_done != was_done)
            _count();
    }
}

/* ---- the list ---------------------------------------------------------- */

static void _list_update(void)
{
    u32 n = gui_bt_dev_count();
    int cur = gui_bt_connected();
    for (u32 i = 0; i < BT_DEV_MAX; i++) {
        if (i >= n) {
            if (!lv_obj_get_hidden(s_row[i]))
                gui_hide(s_row[i], true);
            continue;
        }
        const gui_bt_dev_t *d = gui_bt_dev(i);
        char buf[96];
        s_printf(buf, "%s%s", (int)i == cur ? "#" GUI_HEX_ACCENT " " ICON_BLUETOOTH "#  " : "",
                 d->name[0] ? d->name : "Gamepad");
        _set(s_row_name[i], buf);
        s_printf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1], d->addr[0]);
        if (d->rssi)
            s_printf(buf + strlen(buf), ", %d dBm", d->rssi);
        _set(s_row_sub[i], buf);
        if (lv_obj_get_hidden(s_row[i]))
            gui_hide(s_row[i], false);
    }
}

/* ---- the whole screen, every UI_MS ------------------------------------- */

static void _ui_update(void)
{
    gui_bt_state_t st = gui_bt_state();
    const gui_bt_pad_state_t *pad = gui_bt_pad();
    const char *why = gui_bt_why();
    char buf[256];

    /* Left: what to do. */
    if (st == BT_SCANNING)
        s_printf(buf, "#" GUI_HEX_ACCENT " Searching#, for about ten seconds.");
    else if (st == BT_NAMING)
        s_printf(buf, "#" GUI_HEX_ACCENT " Asking#  each one for its name.");
    else if (st == BT_OFF || st == BT_FAILED)
        s_printf(buf, "The radio is off.");
    else if (!gui_bt_dev_count())
        s_printf(buf, "Hold the controller's small sync button (on a Joy-Con's "
                      "rail, on top of a Pro Controller) until its lights run, "
                      "then Search.");
    else
        s_printf(buf, "Pick a controller to connect to it. Search again if "
                      "yours is not here.");
    _set(s_hint, buf);
    _list_update();

    bool busy = st >= BT_CONNECTING;
    if (lv_obj_get_hidden(s_disc) != !busy)
        gui_hide(s_disc, !busy);
    bool can_search = st == BT_IDLE;
    if (lv_obj_get_hidden(s_search) != !can_search)
        gui_hide(s_search, !can_search);
    bool rails = st == BT_IDLE || st == BT_SCANNING || st == BT_NAMING;
    if (s_rails && lv_obj_get_hidden(s_rails) != !rails)
        gui_hide(s_rails, !rails);

    /* Right: the controller. */
    int cur = gui_bt_connected();
    if (st == BT_READY && pad->type != (gui_bt_pad_t)s_built)
        _view_build(pad->type);
    if (cur >= 0) {
        const gui_bt_dev_t *d = gui_bt_dev(cur);
        s_printf(buf, "#" GUI_HEX_ACCENT " " ICON_GAMEPAD "#  %s",
                 d && d->name[0] ? d->name : _pad_name(pad->type));
    } else {
        s_printf(buf, "#" GUI_HEX_ACCENT " " ICON_BLUETOOTH "#  No controller");
    }
    _set(s_title, buf);

    if (st == BT_READY) {
        static const char *batt[] = { "empty", "very low", "low", "half", "full" };
        u32 lvl = pad->batt >> 1;
        s_printf(buf, "#" GUI_HEX_OK " Connected#, battery %s%s",
                 lvl < 5 ? batt[lvl] : "unknown", pad->charging ? ", charging" : "");
    } else if (st >= BT_CONNECTING) {
        s_printf(buf, "#" GUI_HEX_ACCENT " %s...#", gui_bt_step());
    } else if (st == BT_FAILED) {
        s_printf(buf, "#" GUI_HEX_WARN " %s.#\nThe health check's Wi-Fi and "
                      "Bluetooth row says more.", why);
    } else if (why[0]) {
        s_printf(buf, "#" GUI_HEX_WARN " %s#", why);
    } else {
        s_printf(buf, "Not connected");
    }
    _set(s_status, buf);

    if (st == BT_READY && s_built >= 0) {
        _view_update(pad);
        const u8 *a = gui_bt_local_addr();
        s_printf(buf, "%d reports a second\nLine errors: %d\n\n%s\n\n"
                      "The console's radio:\n%02X:%02X:%02X:%02X:%02X:%02X",
                 pad->rate, gui_bt_line_errors(),
                 !gui_bt_paired() ? "No pairing was needed."
                 : gui_bt_bonded() ? "Paired and bonded: it now remembers this radio."
                                   : "Paired, not bonded.",
                 a[5], a[4], a[3], a[2], a[1], a[0]);
        _set(s_info, buf);
    }
    if (st != BT_READY && s_built >= 0 && cur < 0) {
        lv_obj_clean(s_box);
        s_built = -1;
        _set(s_count, "");
    }

    /* Until a controller is shown, the steps the radio went through, so a
     * failure can be read off the screen (they also go to the debug UART). */
    bool steps = s_built < 0;
    if (lv_obj_get_hidden(s_steps) == steps)
        lv_obj_set_hidden(s_steps, !steps);
    if (steps && gui_bt_log_seq() != s_steps_seq) {
        static char log[900];
        log[0] = 0;
        s_steps_seq = gui_bt_log_seq();
        for (int back = 9; back >= 0; back--) {
            const char *l = gui_bt_log(back);
            if (!l)
                continue;
            strcat(log, l);
            if (back)
                strcat(log, "\n");
        }
        lv_label_set_text(s_steps, log);
    }
}

static void _tick(void *param)
{
    gui_bt_poll();
    u32 now = get_tmr_ms();
    if ((s32)(now - s_ui_at) < UI_MS)
        return;
    s_ui_at = now;
    _ui_update();
}

/* ---- the rails --------------------------------------------------------- */

/* Joy-Cons on the rails told, over the rail, to reboot into Bluetooth
 * pairing mode (subcommand 0x06, "set HCI state", argument 0x02), as the
 * sync button does: slid off, they are found by a search. The rails are
 * not polled after that, until the next time this is asked or the screen
 * is left, so the rail driver does not take a rebooted Joy-Con back. The
 * number asked, or -1 for none there. */
int gui_bt_rail_pair(void)
{
    if (gui_is_lite())
        return -1;
    /* The rails are polled again, until the driver has a Joy-Con up: one
     * slid back on while they were left alone takes it a moment, and a
     * single poll found none. */
    gui_jc_nav(true);
    jc_gamepad_rpt_t *jc = NULL;
    for (u32 t0 = get_tmr_ms(); get_tmr_ms() - t0 < RAIL_WAIT_MS; ) {
        jc = gui_jc_state();
        if (jc && !jc->sio_mode && (jc->conn_l || jc->conn_r))
            break;
        msleep(10);
    }
    if (!jc || jc->sio_mode || (!jc->conn_l && !jc->conn_r))
        return -1;
    bool conn[2] = { jc->conn_l, jc->conn_r };
    static const u8 pair = 0x02;
    int n = 0;
    for (int side = 0; side < 2; side++) {
        if (!conn[side])
            continue;
        gui_jc_own(side);
        gui_jc_subcmd(side, 0x06, &pair, 1, NULL);   /* it reboots: no answer due */
        gui_jc_release(side);
        n++;
    }
    /* Now the rails are left alone, so the driver does not take the
     * rebooting Joy-Cons back from pairing mode. */
    gui_jc_nav(false);
    char buf[80];
    s_printf(buf, "%d Joy-Con%s on the rails sent to pairing mode: slide off",
             n, n > 1 ? "s" : "");
    gui_bt_note(buf);
    return n;
}

/* ---- actions --------------------------------------------------------- */

static lv_res_t _act_rails(lv_obj_t *b)
{
    if (gui_bt_rail_pair() < 0)
        gui_bt_note("no Joy-Con on the rails");
    return LV_RES_OK;
}

static lv_res_t _act_search(lv_obj_t *b)
{
    gui_bt_scan();
    _ui_update();
    return LV_RES_OK;
}

static lv_res_t _act_disc(lv_obj_t *b)
{
    gui_bt_disconnect();
    return LV_RES_OK;
}

static lv_res_t _confirm_action(lv_obj_t *btns, const char *txt)
{
    if (strstr(txt, "Connect") && s_confirm >= 0)
        gui_bt_connect(s_confirm);
    s_confirm = -1;
    gui_mbox_close(btns);
    return LV_RES_INV;
}

static lv_res_t _act_row(lv_obj_t *b)
{
    if (gui_bt_state() >= BT_CONNECTING)
        return LV_RES_OK;
    u32 idx = lv_obj_get_free_num(b);
    const gui_bt_dev_t *d = gui_bt_dev(idx);
    if (!d)
        return LV_RES_OK;
    s_confirm = idx;
    static char text[512];
    s_printf(text, "Connect to %s?\n\n"
             "If it asks to pair, it may forget the console it was paired "
             "with. A Joy-Con pairs again when slid onto its rail, a Pro "
             "Controller from the console's Change Grip/Order screen or "
             "over its USB cable.",
             d->name[0] ? d->name : "this gamepad");
    static const char *map[] = { "\221Connect", "\221Cancel", "" };
    gui_mbox(text, map, _confirm_action);
    return LV_RES_OK;
}

static void _leave(void)
{
    if (s_task)
        lv_task_del(s_task);
    s_task = NULL;
    gui_bt_close();
}

void gui_bt_test(void)
{
    lv_obj_t *win = gui_win_new(ICON_BLUETOOTH " Bluetooth");
    gui_set_back(gui_joycon_test);
    gui_on_leave(_leave);
    s_search = gui_win_btn(win, ICON_REFRESH " Search", _act_search);
    s_rails = gui_is_lite() ? NULL
            : gui_win_btn(win, ICON_GAMEPAD " Pair from rails", _act_rails);
    s_disc = gui_win_btn(win, ICON_CLOSE " Disconnect", _act_disc);
    /* Disconnect shows instead of the other two, in Search's place next to
     * Close (it is wider, so over part of Pair from rails' too). */
    lv_obj_set_x(s_disc, lv_obj_get_x(s_search) + lv_obj_get_width(s_search) -
                         lv_obj_get_width(s_disc));
    gui_hide(s_search, true);
    gui_hide(s_disc, true);

    lv_theme_t *th = gui_th();
    gui_box_style(&st_ring, GUI_COL_BG_DARK, GUI_COL_LINE, LV_RADIUS_CIRCLE, 3);
    gui_box_style(&st_dead, GUI_COL_BG_LIGHT, GUI_COL_LINE, LV_RADIUS_CIRCLE, 2);
    gui_box_style(&st_dot, GUI_COL_ACCENT, GUI_COL_ACCENT, LV_RADIUS_CIRCLE, 0);
    lv_style_copy(&st_pill, th->btn.rel);
    st_pill.text.font = &gui_font_20;
    lv_style_copy(&st_pill_on, th->btn.pr);
    st_pill_on.text.font = &gui_font_20;
    lv_style_copy(&st_pill_done, th->btn.tgl_rel);
    st_pill_done.text.font = &gui_font_20;
    st_pill_done.text.color = th->btn.pr->text.color;

    /* Left: what to do, and what was found. */
    lv_obj_t *t = gui_label(win, &gui_font_30, GUI_COL_TEXT, "Controllers");
    lv_obj_set_pos(t, GUI_MARGIN, GUI_TOP + 4);
    s_hint = gui_hint(win, GUI_COL1_W, "Starting the radio...");
    lv_obj_set_pos(s_hint, GUI_MARGIN, GUI_TOP + 52);
    lv_obj_t *page = gui_page(win, GUI_MARGIN, LIST_Y, GUI_COL1_W,
                              GUI_BOTTOM - LIST_Y);
    lv_coord_t rw = GUI_COL1_W - GUI_PAGE_SB_W;
    for (u32 i = 0; i < BT_DEV_MAX; i++) {
        lv_obj_t *r = gui_list_row(page, i * ROW_H, rw, ROW_H, _act_row);
        lv_obj_set_free_num(r, i);
        s_row_name[i] = gui_label(r, &gui_font_20, GUI_COL_TEXT, "");
        lv_obj_set_pos(s_row_name[i], 16, 10);
        s_row_sub[i] = gui_label(r, &gui_font_20, GUI_COL_HINT, "");
        lv_obj_set_pos(s_row_sub[i], 16, 38);
        s_row[i] = r;
        gui_hide(r, true);
    }
    gui_divider(win, GUI_DIVIDER_X, GUI_TOP, GUI_CONTENT_H);

    /* Right: the controller connected. */
    s_panel = gui_panel(win, GUI_COL2_X, GUI_TOP, GUI_COL2_W, GUI_CONTENT_H);
    s_title = gui_label(s_panel, &gui_font_30, GUI_COL_TEXT, "");
    lv_obj_set_pos(s_title, PAD_IN, 26);
    s_count = gui_hint(s_panel, 0, "");
    s_status = gui_hint(s_panel, GUI_COL2_W - 2 * PAD_IN, "");
    lv_obj_set_pos(s_status, PAD_IN, 68);
    s_box = lv_obj_create(s_panel, NULL);
    lv_obj_set_style(s_box, &lv_style_transp);
    lv_obj_set_click(s_box, false);
    lv_obj_set_pos(s_box, PAD_IN, 108);
    lv_obj_set_size(s_box, GUI_COL2_W - 2 * PAD_IN, GUI_CONTENT_H - 108 - PAD_IN);
    s_steps = gui_hint(s_panel, GUI_COL2_W - 2 * PAD_IN, "");
    lv_obj_set_pos(s_steps, PAD_IN, 108);
    s_steps_seq = ~0u;
    s_built = -1;
    s_confirm = -1;
    _set(s_title, "#" GUI_HEX_ACCENT " " ICON_BLUETOOTH "#  No controller");

    /* The radio takes about half a second to come up: show the screen
     * first. */
    gui_refresh_now();
    gui_bt_open();
    _ui_update();

    if (s_task)
        lv_task_del(s_task);
    s_task = lv_task_create(_tick, 10, LV_TASK_PRIO_MID, NULL);
}
