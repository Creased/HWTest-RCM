/*
 * hwtest GUI - look and building blocks.
 *
 * Nyx's look, built the way Nyx builds it: bdk's hekate theme draws the
 * widgets, and these helpers only arrange them as Nyx's screens do - the
 * tab view with its 256 px buttons, a header with a Close button on every
 * other screen, flat text buttons, message boxes over a darkened screen,
 * the status bar - on the one grid gui.h lays down. The one addition is
 * the focus outline that VOL+/VOL- and the Joy-Con move: Nyx is driven by
 * touch or a pointer, and hwtest has to work with neither.
 */
#include "gui.h"
#include <power/bq24193.h>

#ifndef GUI_VERSION
#define GUI_VERSION ""
#endif

lv_style_t gui_st_mono;

static lv_theme_t *s_th;
static lv_style_t st_bg, st_hint, st_text_rel, st_text_pr, st_darken, st_bar;
static lv_style_t st_page, st_label[48];
static lv_style_t st_tab[4], st_hdr_rel, st_hdr_pr, st_mbox_btns;
static u32 st_label_n;

static lv_group_t *s_group;
static lv_obj_t *s_root, *s_page, *s_mbox, *s_mbox_bg;
static lv_obj_t *s_hdr, *s_hdr_close;
static lv_coord_t s_hdr_x;      /* where the next header button's text ends */
static void *s_hdr_first;       /* the focus list node of the leftmost one   */
static lv_obj_t *s_bar, *s_bar_batt, *s_bar_more;
static char s_name[32];
static serial_res_t s_serial = SERIAL_NO_STORAGE;

lv_theme_t *gui_th(void)    { return s_th; }
lv_group_t *gui_group(void) { return s_group; }
const char *gui_console_name(void) { return s_name; }

/* A focused object looks like a pressed hekate button: a turquoise
 * outline. */
static void _focus_style(lv_style_t *st)
{
    st->body.border.color = GUI_COL_ACCENT;
    st->body.border.width = 4;
    st->body.border.opa   = LV_OPA_COVER;
    st->body.border.part  = LV_BORDER_FULL;
}

/* Shown: neither the object nor any parent of it hidden. */
static bool _shown(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_get_hidden(o))
            return false;
    return true;
}

static void _focus_page(lv_obj_t *f)
{
    if (f && s_page && lv_obj_get_parent(f) == lv_page_get_scrl(s_page))
        lv_page_focus(s_page, f, 150);
}

static bool s_refocusing;

/* LVGL 5's group steps onto hidden objects as onto any other: the focus goes
 * on past them, the way it was going, then the other way from an end of a
 * group that does not wrap. */
static void _focus_past_hidden(lv_group_t *g, bool back)
{
    u32 n = 0;
    lv_obj_t **i;
    LL_READ(g->obj_ll, i)
        n++;
    s_refocusing = true;
    for (u32 pass = 0; pass < 2; pass++, back = !back) {
        for (u32 k = 0; k < n && !_shown(lv_group_get_focused(g)); k++) {
            if (back)
                lv_group_focus_prev(g);
            else
                lv_group_focus_next(g);
        }
    }
    s_refocusing = false;
}

/* The focused object that hid, and where the focus went then: it comes back
 * when the object shows again and the focus has not moved since. */
static lv_obj_t *s_hid_from, *s_hid_to;

/* Off a hidden object that has the focus, to the one before it: on a header
 * that is the one on its left, away from Close, so a press meant for the
 * hidden button does not leave the screen. */
static void _focus_off(lv_obj_t *f)
{
    _focus_past_hidden(s_group, true);
    s_hid_from = f;
    s_hid_to = lv_group_get_focused(s_group);
    _focus_page(s_hid_to);
}

