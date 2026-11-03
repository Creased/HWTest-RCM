/*
 * hwtest GUI stage - entry, display, input and navigation.
 *
 * hwtest.bin has already run hw_init, trained the DRAM, brought the panel
 * up and set UART-B up as the debug console (main.c) before it jumps here;
 * this stage only adds what the GUI needs on top. The display path is
 * Nyx's: LVGL renders a 1280x720 landscape picture into NYX_FB2_ADDRESS
 * and the VIC rotates it into the panel's portrait framebuffer.
 *
 * Input comes from three places, and any one is enough to use every screen:
 *  - the touchscreen, as a pointer;
 *  - the Joy-Con: stick and D-pad move the focus, A selects, B goes back;
 *  - VOL+/VOL-/POWER: next, previous, select - the only input a console
 *    with a dead touchscreen and no Joy-Con has left.
 */
#include "gui.h"
#include <display/vic.h>
#include <input/joycon.h>
#include <input/touch.h>

/* ---- display ------------------------------------------------------------ */

/* LVGL draws straight into the VIC's input buffer. Nyx has it draw into a
 * buffer of its own and copies each area into the VIC's afterwards; on the
 * BPMP's bus that copy is a large part of what a frame costs, so here it is
 * not made at all. Two things let LVGL draw in place. It lays an area out
 * with the area's width as the row stride, so every invalidated area is
 * widened to the full screen width (_round_area, LVGL's round_cb) and the
 * stride is always the framebuffer's. And it addresses its buffer from the
 * area's top row, so the address has to be moved to that row before each
 * area is drawn: LVGL has no hook for that, but before drawing an area it
 * asks the screen's newest child first whether it covers the area, and the
 * sentinel - an invisible full-screen object kept newest on every screen
 * (gui_sentinel_raise) - takes that call. */
static bool s_vic_on;
static bool s_dirty;
static lv_obj_t *s_sentinel;
static lv_style_t st_sentinel;
static u32 s_vic_wait_us;

static void _vic_switch(void)
{
    vic_surface_t sfc = {
        .src_buf  = NYX_FB2_ADDRESS,
        .dst_buf  = NYX_FB_ADDRESS,
        .width    = GUI_W,
        .height   = GUI_H,
        .pix_fmt  = VIC_PIX_FORMAT_X8R8G8B8,
        .rotation = VIC_ROTATION_270,
    };
    vic_init();
    vic_set_surface(&sfc);
    vic_compose();
    vic_wait_idle();
    display_init_window_a_pitch_vic();
}

static void _round_area(lv_area_t *a)
{
    a->x1 = 0;
    a->x2 = GUI_W - 1;
}

static bool _sentinel_design(lv_obj_t *obj, const lv_area_t *mask,
                             lv_design_mode_t mode)
{
    if (mode != LV_DESIGN_COVER_CHK)
        return true;    /* nothing to draw */
    /* The VIC may still be reading the previous frame out of the buffer. */
    if (s_vic_on) {
        u32 t0 = get_tmr_us();
        vic_wait_idle();
        s_vic_wait_us += get_tmr_us() - t0;
    }
    lv_vdb_set_adr((u32 *)NYX_FB2_ADDRESS + mask->y1 * GUI_W, NULL);
    return false;
}

/* Called whenever something is created directly on the screen (a root, a
 * message box), so the sentinel stays its newest child. */
void gui_sentinel_raise(void)
{
    if (s_sentinel)
        lv_obj_del(s_sentinel);
    s_sentinel = lv_obj_create(lv_scr_act(), NULL);
    lv_obj_set_style(s_sentinel, &st_sentinel);
    lv_obj_set_size(s_sentinel, GUI_W, GUI_H);
    lv_obj_set_click(s_sentinel, false);
    lv_obj_set_design_func(s_sentinel, _sentinel_design);
}

static void _disp_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        const lv_color_t *src)
{
    (void)y1; (void)y2; (void)src;    /* already in the framebuffer */
    /* The window is only pointed at the VIC output once a full first frame
     * exists, so the panel never shows a half-drawn screen. After that, a
     * frame can arrive in several areas, and each VIC pass rotates the
     * whole picture: _present() runs it once per frame instead. */
    if (s_vic_on)
        s_dirty = true;
    else if (x2 - x1 + 1 == GUI_W) {
        s_vic_on = true;
        _vic_switch();
    }
    lv_flush_ready();
}

static void _present(void)
{
    if (s_dirty) {
        s_dirty = false;
        vic_compose();
    }
}

void gui_refresh_now(void)
{
    lv_refr_now();
    _present();
}

