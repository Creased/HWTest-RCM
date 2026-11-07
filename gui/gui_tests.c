/*
 * hwtest GUI - the hands-on tests: Joy-Con and touchscreen.
 *
 * Both read the hardware directly, many times a second, instead of going
 * through LVGL's input: every Joy-Con button is under test in the first,
 * every touch in the second. So the way out is always something the test
 * does not use - POWER (the window's focused Close button, the touch test's
 * Finish), or the touch test's message box once it is over.
 */
#include "gui.h"
#include "gui_stick.h"
#include "gui_jcwire.h"

/* ======================================================================= */
/* Joy-Con                                                                 */

#define STICK_RING  220
#define STICK_DOT   28
#define STICK_DEAD  56

/* The panel: two side by side across the content, 24 px in from their
 * frame for everything inside. */
#define JC_PANEL_W  ((GUI_CONTENT_W - 36) / 2)
#define JC_IN       24
#define JC_TEXT_X   (JC_IN + STICK_RING + 24)

typedef struct {
    const char *name;
    u8 bit;
} jc_btn_t;

/* Bit numbers of jc_gamepad_rpt_t.buttons (bdk input/joycon.h). Not SL
 * and SR: they sit inside the rail once a Joy-Con is on it, and the test
 * only reads Joy-Cons on the rails. */
static const jc_btn_t s_btn_l[] = {
    { "Up", 17 }, { "Down", 16 }, { "Left", 19 }, { "Right", 18 },
    { "L", 22 }, { "ZL", 23 }, { "-", 8 }, { "Capture", 13 },
    { "Stick", 11 },
};
static const jc_btn_t s_btn_r[] = {
    { "A", 3 }, { "B", 2 }, { "X", 1 }, { "Y", 0 },
    { "R", 6 }, { "ZR", 7 }, { "+", 9 }, { "Home", 12 },
    { "Stick", 10 },
};
#define JC_BTNS 9

typedef struct {
    lv_obj_t *status, *dot, *centre, *range, *count;
    lv_obj_t *btn[JC_BTNS];
    u32 lit;                 /* held right now                            */
    u32 done;                /* pressed at some point                     */
    bool seen;               /* connected at the last tick                */
    bool ever;               /* connected at some point                   */
    u32 drops;               /* times the connection went since then      */
    u8 batt;
    bool buzz;               /* the rumble test has it buzzing            */
    gui_stick_t stick;
    jc_cal_t cal;            /* from its flash, when cal_ok               */
    bool cal_ok;
} jc_side_t;

static jc_side_t s_jc[2];
static lv_task_t *s_jc_task;
static lv_style_t st_ring, st_dead, st_dot, st_pill, st_pill_on, st_pill_done;

/* The rumble test: the left Joy-Con buzzes for half a second, then the
 * right, so a dead motor is told from a working one by feel. */
#define RUMBLE_ON_MS   500
#define RUMBLE_GAP_MS  250
static u32 s_rumble_at;

/* The sensor screens take the rails over; the driver takes a few seconds
 * to find the Joy-Cons again once they are closed. */
#define JC_RESUME_MS   8000
static u32 s_jc_resume_at;

void gui_jc_test_resumed(void)
{
    s_jc_resume_at = get_tmr_ms();
}

/* How many of the buttons have been pressed so far, on the title line,
 * against the panel's right inset. */
static void _jc_count(jc_side_t *s)
{
    u32 n = 0;
    for (u32 b = s->done; b; b >>= 1)
        n += b & 1;
    char buf[48];
    if (n == JC_BTNS)
        s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  All %d buttons#", JC_BTNS);
    else
        s_printf(buf, "%d of %d buttons", n, JC_BTNS);
    lv_label_set_text(s->count, buf);
    lv_obj_align(s->count, NULL, LV_ALIGN_IN_TOP_RIGHT, -JC_IN,
                 26 + (gui_font_30.h_px - gui_font_20.h_px) / 2);
}