static void _focus_follow(lv_group_t *g)
{
    if (s_refocusing)
        return;                 /* a step past a hidden one: the last one follows */
    lv_obj_t *f = lv_group_get_focused(g);
    if (f && !_shown(f)) {
        lv_indev_t *in = lv_indev_get_act();
        u32 key = lv_indev_get_type(in) == LV_INDEV_TYPE_KEYPAD ? in->proc.last_key : 0;
        if (key != LV_GROUP_KEY_NEXT && key != LV_GROUP_KEY_PREV) {
            /* A touch on a button that hid itself: LVGL focuses what was
             * touched after its action ran. */
            _focus_off(f);
            return;
        }
        /* VOL or the D-pad: on past it the way they moved. */
        _focus_past_hidden(g, key == LV_GROUP_KEY_PREV);
        f = lv_group_get_focused(g);
    }
    _focus_page(f);
}

void gui_hide(lv_obj_t *obj, bool hide)
{
    lv_obj_set_hidden(obj, hide);
    lv_obj_t *f = lv_group_get_focused(s_group);
    if (hide) {
        if (f && !_shown(f))
            _focus_off(f);
    } else if (s_hid_from && _shown(s_hid_from)) {
        if (f == s_hid_to)
            lv_group_focus_obj(s_hid_from);
        s_hid_from = NULL;
    }
}

/* The status bar names the console by the serial on its sticker. That lives
 * in the encrypted PRODINFO, readable only with sd:/switch/prod.keys (from
 * Lockpick_RCM); without the keys the bar says so instead. */
static void _console_name(void)
{
    s_serial = console_serial(s_name);
    if (s_serial != SERIAL_OK)
        s_name[0] = 0;
}

void gui_theme_init(void)
{
    /* bdk's LVGL spaces letters 3 px apart for Nyx's fonts, whose glyphs
     * are cut to their ink. Ours carry the face's own advance, so the
     * spacing is already in the glyph; zero it before anything copies the
     * built-in styles. */
    lv_style_t *builtin[] = {
        &lv_style_scr, &lv_style_transp, &lv_style_transp_fit,
        &lv_style_transp_tight, &lv_style_plain, &lv_style_plain_color,
        &lv_style_pretty, &lv_style_pretty_color, &lv_style_btn_rel,
        &lv_style_btn_pr, &lv_style_btn_tgl_rel, &lv_style_btn_tgl_pr,
        &lv_style_btn_ina,
    };
    for (u32 i = 0; i < ARRAY_SIZE(builtin); i++)
        builtin[i]->text.letter_space = 0;

    s_th = lv_theme_hekate_init(GUI_THEME_BG, GUI_THEME_HUE, &gui_font_30);
    lv_theme_set_current(s_th);
    gui_fonts_init();
    _console_name();

    /* The text sits in the middle of a bar-high box. */
    lv_coord_t bar_pad = (GUI_BAR_H - gui_font_30.h_px) / 2;

    /* Nyx's own extra styles: the plain background, the small hint text,
     * report text, flat text buttons and the message box's dimming. */
    lv_style_copy(&st_bg, &lv_style_plain);
    st_bg.body.main_color = GUI_COL_BG;
    st_bg.body.grad_color = GUI_COL_BG;

    lv_style_copy(&st_hint, s_th->label.hint);
    st_hint.text.font = &gui_font_20;
    st_hint.text.letter_space = 1;

    lv_style_copy(&gui_st_mono, &lv_style_plain);
    gui_st_mono.body.opa = LV_OPA_TRANSP;
    gui_st_mono.text.color = LV_COLOR_HEX(0xD8D8D8);
    gui_st_mono.text.font = &gui_font_mono;

    lv_style_copy(&st_text_rel, s_th->btn.rel);
    st_text_rel.body.shadow.width = 0;
    st_text_rel.body.border.width = 0;
    st_text_rel.body.radius = 0;
    st_text_rel.body.padding.hor = GUI_TEXT_PAD;
    st_text_rel.body.padding.ver = bar_pad;
    st_text_rel.body.empty = 1;
    lv_style_copy(&st_text_pr, &st_text_rel);
    st_text_pr.body.main_color = GUI_COL_BG_LIGHT;
    st_text_pr.body.grad_color = GUI_COL_BG_LIGHT;
    st_text_pr.text.color = s_th->btn.pr->text.color;
    st_text_pr.body.empty = 0;

    /* The header's buttons: the theme's, with the text boxed as the flat
     * buttons are, so neighbours keep apart. */
    lv_style_copy(&st_hdr_rel, s_th->win.btn.rel);
    st_hdr_rel.body.padding.hor = GUI_TEXT_PAD;
    lv_style_copy(&st_hdr_pr, s_th->win.btn.pr);
    st_hdr_pr.body.padding.hor = GUI_TEXT_PAD;

    /* The tab bar, at the bar height. */
    lv_style_t *tab_src[4] = {
        s_th->tabview.btn.rel, s_th->tabview.btn.pr,
        s_th->tabview.btn.tgl_rel, s_th->tabview.btn.tgl_pr,
    };
    for (u32 i = 0; i < 4; i++) {
        lv_style_copy(&st_tab[i], tab_src[i]);
        st_tab[i].body.padding.ver = bar_pad;
    }

    /* A message box's buttons, 20 px apart instead of touching. */
    lv_style_copy(&st_mbox_btns, &lv_style_transp_tight);
    st_mbox_btns.body.padding.inner = GUI_TEXT_PAD;

    lv_style_copy(&st_darken, &lv_style_plain);
    st_darken.body.main_color = LV_COLOR_BLACK;
    st_darken.body.grad_color = LV_COLOR_BLACK;
    st_darken.body.opa = LV_OPA_30;

    lv_style_copy(&st_bar, &lv_style_plain_color);
    st_bar.body.opa = LV_OPA_0;
    st_bar.body.shadow.width = 0;

    lv_style_copy(&st_page, &lv_style_transp_tight);

    s_group = lv_group_create();
    lv_group_set_style_mod_cb(s_group, _focus_style);
    lv_group_set_focus_cb(s_group, _focus_follow);
}