/* A line for the debug UART - only while it is still the debug UART: once
 * a right Joy-Con sits on the rail, bdk's driver owns UART-B and anything
 * written to it lands in the Joy-Con's protocol. */
void gui_uart_puts(const char *s)
{
    if (g_uart_debug)
        uart_send_crlf(UART_B, (u8 *)s, strlen(s));
}

/* What a frame costs on this console, on the debug UART every couple of
 * seconds while anything is being drawn: the drawing itself, and the time
 * spent waiting for the VIC to finish rotating the previous frame. */
static void _frame_stat(u32 us)
{
    static u32 n, sum, worst, since, vic;
    u32 now = get_tmr_ms();
    if (!n)
        since = now;
    n++;
    sum += us;
    if (us > worst)
        worst = us;
    vic += s_vic_wait_us;
    s_vic_wait_us = 0;
    if (now - since < 2000)
        return;
    char buf[96];
    s_printf(buf, "gui: %d frames, %d ms avg, %d ms worst, VIC wait %d ms/frame\n",
             n, sum / n / 1000, worst / 1000, vic / n / 1000);
    gui_uart_puts(buf);
    n = sum = worst = vic = 0;
}

static void _display_init(void)
{
    lv_disp_drv_t disp;
    lv_disp_drv_init(&disp);
    disp.disp_flush = _disp_flush;
    lv_disp_drv_register(&disp);

    /* Whatever the first frame does not cover, the VIC still rotates. */
    memset((void *)NYX_FB2_ADDRESS, 0, GUI_W * GUI_H * sizeof(u32));
    lv_vdb_set_adr((void *)NYX_FB2_ADDRESS, NULL);
    lv_refr_set_round_cb(_round_area);

    /* Fully opaque to LVGL's cover query, so it is asked; its own "empty"
     * flag keeps it from being drawn. */
    lv_style_copy(&st_sentinel, &lv_style_transp);
    st_sentinel.body.opa = LV_OPA_COVER;
    gui_sentinel_raise();
}

/* ---- input -------------------------------------------------------------- */

static touch_event_t s_touch;
static bool s_touch_ok;
static bool s_jc_ok;
static bool s_input_hold;
static bool s_jc_nav = true;
static gui_touch_sink_t s_touch_sink;
static bool s_lite;

bool gui_touch_ok(void) { return s_touch_ok; }
bool gui_jc_ok(void)    { return s_jc_ok; }
bool gui_is_lite(void)  { return s_lite; }
void gui_input_hold(bool hold) { s_input_hold = hold; }
void gui_jc_nav(bool on) { s_jc_nav = on; }
void gui_touch_sink(gui_touch_sink_t sink) { s_touch_sink = sink; }

static bool _touch_read(lv_indev_data_t *data)
{
    if (!s_touch_ok || s_input_hold) {
        data->state = LV_INDEV_STATE_REL;
        return false;
    }
    touch_poll(&s_touch);
    /* A second finger takes a screenshot, as a third does in Nyx. */
    if (s_touch.finger >= 2) {
        gui_screenshot();
        data->state = LV_INDEV_STATE_REL;
        return false;
    }
    /* A test screen can take the samples for itself; LVGL then sees none. */
    if (s_touch_sink && s_touch_sink(&s_touch)) {
        data->state = LV_INDEV_STATE_REL;
        return false;
    }
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.touch ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    return false;
}

/* The right rail's IsAttached line (PH6) shares its contact with UART-B
 * TX, so it can only be read with TX released - which is how bdk's
 * Joy-Con driver reads it, and this does the same. A real Joy-Con pulls
 * it low; the UART adapter used on a test bench does not. */
static bool _right_rail_attached(void)
{
    PINMUX_AUX(PINMUX_AUX_UART2_TX) = PINMUX_INPUT_ENABLE;
    gpio_direction_input(GPIO_PORT_G, GPIO_PIN_0);
    usleep(20);
    (void)gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    bool on = !gpio_read(GPIO_PORT_H, GPIO_PIN_6);
    PINMUX_AUX(PINMUX_AUX_UART2_TX) = 0;
    gpio_config(GPIO_PORT_G, GPIO_PIN_0, GPIO_MODE_SPIO);
    usleep(20);
    return on;
}

/* Give UART-B back to the debug console after jc_init_hw() re-clocked it
 * for the Joy-Con protocol: the same set-up main.c does before the GUI is
 * loaded. */
