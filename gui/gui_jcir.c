/*
 * hwtest GUI - the right Joy-Con's infrared camera.
 *
 * The right Joy-Con carries a 320 x 240 infrared camera with a controller
 * of its own (the NFC/IR MCU), which the console drives through output
 * report 0x11 and reads through input report 0x31. Nothing in bdk touches
 * it, so this screen takes the right rail over (gui_jcwire.h) and runs the
 * sequence CTCaer's jc_toolkit established, on the wire instead of over
 * Bluetooth: wake the controller, switch it to camera mode, set the image
 * size and the sensor's registers, then ask for the frames fragment by
 * fragment, 300 pixels at a time, acknowledging each. What the camera sees
 * is shown enlarged; a hand in front of it is the test.
 */
#include "gui.h"
#include "gui_jcwire.h"

#define IR_SIDE      1                   /* the right Joy-Con            */
#define IMG_W        480                 /* as shown                     */
#define IMG_H        360
#define FRAG_LEN     300                 /* pixels in one 0x31 report    */
#define MCU_RPT_LEN  362                 /* a 0x31 report                */
#define IR_TRY_MS    64                  /* a reply's allowance          */
#define IR_STALL_MS  6000                /* no frames: started over      */
#define IR_STEPS     8
#define IR_TEXT_X    (GUI_MARGIN + IMG_W + GUI_GAP)
#define IR_TEXT_W    (GUI_RIGHT - IR_TEXT_X)

enum {
    IR_RPT_MODE,      /* input reports of the 0x31 kind                  */
    IR_MCU_ON,        /* the controller out of suspend                   */
    IR_MCU_STATUS,    /* ...and ready (standby)                          */
    IR_MODE_SET,      /* camera mode asked for                           */
    IR_MODE_STATUS,   /* ...and reached                                  */
    IR_IMAGE_SIZE,    /* image transfer, in this many fragments          */
    IR_SENSOR,        /* resolution, exposure, lights, gain, filter      */
    IR_LIGHTS,        /* light intensity, de-noise, buffer timing, apply */
    IR_STREAM,
    IR_FAILED,
    IR_OFF,
};

static const char *s_step_name[IR_STEPS] = {
    "asking for camera reports", "waking the camera's controller",
    "waiting for it to be ready", "switching it to camera mode",
    "confirming camera mode", "setting the image size",
    "setting the sensor up", "setting the lights up",
};

typedef struct {
    u16 w, h;
    u8  reg;              /* the sensor's resolution register             */
    u16 frags;            /* fragments per frame                          */
    u8  update;           /* the controller's buffer update time          */
    const char *name;
} ir_size_t;

static const ir_size_t s_sizes[] = {
    {  80,  60, 0x64,  16, 0x32, "80 x 60"   },
    { 160, 120, 0x50,  64, 0x32, "160 x 120" },
    { 320, 240, 0x00, 256, 0x32, "320 x 240" },
    {  40,  30, 0x69,   4, 0x2D, "40 x 30"   },
};

static struct {
    int  state, failed;   /* failed: the step that gave up                */
    u32  size;            /* index into s_sizes                           */
    u32  tries, sent_at;
    u8   last_id, last[8]; /* of the last reply, for the failure line     */
    bool owned;
    u32  frames, fps10;
    u32  fps_at, fps_n;   /* the rate: frames over the last second or so   */
    u32  last_frag, last_frag_at;
    bool stalled;         /* no fragment for seconds, and said so         */
    u8   avg;             /* the controller's own brightness figure, 0..255 */
    bool lit;             /* a frame with something in it has been seen   */
    u16  col[IMG_W];      /* the source column of each shown one          */
} s;

static u8 s_frame[320 * 240];
static lv_color_t s_pix[IMG_W * IMG_H];
static lv_img_dsc_t s_img;
static lv_obj_t *s_img_obj, *s_status, *s_detail, *s_verdict, *s_caption;
static lv_task_t *s_ir_task;

/* ---- the reports ------------------------------------------------------ */