/* Label styles are cached per (font, colour): LVGL keeps a pointer to the
 * style, so it has to outlive the label. The screens use a few dozen
 * combinations in all; past the table they come from LVGL's pool and are
 * never freed, which stays bounded for the same reason. */
static lv_style_t *_label_style(const lv_font_t *font, lv_color_t col)
{
    for (u32 i = 0; i < st_label_n; i++)
        if (st_label[i].text.font == font &&
            st_label[i].text.color.full == col.full)
            return &st_label[i];
    lv_style_t *st = st_label_n < ARRAY_SIZE(st_label) ?
                     &st_label[st_label_n++] : lv_mem_alloc(sizeof(lv_style_t));
    lv_style_copy(st, s_th->label.prim);
    st->text.font  = font;
    st->text.color = col;
    return st;
}

lv_obj_t *gui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col,
                    const char *text)
{
    lv_obj_t *l = lv_label_create(parent, NULL);
    lv_label_set_style(l, _label_style(font, col));
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    lv_obj_set_click(l, false);
    return l;
}

lv_obj_t *gui_label_wrap(lv_obj_t *parent, const lv_font_t *font,
                         lv_color_t col, lv_coord_t w, const char *text)
{
    lv_obj_t *l = gui_label(parent, font, col, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_BREAK);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, text);
    return l;
}

/* Nyx's small grey text, for taglines and explanations. */
lv_obj_t *gui_hint(lv_obj_t *parent, lv_coord_t w, const char *text)
{
    lv_obj_t *l = lv_label_create(parent, NULL);
    lv_label_set_style(l, &st_hint);
    lv_label_set_recolor(l, true);
    if (w) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_BREAK);
        lv_obj_set_width(l, w);
    }
    lv_label_set_text(l, text);
    lv_obj_set_click(l, false);
    return l;
}