static void _uart_debug_restore(void)
{
    uart_init(UART_B, 115200, UART_AO_TX_AO_RX);
    gpio_config(GPIO_PORT_G, GPIO_PIN_1, GPIO_MODE_SPIO);
    PINMUX_AUX(PINMUX_AUX_UARTX_RX(UART_B)) = PINMUX_INPUT_ENABLE | PINMUX_PULL_UP;
}

/* The driver brings a Joy-Con up over several polls - handshake, the reply,
 * then status requests 15 ms apart - and starts over whenever 1.8 s pass
 * without a reply, so it has to be polled at the input rate all the time,
 * as Nyx does. Each poll briefly releases both rails' TX pins to read the
 * attach lines: while UART-B is still the debug console its last bytes go
 * out first, and the right rail is checked before the driver can take
 * UART-B over. */
static jc_gamepad_rpt_t *_jc_poll(void)
{
    if (!s_jc_ok)
        return NULL;
    if (g_uart_debug && !s_lite) {
        uart_wait_xfer(UART_B, UART_TX_IDLE);
        if (_right_rail_attached())
            g_uart_debug = false;    /* the Joy-Con owns UART-B from here on */
    }
    return joycon_poll();
}

jc_gamepad_rpt_t *gui_jc_state(void)
{
    return _jc_poll();
}

/* One stick (or the D-pad) as a direction: down/right move forward. The
 * stick is read from where it sat when its Joy-Con was first seen, as Nyx
 * does: Joy-Con sticks rest up to a tenth of their travel away from the
 * nominal centre, each unit its own way. The Lite's built-in controls
 * report Y inverted. */
static bool s_nav_cal, s_nav_from_l;
static u16 s_nav_cx, s_nav_cy;

static u32 _jc_key(const jc_gamepad_rpt_t *jc)
{
    if (jc->a)
        return LV_GROUP_KEY_ENTER;
    bool use_l = jc->conn_l;
    u16 sx = use_l ? jc->lstick_x : jc->rstick_x;
    u16 sy = use_l ? jc->lstick_y : jc->rstick_y;
    if (!s_nav_cal || use_l != s_nav_from_l) {
        /* Not while it is being held over: that is not its centre. */
        if (sx < 0x400 || sx > 0xC00 || sy < 0x400 || sy > 0xC00)
            return 0;
        s_nav_cx = sx;
        s_nav_cy = sy;
        s_nav_from_l = use_l;
        s_nav_cal = true;
    }
    u16 cx = s_nav_cx, cy = s_nav_cy;
    s32 dx = (s32)sx - cx, dy = (s32)sy - cy;
    if (jc->sio_mode)
        dy = -dy;
    bool fwd  = dy < -0x280 || dx > 0x280;
    bool back = dy > 0x280 || dx < -0x280;
    if (jc->down || jc->right || fwd)
        return LV_GROUP_KEY_NEXT;
    if (jc->up || jc->left || back)
        return LV_GROUP_KEY_PREV;
    return 0;
}

/* Buttons and Joy-Con as a keypad: NEXT/PREV walk the focus group, ENTER
 * activates. Joy-Con B is handled here rather than sent to LVGL: it means
 * "back" on every screen. A direction held down repeats, as a held key
 * does anywhere: LVGL's own repeat is kept off (lv_conf.h) because a
 * message box's buttons would fire on it, so a held direction is shown
 * to LVGL as a new press every so often. */
#define KEY_REPEAT_AFTER 400
#define KEY_REPEAT_EVERY 120

static bool _keys_read(lv_indev_data_t *data)
{
    static u32 last = LV_GROUP_KEY_ENTER;
    static u32 held, held_since, repeated;
    static bool b_held;
    u32 key = 0;

    if (!s_input_hold) {
        u8 btn = btn_read();
        if (btn & BTN_VOL_UP)
            key = LV_GROUP_KEY_NEXT;
        else if (btn & BTN_VOL_DOWN)
            key = LV_GROUP_KEY_PREV;
        else if (btn & BTN_POWER)
            key = LV_GROUP_KEY_ENTER;

        /* The Joy-Con test reads the Joy-Con itself (gui_jc_state); there
         * every button is under test, so none of them navigates. */
        jc_gamepad_rpt_t *jc = s_jc_nav ? _jc_poll() : NULL;
        if (jc && (jc->conn_l || jc->conn_r)) {
            if (!key)
                key = _jc_key(jc);
            if (jc->b && !b_held)
                gui_back();
            b_held = jc->b;
        } else {
            s_nav_cal = false;    /* the next Joy-Con is read afresh */
        }
    }

    bool pressed = key != 0;
    u32 now = get_tmr_ms();
    if (key && key == held && key != LV_GROUP_KEY_ENTER) {
        if (now - held_since >= KEY_REPEAT_AFTER && now - repeated >= KEY_REPEAT_EVERY) {
            repeated = now;
            pressed = false;    /* released for this one read */
        }
    } else {
        held = key;
        held_since = now;
        repeated = now;
    }

    /* A message box takes the focus while it is open, and picks between
     * its own buttons with LEFT/RIGHT. */
    if (gui_mbox_open()) {
        if (key == LV_GROUP_KEY_NEXT)
            key = LV_GROUP_KEY_RIGHT;
        else if (key == LV_GROUP_KEY_PREV)
            key = LV_GROUP_KEY_LEFT;
    }

    if (key)
        last = key;
    data->key = last;
    data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    return false;
}

