/*
 * hwtest GUI stage - shared declarations.
 *
 * The GUI is a second stage that hwtest.bin loads from the SD card: the
 * same probes as the text payload, behind an LVGL front end (bdk's LVGL 5,
 * as Nyx uses) driven by touch, the Joy-Con, or VOL+/VOL-/POWER alone.
 *
 * It looks like Nyx on purpose, since that is the screen Switch owners
 * already know: bdk's hekate theme at Nyx's default colours, Inter at Nyx's
 * sizes, a tab view for the main screen, a window with a Close button for
 * everything else, message boxes for questions, and a status bar along the
 * bottom.
 */
#ifndef HWTEST_GUI_H
#define HWTEST_GUI_H

#include "../hwtest.h"
#include <libs/lvgl/lvgl.h>
#include <input/joycon.h>
#include <input/touch.h>
#include "gui_fonts.h"

/* ---- palette: the hekate theme's, at Nyx's defaults ---- */

#define GUI_THEME_BG      0x2D2D2D
#define GUI_THEME_HUE     167

#define GUI_COL_BG        LV_COLOR_HEX(0x2D2D2D)
#define GUI_COL_BG_DARK   LV_COLOR_HEX(0x222222)
#define GUI_COL_BG_LIGHT  LV_COLOR_HEX(0x3D3D3D)
#define GUI_COL_LINE      LV_COLOR_HEX(0x656565)
#define GUI_COL_TEXT      LV_COLOR_HEX(0xFBFBFB)
#define GUI_COL_HINT      LV_COLOR_HEX(0xCCCCCC)
#define GUI_COL_ACCENT    LV_COLOR_HEX(0x00FFC7)   /* hue 167, as the theme makes it */
#define GUI_COL_OK        LV_COLOR_HEX(0x96FF00)
#define GUI_COL_WARN      LV_COLOR_HEX(0xFFDD00)
#define GUI_COL_FAIL      LV_COLOR_HEX(0xFF3C28)

/* The same colours as label recolour text ("#96FF00 OK#"). */
#define GUI_HEX_ACCENT    "00FFC7"
#define GUI_HEX_OK        "96FF00"
#define GUI_HEX_WARN      "FFDD00"
#define GUI_HEX_FAIL      "FF3C28"
#define GUI_HEX_HINT      "AAAAAA"

/* ---- geometry ----
 *
 * One grid for every screen. The side margin is where Nyx's home buttons
 * start, and everything lines up on it: titles, the header's buttons, the
 * status bar's text, panels, lists and dividers. The top bar (a window's
 * header, the home screen's tabs) and the status bar are the same height,
 * and the content keeps 20 px from each. */

#define GUI_W             1280
#define GUI_H             720
#define GUI_BAR_H         64                        /* top bar, status bar  */
#define GUI_MARGIN        58                        /* left and right       */
#define GUI_RIGHT         (GUI_W - GUI_MARGIN)      /* 1222                 */
#define GUI_GAP           35                        /* between columns      */
#define GUI_TOP           (GUI_BAR_H + 20)          /* 84: content's top    */
#define GUI_BOTTOM        (GUI_H - GUI_BAR_H - 20)  /* 636: and its bottom  */
#define GUI_CONTENT_W     (GUI_RIGHT - GUI_MARGIN)  /* 1164                 */
#define GUI_CONTENT_H     (GUI_BOTTOM - GUI_TOP)    /* 552                  */
#define GUI_TEXT_PAD      20    /* a flat button's box past its text        */
#define GUI_PAGE_SB_W     16    /* a list's rows stop here before its edge  */
#define GUI_BACKLIGHT     100   /* the level main.c set before the GUI      */

/* Two columns: a narrow one on the left, the divider centred in the gap,
 * and the wide one from GUI_COL2_X to the right margin. */
#define GUI_COL1_W        352
#define GUI_DIVIDER_X     (GUI_MARGIN + GUI_COL1_W + GUI_GAP / 2)   /* 427 */
#define GUI_COL2_X        (GUI_MARGIN + GUI_COL1_W + GUI_GAP)       /* 445 */
#define GUI_COL2_W        (GUI_RIGHT - GUI_COL2_X)                  /* 777 */

/* ---- theme and building blocks (gui_theme.c) ---- */

void gui_theme_init(void);
lv_theme_t *gui_th(void);
lv_group_t *gui_group(void);

/* The main screen: a tab view, Nyx's way. Returns the Home tab, whose
 * page starts under the tab bar: a screen y is GUI_BAR_H further down. */
lv_obj_t *gui_main_new(void);
lv_obj_t *gui_blank_new(void);           /* full screen, no header       */
/* Any other screen: a header with the title and a Close button (which goes
 * back), the rest free. Objects created on it are placed in screen
 * coordinates, inside GUI_MARGIN/GUI_RIGHT and GUI_TOP/GUI_BOTTOM. Header
 * buttons are added right to left, the Close button first. */