/* ---- screens ---------------------------------------------------------- */

/* The previous screen goes, and with it its objects in the focus group and
 * any message box still open over it. */
static void _root_reset(void)
{
    if (s_mbox_bg)
        lv_obj_del(s_mbox_bg);
    s_mbox_bg = NULL;
    s_mbox = NULL;
    lv_group_focus_freeze(s_group, false);
    if (s_root)
        lv_obj_del(s_root);
    s_root = NULL;
    s_page = NULL;
    s_hdr = NULL;
    s_hid_from = NULL;
    s_hid_to = NULL;
}

static lv_obj_t *_root_new(void)
{
    s_root = lv_cont_create(lv_scr_act(), NULL);
    lv_cont_set_style(s_root, &st_bg);
    lv_cont_set_layout(s_root, LV_LAYOUT_OFF);
    lv_obj_set_size(s_root, GUI_W, GUI_H);
    return s_root;
}

/* A tab's page as a plain canvas: no padding, no fit, no scrolling, so its
 * children sit where they are put, GUI_BAR_H under the screen's top. */
static void _tab_plain(lv_obj_t *tab)
{
    lv_page_set_style(tab, LV_PAGE_STYLE_BG, &st_page);
    lv_page_set_style(tab, LV_PAGE_STYLE_SCRL, &st_page);
    lv_page_set_scrl_layout(tab, LV_LAYOUT_OFF);
    lv_page_set_scrl_fit(tab, false, false);
    lv_page_set_scrl_width(tab, GUI_W);
    lv_page_set_scrl_height(tab, GUI_H - GUI_BAR_H);
}

static void _about_tab(lv_obj_t *tab)
{
    lv_obj_t *t = gui_label(tab, &gui_font_30, GUI_COL_TEXT,
                            "#" GUI_HEX_ACCENT " hwtest:# hardware diagnostics");
    lv_obj_set_pos(t, GUI_MARGIN, GUI_TOP - GUI_BAR_H);
    lv_obj_t *d = gui_hint(tab, GUI_CONTENT_W,
        "Checks the console's hardware from RCM, without starting its system: "
        "battery, charging and power, temperature and fan, storage and the "
        "game card reader, Wi-Fi and Bluetooth, processor and memory, sound, "
        "screen, touch, sensors and buttons.\n\n"
        "Each full check saves its complete report to the SD card, under "
        "backup/<eMMC serial>/hwtest.txt.\n\n"
        "Built on hekate's BDK and the LVGL it carries, as Nyx is. Fonts: "
        "Inter, Source Code Pro and Font Awesome, under the SIL Open Font "
        "License 1.1.");
    lv_obj_set_pos(d, GUI_MARGIN, GUI_TOP - GUI_BAR_H + 56);
}

lv_obj_t *gui_main_new(void)
{
    _root_reset();
    _root_new();

    lv_obj_t *tv = lv_tabview_create(s_root, NULL);
    lv_tabview_set_sliding(tv, false);
    lv_tabview_set_anim_time(tv, 0);
    lv_obj_set_size(tv, GUI_W, GUI_H);
    lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_REL, &st_tab[0]);
    lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_PR, &st_tab[1]);
    lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_TGL_REL, &st_tab[2]);
    lv_tabview_set_style(tv, LV_TABVIEW_STYLE_BTN_TGL_PR, &st_tab[3]);

    char ver[48];
    s_printf(ver, "hwtest %s", GUI_VERSION);
    lv_obj_t *about = lv_tabview_add_tab(tv, ver);
    lv_obj_t *home = lv_tabview_add_tab(tv, ICON_HOME " Home");
    _tab_plain(about);
    _tab_plain(home);
    _about_tab(about);
    lv_tabview_set_tab_act(tv, 1, false);
    gui_sentinel_raise();
    return home;
}

/* A bare full-screen root, for the touch test's grid. */
lv_obj_t *gui_blank_new(void)
{
    _root_reset();
    _root_new();
    gui_sentinel_raise();
    return s_root;
}

