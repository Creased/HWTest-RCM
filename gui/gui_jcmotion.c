/*
 * hwtest GUI - the Joy-Cons' motion sensors.
 *
 * Each Joy-Con has an accelerometer and a gyroscope in one chip, and the
 * console reads them in every input report once asked to (subcommand
 * 0x40). bdk's driver never asks and parses only the buttons and sticks,
 * so this screen takes the rails over (gui_jcwire.h), switches the sensors
 * on and polls the Joy-Cons itself: a needle that hangs the way gravity
 * pulls, three bars that swing when the Joy-Con is turned, and the numbers
 * behind them.
 *
 * Raw units: 4096 per g on the accelerometer (the +/-8 g range), 70 mdps
 * per count on the gyroscope (the +/-2000 dps range), as dekuNukem's notes
 * give them. No calibration is applied: what is judged is that the chip
 * answers, that it reads about 1 g at rest, and that it moves.
 */
#include "gui.h"
#include "gui_jcwire.h"

#define MO_PANEL_W  ((GUI_CONTENT_W - 36) / 2)
#define MO_IN       24
#define MO_RING     200                  /* the gauge the needle swings in */
#define MO_TIP      16                   /* the needle's end               */
#define MO_REACH    (MO_RING / 2 - MO_TIP)   /* its length at 1 g          */
#define MO_TEXT_X   (MO_IN + MO_RING + 24)
#define MO_BAR_X    (MO_IN + 36)
#define MO_BAR_W    (MO_PANEL_W - MO_BAR_X - MO_IN)
#define MO_BAR_H    18
#define MO_MARK_W   12
#define MO_POLL_MS  15
#define MO_REST_MS  600
#define MO_SILENT_MS 1500
#define MO_LOST_MS  3000                 /* silent this long: started over */

#define SUBCMD_IMU   0x40

enum { V_NONE, V_READING, V_OK, V_SILENT, V_STOPPED };

typedef struct {
    lv_obj_t *status, *needle, *tip, *verdict, *num, *mark[3];
    lv_point_t pts[2];       /* the needle's, which lv_line keeps pointing at */
    jc_rail_t rail;          /* its rail: owned and up when JCR_RUN       */
    bool ok, moved;
    u32  first_at;           /* when polling began                        */
    u32  rest_since;         /* reading about 1 g and still since...      */
    u32  data_at;            /* the last report with a sample that was not all zero */
    u32  rpt_at;             /* the last report of any kind               */
    int  said;               /* the verdict on screen (V_*)...            */
    u32  said_at;            /* ...and when it was last written           */
    s32  acc[3], gyr[3];     /* the newest sample, raw                    */
} mo_side_t;

static const lv_coord_t s_bar_y[3] = { 382, 416, 450 };   /* the gyro bars */

static mo_side_t s_mo[2];
static lv_task_t *s_mo_task;
static lv_style_t st_ring, st_cross, st_needle, st_tip, st_track, st_mark;

