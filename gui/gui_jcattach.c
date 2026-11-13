/*
 * hwtest GUI - a Joy-Con slid onto a rail, shown.
 *
 * A soft glow in the Joy-Con's body colour (from its flash) along the edge
 * of the screen on its side: it fades in, rests a moment and fades out.
 *
 * LVGL does not draw it. This GUI redraws whole rows (gui_main.c), so a
 * glow the screen's height made every step of its fade a full-screen
 * redraw, about 250 ms each on a Mariko, and the GUI lagged through it.
 * Here the glow is a display controller window of its own, laid over the
 * GUI's window A: drawn once into a small buffer, blended by the display
 * controller through its alpha channel, and faded by the window's constant
 * K1, one register per step. Nothing of the GUI is redrawn. Neither the TV
 * nor a screenshot shows it: both take the GUI's own buffer.
 *
 * The panel is portrait and the GUI is turned onto it (the VIC's 270, as
 * Nyx does): the GUI's left edge is the panel's last rows, its right edge
 * the first, and the GUI's y is the panel's x. So each glow is a strip
 * across the panel, the panel's width long and GLOW_W deep.
 */
#include "gui.h"
#include <display/di.h>
#include <mem/heap.h>
#include <soc/t210.h>

#define GLOW_W       96          /* how far in it reaches              */
#define GLOW_ENDS    160         /* how far it softens at top and bottom */
#define GLOW_PEAK    150         /* its alpha at the edge, of 255      */
#define GLOW_LIFT    0x60        /* darker colours are lifted to show  */
#define GLOW_FADE_MS 300
#define GLOW_REST_MS 700

#define PANEL_W      720         /* the panel as it scans: portrait    */
#define PANEL_H      1280

/* Per side, left then right: the window (A is the GUI's, D bdk's log) and
 * the panel row its strip starts at. */
static const u32 s_win[2] = { 1, 2 };                 /* B, C */
static const u32 s_row[2] = { PANEL_H - GLOW_W, 0 };  /* bottom, top */

static lv_color_t *s_pix[2];
static bool s_on[2];
static u8 s_var[2];              /* the fade's handle, one per side */

static void _select(int side)
{
    DISPLAY_A(DC_CMD_DISPLAY_WINDOW_HEADER) = BIT(WINDOW_SELECT + s_win[side]);
}

static void _commit(int side)
{
    DISPLAY_A(DC_CMD_STATE_CONTROL) = GENERAL_UPDATE  | BIT(WIN_UPDATE  + s_win[side]);
    DISPLAY_A(DC_CMD_STATE_CONTROL) = GENERAL_ACT_REQ | BIT(WIN_ACT_REQ + s_win[side]);
}

/* Over everything (depth 0), each pixel blended by K1 times its alpha. */
static void _k1(int side, u32 k1)
{
    _select(side);
    DISPLAY_A(DC_WINBUF_BLEND_LAYER_CONTROL) = WIN_BLEND_ENABLE | WIN_K1(k1) | WIN_BLEND_DEPTH(0);
    _commit(side);
}

/* The registers hekate sets for its log window D, for a strip. */
static void _win_on(int side)
{
    _select(side);
    DISPLAY_A(DC_WIN_WIN_OPTIONS)      = 0;
    DISPLAY_A(DC_WIN_COLOR_DEPTH)      = WIN_COLOR_DEPTH_B8G8R8A8;
    DISPLAY_A(DC_WIN_POSITION)         = V_POSITION(s_row[side]) | H_POSITION(0);
    DISPLAY_A(DC_WIN_H_INITIAL_DDA)    = 0;
    DISPLAY_A(DC_WIN_V_INITIAL_DDA)    = 0;
    DISPLAY_A(DC_WIN_PRESCALED_SIZE)   = V_PRESCALED_SIZE(GLOW_W) | H_PRESCALED_SIZE(PANEL_W * 4);
    DISPLAY_A(DC_WIN_DDA_INC)          = V_DDA_INC(0x1000) | H_DDA_INC(0x1000);
    DISPLAY_A(DC_WIN_SIZE)             = V_SIZE(GLOW_W) | H_SIZE(PANEL_W);
    DISPLAY_A(DC_WIN_LINE_STRIDE)      = UV_LINE_STRIDE(PANEL_W * 2) | LINE_STRIDE(PANEL_W * 4);
    DISPLAY_A(DC_WIN_BUFFER_CONTROL)   = BUFFER_CONTROL_HOST;
    DISPLAY_A(DC_WINBUF_SURFACE_KIND)  = PITCH;
    DISPLAY_A(DC_WINBUF_START_ADDR)    = (u32)s_pix[side];
    DISPLAY_A(DC_WINBUF_ADDR_H_OFFSET) = 0;
    DISPLAY_A(DC_WINBUF_ADDR_V_OFFSET) = 0;
    DISPLAY_A(DC_WINBUF_BLEND_LAYER_CONTROL) = WIN_BLEND_ENABLE | WIN_K1(0) | WIN_BLEND_DEPTH(0);
    DISPLAY_A(DC_WINBUF_BLEND_MATCH_SELECT)  = WIN_BLEND_FACT_SRC_COLOR_MATCH_SEL_K1_TIMES_SRC |
                                               WIN_BLEND_FACT_DST_COLOR_MATCH_SEL_NEG_K1_TIMES_SRC;
    DISPLAY_A(DC_WIN_WIN_OPTIONS)      = WIN_ENABLE;
    _commit(side);
    s_on[side] = true;
}

