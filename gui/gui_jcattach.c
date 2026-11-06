/*
 * hwtest GUI - a Joy-Con slid onto a rail, shown.
 *
 * A soft glow in the Joy-Con's body colour (from its flash) along the edge
 * of the screen on its side: it fades in, rests a moment and fades out.
 * The glow is an image with an alpha channel, strongest at the edge and
 * gone a little way in, softened towards the top and bottom; only its
 * opacity is animated.
 */
#include "gui.h"
#include <mem/heap.h>

#define GLOW_W       96          /* how far in it reaches              */
#define GLOW_ENDS    160         /* how far it softens at top and bottom */
#define GLOW_PEAK    150         /* its alpha at the edge, of 255      */
#define GLOW_LIFT    0x60        /* darker colours are lifted to show  */
#define GLOW_FADE_MS 300
#define GLOW_REST_MS 700

static lv_color_t *s_pix[2];
static lv_img_dsc_t s_img[2];
static lv_style_t s_st[2];
static lv_obj_t *s_shown[2];

static void _gone(void *obj)
{
    for (u32 i = 0; i < 2; i++)
        if (s_shown[i] == obj)
            s_shown[i] = NULL;
    lv_obj_del(obj);
}

static void _opa(void *obj, int32_t v)
{
    lv_style_t *st = (lv_style_t *)lv_obj_get_style(obj);
    st->image.opa = v;
    lv_obj_invalidate(obj);
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

/* The glow for one side: alpha falls off with the square of the distance
 * from the edge, and eases in over GLOW_ENDS at either end. */
static bool _render(int side, lv_color_t col)
{
    if (!s_pix[side]) {
        s_pix[side] = malloc(GLOW_W * GUI_H * sizeof(lv_color_t));
        if (!s_pix[side])
            return false;
    }
    lv_color_t *p = s_pix[side];
    for (u32 y = 0; y < GUI_H; y++) {
        u32 end = y < GUI_H - 1 - y ? y : GUI_H - 1 - y;
        u32 ends = end >= GLOW_ENDS ? 256 : end * 256 / GLOW_ENDS;
        ends = ends * (512 - ends) >> 8;                     /* eased */
        for (u32 x = 0; x < GLOW_W; x++) {
            u32 d = side ? GLOW_W - 1 - x : x;               /* from the edge */
            u32 f = (GLOW_W - d) * 256 / GLOW_W;
            f = f * f >> 8;
            lv_color_t c = col;
            c.alpha = GLOW_PEAK * f * ends >> 16;
            p[y * GLOW_W + x] = c;
        }
    }

    lv_img_dsc_t *img = &s_img[side];
    img->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    img->header.always_zero = 0;
    img->header.w = GLOW_W;
    img->header.h = GUI_H;
    img->data_size = GLOW_W * GUI_H * sizeof(lv_color_t);
    img->data = (const u8 *)p;
    return true;
}

void gui_jc_attach_show(int side, const u8 *body)
{
    if (s_shown[side])
        _gone(s_shown[side]);
    if (!_render(side, _colour(body)))
        return;

    lv_style_copy(&s_st[side], &lv_style_plain);
    s_st[side].image.opa = LV_OPA_TRANSP;

    lv_obj_t *glow = lv_img_create(lv_layer_top(), NULL);
    lv_img_set_src(glow, &s_img[side]);
    lv_img_set_style(glow, &s_st[side]);
    lv_obj_set_click(glow, false);
    lv_obj_set_pos(glow, side ? GUI_W - GLOW_W : 0, 0);

    /* In, a rest, and out; then it is deleted. */
    lv_anim_t a;
    memset(&a, 0, sizeof(a));
    a.var = glow;
    a.fp = _opa;
    a.end_cb = _gone;
    a.path = lv_anim_path_ease_in_out;
    a.start = LV_OPA_TRANSP;
    a.end = LV_OPA_COVER;
    a.time = GLOW_FADE_MS;
    a.playback = 1;
    a.playback_pause = GLOW_REST_MS;
    lv_anim_create(&a);
    s_shown[side] = glow;
}