static u32 _isqrt(u32 v)
{
    u32 r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static s32 _clamp(s32 v, s32 lim)
{
    return v < -lim ? -lim : v > lim ? lim : v;
}

static s32 abs(s32 v) { return v < 0 ? -v : v; }

/* Milli-g and degrees per second from the raw counts. */
static s32 _mg(s32 raw)  { return raw * 244 / 1000; }
static s32 _dps(s32 raw) { return raw * 70 / 1000; }

static void _mo_show(mo_side_t *s)
{
    /* The needle: from the gauge's centre, as far as gravity pulls in the
     * Joy-Con's own plane, full length at 1 g. */
    s32 px = _clamp(_mg(s->acc[0]), 1000) * MO_REACH / 1000;
    s32 py = -_clamp(_mg(s->acc[1]), 1000) * MO_REACH / 1000;
    s->pts[0].x = MO_RING / 2;
    s->pts[0].y = MO_RING / 2;
    s->pts[1].x = MO_RING / 2 + px;
    s->pts[1].y = MO_RING / 2 + py;
    lv_line_set_points(s->needle, s->pts, 2);
    lv_obj_set_pos(s->tip, MO_RING / 2 + px - MO_TIP / 2, MO_RING / 2 + py - MO_TIP / 2);

    for (u32 i = 0; i < 3; i++) {
        s32 off = _clamp(_dps(s->gyr[i]), 2000) * ((MO_BAR_W - MO_MARK_W) / 2) / 2000;
        lv_obj_set_pos(s->mark[i], MO_BAR_X + (MO_BAR_W - MO_MARK_W) / 2 + off,
                       s_bar_y[i] - MO_BAR_H / 2);
    }

    char buf[128];
    s32 a[3], g[3];
    for (u32 i = 0; i < 3; i++) {
        a[i] = _mg(s->acc[i]);
        g[i] = _dps(s->gyr[i]);
    }
    s_printf(buf, "%c%d.%02d  %c%d.%02d  %c%d.%02d g\n%c%d  %c%d  %c%d deg/s",
             a[0] < 0 ? '-' : '+', abs(a[0]) / 1000, abs(a[0]) % 1000 / 10,
             a[1] < 0 ? '-' : '+', abs(a[1]) / 1000, abs(a[1]) % 1000 / 10,
             a[2] < 0 ? '-' : '+', abs(a[2]) / 1000, abs(a[2]) % 1000 / 10,
             g[0] < 0 ? '-' : '+', abs(g[0]), g[1] < 0 ? '-' : '+', abs(g[1]),
             g[2] < 0 ? '-' : '+', abs(g[2]));
    lv_label_set_text(s->num, buf);
}

/* The verdict line, rewritten only when it changes: a label's text is laid
 * out again each time it is set. */
static void _mo_say(mo_side_t *s, int what, u32 now, const char *text)
{
    if (what == s->said && (what != V_OK || now - s->said_at < 500))
        return;
    s->said = what;
    s->said_at = now;
    lv_label_set_text(s->verdict, text);
}

/* What the readings say so far. */
static void _mo_judge(mo_side_t *s, u32 now)
{
    char buf[160];
    bool zero = !s->acc[0] && !s->acc[1] && !s->acc[2] && !s->gyr[0] && !s->gyr[1] && !s->gyr[2];
    if (!zero)
        s->data_at = now;
    if (!s->data_at) {
        if (now - s->first_at > MO_SILENT_MS)
            _mo_say(s, V_SILENT, now, "#" GUI_HEX_FAIL " " ICON_FAIL
                    "  No readings.# The sensor sends nothing.");
        return;
    }
    s32 mg[3] = { _mg(s->acc[0]), _mg(s->acc[1]), _mg(s->acc[2]) };
    u32 g = _isqrt(mg[0] * mg[0] + mg[1] * mg[1] + mg[2] * mg[2]);
    s32 spin = 0;
    for (u32 i = 0; i < 3; i++) {
        s32 d = abs(_dps(s->gyr[i]));
        if (d > spin)
            spin = d;
    }
    if (spin > 250)
        s->moved = true;
    bool still = g >= 750 && g <= 1250 && spin < 100;
    if (!still)
        s->rest_since = 0;
    else if (!s->rest_since)
        s->rest_since = now;
    else if (now - s->rest_since >= MO_REST_MS)
        s->ok = true;

    if (s->ok) {
        s_printf(buf, "#" GUI_HEX_OK " " ICON_OK "  The sensor works.# At rest it reads "
                 "%d.%02d g%s.", g / 1000, g % 1000 / 10,
                 s->moved ? ", and it felt the turn" : "");
        _mo_say(s, V_OK, now, buf);
    } else if (now - s->data_at > MO_SILENT_MS) {
        _mo_say(s, V_STOPPED, now, "#" GUI_HEX_WARN " " ICON_WARN "  Readings stopped#");
    } else {
        _mo_say(s, V_READING, now, "Reading: hold it still a moment");
    }
}

/* A side's readings forgotten: a Joy-Con taken (again) starts afresh. */
static void _mo_reset(mo_side_t *s)
{
    memset(s->acc, 0, sizeof(s->acc));
    memset(s->gyr, 0, sizeof(s->gyr));
    s->ok = false;
    s->moved = false;
    s->rest_since = 0;
    s->data_at = 0;
    s->said = V_NONE;
    lv_label_set_text(s->verdict, "");
    _mo_show(s);
}

static void _mo_status(mo_side_t *s, const char *text)
{
    if (strcmp(lv_label_get_text(s->status), text))
        lv_label_set_text(s->status, text);
}

/* What the panel says while its rail has no Joy-Con up. */
static void _mo_rail_status(mo_side_t *s)
{
    switch (s->rail.state) {
    case JCR_OFF:
        _mo_status(s, "Not attached: slide it onto the rail");
        break;
    case JCR_UP:
        _mo_status(s, s->rail.drops ? "It went silent, connecting again"
                                    : "Attached, connecting");
        break;
    case JCR_STUCK:
        _mo_status(s, s->rail.drops
            ? "#" GUI_HEX_WARN " " ICON_WARN "  It keeps going silent:# reseat it"
            : "#" GUI_HEX_WARN " " ICON_WARN "  Attached, but it does not answer:# reseat it");
        break;
    }
}

/* Its Joy-Con up and owned: sensor on, readings from scratch. */
static void _mo_sensor_on(int side)
{
    mo_side_t *s = &s_mo[side];
    _mo_reset(s);
    s->first_at = s->rpt_at = get_tmr_ms();
    static const u8 on = 1;
    bool answered = gui_jc_subcmd(side, SUBCMD_IMU, &on, 1, NULL);
    _mo_status(s, answered ? "#" GUI_HEX_OK " Connected#, sensor on"
                           : "#" GUI_HEX_WARN " " ICON_WARN "  The sensor did not answer#");
    /* For the bench: the left Joy-Con's side of this reaches the debug
     * UART while the adapter holds the right rail. */
    gui_uart_puts(side ? (answered ? "gui: motion R sensor on\n" : "gui: motion R no answer\n")
                       : (answered ? "gui: motion L sensor on\n" : "gui: motion L no answer\n"));
}

static void _mo_tick(void *param)
{
    /* Each rail on its own: a Joy-Con slid on is started, one slid off is
     * let go, and the other side carries on. */
    for (int side = 0; side < 2; side++) {
        mo_side_t *s = &s_mo[side];
        int ev = gui_jc_rail_tick(&s->rail, side);
        if (ev == JCR_EV_UP) {
            _mo_sensor_on(side);
        } else if (ev != JCR_EV_NONE) {
            if (ev == JCR_EV_GONE)
                gui_uart_puts(side ? "gui: motion R left its rail\n"
                                   : "gui: motion L left its rail\n");
            _mo_rail_status(s);
        }
    }
    /* Read after the rails: a Joy-Con started just now stamped its readings
     * later than a time read before, and the difference, unsigned, wrapped
     * round to a silence of years (on the bench: "It keeps going silent"
     * the moment a Joy-Con came up). */
    u32 now = get_tmr_ms();
    for (int side = 0; side < 2; side++)
        if (s_mo[side].rail.state == JCR_RUN)
            gui_jc_poll(side);
    for (int side = 0; side < 2; side++) {
        mo_side_t *s = &s_mo[side];
        if (s->rail.state != JCR_RUN)
            continue;
        u8 rpt[JC_RPT_LEN];
        bool got = false;
        u32 n;
        while ((n = gui_jc_recv(side, rpt, sizeof(rpt), got ? 1 : 8))) {
            /* Three samples of 12 bytes from byte 13, 5 ms apart: the last
             * complete one is the newest. */
            u32 samples = n > 13 ? (n - 13) / 12 : 0;
            if (rpt[0] != JC_IN_STD || !samples)
                continue;
            const u8 *p = rpt + 13 + (samples > 3 ? 2 : samples - 1) * 12;
            for (u32 i = 0; i < 3; i++) {
                s->acc[i] = (s16)(p[2 * i] | p[2 * i + 1] << 8);
                s->gyr[i] = (s16)(p[6 + 2 * i] | p[6 + 2 * i + 1] << 8);
            }
            got = true;
        }
        if (got) {
            s->rpt_at = now;
            if (!s->data_at && (s->acc[0] || s->acc[1] || s->acc[2])) {
                char raw[96];
                s_printf(raw, "gui: motion %c first sample acc %d %d %d gyr %d %d %d\n",
                         side ? 'R' : 'L', s->acc[0], s->acc[1], s->acc[2],
                         s->gyr[0], s->gyr[1], s->gyr[2]);
                gui_uart_puts(raw);
            }
            _mo_show(s);
        } else if (now - s->rpt_at > MO_LOST_MS) {
            /* On its rail and silent: started again, this side alone. */
            gui_uart_puts(side ? "gui: motion R silent, starting it again\n"
                               : "gui: motion L silent, starting it again\n");
            gui_jc_rail_lost(&s->rail);
            _mo_rail_status(s);
            continue;
        }
        _mo_judge(s, now);
    }
}

static void _mo_side_build(lv_obj_t *win, int side, lv_coord_t x0)
{
    mo_side_t *s = &s_mo[side];
    memset(s, 0, sizeof(*s));
    lv_obj_t *panel = gui_panel(win, x0, GUI_TOP, MO_PANEL_W, GUI_CONTENT_H);

    lv_obj_t *t = gui_label(panel, &gui_font_30, GUI_COL_TEXT,
        side ? "#" GUI_HEX_ACCENT " " ICON_GAMEPAD "#  Right Joy-Con"
             : "#" GUI_HEX_ACCENT " " ICON_GAMEPAD "#  Left Joy-Con");
    lv_obj_set_pos(t, MO_IN, 26);
    s->status = gui_hint(panel, MO_PANEL_W - 2 * MO_IN, "");
    lv_obj_set_pos(s->status, MO_IN, 68);

    /* The gauge: a crosshair in a ring, and the needle over it. */
    lv_obj_t *ring = gui_box(panel, &st_ring, MO_IN, 108, MO_RING, MO_RING);
    gui_box(ring, &st_cross, 12, MO_RING / 2 - 1, MO_RING - 24, 2);
    gui_box(ring, &st_cross, MO_RING / 2 - 1, 12, 2, MO_RING - 24);
    s->needle = lv_line_create(ring, NULL);
    lv_line_set_auto_size(s->needle, false);
    lv_line_set_style(s->needle, &st_needle);
    lv_obj_set_size(s->needle, MO_RING, MO_RING);
    lv_obj_set_pos(s->needle, 0, 0);
    s->tip = gui_box(ring, &st_tip, (MO_RING - MO_TIP) / 2, (MO_RING - MO_TIP) / 2,
                     MO_TIP, MO_TIP);

    lv_coord_t tw = MO_PANEL_W - MO_TEXT_X - MO_IN;
    lv_obj_t *th = gui_label(panel, &gui_font_30, GUI_COL_TEXT, "Tilt");
    lv_obj_set_pos(th, MO_TEXT_X, 112);
    lv_obj_t *d = gui_hint(panel, tw, "The needle hangs the way gravity pulls. "
                                      "Hold the Joy-Con upright and it points "
                                      "down; lay it flat and it shrinks to a dot.");
    lv_obj_set_pos(d, MO_TEXT_X, 158);
    s->verdict = gui_hint(panel, tw, "");
    lv_obj_set_pos(s->verdict, MO_TEXT_X, 158 + lv_obj_get_height(d) + 20);

    lv_obj_t *sh = gui_label(panel, &gui_font_30, GUI_COL_TEXT, "Turn");
    lv_obj_set_pos(sh, MO_IN, 334);
    static const char *axis[] = { "X", "Y", "Z" };
    for (u32 i = 0; i < 3; i++) {
        lv_coord_t y = s_bar_y[i];
        lv_obj_t *l = gui_hint(panel, 0, axis[i]);
        lv_obj_set_pos(l, MO_IN, y - 4);
        lv_obj_t *track = gui_box(panel, &st_track, MO_BAR_X, y, MO_BAR_W, MO_BAR_H);
        gui_box(track, &st_cross, (MO_BAR_W - 2) / 2, 0, 2, MO_BAR_H);
        /* The mark stands taller than its bar, so it is the panel's, drawn
         * over the bar. */
        s->mark[i] = gui_box(panel, &st_mark, MO_BAR_X + (MO_BAR_W - MO_MARK_W) / 2,
                             y - MO_BAR_H / 2, MO_MARK_W, 2 * MO_BAR_H);
    }
    s->num = gui_label(panel, &gui_font_20, GUI_COL_HINT, "");
    lv_obj_set_style(s->num, &gui_st_mono);
    lv_obj_set_pos(s->num, MO_BAR_X, s_bar_y[2] + 2 * MO_BAR_H + 2);
    /* Everything is there now: the needle, the marks and the numbers at
     * rest, until the first report. */
    _mo_show(s);
}

static void _mo_leave(void)
{
    if (s_mo_task)
        lv_task_del(s_mo_task);
    s_mo_task = NULL;
    static const u8 off = 0;
    bool used = false;
    for (int side = 0; side < 2; side++) {
        mo_side_t *s = &s_mo[side];
        if (s->rail.state == JCR_RUN)
            gui_jc_subcmd(side, SUBCMD_IMU, &off, 1, NULL);
        used |= gui_jc_rail_end(&s->rail, side);
    }
    if (used)
        gui_jc_test_resumed();
}

void gui_jc_motion(void)
{
    lv_obj_t *win = gui_win_new(ICON_GAMEPAD " Motion Sensors");
    gui_set_back(gui_joycon_test);
    gui_jc_nav(false);
    gui_on_leave(_mo_leave);

    if (gui_is_lite()) {
        lv_obj_t *l = gui_hint(win, GUI_CONTENT_W * 2 / 3,
            "Not available on a Switch Lite: its built-in controls do not "
            "speak the Joy-Con protocol.");
        lv_obj_set_pos(l, GUI_MARGIN, GUI_TOP);
        return;
    }

    gui_box_style(&st_ring, GUI_COL_BG_DARK, GUI_COL_LINE, LV_RADIUS_CIRCLE, 2);
    gui_box_style(&st_cross, GUI_COL_LINE, GUI_COL_LINE, 0, 0);
    gui_box_style(&st_tip, GUI_COL_ACCENT, GUI_COL_ACCENT, LV_RADIUS_CIRCLE, 0);
    gui_box_style(&st_track, GUI_COL_BG_DARK, GUI_COL_LINE, 4, 2);
    gui_box_style(&st_mark, GUI_COL_ACCENT, GUI_COL_ACCENT, 3, 0);
    lv_style_copy(&st_needle, &lv_style_plain);
    st_needle.line.color = GUI_COL_ACCENT;
    st_needle.line.width = 6;
    st_needle.line.opa = LV_OPA_COVER;

    _mo_side_build(win, 0, GUI_MARGIN);
    _mo_side_build(win, 1, GUI_RIGHT - MO_PANEL_W);

    /* What is on the rails: one the driver has up is taken as it is, one
     * that does not answer yet is started here, and from now on each rail
     * is watched on its own. */
    for (int side = 0; side < 2; side++) {
        if (gui_jc_rail_start(&s_mo[side].rail, side))
            _mo_sensor_on(side);
        else
            _mo_rail_status(&s_mo[side]);
    }

    if (s_mo_task)
        lv_task_del(s_mo_task);
    s_mo_task = lv_task_create(_mo_tick, MO_POLL_MS, LV_TASK_PRIO_MID, NULL);
}