static void _win_off(int side)
{
    if (!s_on[side])
        return;
    s_on[side] = false;
    display_window_disable(s_win[side]);
}

static void _fade(void *var, int32_t v)
{
    _k1((u8 *)var - s_var, v);
}

static void _gone(void *var)
{
    _win_off((u8 *)var - s_var);
}

/* The colour, lifted towards white as far as it lacks brightness, so a
 * black or dark grey Joy-Con still shows on the dark background. */
static lv_color_t _colour(const u8 *rgb)
{
    u32 lum = (rgb[0] * 3 + rgb[1] * 6 + rgb[2]) / 10;
    u32 k = lum < GLOW_LIFT ? (GLOW_LIFT - lum) * 128 / GLOW_LIFT : 0;  /* of 256 */
    u8 c[3];
    for (u32 i = 0; i < 3; i++)
        c[i] = rgb[i] + ((255 - rgb[i]) * k >> 8);
    return LV_COLOR_MAKE(c[0], c[1], c[2]);
}

/* The strip for one side: alpha falls off with the square of the distance
 * from the edge, and eases in over GLOW_ENDS at either end of the rail. In
 * the panel's terms the distance runs down the strip's rows, from its
 * first row for the right Joy-Con (top) and from its last for the left. */
static bool _render(int side, lv_color_t col)
{
    if (!s_pix[side]) {
        u8 *mem = malloc(PANEL_W * GLOW_W * sizeof(lv_color_t) + 0x100);   /* kept */
        if (!mem)
            return false;
        s_pix[side] = (lv_color_t *)ALIGN((u32)mem, 0x100);
    }
    static u32 ends[PANEL_W];
    for (u32 x = 0; x < PANEL_W; x++) {
        u32 end = x < PANEL_W - 1 - x ? x : PANEL_W - 1 - x;
        u32 e = end >= GLOW_ENDS ? 256 : end * 256 / GLOW_ENDS;
        ends[x] = e * (512 - e) >> 8;                        /* eased */
    }
    lv_color_t *p = s_pix[side];
    for (u32 y = 0; y < GLOW_W; y++) {
        u32 d = side ? y : GLOW_W - 1 - y;                   /* from the edge */
        u32 f = (GLOW_W - d) * 256 / GLOW_W;
        f = GLOW_PEAK * (f * f >> 8);
        for (u32 x = 0; x < PANEL_W; x++) {
            lv_color_t c = col;
            c.alpha = f * ends[x] >> 16;
            *p++ = c;
        }
    }
    return true;
}

void gui_jc_attach_show(int side, const u8 *body)
{
    lv_anim_del(&s_var[side], NULL);
    _win_off(side);
    if (!_render(side, _colour(body)))
        return;
    _win_on(side);

    /* In, a rest, and out; then the window is switched off. */
    lv_anim_t a;
    memset(&a, 0, sizeof(a));
    a.var = &s_var[side];
    a.fp = _fade;
    a.end_cb = _gone;
    a.path = lv_anim_path_ease_in_out;
    a.start = 0;
    a.end = 255;
    a.time = GLOW_FADE_MS;
    a.playback = 1;
    a.playback_pause = GLOW_REST_MS;
    lv_anim_create(&a);
}