static void _jc_side_build(lv_obj_t *win, int side, lv_coord_t x0)
{
    jc_side_t *s = &s_jc[side];
    memset(s, 0, sizeof(*s));
    lv_obj_t *panel = gui_panel(win, x0, GUI_TOP, JC_PANEL_W, GUI_CONTENT_H);

    lv_obj_t *t = gui_label(panel, &gui_font_30, GUI_COL_TEXT,
        side ? "#" GUI_HEX_ACCENT " " ICON_GAMEPAD "#  Right Joy-Con"
             : "#" GUI_HEX_ACCENT " " ICON_GAMEPAD "#  Left Joy-Con");
    lv_obj_set_pos(t, JC_IN, 26);
    s->count = gui_hint(panel, 0, "");
    _jc_count(s);
    s->status = gui_hint(panel, 0, s_jc_resume_at
        ? "Reconnecting after a sensor test (a few seconds)"
        : "Not attached: slide it onto the rail");
    lv_obj_set_pos(s->status, JC_IN, 68);

    lv_obj_t *ring = gui_box(panel, &st_ring, JC_IN, 108, STICK_RING, STICK_RING);
    gui_box(ring, &st_dead, (STICK_RING - STICK_DEAD) / 2, (STICK_RING - STICK_DEAD) / 2,
         STICK_DEAD, STICK_DEAD);
    s->dot = gui_box(ring, &st_dot, (STICK_RING - STICK_DOT) / 2,
                  (STICK_RING - STICK_DOT) / 2, STICK_DOT, STICK_DOT);

    lv_coord_t tw = JC_PANEL_W - JC_TEXT_X - JC_IN;
    lv_obj_t *sh = gui_label(panel, &gui_font_30, GUI_COL_TEXT, "Stick");
    lv_obj_set_pos(sh, JC_TEXT_X, 112);
    s->centre = gui_hint(panel, tw, "Let go of the stick for a second, then "
                                    "roll it all the way around.");
    lv_obj_set_pos(s->centre, JC_TEXT_X, 158);
    s->range = gui_hint(panel, tw, "");
    lv_obj_set_pos(s->range, JC_TEXT_X, 248);

    /* The buttons, drawn as theme buttons in four even columns: one held
     * on the Joy-Con looks pressed here, and once let go it keeps the
     * theme's toggled look, so the plain ones are what is left to press. */
    const jc_btn_t *btns = side ? s_btn_r : s_btn_l;
    lv_coord_t pw = (JC_PANEL_W - 2 * JC_IN - 3 * 12) / 4;
    for (u32 i = 0; i < JC_BTNS; i++) {
        lv_obj_t *p = lv_cont_create(panel, NULL);
        lv_cont_set_style(p, &st_pill);
        lv_obj_set_click(p, false);
        lv_obj_set_size(p, pw, 50);
        lv_obj_set_pos(p, JC_IN + (i % 4) * (pw + 12), 352 + (i / 4) * 62);
        lv_obj_t *l = lv_label_create(p, NULL);
        lv_label_set_text(l, btns[i].name);
        lv_obj_align(l, NULL, LV_ALIGN_CENTER, 0, 0);
        s->btn[i] = p;
    }
}

static void _jc_status(jc_side_t *s)
{
    static const char *batt_name[] = { "empty", "very low", "low", "half", "full" };
    u8 lvl = s->batt >> 1;
    char buf[128];
    s_printf(buf, "#" GUI_HEX_OK " Connected#, battery %s",
             lvl < 5 ? batt_name[lvl] : "unknown");
    /* One line only, the stick ring sits right under it: "battery very low,
     * 12 drops" is the longest this gets, and it fits the panel. */
    if (s->drops)
        s_printf(buf + strlen(buf), ", #" GUI_HEX_WARN " %d drop%s#",
                 s->drops, s->drops > 1 ? "s" : "");
    if (s->buzz)
        s_printf(buf + strlen(buf), ", #" GUI_HEX_ACCENT " buzzing#");
    lv_label_set_text(s->status, buf);
}

/* The travel: how many directions it has been round, then whether each
 * reaches as far as the stick's own best one. */
