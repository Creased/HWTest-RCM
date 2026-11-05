/*
 * hwtest GUI - the screen test.
 *
 * The whole panel in one colour at a time - white, red, green, blue,
 * black - then white again while the backlight dims and brightens. Dots
 * that stay dark or bright, lines and patches show against a flat field,
 * and a backlight that flickers or lights unevenly shows on the last page.
 * The health check cannot see the panel; eyes can. A touch, VOL+, VOL- or
 * any Joy-Con button turns the page; POWER is left to the message box at
 * the end, so the press that reaches it is not also the one that opened it.
 */
#include "gui.h"
#include <display/di.h>
#include <utils/btn.h>

typedef struct {
    u32 hex, text;           /* the field, and a caption that reads on it */
    const char *name;
} sc_page_t;

static const sc_page_t s_pages[] = {
    { 0xFFFFFF, 0x333333, "White" },
    { 0xFF0000, 0xFFFFFF, "Red" },
    { 0x00FF00, 0x333333, "Green" },
    { 0x0000FF, 0xFFFFFF, "Blue" },
    { 0x000000, 0xAAAAAA, "Black" },
    { 0xFFFFFF, 0x333333, "Backlight" },
};
#define SC_PAGES    (sizeof(s_pages) / sizeof(s_pages[0]))
#define SC_BL_PAGE  (SC_PAGES - 1)
#define SC_LOCK_MS  400                /* after a page turn, input is ignored */
#define SC_TICK_MS  30
#define SC_BL_STEP  6                  /* a level a tick: 8 to 255 in 1.2 s */
#define SC_BL_LOW   8

static lv_obj_t *s_root, *s_field, *s_cap, *s_hint;
static lv_style_t st_field;
static lv_task_t *s_sc_task;
static u32 s_page, s_changed;
static bool s_down, s_keys_was, s_over;
static s32 s_bl_level, s_bl_dir;

static void _sc_result(void);

static void _sc_apply(void)
{
    const sc_page_t *p = &s_pages[s_page];
    st_field.body.main_color = LV_COLOR_HEX(p->hex);
    st_field.body.grad_color = LV_COLOR_HEX(p->hex);
    lv_obj_set_style(s_field, &st_field);
    lv_obj_invalidate(s_field);

    char buf[96];
    s_printf(buf, "%s (%d of %d)%s", p->name, s_page + 1, SC_PAGES,
             s_page == SC_BL_PAGE ? ": the backlight dims and brightens" : "");
    if (s_cap)
        lv_obj_del(s_cap);
    s_cap = gui_label(s_root, &gui_font_20, LV_COLOR_HEX(p->text), buf);
    lv_obj_set_pos(s_cap, GUI_MARGIN, GUI_H - 56);
    lv_obj_set_hidden(s_hint, s_page != 0);

    if (s_page == SC_BL_PAGE) {
        s_bl_level = 255;
        s_bl_dir = -SC_BL_STEP;
    } else {
        display_backlight_brightness(GUI_BACKLIGHT, 0);
    }
    s_changed = get_tmr_ms();
}

static void _sc_next(void)
{
    if (s_page + 1 >= SC_PAGES) {
        _sc_result();
        return;
    }
    s_page++;
    _sc_apply();
}

static bool _sc_sink(const touch_event_t *ev)
{
    if (s_over)
        return false;            /* the message box's taps are LVGL's */
    if (ev->touch && !s_down && get_tmr_ms() - s_changed > SC_LOCK_MS)
        _sc_next();
    s_down = ev->touch;
    return true;
}

static void _sc_tick(void *param)
{
    if (s_over)
        return;
    u32 now = get_tmr_ms();
    u8 b = btn_read();
    jc_gamepad_rpt_t *jc = gui_jc_state();
    bool pressed = (b & (BTN_VOL_UP | BTN_VOL_DOWN)) || (jc && jc->buttons);
    if (pressed && !s_keys_was && now - s_changed > SC_LOCK_MS)
        _sc_next();
    s_keys_was = pressed;

    if (s_page == SC_BL_PAGE && !s_over) {
        s_bl_level += s_bl_dir;
        if (s_bl_level <= SC_BL_LOW) {
            s_bl_level = SC_BL_LOW;
            s_bl_dir = SC_BL_STEP;
        } else if (s_bl_level >= 255) {
            s_bl_level = 255;
            s_bl_dir = -SC_BL_STEP;
        }
        display_backlight_brightness(s_bl_level, 0);
    }
}

static lv_res_t _sc_action(lv_obj_t *btns, const char *txt)
{
    if (strstr(txt, "Again"))
        gui_show(gui_screen_test);
    else
        gui_back();
    return LV_RES_OK;
}

static void _sc_result(void)
{
    if (s_over)
        return;
    s_over = true;
    display_backlight_brightness(GUI_BACKLIGHT, 0);
    static const char *map[] = { "\221Again", "\221Done", "" };
    gui_mbox("That was every colour, and the backlight at every level.\n\n"
             "Dots that stayed dark or bright, lines, or patches of another "
             "shade are faults in the panel. Brightness that was uneven or "
             "flickered on the last page is the backlight.", map, _sc_action);
}

static void _sc_leave(void)
{
    if (s_sc_task)
        lv_task_del(s_sc_task);
    s_sc_task = NULL;
    s_cap = NULL;
    display_backlight_brightness(GUI_BACKLIGHT, 0);
}

void gui_screen_test(void)
{
    s_root = gui_blank_new();
    gui_statusbar_hide(true);
    gui_touch_sink(_sc_sink);
    gui_jc_nav(false);
    gui_on_leave(_sc_leave);
    s_page = 0;
    s_over = false;
    s_down = false;
    s_keys_was = true;       /* whatever is held now does not count */
    s_cap = NULL;

    gui_box_style(&st_field, LV_COLOR_WHITE, LV_COLOR_WHITE, 0, 0);
    s_field = gui_box(s_root, &st_field, 0, 0, GUI_W, GUI_H);
    s_hint = gui_label_wrap(s_root, &gui_font_20, LV_COLOR_HEX(0x333333), 720,
        "Look over the whole screen for dots that stay dark or bright, lines, "
        "or patches of another shade.\n\nTap, press VOL+ or VOL-, or any "
        "Joy-Con button, for the next colour.");
    lv_obj_align(s_hint, NULL, LV_ALIGN_CENTER, 0, 0);
    _sc_apply();

    if (s_sc_task)
        lv_task_del(s_sc_task);
    s_sc_task = lv_task_create(_sc_tick, SC_TICK_MS, LV_TASK_PRIO_MID, NULL);
}
