/*
 * hwtest GUI - the custom check: which parts of the console to look at.
 *
 * The text launcher's Custom test as a window: one checkbox per group of
 * checks, all on to begin with and remembered between visits, and Run in
 * the header. The health check then runs only those, its step list greys
 * out the rest, and the results say "not checked" for them.
 */
#include "gui.h"

static const struct {
    const char *label;
    u32 bit;
} s_groups[] = {
    { "Identifying the console",           PROBE_G_IDENTITY },
    { "Battery, charging and power",       PROBE_G_POWER },
    { "Memory and clocks",                 PROBE_G_MEMORY },
    { "Storage and game card reader",      PROBE_G_STORAGE },
    { "Wi-Fi and Bluetooth",               PROBE_G_WIRELESS },
    { "Sound chip",                        PROBE_G_AUDIO },
    { "Sound output (plays a short tone)", PROBE_G_SOUND },
    { "Screen",                            PROBE_G_DISPLAY },
    { "Touch, sensors and buttons",        PROBE_G_INPUTS },
    { "System details",                    PROBE_G_RAW },
};
#define N_GROUPS ARRAY_SIZE(s_groups)
#define ROW_H 48

static u32 s_mask = PROBE_G_ALL;    /* the last selection, kept between visits */
static lv_obj_t *s_cb[N_GROUPS];
static lv_style_t st_cb_bg;

static lv_res_t _act_run(lv_obj_t *b)
{
    u32 mask = 0;
    for (u32 i = 0; i < N_GROUPS; i++)
        if (lv_cb_is_checked(s_cb[i]))
            mask |= s_groups[i].bit;
    if (!mask)
        return LV_RES_OK;       /* nothing ticked: nothing to run */
    s_mask = mask;
    gui_run_mask(mask);
    gui_show(gui_run);
    return LV_RES_OK;
}

void gui_custom(void)
{
    lv_obj_t *win = gui_win_new(ICON_SLIDERS " Custom Check");
    lv_obj_t *run = gui_win_btn(win, ICON_PLAY " Run", _act_run);

    /* The theme's checkbox, with its box on the left margin: no padding
     * round it, a gap between the box and its text. */
    lv_style_copy(&st_cb_bg, &lv_style_transp);
    st_cb_bg.body.padding.hor = 0;
    st_cb_bg.body.padding.ver = 0;
    st_cb_bg.body.padding.inner = 16;

    for (u32 i = 0; i < N_GROUPS; i++) {
        lv_obj_t *cb = lv_cb_create(win, NULL);
        lv_cb_set_style(cb, LV_CB_STYLE_BG, &st_cb_bg);
        lv_cb_set_text(cb, s_groups[i].label);
        lv_cb_set_checked(cb, (s_mask & s_groups[i].bit) != 0);
        lv_obj_set_pos(cb, GUI_MARGIN, GUI_TOP + i * ROW_H);
        lv_group_add_obj(gui_group(), cb);
        s_cb[i] = cb;
    }

    lv_obj_t *note = gui_hint(win, GUI_CONTENT_W,
        "Only what is ticked is checked; the results say \"not checked\" "
        "for the rest.");
    lv_obj_set_pos(note, GUI_MARGIN, GUI_BOTTOM - 50);

    lv_group_focus_obj(run);
}
