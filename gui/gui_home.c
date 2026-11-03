/*
 * hwtest GUI - the main screen.
 *
 * Laid out as Nyx's Home tab: the name and a tagline, a row of 256 px
 * buttons - the health check first, then the hands-on tests, and past a
 * divider the RAM test, which leaves for its own payload - and flat text
 * buttons along the bottom, with Reboot and Power Off asking first.
 */
#include "gui.h"

/* The tab's page starts under the tab bar. */
#define TAB_Y(y) ((y) - GUI_BAR_H)

static lv_res_t _act_check(lv_obj_t *b)
{
    gui_run_mask(PROBE_G_ALL);    /* the whole check, whatever was picked before */
    gui_show(gui_run);
    return LV_RES_OK;
}
static lv_res_t _act_custom(lv_obj_t *b)  { gui_show(gui_custom); return LV_RES_OK; }
static lv_res_t _act_joycon(lv_obj_t *b)  { gui_show(gui_joycon_test); return LV_RES_OK; }
static lv_res_t _act_touch(lv_obj_t *b)   { gui_show(gui_touch_test); return LV_RES_OK; }
static lv_res_t _act_ram(lv_obj_t *b)     { gui_show(gui_ram_test); return LV_RES_OK; }
static lv_res_t _act_results(lv_obj_t *b) { gui_show(gui_results); return LV_RES_OK; }

static lv_res_t _power_action(lv_obj_t *btns, const char *txt)
{
    if (strstr(txt, "Power Off")) {
        /* MAX77620 PWR_OFF drops every rail in sequence: a real power-off,
         * the same as the text launcher's. */
        i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG1,
                      MAX77620_ONOFFCNFG1_PWR_OFF);
        while (1)
            ;
    }
    if (strstr(txt, "Reboot"))
        do_reboot();
    gui_mbox_close(btns);
    return LV_RES_INV;
}

static lv_res_t _act_poweroff(lv_obj_t *b)
{
    static const char *map[] = { "\221Power Off", "\221Cancel", "" };
    gui_mbox("#FF8000 Do you really want to power off?#", map, _power_action);
    return LV_RES_OK;
}

static lv_res_t _act_reboot(lv_obj_t *b)
{
    static const char *map[] = { "\221Reboot", "\221Cancel", "" };
    gui_mbox("#FF8000 Do you really want to reboot?#\n\n"
             "The console powers off and on again.", map, _power_action);
    return LV_RES_OK;
}

void gui_home(void)
{
    lv_obj_t *tab = gui_main_new();

    lv_obj_t *brand = gui_label(tab, &gui_font_30, GUI_COL_TEXT,
                                "#" GUI_HEX_ACCENT " hwtest#");
    lv_obj_set_pos(brand, GUI_MARGIN, TAB_Y(108));
    lv_obj_t *tag = gui_hint(tab, 0, "HARDWARE HEALTH CHECK FOR THE NINTENDO SWITCH");
    lv_obj_set_pos(tag, GUI_MARGIN, TAB_Y(148));

    /* Nyx's row: 256 px buttons 35 px apart from the left margin, the
     * divider, and the last one ending on the right margin. */
    lv_coord_t y = TAB_Y(240);
    lv_obj_t *check = gui_big_button(tab, GUI_MARGIN, y, ICON_HEALTH,
                                     "Health Check", _act_check);
    gui_big_button(tab, GUI_MARGIN + 256 + GUI_GAP, y, ICON_GAMEPAD,
                   "Joy-Con", _act_joycon);
    gui_big_button(tab, GUI_MARGIN + 2 * (256 + GUI_GAP), y, ICON_TOUCH,
                   "Touchscreen", _act_touch);
    gui_divider(tab, GUI_MARGIN + 3 * (256 + GUI_GAP) - 1, TAB_Y(218), 300);
    gui_big_button(tab, GUI_RIGHT - 256, y, ICON_RAM, "RAM Test", _act_ram);

    /* The bottom row: the flat buttons' text lines up with the margins and
     * their boxes end where every window's content ends (the tab page runs
     * on under the status bar, so the offset is taken from the screen edge). */
    const lv_coord_t dy = -(GUI_H - GUI_BOTTOM);
    lv_obj_t *custom = gui_text_button(tab, ICON_SLIDERS " Custom Check", _act_custom);
    lv_obj_align(custom, NULL, LV_ALIGN_IN_BOTTOM_LEFT, GUI_MARGIN - GUI_TEXT_PAD, dy);
    if (gui_report_ready()) {
        lv_obj_t *res = gui_text_button(tab, ICON_LIST " Last Results", _act_results);
        lv_obj_align(res, custom, LV_ALIGN_OUT_RIGHT_MID, GUI_TEXT_PAD, 0);
    }

    lv_obj_t *off = gui_text_button(tab, ICON_POWER " Power Off", _act_poweroff);
    lv_obj_align(off, NULL, LV_ALIGN_IN_BOTTOM_RIGHT, -(GUI_MARGIN - GUI_TEXT_PAD), dy);
    lv_obj_t *reboot = gui_text_button(tab, ICON_REFRESH " Reboot", _act_reboot);
    lv_obj_align(reboot, off, LV_ALIGN_OUT_LEFT_MID, -GUI_TEXT_PAD, 0);

    lv_group_focus_obj(check);
}