static void _input_init(void)
{
    lv_indev_drv_t drv;

    s_touch_ok = !touch_power_on_safe();
    lv_indev_drv_init(&drv);
    drv.type = LV_INDEV_TYPE_POINTER;
    drv.read = _touch_read;
    lv_indev_drv_register(&drv);

    /* bdk's driver (built here without DEBUG_UART_PORT) takes UART-B and
     * UART-C. With nothing on the right rail, UART-B goes straight back to
     * being the debug console. On a Lite it drives the built-in controls
     * (the Sio) instead and leaves UART-B alone. */
    s_lite = fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG;
    jc_init_hw();
    s_jc_ok = true;
    if (!s_lite) {
        if (!_right_rail_attached())
            _uart_debug_restore();
        else
            g_uart_debug = false;
    }

    lv_indev_drv_init(&drv);
    drv.type = LV_INDEV_TYPE_KEYPAD;
    drv.read = _keys_read;
    lv_indev_t *keys = lv_indev_drv_register(&drv);
    lv_indev_set_group(keys, gui_group());
}

/* ---- navigation ---------------------------------------------------------- */

static gui_screen_fn_t s_pending;
static gui_screen_fn_t s_back = gui_home;
static void (*s_leave)(void);

void gui_show(gui_screen_fn_t screen)
{
    /* Deferred to the main loop: a screen is usually changed from inside an
     * action of the screen being deleted. Back leads home unless the new
     * screen says otherwise. */
    s_pending = screen;
}

void gui_on_leave(void (*fn)(void))
{
    s_leave = fn;
}

/* What a screen may have changed is put back before the next one is
 * built: its own clean-up first (tasks it started), then the input. */
static void _switch_screen(gui_screen_fn_t fn)
{
    if (s_leave) {
        void (*leave)(void) = s_leave;
        s_leave = NULL;
        leave();
    }
    s_back = gui_home;
    s_jc_nav = true;
    s_touch_sink = NULL;
    gui_statusbar_hide(false);
    fn();
}

void gui_set_back(gui_screen_fn_t screen)
{
    s_back = screen;
}

void gui_back(void)
{
    gui_show(s_back);
}

void gui_input_resume(void)
{
    s_touch_ok = !touch_power_on_safe();
}

/* ---- fonts -------------------------------------------------------------- */

void gui_fonts_init(void)
{
    lv_font_add(&gui_icons_30, &gui_font_30);
    lv_font_add(&gui_icons_20, &gui_font_20);
}

/* ---- entry --------------------------------------------------------------- */

void ipl_main(void)
{
    heap_init((void *)IPL_HEAP_START);
    bpmp_mmu_enable();
    bpmp_clk_rate_set(BPMP_CLK_NORMAL);

    /* Everything the probes print goes to UART and the report buffer; the
     * screen belongs to LVGL. */
    _log_uart_only = true;
    static const char banner[] = "\n=== hwtest GUI ===\n";
    uart_send_crlf(UART_B, (u8 *)banner, sizeof(banner) - 1);

    lv_init();
    _display_init();

    /* The PMIC's clear-on-read latches, before anything active runs - the
     * touchscreen and Joy-Con bring-up included. */
    probe_reset_snapshot();

    gui_hw_prepare();
    gui_clock_init();
    gui_theme_init();
    gui_statusbar_init();
    _input_init();
    gui_home();

    for (;;) {
        if (s_pending) {
            gui_screen_fn_t fn = s_pending;
            s_pending = NULL;
            _switch_screen(fn);
        }
        u32 t0 = get_tmr_us();
        lv_task_handler();
        if (s_dirty) {
            _present();
            _frame_stat(get_tmr_us() - t0);
        }
        uart_poll_reboot();
        usleep(500);
    }
}