lv_obj_t *gui_win_new(const char *title);
lv_obj_t *gui_win_btn(lv_obj_t *win, const char *text, lv_action_t action);
void gui_win_close_hide(bool hide);      /* for a screen that cannot be left yet */
void gui_hide(lv_obj_t *obj, bool hide); /* the focus leaves it while hidden */

lv_obj_t *gui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col,
                    const char *text);
lv_obj_t *gui_label_wrap(lv_obj_t *parent, const lv_font_t *font,
                         lv_color_t col, lv_coord_t w, const char *text);
lv_obj_t *gui_hint(lv_obj_t *parent, lv_coord_t w, const char *text);
lv_obj_t *gui_big_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, const char *icon, const char *caption,
                         lv_action_t action);
lv_obj_t *gui_text_button(lv_obj_t *parent, const char *text, lv_action_t action);
lv_obj_t *gui_button(lv_obj_t *parent, const char *text, lv_action_t action);
lv_obj_t *gui_page(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                   lv_coord_t w, lv_coord_t h);
lv_obj_t *gui_list_row(lv_obj_t *page, lv_coord_t y, lv_coord_t w,
                       lv_coord_t h, lv_action_t action);
lv_obj_t *gui_focus_row(lv_obj_t *page, lv_coord_t y, lv_coord_t w);
lv_obj_t *gui_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                    lv_coord_t w, lv_coord_t h);
/* A plain shape: a filled, bordered box the tests draw rings and bars
 * with. The style is the caller's to keep. */
lv_style_t *gui_box_style(lv_style_t *st, lv_color_t bg, lv_color_t border,
                          lv_coord_t radius, lv_coord_t border_w);
lv_obj_t *gui_box(lv_obj_t *parent, lv_style_t *st, lv_coord_t x,
                  lv_coord_t y, lv_coord_t w, lv_coord_t h);
lv_obj_t *gui_divider(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                      lv_coord_t h);

/* A Nyx message box over the current screen. Its buttons are the map's
 * entries; the action gets the pressed one's text and closes the box with
 * gui_mbox_close(). */
void gui_mbox(const char *text, const char **btns, lv_btnm_action_t action);
void gui_mbox_close(lv_obj_t *btnm);
bool gui_mbox_open(void);

void gui_statusbar_init(void);
void gui_statusbar_hide(bool hide);
void gui_uart_puts(const char *s);       /* a debug line, while UART-B is still ours */

extern lv_style_t gui_st_mono;

/* ---- navigation (gui_main.c) ---- */

typedef void (*gui_screen_fn_t)(void);

void gui_show(gui_screen_fn_t screen);   /* replace the current screen   */
void gui_set_back(gui_screen_fn_t screen); /* where B / Close leads (home */
                                         /* unless a screen says other) */
void gui_back(void);
void gui_refresh_now(void);              /* draw pending changes now    */
void gui_input_sample(void);             /* latch VOL presses (long work) */
u32  gui_tv_fb(void);                    /* the TV's copy of the screen  */
void gui_sentinel_raise(void);           /* after creating on the screen */

/* ---- input state the test screens read (gui_main.c) ---- */

bool gui_touch_ok(void);
bool gui_jc_ok(void);
bool gui_is_lite(void);                  /* built-in controls, no rails  */
jc_gamepad_rpt_t *gui_jc_state(void);
void gui_input_hold(bool hold);          /* test screens own the input */
void gui_input_resume(void);             /* power touch back up         */

void gui_hw_prepare(void);               /* SD, eMMC, TMP451: once      */
const char *gui_console_name(void);      /* serial, or "" without keys  */

/* Clocks (gui_clock.c): fast for drawing, the payload's for the probes. */
void gui_clock_init(void);
void gui_clock_fast(bool fast);
u32  gui_clock_dram_mhz(void);

/* A test screen can take the raw touch samples (the sink returns true for
 * the ones it consumed) and keep the Joy-Con from navigating; both are
 * reset, and the leave hook run, when the screen is left. */
typedef bool (*gui_touch_sink_t)(const touch_event_t *ev);
void gui_touch_sink(gui_touch_sink_t sink);
void gui_jc_nav(bool on);
void gui_on_leave(void (*fn)(void));

/* ---- screens ---- */

void gui_home(void);
void gui_custom(void);                   /* pick the groups, then gui_run */
void gui_run(void);
void gui_run_mask(u32 mask);             /* what gui_run checks from now on */
void gui_joycon_test(void);
void gui_jc_motion(void);                /* from the Joy-Con test        */
void gui_jc_ir(void);
void gui_jc_test_resumed(void);          /* a sensor screen gave the rails back */
void gui_touch_test(void);
void gui_screen_test(void);
void gui_dock_test(void);
void gui_ram_test(void);
void gui_screenshot(void);               /* two fingers, from gui_main.c */
void gui_results(void);
void gui_cat_detail(void);
void gui_tech_report(void);

/* ---- the last run's report (gui_run.c) ---- */

#define PAGE_LIST_MAX 16
const char *gui_report_page(const char *name);
u32 gui_report_page_count(void);
const char *gui_report_page_name(u32 idx);
bool gui_report_saved(void);
bool gui_report_ready(void);

#endif