static void _jc_range(jc_side_t *s, int side)
{
    static const char *dir[] = { "left", "right", "up", "down" };
    u32 reach[4], pct[4];
    char buf[128];
    u32 covered = gui_stick_range(&s->stick, reach, pct);
    if (covered < 4) {
        s_printf(buf, "Roll it all the way around: %d of 4 directions so far",
                 covered);
    } else {
        u32 lo = 0, hi = 0;
        for (u32 i = 0; i < 4; i++) {
            if (pct[i] < pct[lo]) lo = i;
            if (reach[i] > reach[hi]) hi = i;
        }
        /* The raw numbers, for the debug UART: what a good stick reads is
         * learnt from these. */
        char raw[96];
        s_printf(raw, "gui: jc %c stick rest %d,%d reach l%d r%d u%d d%d\n",
                 side ? 'R' : 'L', s->stick.rest_x, s->stick.rest_y,
                 reach[0], reach[1], reach[2], reach[3]);
        gui_uart_puts(raw);
        /* Without a calibration the reach is measured from the resting
         * point, so a stick that drifts reaches less one way: that is the
         * drift's doing, said above. With one it is measured from the
         * calibrated centre, as the firmware measures it. */
        if (!s->cal_ok && gui_stick_drift(&s->stick) > 15)
            s_printf(buf, "Range %d-100%% from where it rests", pct[lo]);
        else if (reach[hi] < STICK_SHORT)
            s_printf(buf, "#" GUI_HEX_WARN " " ICON_WARN "  Short travel all round#, "
                     "worn or not pushed to the edge");
        else if (pct[lo] >= 85)
            s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  Reaches the edge all round#%s",
                     s->cal_ok ? " (by its calibration)" : "");
        else
            s_printf(buf, "#" GUI_HEX_WARN " " ICON_WARN "  Short to the %s: %d%% of "
                     "its %stravel#, worn or not pushed to the edge", dir[lo], pct[lo],
                     s->cal_ok ? "calibrated " : "");
    }
    lv_label_set_text(s->range, buf);
}

/* Drift: where it rests against the middle of its travel, once the travel
 * is known. */
static void _jc_judge(jc_side_t *s)
{
    u32 reach[4], pct[4];
    char buf[128];
    if (gui_stick_range(&s->stick, reach, pct) < 4) {
        lv_label_set_text(s->centre, "Resting point noted. Now roll it all "
                                     "the way around.");
        return;
    }
    u32 off = gui_stick_drift(&s->stick);
    if (off <= 8)
        s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  Centred when let go#");
    else if (off <= 15)
        s_printf(buf, "#" GUI_HEX_WARN " " ICON_WARN "  Rests %d%% off centre, "
                 "slight drift possible#", off);
    else
        s_printf(buf, "#" GUI_HEX_FAIL " " ICON_FAIL "  Rests %d%% off centre: "
                 "the stick drifts#", off);
    lv_label_set_text(s->centre, buf);
}