static void _out(u8 *rpt, u8 id, u8 subcmd)
{
    static const u8 mute[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };
    memset(rpt, 0, JC_RPT_LEN);
    rpt[0] = id;
    memcpy(rpt + 2, mute, sizeof(mute));
    rpt[10] = subcmd;
}

/* Subcommand 0x21 configures the controller: its checksum covers bytes 12
 * to 47. */
static void _send_cfg(u8 *rpt)
{
    rpt[48] = gui_jc_crc8(rpt + 12, 36);
    gui_jc_send(IR_SIDE, rpt, JC_RPT_LEN);
}

/* An 0x11 request to it: the checksum covers bytes 11 to 46, and byte 48
 * is 0xFF. */
static void _send_req(u8 *rpt)
{
    rpt[47] = gui_jc_crc8(rpt + 11, 36);
    rpt[48] = 0xFF;
    gui_jc_send(IR_SIDE, rpt, JC_RPT_LEN);
}

static void _reg(u8 *p, u16 addr, u8 val)
{
    p[0] = addr & 0xFF;
    p[1] = addr >> 8;
    p[2] = val;
}

static void _ack(u32 frag)
{
    u8 r[JC_RPT_LEN];
    _out(r, JC_OUT_MCU, 0x03);
    r[14] = frag;
    _send_req(r);
    s.sent_at = get_tmr_ms();
}

static void _send(void)
{
    const ir_size_t *z = &s_sizes[s.size];
    u8 r[JC_RPT_LEN];
    switch (s.state) {
    case IR_RPT_MODE:
        _out(r, JC_OUT_SUBCMD, 0x03);
        r[11] = JC_IN_MCU;
        gui_jc_send(IR_SIDE, r, JC_RPT_LEN);
        break;
    case IR_MCU_ON:
        _out(r, JC_OUT_SUBCMD, 0x22);
        r[11] = 0x01;
        gui_jc_send(IR_SIDE, r, JC_RPT_LEN);
        break;
    case IR_MCU_STATUS:
    case IR_MODE_STATUS:
        _out(r, JC_OUT_MCU, 0x01);
        gui_jc_send(IR_SIDE, r, JC_RPT_LEN);
        break;
    case IR_MODE_SET:
        _out(r, JC_OUT_SUBCMD, 0x21);
        r[11] = 0x21;             /* set the controller's mode...          */
        r[12] = 0x00;
        r[13] = 0x05;             /* ...to IR                              */
        _send_cfg(r);
        break;
    case IR_IMAGE_SIZE:
        _out(r, JC_OUT_SUBCMD, 0x21);
        r[11] = 0x23;             /* IR configuration...                   */
        r[12] = 0x01;             /* ...the mode                           */
        r[13] = 0x07;             /* image transfer                        */
        r[14] = z->frags - 1;     /* the last fragment's number            */
        r[15] = 0x00;             /* controller firmware 5.18 or later     */
        r[16] = 0x05;
        r[17] = 0x00;
        r[18] = 0x18;
        _send_cfg(r);
        break;
    case IR_SENSOR:
        _out(r, JC_OUT_SUBCMD, 0x21);
        r[11] = 0x23;
        r[12] = 0x04;             /* register writes                       */
        r[13] = 9;
        _reg(r + 14, 0x2E00, z->reg);   /* resolution                      */
        _reg(r + 17, 0x3001, 0x90);     /* exposure: 300 us (31.2 per us)  */
        _reg(r + 20, 0x3101, 0x24);
        _reg(r + 23, 0x3201, 0x00);     /* that exposure, not the maximum  */
        _reg(r + 26, 0x1000, 0x00);     /* both groups of lights on        */
        _reg(r + 29, 0x2E01, 0x10);     /* digital gain 1                  */
        _reg(r + 32, 0x2F01, 0x00);
        _reg(r + 35, 0x0E00, 0x00);     /* no external light filter        */
        _reg(r + 38, 0x4301, 0xC8);     /* white pixel threshold, default  */
        _send_cfg(r);
        /* And, as jc_toolkit does, the mode query straight after: the
         * controller answers it with the register write's acknowledgement
         * in front. */
        _out(r, JC_OUT_MCU, 0x03);
        r[11] = 0x02;
        _send_req(r);
        break;
    case IR_LIGHTS:
        _out(r, JC_OUT_SUBCMD, 0x21);
        r[11] = 0x23;
        r[12] = 0x04;
        r[13] = 8;
        _reg(r + 14, 0x1100, 0x0F);     /* far lights, full               */
        _reg(r + 17, 0x1200, 0x10);     /* near lights, full              */
        _reg(r + 20, 0x2D00, 0x00);     /* no flip                        */
        _reg(r + 23, 0x6701, 0x01);     /* de-noise on, its defaults      */
        _reg(r + 26, 0x6801, 0x23);
        _reg(r + 29, 0x6901, 0x44);
        _reg(r + 32, 0x0400, z->update);
        _reg(r + 35, 0x0700, 0x01);     /* apply                          */
        _send_cfg(r);
        break;
    case IR_STREAM:
        _ack(0);
        break;
    }
    s.sent_at = get_tmr_ms();
}