static lv_res_t _act_close(lv_obj_t *b)
{
    gui_back();
    return LV_RES_OK;
}

/* Nyx's window header - the title on the left, its buttons on the right -
 * built on the grid rather than taken from lv_win, which keeps its own
 * margins. */
lv_obj_t *gui_win_new(const char *title)
{
    _root_reset();
    _root_new();

    s_hdr = lv_cont_create(s_root, NULL);
    lv_cont_set_style(s_hdr, s_th->win.header);
    lv_cont_set_layout(s_hdr, LV_LAYOUT_OFF);
    lv_obj_set_click(s_hdr, false);
    lv_obj_set_size(s_hdr, GUI_W, GUI_BAR_H);
    lv_obj_t *t = gui_label(s_hdr, &gui_font_30, GUI_COL_TEXT, title);
    lv_obj_align(t, NULL, LV_ALIGN_IN_LEFT_MID, GUI_MARGIN, 0);
    s_hdr_x = GUI_RIGHT;
    s_hdr_first = NULL;

    gui_sentinel_raise();
    s_hdr_close = gui_win_btn(s_root, ICON_CLOSE " Close", _act_close);
    return s_root;
}

/* A screen that cannot be left yet (the health check while it runs) takes
 * its Close button away rather than ignore it. */
void gui_win_close_hide(bool hide)
{
    if (s_hdr_close)
        gui_hide(s_hdr_close, hide);
}

/* Right to left: the text of the first one ends on the right margin, each
 * next box 20 px left of the previous. */
lv_obj_t *gui_win_btn(lv_obj_t *win, const char *text, lv_action_t action)
{
    (void)win;
    lv_obj_t *b = lv_btn_create(s_hdr, NULL);
    lv_btn_set_style(b, LV_BTN_STYLE_REL, &st_hdr_rel);
    lv_btn_set_style(b, LV_BTN_STYLE_PR, &st_hdr_pr);
    lv_btn_set_fit(b, true, false);
    lv_obj_set_height(b, GUI_BAR_H - 8);
    lv_btn_set_action(b, LV_BTN_ACTION_CLICK, action);
    lv_obj_t *l = lv_label_create(b, NULL);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);

    lv_coord_t w = lv_obj_get_width(b);
    lv_obj_set_pos(b, s_hdr_x + GUI_TEXT_PAD - w, 4);
    s_hdr_x -= w + GUI_TEXT_PAD;
    lv_group_add_obj(s_group, b);
    /* Buttons are laid out from the right, Close first; in the focus list
     * each goes before the ones made earlier, so VOL+ and the D-pad move
     * right across the header, as the buttons read. */
    {
        lv_ll_t *ll = &s_group->obj_ll;
        void *node = lv_ll_get_tail(ll);
        if (s_hdr_first) {
            /* LVGL 5's lv_ll_move_before links a node in before the head
             * but leaves the head pointer alone, which orphans the node:
             * the head is set here. */
            bool was_head = ll->head == s_hdr_first;
            lv_ll_move_before(ll, node, s_hdr_first);
            if (was_head)
                ll->head = node;
        }
        s_hdr_first = node;
    }
    return b;
}

/* ---- widgets ---------------------------------------------------------- */

/* Nyx's home screen button: 256 px square, a 120 px symbol, the caption
 * under it, both in the theme colour. */
