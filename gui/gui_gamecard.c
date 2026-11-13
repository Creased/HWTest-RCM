/*
 * hwtest GUI - the game card slot, live.
 *
 * Whether a cartridge sits in the slot, from the slot's card-detect line,
 * read ten times a second. The line is wired straight to the connector
 * (slot pin 2, grounded inside the cartridge shell), so it reads with the
 * reader unpowered: no rail is switched on and nothing is sent to the
 * reader or to a cartridge. The reading is the health check's
 * (probe_gamecard.c), held while the screen is open: the pad, which boots
 * muxed to I2CVI, a pulled-up input, PS3 a GPIO input; low is a cartridge,
 * high an empty slot. The pad goes back as it was on the way out.
 *
 * The title is out of reach: the reader's ASIC takes nothing but its
 * firmware, and loading that burns OTP fuses in it (docs/gamecard.md).
 */
#include "gui.h"

#define GC_CD_PAD       PINMUX_AUX_CAM_I2C_SDA   /* PS3 */
#define GPIO_S_MSK_CNF  0x488
#define GPIO_S_MSK_OE   0x498
#define GC_TICK_MS      100
#define GC_SAME         2       /* readings in a row before a change counts */

static lv_obj_t *s_gc_state, *s_gc_count;
static lv_task_t *s_gc_task;
static u32 s_gc_pad;
static bool s_gc_in, s_gc_seen;
static u32 s_gc_same, s_gc_ins, s_gc_outs;

static bool _gc_read(void)
{
    return !gpio_read(GPIO_PORT_S, GPIO_PIN_3);
}

static void _gc_show(void)
{
    lv_label_set_text(s_gc_state, s_gc_in
        ? "#" GUI_HEX_OK " " ICON_OK "  A cartridge is in the slot#"
        : "#" GUI_HEX_HINT " " ICON_INFO "  The slot is empty#");
    char buf[96];
    s_printf(buf, "Inserted %d %s, taken out %d %s since this screen opened.",
             s_gc_ins, s_gc_ins == 1 ? "time" : "times",
             s_gc_outs, s_gc_outs == 1 ? "time" : "times");
    lv_label_set_text(s_gc_count, buf);
}

/* A change counts once the line has read the same twice in a row: a
 * cartridge going in can make the contact chatter. */
static void _gc_tick(void *param)
{
    bool in = _gc_read();
    if (in != s_gc_seen) {
        s_gc_seen = in;
        s_gc_same = 0;
    }
    if (++s_gc_same < GC_SAME || in == s_gc_in)
        return;
    s_gc_in = in;
    if (in)
        s_gc_ins++;
    else
        s_gc_outs++;
    gui_uart_puts(in ? "gui: gamecard inserted\n" : "gui: gamecard taken out\n");
    _gc_show();
}

static void _gc_leave(void)
{
    if (s_gc_task)
        lv_task_del(s_gc_task);
    s_gc_task = NULL;
    PINMUX_AUX(GC_CD_PAD) = s_gc_pad;
}

void gui_gamecard_test(void)
{
    lv_obj_t *win = gui_win_new(ICON_CHIP " Game Card");
    gui_on_leave(_gc_leave);
    lv_coord_t w = GUI_CONTENT_W * 2 / 3;    /* a readable line length */

    /* The health check's reading. Masked writes: port S also carries the
     * fan tacho (PS7) and the charger status lines (PS1, PS6). */
    s_gc_pad = PINMUX_AUX(GC_CD_PAD);
    PINMUX_AUX(GC_CD_PAD) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP | PINMUX_TRISTATE;
    GP_MWR(GPIO_S_MSK_CNF, GPIO_PIN_3, 1);
    GP_MWR(GPIO_S_MSK_OE,  GPIO_PIN_3, 0);
    usleep(200);

    lv_obj_t *d = gui_hint(win, w,
        "Slide a cartridge in and take it out: the line below follows it. "
        "Only the slot's card-detect switch is read, with the reader off: "
        "nothing is sent to the reader or to the cartridge.");
    lv_obj_set_pos(d, GUI_MARGIN, GUI_TOP);

    lv_coord_t y = GUI_TOP + lv_obj_get_height(d) + 40;
    s_gc_state = gui_label(win, &gui_font_30, GUI_COL_TEXT, "");
    lv_obj_set_pos(s_gc_state, GUI_MARGIN, y);
    s_gc_count = gui_label(win, &gui_font_20, GUI_COL_HINT, "");
    lv_obj_set_pos(s_gc_count, GUI_MARGIN, y + 52);

    s_gc_in = s_gc_seen = _gc_read();
    s_gc_same = GC_SAME;
    s_gc_ins = s_gc_outs = 0;
    _gc_show();
    gui_uart_puts(s_gc_in ? "gui: gamecard screen, a cartridge is in\n"
                          : "gui: gamecard screen, the slot is empty\n");
    s_gc_task = lv_task_create(_gc_tick, GC_TICK_MS, LV_TASK_PRIO_MID, NULL);
}