/* Whether a report is the answer the current step waits for. The checks
 * are jc_toolkit's. */
static bool _match(const u8 *p, u32 n)
{
    bool sub = n >= 24 && p[0] == JC_IN_SUBCMD && (p[13] & 0x80);
    bool mcu = n >= 57 && p[0] == JC_IN_MCU;
    switch (s.state) {
    case IR_RPT_MODE:    return sub && p[14] == 0x03;
    case IR_MCU_ON:      return sub && p[14] == 0x22;
    case IR_MCU_STATUS:  return mcu && p[49] == 0x01 && p[56] == 0x01;
    case IR_MODE_SET:    return sub && p[15] == 0x01 && p[22] == 0x01;
    case IR_MODE_STATUS: return mcu && p[49] == 0x01 && p[56] == 0x05;
    case IR_IMAGE_SIZE:  return sub && p[15] == 0x0B;
    case IR_SENSOR:      return sub && p[15] == 0x13 && p[16] == 0x00 && p[17] == 0x07;
    case IR_LIGHTS:      return sub && (p[15] == 0x23 ||
                                        (p[15] == 0x13 && p[16] == 0x00 && p[17] == 0x07));
    }
    return false;
}

/* ---- the picture ------------------------------------------------------ */

static void _blit(void)
{
    const ir_size_t *z = &s_sizes[s.size];
    for (u32 y = 0; y < IMG_H; y++) {
        const u8 *row = s_frame + (y * z->h / IMG_H) * z->w;
        lv_color_t *dst = s_pix + y * IMG_W;
        for (u32 x = 0; x < IMG_W; x++) {
            u32 g = row[s.col[x]];
            dst[x].full = 0xFF000000 | (g << 16) | (g << 8) | g;
        }
    }
    lv_obj_invalidate(s_img_obj);
}