static void _jc_side_update(int side, bool conn, u16 sx, u16 sy, u32 buttons,
                            u8 batt, bool lite)
{
    jc_side_t *s = &s_jc[side];

    if (conn != s->seen) {
        s->seen = conn;
        if (conn) {
            /* Not the comings and goings of the driver bringing it up again
             * after a sensor screen: those are not the rail's doing. */
            if (s->ever && !s_jc_resume_at)
                s->drops++;
            s->ever = true;
            s->batt = batt;
            _jc_status(s);
            /* Its calibration, from its flash, so the stick is judged as
             * the firmware judges it. A Lite's controls and third-party
             * pads have none; they are judged on their own travel. */
            if (!lite && !s->cal_ok)
                s->cal_ok = gui_jc_cal_read(side, &s->cal);
            if (s->cal_ok) {
                gui_stick_set_cal(&s->stick, s->cal.cx, s->cal.cy,
                                  s->cal.xmin, s->cal.xmax, s->cal.ymax, s->cal.ymin);
                char raw[96];
                s_printf(raw, "gui: jc %c cal %s centre %d,%d travel l%d r%d u%d d%d\n",
                         side ? 'R' : 'L', s->cal.user ? "user" : "factory",
                         s->cal.cx, s->cal.cy, s->cal.xmin, s->cal.xmax,
                         s->cal.ymax, s->cal.ymin);
                gui_uart_puts(raw);
            } else {
                gui_uart_puts(side ? "gui: jc R cal: none read\n"
                                   : "gui: jc L cal: none read\n");
            }
        } else {
            /* bdk's driver drops a Joy-Con it got no report from for 1.8 s
             * or whose rail says it is gone, and brings it up again; what
             * was learnt about the stick is kept across that. */
            lv_label_set_text(s->status,
                s_jc_resume_at ? "Reconnecting after a sensor test (a few seconds)" :
                s->ever ? "#" GUI_HEX_WARN " " ICON_WARN "  Connection lost:# "
                          "reseat it on the rail" :
                          "Not attached: slide it onto the rail");
        }
    }
    if (!conn)
        return;
    /* The Lite's two sticks sit half a turn apart: against a Joy-Con's,
     * the left one reads Y upside down and the right one X backwards, as
     * switchroot's joycon-serdev driver maps them. */
    if (lite && side)
        sx = 0xFFF - sx;
    else if (lite)
        sy = 0xFFF - sy;

    u32 ev = gui_stick_feed(&s->stick, sx, sy, get_tmr_ms());
    s32 half = (STICK_RING - STICK_DOT) / 2, px, py;
    gui_stick_dot(&s->stick, sx, sy, half, &px, &py);
    lv_obj_set_pos(s->dot, half + px, half + py);
    if (ev & STICK_RESTED)
        _jc_judge(s);
    if ((ev & STICK_GREW) && s->stick.rest_known) {
        _jc_range(s, side);
        /* The verdict, once the travel is complete and it has rested. */
        u32 reach[4], pct[4];
        if (gui_stick_range(&s->stick, reach, pct) == 4 && !s->stick.still_since)
            _jc_judge(s);
    }

    const jc_btn_t *btns = side ? s_btn_r : s_btn_l;
    for (u32 i = 0; i < JC_BTNS; i++) {
        bool on = (buttons >> btns[i].bit) & 1;
        bool was = (s->lit >> i) & 1;
        if (on == was)
            continue;
        s->lit ^= BIT(i);
        lv_cont_set_style(s->btn[i], on ? &st_pill_on : &st_pill_done);
        if (on && !(s->done & BIT(i))) {
            s->done |= BIT(i);
            _jc_count(s);
        }
    }
}

static void _jc_buzz(int side, bool on)
{
    jc_side_t *s = &s_jc[side];
    if (!s->seen)
        on = false;
    /* Enabled afresh at each burst: a Joy-Con the driver brought up again
     * after a sensor screen or a drop has lost it. Kept up while on: the
     * driver's own reports could mute it. */
    if (on && !s->buzz)
        gui_jc_rumble_enable(side);
    if (on || s->buzz)
        gui_jc_rumble(side, on);
    if (on != s->buzz) {
        s->buzz = on;
        if (s->seen)
            _jc_status(s);
    }
}

static void _jc_tick(void *param)
{
    jc_gamepad_rpt_t *jc = gui_jc_state();
    if (!jc)
        return;
    if (jc->sio_mode) {
        /* Lite: one controller, both halves always present. */
        _jc_side_update(0, jc->conn_l, jc->lstick_x, jc->lstick_y,
                        jc->buttons, jc->batt_info_l, true);
        _jc_side_update(1, jc->conn_l, jc->rstick_x, jc->rstick_y,
                        jc->buttons, jc->batt_info_l, true);
        return;
    }
    _jc_side_update(0, jc->conn_l, jc->lstick_x, jc->lstick_y, jc->buttons,
                    jc->batt_info_l, false);
    _jc_side_update(1, jc->conn_r, jc->rstick_x, jc->rstick_y, jc->buttons,
                    jc->batt_info_r, false);

    u32 now = get_tmr_ms();
    if (s_rumble_at) {
        u32 t = now - s_rumble_at;
        _jc_buzz(0, t < RUMBLE_ON_MS);
        _jc_buzz(1, t >= RUMBLE_ON_MS + RUMBLE_GAP_MS
                    && t < 2 * RUMBLE_ON_MS + RUMBLE_GAP_MS);
        if (t >= 2 * RUMBLE_ON_MS + RUMBLE_GAP_MS)
            s_rumble_at = 0;
    }
    /* A Joy-Con that has not come back by now is not coming. */
    if (s_jc_resume_at && now - s_jc_resume_at >= JC_RESUME_MS) {
        s_jc_resume_at = 0;
        for (int side = 0; side < 2; side++)
            if (!s_jc[side].ever)
                lv_label_set_text(s_jc[side].status,
                                  "Not attached: slide it onto the rail");
    }
}

