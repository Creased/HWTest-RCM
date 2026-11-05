/*
 * hwtest GUI - the RAM test.
 *
 * The RAM test is its own payload (ramtest/, hwtest_ram.bin on the SD card):
 * it retrains the DRAM, which nothing running from DRAM - this GUI included -
 * can survive. So this window explains that, then hands over to it the same
 * way the text launcher does (ram_payload_load/run, ui_menu.c); there is no
 * coming back without a restart.
 */
#include "gui.h"
#include "../ui_menu.h"

static lv_obj_t *s_msg;

static lv_res_t _act_start(lv_obj_t *b)
{
    const char *used = NULL;
    u32 size = 0;
    int err;
    lv_label_set_text(s_msg, "#" GUI_HEX_ACCENT " Loading the RAM test...#");
    gui_refresh_now();

    void *buf = ram_payload_load(&used, &size, &err);
    if (buf) {
        gui_clock_fast(false);        /* it trains from the boot clock */
        ram_payload_run(buf, size);   /* does not return */
    }
    /* The loader powers the card down when it fails; bring it back for a
     * health check run after this. */
    g_sd_ok = sd_initialize(false) == 0;

    lv_label_set_text(s_msg,
        err == 1 ? "#" GUI_HEX_FAIL " The SD card could not be read.#" :
        err == 2 ? "#" GUI_HEX_FAIL " hwtest_ram.bin is not on the SD card.# "
                   "Copy it to the root of the card." :
                   "#" GUI_HEX_FAIL " hwtest_ram.bin on the SD card is damaged.# "
                   "Copy it again.");
    return LV_RES_OK;
}

void gui_ram_test(void)
{
    lv_obj_t *win = gui_win_new(ICON_RAM " RAM Test");
    lv_coord_t w = GUI_CONTENT_W * 2 / 3;    /* a readable line length */

    lv_obj_t *t = gui_label(win, &gui_font_30, GUI_COL_TEXT, "Deep memory test");
    lv_obj_set_pos(t, GUI_MARGIN, GUI_TOP);

    lv_obj_t *d = gui_hint(win, w,
        "Writes and reads back every part of the console's memory at full "
        "speed, to find faults the health check cannot see, the kind that "
        "cause random crashes. It takes several minutes.\n\n"
        "It runs as a separate tool from the SD card (hwtest_ram.bin), with "
        "its own screens. When it is finished, power the console off or "
        "reboot it to come back here.");
    lv_obj_set_pos(d, GUI_MARGIN, GUI_TOP + 56);

    lv_coord_t y = GUI_TOP + 56 + lv_obj_get_height(d) + 32;
    lv_obj_t *go = gui_button(win, ICON_PLAY " Start", _act_start);
    lv_obj_set_pos(go, GUI_MARGIN, y);
    lv_group_focus_obj(go);

    s_msg = gui_label_wrap(win, &gui_font_20, GUI_COL_TEXT, w, "");
    lv_obj_set_pos(s_msg, GUI_MARGIN, y + lv_obj_get_height(go) + 24);
}