static void _text(void)
{
    char buf[200];
    const ir_size_t *z = &s_sizes[s.size];
    if (s.state < IR_STREAM) {
        s_printf(buf, "Starting: step %d of %d, %s", s.state + 1, IR_STEPS,
                 s_step_name[s.state]);
        lv_label_set_text(s_status, buf);
        lv_label_set_text(s_detail, "");
        lv_label_set_text(s_verdict, "");
    } else if (s.state == IR_STREAM) {
        if (!s.frames)
            s_printf(buf, "Started, waiting for the first %s frame", z->name);
        else if (!s.fps10)
            s_printf(buf, "#" GUI_HEX_OK " Streaming %s#, measuring the rate", z->name);
        else
            s_printf(buf, "#" GUI_HEX_OK " Streaming %s# at %d.%d frames a second",
                     z->name, s.fps10 / 10, s.fps10 % 10);
        lv_label_set_text(s_status, buf);
        s_printf(buf, "Brightness %d%%, %d frame%s so far", s.avg * 100 / 255,
                 s.frames, s.frames == 1 ? "" : "s");
        lv_label_set_text(s_detail, buf);
        if (s.stalled)
            lv_label_set_text(s_verdict, "#" GUI_HEX_WARN " " ICON_WARN
                "  No frames are coming.# Is it still on the rail?");
        else if (s.lit)
            lv_label_set_text(s_verdict, "#" GUI_HEX_OK " " ICON_OK "  The camera sees.# "
                "Wave a hand in front of it and watch it move.");
        else if (s.frames >= 10)
            lv_label_set_text(s_verdict, "#" GUI_HEX_WARN " " ICON_WARN
                "  Frames come, but black.# The lights or the sensor may be dead, "
                "or the window is covered.");
    } else if (s.state == IR_FAILED) {
        s_printf(buf, "#" GUI_HEX_FAIL " " ICON_FAIL "  Could not start: step %d of %d, %s#",
                 s.failed + 1, IR_STEPS, s_step_name[s.failed]);
        lv_label_set_text(s_status, buf);
        if (!s.last_id)
            s_printf(buf, "Nothing came back, after %d tries.", s.tries);
        else
            s_printf(buf, "The Joy-Con answered %02X: %02X %02X %02X %02X %02X %02X %02X %02X",
                     s.last_id, s.last[0], s.last[1], s.last[2], s.last[3],
                     s.last[4], s.last[5], s.last[6], s.last[7]);
        lv_label_set_text(s_detail, buf);
        lv_label_set_text(s_verdict, "A Joy-Con whose controller does not answer at all "
            "may have an older firmware, or no camera.");
    }
    s_printf(buf, "What the camera sees, enlarged (%s)", z->name);
    lv_label_set_text(s_caption, buf);
}

/* ---- the state machine ------------------------------------------------ */

static void _fail(void)
{
    s.failed = s.state;
    s.state = IR_FAILED;
    _text();
}

static void _frame_done(u32 now)
{
    _blit();
    s.frames++;
    if (s.avg > 5)
        s.lit = true;
    /* Over a second, not frame to frame: fragments the Joy-Con had queued
     * can finish two frames in one tick. */
    s.fps_n++;
    if (now - s.fps_at >= 1000) {
        s.fps10 = s.fps_n * 10000 / (now - s.fps_at);
        s.fps_n = 0;
        s.fps_at = now;
    }
    _text();
}

static jc_rail_t s_ir_rail;          /* the right rail */
static void _ir_down(void);
static void _start(void);

static void _pump(u32 now)
{
    static u8 rpt[MCU_RPT_LEN];
    const ir_size_t *z = &s_sizes[s.size];
    for (u32 k = 0; k < 8; k++) {
        u32 n = gui_jc_recv(IR_SIDE, rpt, sizeof(rpt), 6);
        if (!n)
            break;
        if (rpt[0] != JC_IN_MCU || n < 59 + FRAG_LEN || rpt[49] != 0x03)
            continue;
        u32 frag = rpt[52];
        s.last_frag_at = now;
        s.stalled = false;
        /* The same fragment again means the acknowledgement was missed:
         * acknowledge it once more, and count nothing. */
        if (frag == s.last_frag) {
            _ack(frag);
            continue;
        }
        if (frag < z->frags)
            memcpy(s_frame + frag * FRAG_LEN, rpt + 59, FRAG_LEN);
        s.avg = rpt[53];
        s.last_frag = frag;
        _ack(frag);
        if (frag == (u32)z->frags - 1)
            _frame_done(now);
    }
    /* Nothing for a while: ask again for what follows the last fragment. */
    if (now - s.last_frag_at > 200 && now - s.sent_at > 200)
        _ack(s.last_frag == ~0u ? 0 : s.last_frag);
    if (now - s.last_frag_at > 3000 && !s.stalled) {
        s.stalled = true;
        _text();
    }
    if (now - s.last_frag_at > IR_STALL_MS) {
        gui_uart_puts("gui: ir: no frames for 6 s, starting the Joy-Con again\n");
        gui_jc_rail_lost(&s_ir_rail);
        _ir_down();
    }
}


static void _ir_say(const char *text)
{
    if (strcmp(lv_label_get_text(s_status), text))
        lv_label_set_text(s_status, text);
}

