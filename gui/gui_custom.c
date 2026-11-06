/*
 * hwtest GUI - the custom check: which parts of the console to look at.
 *
 * The text launcher's Custom test as a window: one checkbox per group of
 * checks, all on to begin with and remembered between visits, and Run in
 * the header. The health check then runs only those, its step list greys
 * out the rest, and the results say "not checked" for them.
 *
 * Beside them, where the UART output goes (gui_dbg.c): one port at a time,
 * for the rest of the session.
 */
#include "gui.h"
#include "usbser.h"

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
#define CB_PAD 10

static u32 s_mask = PROBE_G_ALL;    /* the last selection, kept between visits */
static lv_obj_t *s_cb[N_GROUPS];
static lv_style_t st_cb_bg;

#define PORT_X 720
#define PORT_NOTE_MS 300
static lv_obj_t *s_port_cb[DBG_PORTS], *s_port_note;
static lv_task_t *s_port_task;

static const char *const s_port_where[DBG_PORTS] = {
    "The right rail's data pin, 115200 8N1 at 1.8 V, as the text payload uses it.",
    "The left rail's data pin, 115200 8N1 at 1.8 V.",
    "The board's test pads, 115200 8N1 at 1.8 V: pad 3 of cluster C is TX, pad 2 RX.",
    "A serial port on the PC the console's USB-C port is plugged into.",
};

/* Where the USB serial port has got to, as a sentence. */
static const char *_usb_state(void)
{
    switch (usbser_state()) {
    case USBSER_WAITING:   return " Waiting for a PC.";
    case USBSER_CONNECTED: return " The PC has it: open the port there (COMx on Windows, "
                                  "/dev/ttyACM0 on Linux).";
    case USBSER_OPEN:      return " Open on the PC.";
    case USBSER_FAILED:    return " The USB controller did not come up.";
    default:               return "";
    }
}

static void _port_note(void)
{
    gui_dbg_port_t p = gui_dbg_port();
    bool rail = p == DBG_UART_B || p == DBG_UART_C;
    const char *more = gui_dbg_taken(p)
        ? " A Joy-Con is attached there, so the port is its own: nothing goes out on it."
        : rail ? " A Joy-Con attached there takes the port over."
        : p == DBG_USB ? _usb_state() : "";
    const char *usb = usbser_state() == USBSER_FAILED && p != DBG_USB
        ? "\n\nUSB serial: the USB controller did not come up." : "";
    char buf[320];
    s_printf(buf, "%s%s%s", s_port_where[p], more, usb);
    if (strcmp(lv_label_get_text(s_port_note), buf))
        lv_label_set_text(s_port_note, buf);
}

static void _port_tick(void *param)
{
    _port_note();
}

static void _custom_leave(void)
{
    if (s_port_task)
        lv_task_del(s_port_task);
    s_port_task = NULL;
}

static lv_res_t _act_port(lv_obj_t *cb)
{
    for (u32 i = 0; i < DBG_PORTS; i++)
        if (s_port_cb[i] == cb)
            gui_dbg_set((gui_dbg_port_t)i);
    /* One ticked, the port in use: ticking it again does not untick it. */
    for (u32 i = 0; i < DBG_PORTS; i++)
        if (s_port_cb[i])
            lv_cb_set_checked(s_port_cb[i], i == gui_dbg_port());
    _port_note();
    return LV_RES_OK;
}

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
    gui_on_leave(_custom_leave);
    lv_obj_t *run = gui_win_btn(win, ICON_PLAY " Run", _act_run);

    /* The theme's checkbox, with its box on the left margin: room either
     * side for the focus outline (placed that far left), none above and
     * below, a gap between the box and its text. */
    lv_style_copy(&st_cb_bg, &lv_style_transp);
    st_cb_bg.body.padding.hor = CB_PAD;
    st_cb_bg.body.padding.ver = 0;
    st_cb_bg.body.padding.inner = 16;

    for (u32 i = 0; i < N_GROUPS; i++) {
        lv_obj_t *cb = lv_cb_create(win, NULL);
        lv_cb_set_style(cb, LV_CB_STYLE_BG, &st_cb_bg);
        lv_cb_set_text(cb, s_groups[i].label);
        lv_cb_set_checked(cb, (s_mask & s_groups[i].bit) != 0);
        lv_obj_set_pos(cb, GUI_MARGIN - CB_PAD, GUI_TOP + i * ROW_H);
        lv_group_add_obj(gui_group(), cb);
        s_cb[i] = cb;
    }

    lv_obj_t *t = gui_label(win, &gui_font_30, GUI_COL_TEXT, "UART output");
    lv_obj_set_pos(t, PORT_X, GUI_TOP);
    u32 row = 1;
    for (u32 i = 0; i < DBG_PORTS; i++) {
        s_port_cb[i] = NULL;
        if (i == DBG_UART_C && gui_is_lite())
            continue;
        lv_obj_t *cb = lv_cb_create(win, NULL);
        lv_cb_set_style(cb, LV_CB_STYLE_BG, &st_cb_bg);
        lv_cb_set_text(cb, gui_dbg_name((gui_dbg_port_t)i));
        lv_cb_set_checked(cb, i == gui_dbg_port());
        lv_cb_set_action(cb, _act_port);
        lv_obj_set_pos(cb, PORT_X - CB_PAD, GUI_TOP + row++ * ROW_H);
        lv_group_add_obj(gui_group(), cb);
        s_port_cb[i] = cb;
    }
    s_port_note = gui_hint(win, GUI_RIGHT - PORT_X, "");
    lv_obj_set_pos(s_port_note, PORT_X, GUI_TOP + row * ROW_H + 8);
    _port_note();
    if (s_port_task)
        lv_task_del(s_port_task);
    s_port_task = lv_task_create(_port_tick, PORT_NOTE_MS, LV_TASK_PRIO_LOW, NULL);

    lv_obj_t *note = gui_hint(win, GUI_CONTENT_W,
        "Only what is ticked is checked; the results say \"not checked\" "
        "for the rest.");
    lv_obj_set_pos(note, GUI_MARGIN, GUI_BOTTOM - 50);

    lv_group_focus_obj(run);
}