lv_obj_t *gui_big_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h, const char *icon,
                         const char *caption, lv_action_t action)
{
    lv_obj_t *b = lv_btn_create(parent, NULL);
    lv_btn_set_layout(b, LV_LAYOUT_OFF);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);

    /* Nyx's proportions at 256 px, taken from the shorter side and kept
     * about the middle, so a wide or a tall tile carries its icon and
     * caption as a square one does; the icon and caption faces are cut for
     * the home tiles. A tile with no action is greyed out (the theme's
     * inactive button, its text grey), and neither taken by a touch nor
     * reached by VOL+/VOL- or the stick. */
    const char *hex = action ? GUI_HEX_ACCENT : "777777";
    lv_coord_t s = w < h ? w : h;
    char buf[48];
    s_printf(buf, "#%s %s#", hex, icon);
    lv_obj_t *ic = gui_label(b, &gui_icons_100, GUI_COL_TEXT, buf);
    lv_obj_align(ic, NULL, LV_ALIGN_CENTER, 0, -(s * 28 / 256));
    s_printf(buf, "#%s %s#", hex, caption);
    lv_obj_t *cap = gui_label(b, &gui_font_25, GUI_COL_TEXT, buf);
    lv_obj_align(cap, NULL, LV_ALIGN_IN_TOP_MID, 0, h / 2 + s * 46 / 256);

    if (!action) {
        lv_btn_set_state(b, LV_BTN_STATE_INA);
        lv_obj_set_click(b, false);
        return b;
    }
    lv_btn_set_action(b, LV_BTN_ACTION_CLICK, action);
    lv_group_add_obj(s_group, b);
    return b;
}

/* Nyx's flat button: only its text, until pressed. Bar-high, so a row of
 * them sits on the grid; its text is GUI_TEXT_PAD in from its box. */
lv_obj_t *gui_text_button(lv_obj_t *parent, const char *text, lv_action_t action)
{
    lv_obj_t *b = lv_btn_create(parent, NULL);
    lv_btn_set_style(b, LV_BTN_STYLE_REL, &st_text_rel);
    lv_btn_set_style(b, LV_BTN_STYLE_PR, &st_text_pr);
    lv_btn_set_fit(b, true, true);
    lv_btn_set_action(b, LV_BTN_ACTION_CLICK, action);
    lv_obj_t *l = lv_label_create(b, NULL);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    lv_group_add_obj(s_group, b);
    return b;
}

/* A theme button sized to its text. */
lv_obj_t *gui_button(lv_obj_t *parent, const char *text, lv_action_t action)
{
    lv_obj_t *b = lv_btn_create(parent, NULL);
    lv_btn_set_fit(b, true, true);
    lv_btn_set_action(b, LV_BTN_ACTION_CLICK, action);
    lv_obj_t *l = lv_label_create(b, NULL);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    lv_group_add_obj(s_group, b);
    return b;
}

/* The theme's framed container. */
lv_obj_t *gui_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                    lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_cont_create(parent, NULL);
    lv_cont_set_style(c, s_th->panel);
    lv_cont_set_layout(c, LV_LAYOUT_OFF);
    lv_obj_set_click(c, false);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    return c;
}

lv_style_t *gui_box_style(lv_style_t *st, lv_color_t bg, lv_color_t border,
                          lv_coord_t radius, lv_coord_t border_w)
{
    lv_style_copy(st, &lv_style_plain);
    st->body.main_color = bg;
    st->body.grad_color = bg;
    st->body.radius = radius;
    st->body.border.color = border;
    st->body.border.width = border_w;
    st->body.border.opa = LV_OPA_COVER;
    return st;
}

lv_obj_t *gui_box(lv_obj_t *parent, lv_style_t *st, lv_coord_t x,
                  lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent, NULL);
    lv_obj_set_style(o, st);
    lv_obj_set_click(o, false);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

/* The grey line Nyx draws between two columns. The points have to outlive
 * the line, and there is one such line on a screen at a time. */