/* What the status line says while the right rail has no Joy-Con up. */
static void _ir_rail_say(void)
{
    switch (s_ir_rail.state) {
    case JCR_OFF:
        _ir_say("Waiting for the right Joy-Con, which has the camera: slide it onto "
                "its rail.");
        break;
    case JCR_UP:
        _ir_say(s_ir_rail.drops ? "It stopped sending, connecting again"
                                : "Attached, connecting");
        break;
    case JCR_STUCK:
        _ir_say(s_ir_rail.drops
            ? "#" GUI_HEX_WARN " " ICON_WARN "  It keeps stopping:# reseat it"
            : "#" GUI_HEX_WARN " " ICON_WARN "  Attached, but it does not answer:# reseat it");
        break;
    }
}

/* Its Joy-Con up and owned: the camera's start-up from the top. */
static void _ir_up(void)
{
    gui_uart_puts("gui: ir: the right Joy-Con is up, the camera starts\n");
    s.owned = true;
    _start();
}

/* Not owned any more: left its rail, or started again. */
static void _ir_down(void)
{
    s.owned = false;
    s.state = IR_OFF;
    lv_label_set_text(s_detail, "");
    lv_label_set_text(s_verdict, "");
    _ir_rail_say();
}

static void _ir_tick(void *param)
{
    int ev = gui_jc_rail_tick(&s_ir_rail, IR_SIDE);
    if (ev == JCR_EV_UP) {
        _ir_up();
        return;
    }
    if (ev == JCR_EV_GONE) {
        gui_uart_puts("gui: ir: the right Joy-Con left its rail\n");
        _ir_down();
        return;
    }
    if (ev == JCR_EV_STATE)
        _ir_rail_say();
    if (s_ir_rail.state != JCR_RUN)
        return;
    u32 now = get_tmr_ms();
    if (s.state >= IR_FAILED)
        return;
    if (s.state == IR_STREAM) {
        _pump(now);
        return;
    }
    static u8 rpt[MCU_RPT_LEN];
    u32 n;
    while ((n = gui_jc_recv(IR_SIDE, rpt, sizeof(rpt), 4))) {
        s.last_id = rpt[0];
        memcpy(s.last, rpt + (rpt[0] == JC_IN_MCU ? 49 : 13), sizeof(s.last));
        if (_match(rpt, n)) {
            s.state++;
            s.tries = 0;
            s.last_id = 0;
            if (s.state == IR_STREAM) {
                s.frames = 0;
                s.fps10 = 0;
                s.fps_n = 0;
                s.fps_at = now;
                s.lit = false;
                s.stalled = false;
                s.last_frag = ~0u;    /* none yet: the first 0 is new */
                s.last_frag_at = now;
            }
            _send();
            _text();
            return;
        }
    }
    if (now - s.sent_at >= IR_TRY_MS) {
        /* The controller takes a moment to boot: those two steps get longer. */
        u32 limit = (s.state == IR_MCU_STATUS || s.state == IR_MODE_STATUS) ? 40 : 8;
        if (++s.tries >= limit) {
            _fail();
            return;
        }
        _send();
    }
}

static void _start(void)
{
    const ir_size_t *z = &s_sizes[s.size];
    for (u32 x = 0; x < IMG_W; x++)
        s.col[x] = x * z->w / IMG_W;
    memset(s_frame, 0, sizeof(s_frame));
    _blit();
    s.state = IR_RPT_MODE;
    s.tries = 0;
    s.last_id = 0;
    s.frames = 0;
    s.avg = 0;
    _send();
    _text();
}

/* The controller back to sleep, before another start or the way out. */
static void _suspend(void)
{
    static const u8 off = 0;
    gui_jc_subcmd(IR_SIDE, 0x22, &off, 1, NULL);
}

static lv_res_t _act_size(lv_obj_t *b)
{
    if (!s.owned)
        return LV_RES_OK;
    s.size = (s.size + 1) % (sizeof(s_sizes) / sizeof(s_sizes[0]));
    _suspend();
    _start();
    return LV_RES_OK;
}