static lv_res_t _act_rumble(lv_obj_t *b)
{
    if (!s_rumble_at)
        s_rumble_at = get_tmr_ms();
    return LV_RES_OK;
}
static lv_res_t _act_motion(lv_obj_t *b) { gui_show(gui_jc_motion); return LV_RES_OK; }
static lv_res_t _act_ir(lv_obj_t *b)     { gui_show(gui_jc_ir); return LV_RES_OK; }
static lv_res_t _act_bt(lv_obj_t *b)     { gui_show(gui_bt_test); return LV_RES_OK; }

static void _jc_leave(void)
{
    if (s_jc_task)
        lv_task_del(s_jc_task);
    s_jc_task = NULL;
    if (s_rumble_at) {
        s_rumble_at = 0;
        for (int side = 0; side < 2; side++)
            if (s_jc[side].buzz)
                gui_jc_rumble(side, false);
    }
}

void gui_joycon_test(void)
{
    lv_obj_t *win = gui_win_new(ICON_GAMEPAD " Joy-Con");
    gui_jc_nav(false);
    gui_on_leave(_jc_leave);
    /* The deeper tests, right to left after Close. A Lite's controls do
     * not speak the protocol the sensor ones need; Bluetooth is for a
     * controller off the rails, which a Lite can pair with too. */
    gui_win_btn(win, ICON_BLUETOOTH " Bluetooth", _act_bt);
    if (!gui_is_lite()) {
        gui_win_btn(win, ICON_SUN " IR Camera", _act_ir);
        gui_win_btn(win, ICON_CROSSHAIR " Motion", _act_motion);
        gui_win_btn(win, ICON_BOLT " Rumble", _act_rumble);
    }

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

    _jc_side_build(win, 0, GUI_MARGIN);
    _jc_side_build(win, 1, GUI_RIGHT - JC_PANEL_W);

    if (s_jc_task)
        lv_task_del(s_jc_task);
    s_jc_task = lv_task_create(_jc_tick, 20, LV_TASK_PRIO_MID, NULL);
    _jc_tick(NULL);
}

/* ======================================================================= */
/* Touchscreen                                                             */

#define TT_CELL 80
#define TT_COLS (GUI_W / TT_CELL)
#define TT_ROWS (GUI_H / TT_CELL)

/* The panel in the middle: half the screen wide, 32 px in from its frame. */
#define TT_PANEL_W  (GUI_W / 2)
#define TT_PANEL_H  300
#define TT_IN       32

static lv_obj_t *s_cell[TT_ROWS][TT_COLS];
static bool s_hit[TT_ROWS][TT_COLS];
static u32 s_hits;
static bool s_tt_over, s_tt_full;
static bool s_tt_pass;           /* this stroke is LVGL's: it began on Finish */
static int s_last_x = -1, s_last_y;
static u32 s_tt_up_at;           /* when the finger last lifted               */
static lv_task_t *s_tt_task;
static lv_obj_t *s_tt_info, *s_tt_count, *s_tt_finger, *s_tt_fin;
static lv_style_t st_cell, st_cell_hit, st_cell_miss, st_finger;

/* The panel comes back this long after the finger lifts. */
#define TT_PANEL_BACK_MS 1000

static void _tt_mark(int x, int y)
{
    int c = x / TT_CELL, r = y / TT_CELL;
    if (c < 0 || r < 0 || c >= TT_COLS || r >= TT_ROWS || s_hit[r][c])
        return;
    s_hit[r][c] = true;
    s_hits++;
    lv_obj_set_style(s_cell[r][c], &st_cell_hit);
}

static void _tt_count(void)
{
    char buf[64];
    s_printf(buf, "%d of %d areas", s_hits, TT_COLS * TT_ROWS);
    lv_label_set_text(s_tt_count, buf);
}

static void _tt_result(void);