lv_obj_t *gui_divider(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t h)
{
    static lv_point_t pts[2];
    pts[1].y = h;
    lv_obj_t *l = lv_line_create(parent, NULL);
    lv_line_set_points(l, pts, 2);
    lv_line_set_style(l, s_th->line.decor);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* A vertically scrolling area. Moving the focus onto something inside it
 * scrolls it into view (_focus_follow), so lists work with the buttons and
 * the stick as well as with a finger. Its rows stop 16 px short of its
 * right edge, where the scrollbar goes. */
lv_obj_t *gui_page(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                   lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *p = lv_page_create(parent, NULL);
    lv_page_set_style(p, LV_PAGE_STYLE_BG, &st_page);
    lv_page_set_style(p, LV_PAGE_STYLE_SCRL, &st_page);
    lv_page_set_style(p, LV_PAGE_STYLE_SB, s_th->list.sb);
    lv_page_set_sb_mode(p, LV_SB_MODE_AUTO);
    lv_page_set_scrl_layout(p, LV_LAYOUT_OFF);
    lv_page_set_scrl_fit(p, false, true);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_page_set_scrl_width(p, w - GUI_PAGE_SB_W);
    s_page = p;
    return p;
}

/* A row of a list, as the theme draws lv_list's: a line under it, filled
 * while pressed. */
lv_obj_t *gui_list_row(lv_obj_t *page, lv_coord_t y, lv_coord_t w,
                       lv_coord_t h, lv_action_t action)
{
    lv_obj_t *b = lv_btn_create(lv_page_get_scrl(page), NULL);
    lv_btn_set_layout(b, LV_LAYOUT_OFF);
    lv_btn_set_style(b, LV_BTN_STYLE_REL, s_th->list.btn.rel);
    lv_btn_set_style(b, LV_BTN_STYLE_PR, s_th->list.btn.pr);
    lv_obj_set_pos(b, 0, y);
    lv_obj_set_size(b, w, h);
    lv_btn_set_action(b, LV_BTN_ACTION_CLICK, action);
    lv_page_glue_obj(b, true);
    lv_group_add_obj(s_group, b);
    return b;
}

/* The same row without an action: how a read-only list is walked without
 * a touchscreen. */
lv_obj_t *gui_focus_row(lv_obj_t *page, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *c = lv_cont_create(lv_page_get_scrl(page), NULL);
    lv_cont_set_style(c, s_th->list.btn.rel);
    lv_cont_set_layout(c, LV_LAYOUT_OFF);
    lv_obj_set_pos(c, 0, y);
    lv_obj_set_size(c, w, 60);
    lv_page_glue_obj(c, true);
    lv_group_add_obj(s_group, c);
    return c;
}

/* ---- message boxes ---------------------------------------------------- */

void gui_mbox(const char *text, const char **btns, lv_btnm_action_t action)
{
    s_mbox_bg = lv_obj_create(lv_scr_act(), NULL);
    lv_obj_set_style(s_mbox_bg, &st_darken);
    lv_obj_set_size(s_mbox_bg, GUI_W, GUI_H);
    gui_sentinel_raise();

    s_mbox = lv_mbox_create(s_mbox_bg, NULL);
    lv_mbox_set_recolor_text(s_mbox, true);
    lv_obj_set_width(s_mbox, GUI_W / 2);
    lv_mbox_set_text(s_mbox, text);
    lv_mbox_add_btns(s_mbox, btns, action);
    lv_mbox_set_style(s_mbox, LV_MBOX_STYLE_BTN_BG, &st_mbox_btns);
    lv_obj_align(s_mbox, NULL, LV_ALIGN_CENTER, 0, 0);

    /* VOL+/VOL- and the stick pick a button inside it (gui_main.c turns
     * them into LEFT/RIGHT) until it closes. */
    lv_group_add_obj(s_group, s_mbox);
    lv_group_focus_obj(s_mbox);
    lv_group_focus_freeze(s_group, true);
}

void gui_mbox_close(lv_obj_t *btnm)
{
    (void)btnm;
    lv_group_focus_freeze(s_group, false);
    if (s_mbox_bg)
        lv_obj_del(s_mbox_bg);
    s_mbox_bg = NULL;
    s_mbox = NULL;
}

bool gui_mbox_open(void)
{
    return s_mbox != NULL;
}

/* ---- status bar ------------------------------------------------------- */

/* The battery as Nyx's status bar has it: the level to a tenth, the symbol
 * red when nearly empty, a bolt while the charger is at work, and to the
 * left of that the current (into the battery in green, out of it in red)
 * and the cell voltage, orange with a warning once it is too low. */
static void _bar_update(void *unused)
{
    (void)unused;
    int soc = 0, volt = 0, curr = 0, chg = 0;
    char buf[64];
    if (max17050_get_property(MAX17050_RepSOC, &soc)) {
        strcpy(buf, ICON_BATTERY "  --");
        lv_label_set_text(s_bar_more, "");
    } else {
        max17050_get_property(MAX17050_VCELL, &volt);
        max17050_get_property(MAX17050_Current, &curr);
        bq24193_get_property(BQ24193_ChargeStatus, &chg);
        u32 lvl = (soc >> 8) & 0xFF;
        s_printf(buf, "%d.%d%%  %s" ICON_BATTERY "%s%s", lvl, (soc & 0xFF) / 26,
                 lvl > 15 ? "" : "#" GUI_HEX_FAIL " ", lvl > 15 ? "" : "#",
                 chg ? "  #" GUI_HEX_WARN " " ICON_BOLT "#" : "");
        char more[64];
        s_printf(more, "#%s%d mA#  (%s%d mV%s)",
                 curr >= 0 ? GUI_HEX_OK " +" : GUI_HEX_FAIL " ", curr / 1000,
                 volt < 3200 ? "#FF8000 " : "", volt,
                 volt < 3200 ? "  " ICON_WARN "#" : "");
        lv_label_set_text(s_bar_more, more);
    }
    lv_label_set_text(s_bar_batt, buf);
    lv_obj_align(s_bar_batt, NULL, LV_ALIGN_IN_RIGHT_MID, -GUI_MARGIN, 0);
    lv_obj_align(s_bar_more, s_bar_batt, LV_ALIGN_OUT_LEFT_MID, -GUI_TEXT_PAD, 0);
}

/* Along the bottom of every screen, as in Nyx: the console on the left,
 * the battery on the right. */
void gui_statusbar_init(void)
{
    s_bar = lv_cont_create(lv_layer_top(), NULL);
    lv_cont_set_style(s_bar, &st_bar);
    lv_obj_set_size(s_bar, GUI_W, GUI_BAR_H);
    lv_obj_align(s_bar, NULL, LV_ALIGN_IN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_click(s_bar, false);

    const char *left = NULL;
    char buf[64];
    if (s_name[0]) {
        s_printf(buf, "#" GUI_HEX_ACCENT " " ICON_ID "#  %s", s_name);
        left = buf;
    } else if (s_serial == SERIAL_NO_KEYS) {
        left = "#" GUI_HEX_WARN " " ICON_WARN "  No prod.keys on the SD card#";
    } else if (s_serial == SERIAL_KEYS_UNREAD) {
        left = "#" GUI_HEX_WARN " " ICON_WARN "  prod.keys could not be read#";
    } else if (s_serial == SERIAL_BAD_KEYS) {
        left = "#" GUI_HEX_WARN " " ICON_WARN "  prod.keys is not this console's#";
    }
    if (left) {
        lv_obj_t *l = gui_label(s_bar, &gui_font_20, GUI_COL_TEXT, left);
        lv_obj_align(l, NULL, LV_ALIGN_IN_LEFT_MID, GUI_MARGIN, 0);
    }

    s_bar_batt = gui_label(s_bar, &gui_font_20, GUI_COL_TEXT, "");
    s_bar_more = gui_hint(s_bar, 0, "");
    _bar_update(NULL);
    lv_task_create(_bar_update, 5000, LV_TASK_PRIO_LOW, NULL);
}

void gui_statusbar_hide(bool hide)
{
    if (s_bar)
        lv_obj_set_hidden(s_bar, hide);
}