static lv_res_t _act_retry(lv_obj_t *b)
{
    if (!s.owned)
        return LV_RES_OK;
    _suspend();
    _start();
    return LV_RES_OK;
}

static void _ir_leave(void)
{
    if (s_ir_task)
        lv_task_del(s_ir_task);
    s_ir_task = NULL;
    if (s.owned) {
        static const u8 std = JC_IN_STD;
        _suspend();
        gui_jc_subcmd(IR_SIDE, 0x03, &std, 1, NULL);   /* ordinary reports again */
    }
    if (gui_jc_rail_end(&s_ir_rail, IR_SIDE))
        gui_jc_test_resumed();
    s.owned = false;
    s.state = IR_OFF;
}

void gui_jc_ir(void)
{
    lv_obj_t *win = gui_win_new(ICON_GAMEPAD " IR Camera");
    gui_set_back(gui_joycon_test);
    gui_jc_nav(false);
    gui_on_leave(_ir_leave);
    s.state = IR_OFF;

    if (gui_is_lite()) {
        lv_obj_t *l = gui_hint(win, GUI_CONTENT_W * 2 / 3,
            "Not available on a Switch Lite: it has no Joy-Con rails, and the "
            "camera is in the right Joy-Con.");
        lv_obj_set_pos(l, GUI_MARGIN, GUI_TOP);
        return;
    }

    gui_win_btn(win, ICON_REFRESH " Try Again", _act_retry);
    gui_win_btn(win, ICON_SLIDERS " Image Size", _act_size);

    s_img.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_img.header.always_zero = 0;
    s_img.header.w = IMG_W;
    s_img.header.h = IMG_H;
    s_img.data_size = sizeof(s_pix);
    s_img.data = (const u8 *)s_pix;
    s_img_obj = lv_img_create(win, NULL);
    lv_img_set_src(s_img_obj, &s_img);
    lv_obj_set_pos(s_img_obj, GUI_MARGIN, GUI_TOP);
    s_caption = gui_hint(win, IMG_W, "");
    lv_obj_set_pos(s_caption, GUI_MARGIN, GUI_TOP + IMG_H + 14);

    lv_obj_t *t = gui_label(win, &gui_font_30, GUI_COL_TEXT, "The right Joy-Con's camera");
    lv_obj_set_pos(t, IR_TEXT_X, GUI_TOP);
    s_status = gui_hint(win, IR_TEXT_W, "");
    lv_obj_set_pos(s_status, IR_TEXT_X, GUI_TOP + 48);
    s_detail = gui_hint(win, IR_TEXT_W, "");
    lv_obj_set_pos(s_detail, IR_TEXT_X, GUI_TOP + 84);
    s_verdict = gui_label_wrap(win, &gui_font_20, GUI_COL_TEXT, IR_TEXT_W, "");
    lv_obj_set_pos(s_verdict, IR_TEXT_X, GUI_TOP + 132);
    lv_obj_t *help = gui_hint(win, IR_TEXT_W,
        "Point it at your hand, 10 to 30 cm away. The Joy-Con's own infrared "
        "lights, invisible to the eye, light the hand up, so it shows pale on a "
        "dark background. With nothing in front of it the picture stays dark; "
        "that is normal.\n\nThe other Joy-Con is not polled while this runs, "
        "and both take a few seconds to come back afterwards.");
    lv_obj_align(help, NULL, LV_ALIGN_IN_BOTTOM_LEFT, IR_TEXT_X, -(GUI_H - GUI_BOTTOM));

    /* The camera starts once the right Joy-Con is up: at once when the
     * driver has it up, else once it is started here, slid on or not. */
    s.owned = false;
    s.size = 0;
    memset(s_frame, 0, sizeof(s_frame));
    _blit();
    _text();
    if (gui_jc_rail_start(&s_ir_rail, IR_SIDE))
        _ir_up();
    else
        _ir_rail_say();

    if (s_ir_task)
        lv_task_del(s_ir_task);
    s_ir_task = lv_task_create(_ir_tick, 5, LV_TASK_PRIO_MID, NULL);
}