/* Whether a touch lands on the Finish button, while there is one to see. */
static bool _tt_on_finish(int x, int y)
{
    if (lv_obj_get_hidden(s_tt_info))
        return false;
    lv_area_t a;
    lv_obj_get_coords(s_tt_fin, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

static bool _tt_sink(const touch_event_t *ev)
{
    if (s_tt_over)
        return false;            /* finished: taps go to the message box */
    if (!ev->touch) {
        bool pass = s_tt_pass;
        s_tt_pass = false;
        if (s_last_x >= 0) {
            s_last_x = -1;
            s_tt_up_at = get_tmr_ms();
            lv_obj_set_hidden(s_tt_finger, true);
        }
        return !pass;            /* LVGL must see the end of its tap too */
    }
    if (s_tt_pass)
        return false;
    int x = ev->x, y = ev->y;
    /* Samples arrive at the input rate, so a quick stroke leaves gaps:
     * fill in along the line from the last one. */
    if (s_last_x >= 0) {
        int dx = x - s_last_x, dy = y - s_last_y;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        int steps = (adx > ady ? adx : ady) / (TT_CELL / 4) + 1;
        for (int i = 1; i <= steps; i++)
            _tt_mark(s_last_x + dx * i / steps, s_last_y + dy * i / steps);
    } else {
        /* A new stroke. One that begins on Finish is a tap, LVGL's to
         * handle; any other takes the panel away while the finger is
         * down, so the squares under it can be seen turning. */
        if (_tt_on_finish(x, y)) {
            s_tt_pass = true;
            return false;
        }
        lv_obj_set_hidden(s_tt_info, true);
        _tt_mark(x, y);
    }
    s_last_x = x;
    s_last_y = y;
    lv_obj_set_hidden(s_tt_finger, false);
    lv_obj_set_pos(s_tt_finger, x - 30, y - 30);
    _tt_count();
    /* Not _tt_result() here: this runs inside LVGL's input read, and the
     * verdict deletes and creates objects. _tt_tick picks it up. */
    if (s_hits == TT_COLS * TT_ROWS)
        s_tt_full = true;
    return true;
}

static void _tt_tick(void *param)
{
    if (s_tt_full) {
        _tt_result();
        return;
    }
    /* The panel comes back a moment after the finger lifts. */
    if (!s_tt_over && s_last_x < 0 && lv_obj_get_hidden(s_tt_info)
        && get_tmr_ms() - s_tt_up_at >= TT_PANEL_BACK_MS) {
        lv_obj_set_hidden(s_tt_info, false);
    }
}

static void _tt_leave(void)
{
    if (s_tt_task)
        lv_task_del(s_tt_task);
    s_tt_task = NULL;
}

static lv_res_t _act_tt_finish(lv_obj_t *b)
{
    _tt_result();
    return LV_RES_OK;
}

static lv_res_t _tt_action(lv_obj_t *btns, const char *txt)
{
    if (strstr(txt, "Again"))
        gui_show(gui_touch_test);
    else
        gui_back();
    return LV_RES_OK;
}

/* The verdict: what was never touched turns red, and a message box says
 * how much of the screen answered. */
static void _tt_result(void)
{
    if (s_tt_over)
        return;
    s_tt_over = true;
    u32 total = TT_COLS * TT_ROWS, missed = total - s_hits;
    for (int r = 0; r < TT_ROWS; r++)
        for (int c = 0; c < TT_COLS; c++)
            if (!s_hit[r][c])
                lv_obj_set_style(s_cell[r][c], &st_cell_miss);
    lv_obj_set_hidden(s_tt_finger, true);
    lv_obj_set_hidden(s_tt_info, true);

    static char text[256];
    if (!missed)
        s_printf(text, "#" GUI_HEX_OK " The whole screen responds.#\n\n"
                 "Every part of the touchscreen answered your finger.");
    else if (!s_hits)
        s_printf(text, "#" GUI_HEX_FAIL " No touch was detected.#\n\n"
                 "The touchscreen may be disconnected or broken. The Touch and "
                 "sensors results of the health check say more.");
    else
        s_printf(text, "#" GUI_HEX_WARN " %d area%s did not respond.#\n\n"
                 "The red squares never registered a touch. If you did go over "
                 "them, that part of the touch panel is not working.",
                 missed, missed > 1 ? "s" : "");
    static const char *map[] = { "\221Again", "\221Done", "" };
    gui_mbox(text, map, _tt_action);
}

void gui_touch_test(void)
{
    lv_obj_t *root = gui_blank_new();
    gui_statusbar_hide(true);
    gui_touch_sink(_tt_sink);
    gui_on_leave(_tt_leave);

    gui_box_style(&st_cell, GUI_COL_BG_DARK, GUI_COL_BG_LIGHT, 0, 1);
    gui_box_style(&st_cell_hit, LV_COLOR_HEX(0x00775C), GUI_COL_ACCENT, 0, 1);
    gui_box_style(&st_cell_miss, LV_COLOR_HEX(0x8F2016), GUI_COL_FAIL, 0, 1);
    gui_box_style(&st_finger, GUI_COL_ACCENT, LV_COLOR_WHITE, LV_RADIUS_CIRCLE, 4);
    st_finger.body.opa = LV_OPA_60;

    /* The grid covers the whole panel: dead spots are often along an
     * edge. */
    memset(s_hit, 0, sizeof(s_hit));
    s_hits = 0;
    s_tt_over = false;
    s_tt_full = false;
    s_tt_pass = false;
    s_last_x = -1;
    _tt_leave();
    s_tt_task = lv_task_create(_tt_tick, 50, LV_TASK_PRIO_MID, NULL);
    for (int r = 0; r < TT_ROWS; r++)
        for (int c = 0; c < TT_COLS; c++)
            s_cell[r][c] = gui_box(root, &st_cell, c * TT_CELL, r * TT_CELL,
                                TT_CELL, TT_CELL);

    /* One line taller when the touchscreen failed, so its warning gets a
     * line of its own instead of crowding the bottom inset. */
    bool touch_ok = gui_touch_ok();
    lv_coord_t ph = TT_PANEL_H + (touch_ok ? 0 : 36);
    s_tt_info = gui_panel(root, (GUI_W - TT_PANEL_W) / 2, (GUI_H - ph) / 2,
                          TT_PANEL_W, ph);
    lv_obj_t *t = gui_label(s_tt_info, &gui_font_30, GUI_COL_TEXT,
                            "#" GUI_HEX_ACCENT " " ICON_TOUCH "#  Touchscreen Test");
    lv_obj_set_pos(t, TT_IN, 26);
    lv_obj_t *d = gui_hint(s_tt_info, TT_PANEL_W - 2 * TT_IN,
        "Slide a finger over the whole screen until every square turns "
        "green. Squares that stay dark do not respond.");
    lv_obj_set_pos(d, TT_IN, 80);
    lv_obj_t *fin = gui_button(s_tt_info, ICON_CHECK " Finish", _act_tt_finish);
    lv_obj_align(fin, NULL, LV_ALIGN_IN_BOTTOM_RIGHT, -TT_IN, -TT_IN);
    lv_group_focus_obj(fin);
    s_tt_fin = fin;

    /* The short lines on the left sit 36 px apart, the last of them on the
     * Finish button's centre line. */
    lv_coord_t y = lv_obj_get_y(fin) + lv_obj_get_height(fin) / 2
                   - gui_font_20.h_px / 2 - (touch_ok ? 1 : 2) * 36;
    s_tt_count = gui_label(s_tt_info, &gui_font_20, GUI_COL_TEXT, "");
    lv_obj_set_pos(s_tt_count, TT_IN, y);
    _tt_count();
    lv_obj_t *hint = gui_hint(s_tt_info, 0, "POWER finishes too.");
    lv_obj_set_pos(hint, TT_IN, y + 36);
    if (!touch_ok) {
        lv_obj_t *w = gui_label(s_tt_info, &gui_font_20, GUI_COL_WARN,
                                ICON_WARN "  The touchscreen did not start.");
        lv_obj_set_pos(w, TT_IN, y + 72);
    }

    s_tt_finger = gui_box(root, &st_finger, 0, 0, 60, 60);
    lv_obj_set_hidden(s_tt_finger, true);
}
